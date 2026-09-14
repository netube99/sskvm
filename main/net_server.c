// 自持 HTTPS/WSS 传输层。所有权规则见 net_server.h。
//
// 所有权规则（review 时逐条检查）：
//   1. 一个连接的 TLS 上下文只被它的会话任务访问 —— 没有全局发送锁、没有 fd 表
//   2. 会话结束 = 任务退出：esp_tls_server_session_delete() 后 close(fd)，fd 不会被别人关
//   3. 编码任务从不调用这里的发送 API（WS 回调里校验调用者就是会话任务）
//   4. 所有阻塞都有超时：握手 6s、HTTP 请求 8/15s、WS 读 2ms、发送 100ms
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "psa/crypto.h"
#include <mbedtls/base64.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "net_server.h"

static const char *TAG = "net";

#define HTTPS_PORT       443
#define MAX_SESSIONS     4          // 1 WS + ≤3 HTTP（agent keep-alive + 页面加载）
#define MAX_ROUTES       8
#define HDR_MAX          1536
#define BODY_MAX         1024
#define REQ_PER_CONN     8          // keep-alive 每条连接最多几个请求
#define FIRST_REQ_MS     8000
#define KEEPALIVE_MS     15000
#define REQ_TOTAL_MS     20000     // 整个请求（头+body）读完的硬上限，防慢速连接占满会话槽
#define HANDSHAKE_MS     6000
#define WS_READ_MS       2          // 会话循环节拍：键鼠入站延时上限
#define WS_FRAME_MS      200        // 已收到首字节后，整帧读完的超时
#define WS_IDLE_MS       15000      // 这么久没收到任何入站数据 → 判定对端死了
#define SEND_TIMEOUT_MS  100        // 单次发送阻塞上限；超了直接关会话

extern const uint8_t servercert_pem_start[] asm("_binary_servercert_pem_start");
extern const uint8_t servercert_pem_end[]   asm("_binary_servercert_pem_end");
extern const uint8_t prvtkey_pem_start[]    asm("_binary_prvtkey_pem_start");
extern const uint8_t prvtkey_pem_end[]      asm("_binary_prvtkey_pem_end");

typedef struct {
    const char *method;
    const char *path;
    net_handler_t cb;
    void *ctx;
} route_t;

typedef struct {
    esp_tls_t *tls;
    int fd;
    TaskHandle_t task;
    volatile bool close_req;
    int64_t last_rx_us;
    uint8_t scratch[HDR_MAX + BODY_MAX + 1];   // 静态：连接不再 malloc/free；+1 给 body 结尾的 NUL
    char aux[512];                          // handler 拼 JSON 的小缓冲
} sess_t;

static route_t s_routes[MAX_ROUTES];
static int s_route_n;
static const net_ws_ops_t *s_ws_ops;
static void *s_ws_ctx;
static struct {
    const char *method, *path;          // 流式路由（最多一条）：POST /ota 固件上传
    net_stream_handler_t cb;
    void *ctx;
} s_stream;

static sess_t s_sess[MAX_SESSIONS];
static QueueHandle_t s_sess_pool;      // 空闲会话槽
static SemaphoreHandle_t s_hs_mu;      // 握手串行化（bignum/ECC 并发保护）
static SemaphoreHandle_t s_ws_mu;      // WS 排他（只允许一个浏览器）
static sess_t *volatile s_ws_cur;      // 当前 WS 会话（只有它的任务会写）

// ---------------- 小工具 ----------------
static void set_timeo(int fd, int optname, int ms)
{
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    if (setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof(tv)) != 0) {
        // SO_SNDTIMEO 需要 LWIP_SO_SNDTIMEO=1（顶层 CMakeLists 全局定义）
        ESP_LOGW(TAG, "setsockopt(%d, %dms) failed: errno %d", optname, ms, errno);
    }
}

// 读一次（读到多少算多少）。返回 >0 字节数；0 = 超时没数据；-1 = 对端关闭/致命错误
static int tls_read_some(sess_t *s, uint8_t *buf, size_t cap, int ms)
{
    set_timeo(s->fd, SO_RCVTIMEO, ms);
    ssize_t r = esp_tls_conn_read(s->tls, buf, cap);
    if (r > 0) {
        return (int)r;
    }
    if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    return -1;
}

// 读满 n 字节。返回 n；0 = 超时且一个字节都没读到；-1 = 对端关闭/致命错误
static int tls_read_full(sess_t *s, uint8_t *buf, size_t n, int ms)
{
    size_t got = 0;
    while (got < n) {
        int r = tls_read_some(s, buf + got, n - got, ms);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == 0) {
            return got == 0 ? 0 : -1;
        } else {
            return -1;
        }
    }
    return (int)n;
}

// 一次写完整块。mbedtls_ssl_write 返回 WANT_WRITE 时官方支持"同参数重试"（记录
// 状态机停在 mbedtls 内部），但这里取保守策略：100ms 都写不完说明对端读不动了
// （LAN 下=死了），重试只会越积越多 —— 直接报失败，调用方关会话（有意取舍）。
static esp_err_t tls_write_all(sess_t *s, const void *data, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    set_timeo(s->fd, SO_SNDTIMEO, SEND_TIMEOUT_MS);
    ssize_t r = esp_tls_conn_write(s->tls, data, len);
    if (r == (ssize_t)len) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "send %u/%u failed (%d), closing", (unsigned)len, (unsigned)r, (int)r);
    return ESP_FAIL;
}

// 不区分大小写的子串判断（strcasestr 是 GNU 扩展，别赌 newlib 开了 _GNU_SOURCE）
static bool ci_contains(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static const char *status_text(int st)
{
    switch (st) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 403: return "Forbidden";
    case 500: return "Internal Server Error";
    default:  return "Error";
    }
}

bool net_query_get(const char *query, const char *key, char *out, size_t cap)
{
    if (!query || !key || cap == 0) {
        return false;
    }
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < cap) {
                if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                    char h[3] = { p[1], p[2], 0 };
                    out[o++] = (char)strtol(h, NULL, 16);
                    p += 3;
                } else {
                    out[o++] = *p++;
                }
            }
            out[o] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

// ---------------- WebSocket 发送（只在 WS 会话任务里） ----------------
static sess_t *ws_own(void)
{
    sess_t *s = s_ws_cur;
    if (s && s->task == xTaskGetCurrentTaskHandle()) {
        return s;
    }
    return NULL;
}

// 返回头长度；头写在 hdr[NET_WS_HDR_MAX - n .. NET_WS_HDR_MAX)
static size_t ws_build_header(uint8_t *hdr, size_t len, uint8_t opcode)
{
    size_t n;
    if (len <= 125) {
        n = 2;
        hdr[NET_WS_HDR_MAX - 2] = 0x80 | opcode;
        hdr[NET_WS_HDR_MAX - 1] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        n = 4;
        hdr[NET_WS_HDR_MAX - 4] = 0x80 | opcode;
        hdr[NET_WS_HDR_MAX - 3] = 126;
        hdr[NET_WS_HDR_MAX - 2] = (uint8_t)(len >> 8);
        hdr[NET_WS_HDR_MAX - 1] = (uint8_t)len;
    } else {
        n = 10;
        hdr[NET_WS_HDR_MAX - 10] = 0x80 | opcode;
        hdr[NET_WS_HDR_MAX - 9] = 127;
        for (int i = 0; i < 8; i++) {
            hdr[NET_WS_HDR_MAX - 8 + i] = (uint8_t)((uint64_t)len >> (56 - i * 8));
        }
    }
    return n;
}

esp_err_t net_ws_send_frame(uint8_t *payload, size_t len)
{
    sess_t *s = ws_own();
    if (!s) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t hdr[NET_WS_HDR_MAX];
    size_t hn = ws_build_header(hdr, len, 0x2);
    // 帧头在 payload-8；WS 头紧贴其前。头要连源偏移一起拷，不能拷 hdr 的前 hn 字节
    memcpy(payload - 8 - hn, hdr + NET_WS_HDR_MAX - hn, hn);
    return tls_write_all(s, payload - 8 - hn, hn + len);
}

esp_err_t net_ws_send_text(const char *str)
{
    sess_t *s = ws_own();
    if (!s || !str) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = strlen(str);
    if (len > 256) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t buf[NET_WS_HDR_MAX + 256];
    size_t hn = ws_build_header(buf, len, 0x1);
    memcpy(buf + NET_WS_HDR_MAX, str, len);
    return tls_write_all(s, buf + NET_WS_HDR_MAX - hn, hn + len);
}

bool net_ws_writable(void)
{
    sess_t *s = ws_own();
    if (!s) {
        return false;
    }
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s->fd, &wfds);
    struct timeval tv = { 0, 0 };
    return select(s->fd + 1, NULL, &wfds, NULL, &tv) > 0;
}

void net_ws_close(void)
{
    sess_t *s = ws_own();
    if (s) {
        s->close_req = true;
    }
}

// 读一个 WS 帧；返回 1 = 有帧，0 = 这段时间没数据，-1 = 关闭/协议错
static int ws_read_frame(sess_t *s, uint8_t *payload, size_t cap, size_t *out_len, uint8_t *out_op)
{
    uint8_t b[2];
    int r = tls_read_full(s, b, 1, WS_READ_MS);
    if (r <= 0) {
        return r;
    }
    if (tls_read_full(s, b + 1, 1, WS_FRAME_MS) <= 0) {
        return -1;
    }
    bool fin = b[0] & 0x80;
    uint8_t op = b[0] & 0x0f;
    bool masked = b[1] & 0x80;
    uint64_t len = b[1] & 0x7f;
    if (len == 126) {
        uint8_t e[2];
        if (tls_read_full(s, e, 2, WS_FRAME_MS) <= 0) {
            return -1;
        }
        len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        uint8_t e[8];
        if (tls_read_full(s, e, 8, WS_FRAME_MS) <= 0) {
            return -1;
        }
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | e[i];
        }
    }
    // 浏览器发来的帧必须 masked；RSV 位必须为 0（我们没有扩展）；上行只有
    // ≤13B 的输入帧，控制帧 ≤125B
    if (!masked || !fin || (b[0] & 0x70) || len > cap) {
        ESP_LOGW(TAG, "bad ws frame op=%u fin=%d masked=%d len=%u", op, fin, masked, (unsigned)len);
        return -1;
    }
    uint8_t mask[4];
    if (tls_read_full(s, mask, 4, WS_FRAME_MS) <= 0) {
        return -1;
    }
    if (len && tls_read_full(s, payload, (size_t)len, WS_FRAME_MS) <= 0) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        payload[i] ^= mask[i & 3];
    }
    *out_len = (size_t)len;
    *out_op = op;
    return 1;
}

static esp_err_t ws_send_control(uint8_t opcode, const uint8_t *data, size_t len)
{
    sess_t *s = ws_own();
    if (!s) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > 125) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[NET_WS_HDR_MAX + 125];
    size_t hn = ws_build_header(buf, len, opcode);
    // 头在 [MAX-hn, MAX)，payload 紧跟其后，一次写出去
    memcpy(buf + NET_WS_HDR_MAX, data, len);
    return tls_write_all(s, buf + NET_WS_HDR_MAX - hn, hn + len);
}

// ---------------- WS 会话循环 ----------------
static void ws_loop(sess_t *s)
{
    uint8_t payload[128];
    s->last_rx_us = esp_timer_get_time();
    s_ws_cur = s;
    if (s_ws_ops && s_ws_ops->open) {
        s_ws_ops->open(s_ws_ctx);
    }
    while (!s->close_req) {
        size_t len = 0;
        uint8_t op = 0;
        int r = ws_read_frame(s, payload, sizeof(payload), &len, &op);
        if (r < 0) {
            break;
        }
        if (r > 0) {
            s->last_rx_us = esp_timer_get_time();
            switch (op) {
            case 0x2:                                   // binary = 输入事件
                if (s_ws_ops && s_ws_ops->input) {
                    s_ws_ops->input(payload, len, s_ws_ctx);
                }
                break;
            case 0x9:                                   // ping → pong
                ws_send_control(0xA, payload, len);
                break;
            case 0xA:                                   // pong
                break;
            case 0x8:                                   // close：按 RFC 回一个 close 帧再收尾
                ws_send_control(0x8, payload, len > 125 ? 125 : len);
                s->close_req = true;
                break;
            default:
                break;                                  // 文本上行不用
            }
        }
        if (s_ws_ops && s_ws_ops->tick) {
            s_ws_ops->tick(s_ws_ctx);
        }
        if (esp_timer_get_time() - s->last_rx_us > (int64_t)WS_IDLE_MS * 1000) {
            ESP_LOGW(TAG, "ws idle %ds, dropping", WS_IDLE_MS / 1000);
            break;
        }
    }
    if (s_ws_ops && s_ws_ops->close) {
        s_ws_ops->close(s_ws_ctx);
    }
    s_ws_cur = NULL;
}

// ---------------- HTTP 请求解析 ----------------
typedef struct {
    char method[8];
    char path[128];
    char query[128];
    char auth_hdr[64];
    char host[40];          // Host 头（Origin 校验用）
    char origin[64];        // Origin 头（浏览器一定带；原生客户端没有）
    bool keep_alive;
    bool ws_upgrade;
    char ws_key[32];
    int content_len;
} hreq_t;

// [p, end) 内逐行扫描头部。有界解析，不依赖 NUL 终止符。返回是否找到。
static bool hdr_get(const char *p, const char *end, const char *name, char *out, size_t cap)
{
    size_t nlen = strlen(name);
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) {
            break;
        }
        const char *colon = memchr(p, ':', (size_t)(eol - p));
        if (colon && (size_t)(colon - p) == nlen && strncasecmp(p, name, nlen) == 0) {
            const char *v = colon + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) {
                v++;
            }
            size_t vlen = (size_t)(eol - v);
            while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ')) {
                vlen--;
            }
            if (vlen >= cap) {
                vlen = cap - 1;
            }
            memcpy(out, v, vlen);
            out[vlen] = 0;
            return true;
        }
        p = eol + 1;
    }
    return false;
}

// 流式路由：body 边收边喂给 handler（固件升级）。pre = 已随头部读进来的 body 字节。
static int run_stream(sess_t *s, hreq_t *h, net_req_t *req, uint8_t *pre, size_t pre_n);

// 返回 1 = 有请求，0 = 对端关闭/超时，-1 = 解析失败（回 400 后关），2 = 流式路由已自行应答
// first_req：keep-alive 的后续请求等首字节可以放宽（agent 轮询间隔可能比 8s 长）
static int http_read_request(sess_t *s, hreq_t *h, net_req_t *req, bool first_req)
{
    uint8_t *hdr = s->scratch;
    uint8_t *body = s->scratch + HDR_MAX;
    size_t total = 0, scanned = 0;
    int hdr_end = -1;
    int64_t deadline = 0;               // 首字节到达后启动：整个请求的硬上限（防 slowloris 占槽）

    while (total < HDR_MAX - 1) {
        // 头是流式的，不能"读满"：一次读到多少算多少（见 tls_read_some）
        int ms;
        if (total == 0) {
            ms = first_req ? FIRST_REQ_MS : KEEPALIVE_MS;
        } else if (deadline) {
            ms = (int)((deadline - esp_timer_get_time()) / 1000);
            if (ms <= 0) {
                return -1;              // 整请求超时：慢慢挤牙膏的连接直接断
            }
            if (ms > KEEPALIVE_MS) {
                ms = KEEPALIVE_MS;
            }
        } else {
            ms = KEEPALIVE_MS;
        }
        int r = tls_read_some(s, hdr + total, HDR_MAX - 1 - total, ms);
        if (r == 0) {
            return 0;
        }
        if (r < 0) {
            return total == 0 ? 0 : -1;
        }
        if (deadline == 0) {
            deadline = esp_timer_get_time() + (int64_t)REQ_TOTAL_MS * 1000;
        }
        size_t start = scanned > 3 ? scanned - 3 : 0;
        total += (size_t)r;
        for (size_t i = start; i + 4 <= total; i++) {
            if (memcmp(hdr + i, "\r\n\r\n", 4) == 0) {
                hdr_end = (int)(i + 4);
                break;
            }
        }
        scanned = total;
        if (hdr_end >= 0) {
            break;
        }
    }
    if (hdr_end < 0) {
        return -1;
    }
    size_t have = total - (size_t)hdr_end;

    // 请求行 + 头部全程有界解析（memchr 到 end），不依赖 NUL 终止符。
    // 旧版靠给头部打 NUL 终止，那个 NUL 会顶掉紧随其后的 body 首字节
    // （历史上顺序反了把 body 吃掉，POST /config 永远匹配不上）——整套坑已消灭
    const char *end = (const char *)hdr + hdr_end;
    char *line = (char *)hdr;
    char *sp1 = memchr(line, ' ', (size_t)(end - line));
    if (!sp1) {
        return -1;
    }
    *sp1 = 0;
    snprintf(h->method, sizeof(h->method), "%.7s", line);
    char *uri = sp1 + 1;
    char *sp2 = memchr(uri, ' ', (size_t)(end - uri));
    if (!sp2) {
        return -1;
    }
    *sp2 = 0;
    char *ver = sp2 + 1;                        // "HTTP/1.1\r\n..."
    char *eol = memchr(ver, '\n', (size_t)(end - ver));
    char *lines = eol ? eol + 1 : NULL;         // 第一行头部
    char *q = memchr(uri, '?', (size_t)(sp2 - uri));
    if (q) {
        *q = 0;
        snprintf(h->query, sizeof(h->query), "%.127s", q + 1);
    } else {
        h->query[0] = 0;
    }
    snprintf(h->path, sizeof(h->path), "%.127s", uri);
    if (!lines) {
        return -1;
    }
    bool streaming = s_stream.cb && !strcmp(h->method, s_stream.method) &&
                     !strcmp(h->path, s_stream.path);

    // 头部
    char v[64];
    h->keep_alive = (size_t)(eol - ver) >= 8 && memcmp(ver, "HTTP/1.1", 8) == 0;
    if (hdr_get(lines, end, "Connection", v, sizeof(v))) {
        if (ci_contains(v, "close")) {
            h->keep_alive = false;
        } else if (ci_contains(v, "keep-alive")) {
            h->keep_alive = true;
        }
    }
    if (hdr_get(lines, end, "Upgrade", v, sizeof(v)) && ci_contains(v, "websocket")) {
        h->ws_upgrade = true;
    }
    hdr_get(lines, end, "Sec-WebSocket-Key", h->ws_key, sizeof(h->ws_key));
    hdr_get(lines, end, "X-Auth-Token", h->auth_hdr, sizeof(h->auth_hdr));
    hdr_get(lines, end, "Host", h->host, sizeof(h->host));
    hdr_get(lines, end, "Origin", h->origin, sizeof(h->origin));
    h->content_len = 0;
    if (hdr_get(lines, end, "Content-Length", v, sizeof(v))) {
        // 严格解析：垃圾/溢出不再被 atoi 静默回绕
        char *endp = NULL;
        long cl = strtol(v, &endp, 10);
        if (*endp != 0 || cl < 0 || cl > 0x7FFFFFF) {
            return -1;
        }
        h->content_len = (int)cl;
        // 流式路由不做 BODY_MAX 上限（固件 ~1MB 级，另一头有总时限）
        if (!streaming && h->content_len > BODY_MAX) {
            return -1;
        }
    }

    if (streaming) {
        // 流式路由忽略 Upgrade 头（/ota 只按普通上传应答；101 只能从 /ws 路由出）
        return run_stream(s, h, req, hdr + hdr_end, have);
    }

    // body 可能一次读进来（curl/fetch 就是一次 write）：搬到独立区域，头部区不再动
    size_t bhave = have > BODY_MAX ? BODY_MAX : have;
    if (bhave) {
        memcpy(body, hdr + hdr_end, bhave);
    }

    // body 剩余部分（头部读多了的已经搬过了）。同样受整请求 deadline 约束
    if (h->content_len > (int)bhave) {
        int ms = (int)((deadline - esp_timer_get_time()) / 1000);
        if (ms <= 0) {
            return -1;
        }
        if (ms > KEEPALIVE_MS) {
            ms = KEEPALIVE_MS;
        }
        if (tls_read_full(s, body + bhave, (size_t)h->content_len - bhave, ms) <= 0) {
            return -1;
        }
    }
    body[h->content_len] = 0;

    req->method = h->method;
    req->path = h->path;
    req->query = h->query;
    req->auth_hdr = h->auth_hdr;
    req->body = (const char *)body;
    req->body_len = (size_t)h->content_len;
    req->aux = s->aux;
    req->aux_len = sizeof(s->aux);
    return 1;
}

// ---------------- 响应 ----------------
static esp_err_t send_response(sess_t *s, const net_resp_t *r, bool keep_alive)
{
    char buf[384 + 384];
    int st = r->status ? r->status : 200;
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "%sConnection: %s\r\n\r\n",
                     st, status_text(st), r->ctype ? r->ctype : "text/plain",
                     (unsigned)r->len, r->no_store ? "Cache-Control: no-store\r\n" : "",
                     keep_alive ? "keep-alive" : "close");
    // 小响应（状态 JSON/401/409…）头+body 合并成一次 TLS 写：两个记录在
    // Nagle+延迟 ACK 下第二段要多等一个 ACK（视频路径单写的同款教训）
    if (r->len && r->len <= sizeof(buf) - (size_t)n) {
        memcpy(buf + n, r->data, r->len);
        return tls_write_all(s, buf, (size_t)n + r->len);
    }
    esp_err_t e = tls_write_all(s, buf, (size_t)n);
    if (e == ESP_OK && r->len) {
        e = tls_write_all(s, r->data, r->len);
    }
    return e;
}

// 跨源驱动防护：浏览器一定带 Origin，原生客户端（kvm.py 等）没有。
// 有 Origin 时，其 host 部分必须与 Host 头一致（= 就是这台设备自己），
// 否则别的网页可以借操作者浏览器开 WS 拿键鼠 / simple-request 改配置。
// token 为空时这是唯一防线，所以写操作（POST/WS）一律过这道。
static bool origin_ok(const hreq_t *h)
{
    if (h->origin[0] == 0) {
        return true;                        // 原生客户端
    }
    const char *hp = strstr(h->origin, "://");
    hp = hp ? hp + 3 : h->origin;
    char oh[40], hh[40];
    snprintf(oh, sizeof(oh), "%.*s", (int)strcspn(hp, "/"), hp);
    snprintf(hh, sizeof(hh), "%.*s", (int)strcspn(h->host, ": \r\n"), h->host);
    // 端口不参与比较（浏览器对默认端口省略）：比裸主机名即可
    char oh2[40], hh2[40];
    snprintf(oh2, sizeof(oh2), "%.*s", (int)strcspn(oh, ":"), oh);
    snprintf(hh2, sizeof(hh2), "%.*s", (int)strcspn(hh, ":"), hh);
    return hh2[0] != 0 && strcasecmp(oh2, hh2) == 0;
}

// ---------------- 流式上传（固件升级） ----------------
// 上传总时限/单读空闲时限：独立于普通请求的 REQ_TOTAL_MS（固件 ~1MB 级，还夹着擦分区）
#define STREAM_TOTAL_MS 120000
#define STREAM_IDLE_MS  10000

typedef struct {
    sess_t *s;
    const uint8_t *pre;                 // 头部那次 read 带进来的 body 字节
    size_t pre_n, pre_i;
    size_t left;                        // 距 Content-Length 还差多少
    int64_t deadline;
} stream_rd_t;

static int stream_read(void *buf, size_t cap, void *arg)
{
    stream_rd_t *r = arg;
    size_t want = r->left < cap ? r->left : cap;
    if (want == 0) {
        return 0;
    }
    if (r->pre_i < r->pre_n) {
        size_t n = r->pre_n - r->pre_i < want ? r->pre_n - r->pre_i : want;
        memcpy(buf, r->pre + r->pre_i, n);
        r->pre_i += n;
        r->left -= n;
        return (int)n;
    }
    int ms = (int)((r->deadline - esp_timer_get_time()) / 1000);
    if (ms <= 0) {
        return 0;                       // 总时限到了：对端就是不走完，别陪它
    }
    if (ms > STREAM_IDLE_MS) {
        ms = STREAM_IDLE_MS;
    }
    int n = tls_read_some(r->s, buf, want, ms);
    if (n > 0) {
        r->left -= n;
    }
    return n;
}

static int run_stream(sess_t *s, hreq_t *h, net_req_t *req, uint8_t *pre, size_t pre_n)
{
    net_resp_t resp = { 0 };
    req->method = h->method;
    req->path = h->path;
    req->query = h->query;
    req->auth_hdr = h->auth_hdr;
    req->body = "";
    req->body_len = 0;
    req->aux = s->aux;
    req->aux_len = sizeof(s->aux);
    if (!origin_ok(h)) {
        ESP_LOGW(TAG, "cross-origin %s %s from origin '%s'", h->method, h->path, h->origin);
        resp.status = 403;
        resp.data = (const uint8_t *)"cross origin";
        resp.len = 12;
    } else if (h->content_len <= 0) {
        resp.status = 400;
        resp.data = (const uint8_t *)"content-length required";
        resp.len = 22;
    } else {
        stream_rd_t rd = {
            .s = s,
            .pre = pre,
            .pre_n = pre_n,
            .left = (size_t)h->content_len,
            .deadline = esp_timer_get_time() + (int64_t)STREAM_TOTAL_MS * 1000,
        };
        if (s_stream.cb(req, (size_t)h->content_len, stream_read, &rd, &resp, s_stream.ctx) != ESP_OK) {
            resp.status = resp.status ? resp.status : 500;
            resp.data = (const uint8_t *)"error";
            resp.len = 5;
        }
    }
    send_response(s, &resp, false);     // handler 可能没读干净 body，一律用完即关
    ESP_LOGI(TAG, "%s %s done: HTTP %d", h->method, h->path, resp.status ? resp.status : 200);
    return 2;
}

static bool ws_accept_key(const char *key, char *out, size_t cap)
{
    char buf[96];                           // key ≤31 + GUID 36，别卡在 64 的边界上
    int n = snprintf(buf, sizeof(buf), "%s%s", key,
                     "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return false;                       // key 被截断过：握手不可能成功，别再往下算
    }
    uint8_t digest[32];
    size_t dlen = 0;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_1) != PSA_SUCCESS ||
        psa_hash_update(&op, (const uint8_t *)buf, (size_t)n) != PSA_SUCCESS ||
        psa_hash_finish(&op, digest, sizeof(digest), &dlen) != PSA_SUCCESS) {
        psa_hash_abort(&op);
        ESP_LOGE(TAG, "ws accept: sha1 failed (psa_crypto_init?)");
        return false;
    }
    size_t olen = 0;
    return mbedtls_base64_encode((unsigned char *)out, cap, &olen, digest, dlen) == 0;
}

static esp_err_t send_upgrade(sess_t *s, const char *key)
{
    char accept[64];
    if (!ws_accept_key(key, accept, sizeof(accept))) {
        return ESP_FAIL;
    }
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
    return tls_write_all(s, hdr, (size_t)n);
}

// ---------------- 会话 ----------------
static void sess_release(sess_t *s)
{
    if (s->tls) {
        esp_tls_server_session_delete(s->tls);   // 只释放 TLS，不关 fd
        s->tls = NULL;
    }
    if (s->fd >= 0) {
        close(s->fd);                            // 必须自己关：esp_https_server 同款约定
        s->fd = -1;
    }
    xQueueSend(s_sess_pool, &s, 0);
}

static void session_task(void *arg)
{
    sess_t *s = arg;
    s->task = xTaskGetCurrentTaskHandle();

    set_timeo(s->fd, SO_RCVTIMEO, HANDSHAKE_MS);
    set_timeo(s->fd, SO_SNDTIMEO, HANDSHAKE_MS);
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        goto done;
    }
    esp_tls_cfg_server_t cfg = {
        .servercert_buf = servercert_pem_start,
        .servercert_bytes = servercert_pem_end - servercert_pem_start,
        .serverkey_buf = prvtkey_pem_start,
        .serverkey_bytes = prvtkey_pem_end - prvtkey_pem_start,
        .tls_handshake_timeout_ms = HANDSHAKE_MS,
    };
    // 握手串行化：P4 的硬件 MPI（bignum）并发时会死锁在 RSA_QUERY_CLEAN_REG，
    // 所以全局一把互斥锁 + CONFIG_MBEDTLS_HARDWARE_MPI=n（见 sdkconfig.defaults）
    xSemaphoreTake(s_hs_mu, portMAX_DELAY);
    int ret = esp_tls_server_session_create(&cfg, s->fd, tls);
    xSemaphoreGive(s_hs_mu);
    if (ret != 0) {
        ESP_LOGW(TAG, "TLS handshake failed fd=%d: %d", s->fd, ret);
        esp_tls_server_session_delete(tls);
        goto done;
    }
    s->tls = tls;

    for (int reqs = 0; reqs < REQ_PER_CONN; reqs++) {
        hreq_t h = { 0 };
        net_req_t req = { 0 };
        int r = http_read_request(s, &h, &req, reqs == 0);
        if (r == 0) {
            break;
        }
        if (r < 0) {
            net_resp_t err = { .status = 400, .data = (const uint8_t *)"bad request", .len = 11 };
            send_response(s, &err, false);
            break;
        }
        if (r == 2) {
            break;                          // 流式上传已自行应答并关闭连接
        }

        net_resp_t resp = { 0 };
        net_handler_t cb = NULL;
        void *ctx = NULL;
        for (int i = 0; i < s_route_n; i++) {
            if (!strcmp(s_routes[i].path, h.path) && !strcmp(s_routes[i].method, h.method)) {
                cb = s_routes[i].cb;
                ctx = s_routes[i].ctx;
                break;
            }
        }
        if (!cb) {
            resp.status = 404;
            resp.data = (const uint8_t *)"not found";
            resp.len = 9;
        } else if (h.ws_upgrade || !strcmp(h.method, "POST")) {
            // 写操作/WS 升级过跨源检查（GET 是只读的，跨源也读不走内容）
            if (!origin_ok(&h)) {
                ESP_LOGW(TAG, "cross-origin %s %s from origin '%s'", h.method, h.path, h.origin);
                resp.status = 403;
                resp.data = (const uint8_t *)"cross origin";
                resp.len = 12;
            } else if (cb(&req, &resp, ctx) != ESP_OK) {
                resp.status = resp.status ? resp.status : 500;
                resp.data = (const uint8_t *)"error";
                resp.len = 5;
            }
        } else if (cb(&req, &resp, ctx) != ESP_OK) {
            resp.status = resp.status ? resp.status : 500;
            resp.data = (const uint8_t *)"error";
            resp.len = 5;
        }

        if (resp.upgrade_ws) {
            bool ok = h.ws_upgrade && h.ws_key[0];
            // WS 排他：一个浏览器。占用中 → 409，让前端显示明确原因
            if (ok && xSemaphoreTake(s_ws_mu, 0) != pdTRUE) {
                const char *busy = "{\"error\":\"busy\"}";
                net_resp_t e = { .status = 409, .ctype = "application/json",
                                 .data = (const uint8_t *)busy, .len = strlen(busy) };
                send_response(s, &e, false);
                break;
            }
            if (!ok || send_upgrade(s, h.ws_key) != ESP_OK) {
                if (ok) {
                    xSemaphoreGive(s_ws_mu);
                }
                break;
            }
            ESP_LOGI(TAG, "ws client fd=%d connected", s->fd);
            ws_loop(s);
            xSemaphoreGive(s_ws_mu);
            ESP_LOGI(TAG, "ws client fd=%d gone", s->fd);
            break;
        }

        bool keep = h.keep_alive && reqs + 1 < REQ_PER_CONN;
        if (send_response(s, &resp, keep) != ESP_OK) {
            keep = false;
        }
        if (!keep) {
            break;
        }
    }

done:
    sess_release(s);
    vTaskDelete(NULL);
}

static void accept_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        ESP_LOGE(TAG, "socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(HTTPS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(ls, 4) < 0) {
        ESP_LOGE(TAG, "bind/listen 443: %d", errno);
        close(ls);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "https://<ip>/ listening (%d sessions max)", MAX_SESSIONS);

    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int fd = accept(ls, (struct sockaddr *)&src, &slen);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        sess_t *s = NULL;
        if (xQueueReceive(s_sess_pool, &s, 0) != pdPASS) {
            ESP_LOGW(TAG, "session limit, rejecting");
            close(fd);
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->fd = fd;
        if (xTaskCreate(session_task, "sess", 8192, s, 7, NULL) != pdPASS) {
            close(fd);
            xQueueSend(s_sess_pool, &s, 0);
            ESP_LOGE(TAG, "session task create failed");
        }
    }
}

esp_err_t net_server_add_route(const char *method, const char *path, net_handler_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(s_route_n < MAX_ROUTES, ESP_ERR_NO_MEM, TAG, "routes full");
    s_routes[s_route_n++] = (route_t){ .method = method, .path = path, .cb = cb, .ctx = ctx };
    return ESP_OK;
}

esp_err_t net_server_set_stream_route(const char *method, const char *path,
                                      net_stream_handler_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(!s_stream.cb, ESP_ERR_INVALID_STATE, TAG, "stream route set");
    s_stream.method = method;
    s_stream.path = path;
    s_stream.cb = cb;
    s_stream.ctx = ctx;
    return ESP_OK;
}

void net_server_set_ws(const net_ws_ops_t *ops, void *ctx)
{
    s_ws_ops = ops;
    s_ws_ctx = ctx;
}

// WS 槽被占着 = 互斥量在会话任务手里。探测一下就好，不真正占用。
bool net_server_ws_busy(void)
{
    if (!s_ws_mu) {
        return false;
    }
    if (xSemaphoreTake(s_ws_mu, 0) == pdTRUE) {
        xSemaphoreGive(s_ws_mu);
        return false;
    }
    return true;
}

esp_err_t net_server_start(void)
{
    // PSA 子系统要先初始化：WS 握手算 Sec-WebSocket-Accept 用 psa_hash(SHA-1)
    ESP_RETURN_ON_FALSE(psa_crypto_init() == PSA_SUCCESS, ESP_FAIL, TAG, "psa_crypto_init");

    s_sess_pool = xQueueCreate(MAX_SESSIONS, sizeof(sess_t *));
    s_hs_mu = xSemaphoreCreateMutex();
    s_ws_mu = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_sess_pool && s_hs_mu && s_ws_mu, ESP_ERR_NO_MEM, TAG, "sync");
    for (int i = 0; i < MAX_SESSIONS; i++) {
        sess_t *p = &s_sess[i];
        xQueueSend(s_sess_pool, &p, 0);
    }
    ESP_RETURN_ON_FALSE(xTaskCreate(accept_task, "accept", 3072, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "accept task");
    return ESP_OK;
}
