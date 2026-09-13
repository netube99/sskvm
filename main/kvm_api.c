// KVM 对外接口实现。传输层在 net_server.c（每连接一个任务独占 TLS）。
//
// 路由：
//   GET  /              页面（不鉴权：token 错了前端才有机会提示输入 token）
//   GET  /config        设备状态 JSON
//   POST /config        res=N / hostname=xxx / wol=AA:BB:CC:DD:EE:FF
//   POST /wol           网络唤醒广播（body 可选 = 目标 MAC，缺省用记住的）
//   GET  /snapshot.jpg  采集卡原始 JPEG（agent）
//   POST /input         二进制键鼠事件（agent）
//   POST /type          ASCII 文本（agent，HID 键盘拆键；非 ASCII 设备端跳过）
//   GET  /ws            WebSocket（排他，占用中回 409）
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "net_server.h"
#include "kvm_api.h"
#include "kvm_config.h"
#include "kvm_input.h"
#include "kvm_mdns.h"
#include "eth_net.h"
#include "usb_device.h"
#include "uvc_capture.h"
#include "video_pipeline.h"

static const char *TAG = "api";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// ---------------- 输出缓冲：编码任务 ping-pong 写，WS 会话任务取 ----------------
// 编码器直接把 H264 写进这两块缓冲（载荷前预留 64B：帧头 8B + WS 头 ≤10B），
// publish 只记指针，零拷贝。会话任务把 latest 标 busy 发送、发完解除；
// 编码任务只写非 busy 的缓冲 → 编码与发送并行，但永不超过一帧在途。
#define STALL_MAX 250                   // 连续 ~500ms 不可写就踢（对端死了）
#define OUT_N 2

static uint8_t *s_out_payload[OUT_N];   // H264 载荷起点（缓冲基址 + 64B 预留）
static size_t s_out_cap;                // 单帧上限 = 编码输出缓冲容量（out_register 传入）
static SemaphoreHandle_t s_out_mu;
static bool s_out_busy[OUT_N];          // 会话正在发送
static int s_latest = -1;               // 最新 publish 的缓冲
static size_t s_latest_len;             // 帧头 8B + H264
static uint32_t s_latest_seq;
static bool s_latest_key;

static uint32_t s_sent_seq;
static bool s_need_key = true;
static uint32_t s_sent_key = 0xFFFFFFFF;
static int s_stalls;
static uint32_t s_skip;                 // publish 撞上 busy 的帧（编码中那一下刚好被取走）
static int64_t s_pub_us;                // 发布时刻，量“发布→开始发”的会话循环延迟
static int64_t s_send_us, s_send_max, s_wait_us, s_wait_max;
static uint32_t s_send_n, s_send_bytes;

void kvm_api_out_register(uint8_t *p0, uint8_t *p1, size_t cap)
{
    s_out_payload[0] = p0;
    s_out_payload[1] = p1;
    s_out_cap = cap;
    s_out_mu = xSemaphoreCreateMutex();
}

uint8_t *kvm_api_out_try_acquire(void)
{
    if (!s_out_mu) {
        return NULL;
    }
    uint8_t *p = NULL;
    xSemaphoreTake(s_out_mu, portMAX_DELAY);
    // 严格交替：永远拿非 latest 的那块。若拿 latest，编码期间会话可能把它
    // 取走发送，边发边被覆写（smoke_test 抓到过：起始码被写花）。
    // 两块都不可用（latest 正发、另一块也忙）返回 NULL，编码任务隔 1ms 重试。
    for (int i = 0; i < OUT_N; i++) {
        if (!s_out_busy[i] && i != s_latest) {
            p = s_out_payload[i];
            break;
        }
    }
    xSemaphoreGive(s_out_mu);
    return p;
}

// 视频帧载荷前 8B：[u32 pts_ms][u16 w][u16 h]，WS 头由 net_server 补在最前面
void kvm_api_publish_frame(const uint8_t *data, size_t len, uint32_t pts_ms, bool key, void *ctx)
{
    if (!s_out_mu || len == 0) {
        return;
    }
    if (len > s_out_cap) {
        // 超过缓冲容量的帧发不出去。已编出的帧必须全部发出（H264 参考链），
        // 静默丢 P 帧会让后续帧全烂到下一个 GOP —— 立刻催 IDR 掐断污染
        static int64_t s_last_warn;
        int64_t now = esp_timer_get_time();
        if (now - s_last_warn > 1000000) {
            s_last_warn = now;
            video_pipeline_force_idr();
            ESP_LOGW(TAG, "frame %uKB > out cap %uKB, drop + force IDR",
                     (unsigned)(len / 1024), (unsigned)(s_out_cap / 1024));
        }
        return;
    }
    int i = -1;
    for (int k = 0; k < OUT_N; k++) {
        if (s_out_payload[k] == data) {
            i = k;
            break;
        }
    }
    if (i < 0) {
        return;
    }
    uint16_t w = video_pipeline_width(), h = video_pipeline_height();
    xSemaphoreTake(s_out_mu, portMAX_DELAY);
    if (!s_out_busy[i]) {
        uint8_t *p = (uint8_t *)data - 8;
        p[0] = (uint8_t)pts_ms;
        p[1] = (uint8_t)(pts_ms >> 8);
        p[2] = (uint8_t)(pts_ms >> 16);
        p[3] = (uint8_t)(pts_ms >> 24);
        p[4] = (uint8_t)w;
        p[5] = (uint8_t)(w >> 8);
        p[6] = (uint8_t)h;
        p[7] = (uint8_t)(h >> 8);
        s_latest = i;
        s_latest_len = len + 8;
        s_latest_key = key;
        s_latest_seq++;
        s_pub_us = esp_timer_get_time();
    } else {
        // 编码中那一下刚好被会话取走（窗口极小）：丢的若是关键帧会干等到下个 GOP，催一个 IDR
        video_pipeline_force_idr();
        if (++s_skip % 50 == 1) {
            ESP_LOGW(TAG, "publish skipped: out busy (%u)", (unsigned)s_skip);
        }
    }
    xSemaphoreGive(s_out_mu);
}

// ---------------- 状态：合成键变了才发（不需要跨模块通知，漏不掉） ----------------
static uint32_t state_key(void)
{
    return (uint32_t)(video_pipeline_res() & 3) |
           (video_pipeline_res_busy() ? 4u : 0u) |
           (usb_device_mounted() ? 8u : 0u) |
           (uvc_capture_connected() ? 16u : 0u);
}

static void state_json(char *out, size_t cap)
{
    snprintf(out, cap,
             "{\"t\":\"state\",\"res\":%d,\"resBusy\":%d,\"usb\":%d,\"signal\":%d}",
             video_pipeline_res(), video_pipeline_res_busy() ? 1 : 0,
             usb_device_mounted() ? 1 : 0, uvc_capture_connected() ? 1 : 0);
}

// ---------------- WS 回调（都在 WS 会话任务里） ----------------
static void ws_open(void *ctx)
{
    s_sent_key = 0xFFFFFFFF;            // 连接即推全量状态
    s_sent_seq = s_latest_seq;          // 会话前攒的帧不用补发，直接编新的
    s_need_key = true;                  // 从关键帧开始
    s_stalls = 0;
    kvm_input_release_all();            // 上次断线的按键状态可能还挂着
    video_pipeline_force_idr();
    ESP_LOGI(TAG, "browser attached");
}

// 改分辨率要落 NVS：重启后别再回到默认（resloop 压测直连管线，不写 NVS）
static esp_err_t set_res_persist(int idx)
{
    esp_err_t e = video_pipeline_set_resolution(idx);
    if (e == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(kvm_config_set_res((uint8_t)idx));
    }
    return e;
}

static void ws_input(const uint8_t *data, size_t len, void *ctx)
{
    if (len < 1) {
        return;
    }
    switch (data[0]) {
    case 0x04:                          // 请求 IDR（解码器重建/落后）
        video_pipeline_force_idr();
        break;
    case 0x05: {                        // 应用层心跳：带设备时钟，前端算真实单向延时
        // 客户端停止读 TCP 但还在发 ping 时，pong 会阻塞到 100ms 超时，
        // 把 tick 的踢人节奏（~0.5s）拖成几十秒 —— 不可写就先不发，下轮再算
        if (net_ws_writable()) {
            char pong[48];
            snprintf(pong, sizeof(pong), "{\"t\":\"pong\",\"d\":%u}",
                     (unsigned)(esp_timer_get_time() / 1000));
            net_ws_send_text(pong);
        }
        break;
    }
    case 0x06:                          // 切分辨率（binary 一条搞定，设备端不解析 JSON）
        if (len >= 2) {
            set_res_persist(data[1]);
        }
        break;
    default:                            // 0x01 键盘 / 0x02 鼠标
        kvm_input_handle(data, (int)len);
        break;
    }
}

static void ws_tick(void *ctx)
{
    uint32_t k = state_key();
    if (k != s_sent_key && net_ws_writable()) {
        // 状态变更也不在不可写时硬发：发送卡 100ms 会拖慢踢人节奏；
        // s_sent_key 没更新，可写后下一轮自然补发
        char json[160];
        state_json(json, sizeof(json));
        if (net_ws_send_text(json) == ESP_OK) {
            s_sent_key = k;
        }
    }

    // 对端读不动了：不发，攒够就踢。绝不在不可写时调用 send。
    if (!net_ws_writable()) {
        if (++s_stalls > STALL_MAX) {
            ESP_LOGW(TAG, "client not reading, dropping");
            net_ws_close();
        }
        return;
    }
    s_stalls = 0;

    uint8_t *payload = NULL;
    size_t total = 0;
    bool key = false;
    uint32_t seq = 0;
    int64_t pub_us = 0;
    int idx = -1;
    if (!s_out_mu) {
        return;
    }
    xSemaphoreTake(s_out_mu, portMAX_DELAY);
    if (s_latest >= 0 && !s_out_busy[s_latest] && s_latest_seq != s_sent_seq &&
        (!s_need_key || s_latest_key)) {
        idx = s_latest;
        s_out_busy[idx] = true;         // 编码任务从此改写另一块；发完解除
        payload = s_out_payload[idx];
        total = s_latest_len;
        key = s_latest_key;
        seq = s_latest_seq;
        pub_us = s_pub_us;
    }
    xSemaphoreGive(s_out_mu);
    if (!payload) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    int64_t wait = t0 - pub_us;                // 发布→开始发：会话循环的拾取延迟
    esp_err_t e = net_ws_send_frame(payload, total);   // payload-8 起是帧头，WS 头由 send 函数补
    int64_t send = esp_timer_get_time() - t0;
    s_wait_us += wait; s_send_us += send; s_send_n++; s_send_bytes += total;
    if (wait > s_wait_max) s_wait_max = wait;
    if (send > s_send_max) s_send_max = send;
    if (s_send_n >= 150) {                     // 每 150 帧报一次：谁慢一目了然
        ESP_LOGI(TAG, "send: wait=%.1f/%.1fms send=%.1f/%.1fms %.1fKB/帧 %.2fMbps",
                 s_wait_us / 1000.0 / s_send_n, s_wait_max / 1000.0,
                 s_send_us / 1000.0 / s_send_n, s_send_max / 1000.0,
                 s_send_bytes / 1024.0 / s_send_n,
                 s_send_bytes * 8.0 / (s_send_us > 0 ? s_send_us : 1));
        s_wait_us = s_send_us = s_send_bytes = 0;
        s_wait_max = s_send_max = 0;
        s_send_n = 0;
    }
    xSemaphoreTake(s_out_mu, portMAX_DELAY);
    s_out_busy[idx] = false;
    xSemaphoreGive(s_out_mu);
    if (e != ESP_OK) {
        net_ws_close();
        return;
    }
    s_sent_seq = seq;
    if (key) {
        s_need_key = false;
    }
}

static void ws_close(void *ctx)
{
    kvm_input_release_all();
    xSemaphoreTake(s_out_mu, portMAX_DELAY);
    s_out_busy[0] = s_out_busy[1] = false;   // 会话死了，两块缓冲都放回编码任务
    xSemaphoreGive(s_out_mu);
    ESP_LOGI(TAG, "browser detached");
}

static const net_ws_ops_t s_ws_ops = {
    .open = ws_open,
    .input = ws_input,
    .tick = ws_tick,
    .close = ws_close,
};

// ---------------- 鉴权 ----------------
// token ≤64 字符（kvm_config 的存储上限）：定长比较，长度差不提前返回，
// 不给时序侧信道留"长度先不同"的口子
static bool ct_eq(const char *a, const char *b)
{
    unsigned char da[64] = { 0 }, db[64] = { 0 };
    for (size_t i = 0; i < sizeof(da) && a[i]; i++) {
        da[i] = (unsigned char)a[i];
    }
    for (size_t i = 0; i < sizeof(db) && b[i]; i++) {
        db[i] = (unsigned char)b[i];
    }
    unsigned char d = 0;
    for (size_t i = 0; i < sizeof(da); i++) {
        d |= da[i] ^ db[i];
    }
    return d == 0;
}

static bool auth_ok(const net_req_t *req)
{
    const char *want = kvm_config_token();
    if (want[0] == 0) {
        return true;
    }
    char v[80];
    if (net_query_get(req->query, "token", v, sizeof(v)) && ct_eq(v, want)) {
        return true;
    }
    return req->auth_hdr[0] && ct_eq(req->auth_hdr, want);
}

static esp_err_t deny(net_resp_t *resp)
{
    resp->status = 401;
    resp->data = (const uint8_t *)"unauthorized";
    resp->len = 12;
    return ESP_OK;
}

static esp_err_t ok_text(net_resp_t *resp, const char *msg)
{
    resp->ctype = "text/plain";
    resp->data = (const uint8_t *)msg;
    resp->len = strlen(msg);
    return ESP_OK;
}

// body 尾部的换行/空格（curl/http.client 的习惯动作）去掉再解析
static void trim_tail(char *s)
{
    for (char *e = s + strlen(s); e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ');) {
        *--e = 0;
    }
}

// ---------------- 网络唤醒（WOL） ----------------
// magic packet = 6×0xFF + 16×目标 MAC，UDP 广播到 255.255.255.255:9。
// 网卡在 L2 扫广播帧，端口无关紧要（惯例 9 = discard）。发 3 遍：广播偶尔丢包。
// 目标机网卡必须开了 WOL 且刚关机（软关机 + 待机电源）——我们只负责把包发出去
static esp_err_t wol_send(const uint8_t mac[6])
{
    uint8_t pkt[102];
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(pkt + 6 + i * 6, mac, 6);
    }
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(fd >= 0, ESP_FAIL, TAG, "wol socket");
    int bc = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc));
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(9),
        .sin_addr.s_addr = 0xFFFFFFFFu,     // 有限广播：同一二层内必达，不过路由器
    };
    esp_err_t ret = ESP_OK;
    for (int i = 0; i < 3 && ret == ESP_OK; i++) {
        if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            ESP_LOGE(TAG, "wol sendto errno=%d", errno);
            ret = ESP_FAIL;
        } else if (i < 2) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    close(fd);
    return ret;
}

// POST /wol：body 空 = 唤醒记住的 MAC；显式给了 MAC 就发那个（agent 临时唤醒别的机器）
static esp_err_t wol_post(const net_req_t *req, net_resp_t *resp, void *ctx)
{
    if (!auth_ok(req)) {
        return deny(resp);
    }
    uint8_t m[6];
    if (req->body[0]) {
        char mac[32];
        snprintf(mac, sizeof(mac), "%s", req->body);
        trim_tail(mac);
        if (!kvm_config_parse_mac(mac, m)) {
            resp->status = 400;
            return ok_text(resp, "bad mac");
        }
    } else if (!kvm_config_parse_mac(kvm_config_wol_mac(), m)) {
        resp->status = 400;
        return ok_text(resp, "no mac remembered");
    }
    if (wol_send(m) != ESP_OK) {
        resp->status = 500;
        return ok_text(resp, "send failed");
    }
    ESP_LOGI(TAG, "wol -> %02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    return ok_text(resp, "sent");
}

// ---------------- 路由 ----------------
static esp_err_t index_get(const net_req_t *req, net_resp_t *resp, void *ctx)
{
    resp->ctype = "text/html; charset=utf-8";
    resp->no_store = true;
    resp->data = index_html_start;
    resp->len = (size_t)(index_html_end - index_html_start - 1);   // TEXT 模式末尾有 NUL
    return ESP_OK;
}

static esp_err_t config_get(const net_req_t *req, net_resp_t *resp, void *ctx)
{
    if (!auth_ok(req)) {
        return deny(resp);
    }
    char *json = req->aux;              // 连接自带的静态小缓冲，免 malloc
    if (!json || req->aux_len < 384) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(json, req->aux_len,
             "{\"hostname\":\"%s\",\"ip\":\"%s\",\"mac\":\"%s\",\"wol\":\"%s\",\"version\":\"%s\","
             "\"res\":%d,\"resBusy\":%d,\"usb\":%d,\"signal\":%d,\"wsBusy\":%d}",
             kvm_config_hostname(), eth_net_ip(), eth_net_mac(), kvm_config_wol_mac(),
             esp_app_get_description()->version, video_pipeline_res(),
             video_pipeline_res_busy() ? 1 : 0, usb_device_mounted() ? 1 : 0,
             uvc_capture_connected() ? 1 : 0, net_server_ws_busy() ? 1 : 0);
    resp->ctype = "application/json";
    resp->no_store = true;
    resp->data = (const uint8_t *)json;
    resp->len = strlen(json);
    return ESP_OK;
}

static esp_err_t config_post(const net_req_t *req, net_resp_t *resp, void *ctx)
{
    if (!auth_ok(req)) {
        return deny(resp);
    }
    if (strncmp(req->body, "res=", 4) == 0) {
        char *end = NULL;
        long v = strtol(req->body + 4, &end, 10);
        // 非数字不再被 atoi 静默解析成 0（切到 480P）：严格解析
        if (end == req->body + 4 || (*end && *end != ' ' && *end != '\r' && *end != '\n')) {
            resp->status = 400;
            return ok_text(resp, "bad res");
        }
        if (set_res_persist((int)v) != ESP_OK) {
            resp->status = 400;
            return ok_text(resp, "bad res");
        }
        return ok_text(resp, "ok");
    }
    if (strncmp(req->body, "hostname=", 9) == 0) {
        char *v = (char *)req->body + 9;
        trim_tail(v);
        if (kvm_config_set_hostname(v) != ESP_OK) {
            resp->status = 400;
            return ok_text(resp, "hostname: 1..32 chars [a-z0-9-]");
        }
        kvm_mdns_set_hostname(v);
        return ok_text(resp, "ok");
    }
    if (strncmp(req->body, "wol=", 4) == 0) {
        // 设置页「记忆」按钮：记住目标机 MAC（空串 = 清除）；重启后仍在
        char *v = (char *)req->body + 4;
        trim_tail(v);
        if (kvm_config_set_wol_mac(v) != ESP_OK) {
            resp->status = 400;
            return ok_text(resp, "bad mac");
        }
        return ok_text(resp, "ok");
    }
    if (strncmp(req->body, "uvc=restart", 11) == 0) {
        // 设置页按钮：关流重开、按当前分辨率重新协商（拔插/抽风后画面异常的软件复位）
        uvc_capture_request_restart();
        return ok_text(resp, "restarting");
    }
    resp->status = 400;
    return ok_text(resp, "res= / hostname= / wol= / uvc=restart");
}

#define SNAP_COPY_MAX (2 * 1024 * 1024) // 影子缓冲上限（video_pipeline 的 SHADOW_BUF_SIZE）
static uint8_t *s_snap_copy[2];         // 常驻拷贝缓冲轮换（影子最大 2MB）：不再每次请求
                                        // malloc/free 1.5MB 级 PSRAM（碎片化风险）
static int s_snap_i;
static esp_err_t snapshot_get(const net_req_t *req, net_resp_t *resp, void *ctx)
{
    if (!auth_ok(req)) {
        return deny(resp);
    }
    const uint8_t *buf = NULL;
    size_t len = 0;
    int i = s_snap_i++ % 2;             // 双缓冲轮换：两个并发 agent 各占一块
    if (!s_snap_copy[i] || !video_pipeline_snapshot_begin(&buf, &len) || len > SNAP_COPY_MAX) {
        resp->status = 404;
        return ok_text(resp, "no frame yet");
    }
    // 锁内拷走、锁外发送：慢客户端不能堵住解码任务
    memcpy(s_snap_copy[i], buf, len);
    video_pipeline_snapshot_end();
    resp->ctype = "image/jpeg";
    resp->no_store = true;
    resp->data = s_snap_copy[i];
    resp->len = len;
    return ESP_OK;
}

static esp_err_t input_post(const net_req_t *req, net_resp_t *resp, void *ctx)
{
    if (!auth_ok(req)) {
        return deny(resp);
    }
    if (req->body_len > 0) {
        kvm_input_handle((const uint8_t *)req->body, (int)req->body_len);
    }
    return ok_text(resp, "ok");
}

static esp_err_t type_post(const net_req_t *req, net_resp_t *resp, void *ctx)
{
    if (!auth_ok(req)) {
        return deny(resp);
    }
    int done = kvm_input_text(req->body, (int)req->body_len);
    char msg[48];
    snprintf(msg, sizeof(msg), "typed %d", done);
    return ok_text(resp, msg);
}

static esp_err_t ws_get(const net_req_t *req, net_resp_t *resp, void *ctx)
{
    if (!auth_ok(req)) {
        return deny(resp);
    }
    resp->upgrade_ws = true;            // net_server 负责排他检查与 101
    return ESP_OK;
}

// ---------------- 启动 ----------------
esp_err_t kvm_api_start(void)
{
    net_server_set_ws(&s_ws_ops, NULL);
    // 截图常驻拷贝缓冲（PSRAM 一次分配，与 YUV/输出缓冲同哲学：不做高频大块分配）
    for (int i = 0; i < 2; i++) {
        s_snap_copy[i] = heap_caps_malloc(SNAP_COPY_MAX, MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_snap_copy[i], ESP_ERR_NO_MEM, TAG, "snap copy");
    }
    ESP_RETURN_ON_ERROR(net_server_add_route("GET", "/", index_get, NULL), TAG, "route /");
    ESP_RETURN_ON_ERROR(net_server_add_route("GET", "/config", config_get, NULL), TAG, "route config");
    ESP_RETURN_ON_ERROR(net_server_add_route("POST", "/config", config_post, NULL), TAG, "route config");
    ESP_RETURN_ON_ERROR(net_server_add_route("GET", "/snapshot.jpg", snapshot_get, NULL), TAG, "route snap");
    ESP_RETURN_ON_ERROR(net_server_add_route("POST", "/input", input_post, NULL), TAG, "route input");
    ESP_RETURN_ON_ERROR(net_server_add_route("POST", "/type", type_post, NULL), TAG, "route type");
    ESP_RETURN_ON_ERROR(net_server_add_route("POST", "/wol", wol_post, NULL), TAG, "route wol");
    ESP_RETURN_ON_ERROR(net_server_add_route("GET", "/ws", ws_get, NULL), TAG, "route ws");
    return net_server_start();
}
