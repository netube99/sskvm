// 自持 HTTPS/WSS 传输层：每连接一个任务，独占自己的 TLS 上下文。
//
// 为什么不用 esp_http_server：
//   - 编码任务永远不碰 socket（帧走"发布 → 每会话槽 → 会话任务发"）
//   - 一个 TLS 上下文只被一个任务读写：没有全局发送锁、没有 close_fn 竞态、没有 fd 表
//   - agent 的慢 HTTP 请求只堵它自己的任务，不影响浏览器的键鼠
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// WS 二进制帧发送时，调用者要在 payload 前预留这么多字节给 WS 头（一次写 = 一个 TLS 记录）
#define NET_WS_HDR_MAX 10

typedef struct {
    const char *method;     // "GET" / "POST"
    const char *path;       // "/config"
    const char *query;      // "token=abc&x=1"（没有则 ""）
    const char *auth_hdr;   // X-Auth-Token（没有则 ""）
    const char *body;       // NUL 结尾；没有 body 则 ""
    size_t body_len;
    char *aux;              // 每个连接一块 512B 小缓冲（handler 拼 JSON 用，免 malloc）
    size_t aux_len;
} net_req_t;

typedef struct {
    int status;             // 0 = 200
    const char *ctype;      // NULL = text/plain
    bool no_store;          // 加 Cache-Control: no-store
    const uint8_t *data;
    size_t len;
    bool upgrade_ws;        // 鉴权通过：升级成 WebSocket（占用中则回 409）
} net_resp_t;

typedef esp_err_t (*net_handler_t)(const net_req_t *req, net_resp_t *resp, void *ctx);

// 以下回调都在该 WS 会话任务里调用（同一线程），可以直接发帧
typedef struct {
    void (*open)(void *ctx);                        // 升级完成：发全量状态
    void (*input)(const uint8_t *data, size_t len, void *ctx);
    void (*tick)(void *ctx);                        // 每轮循环：发视频/状态
    void (*close)(void *ctx);                       // 会话结束：松 HID
} net_ws_ops_t;

// 从 query 里取 key 的值（做 %XX 解码）；找到返回 true
bool net_query_get(const char *query, const char *key, char *out, size_t cap);

esp_err_t net_server_start(void);
esp_err_t net_server_add_route(const char *method, const char *path, net_handler_t cb, void *ctx);
void net_server_set_ws(const net_ws_ops_t *ops, void *ctx);

// 只在 WS 回调里调用；会校验调用者就是会话任务，否则返回 ESP_ERR_INVALID_STATE
esp_err_t net_ws_send_text(const char *s);
// len = 帧头 8B + H264。帧头已写在 payload[-8..]；WS 头写进 payload[-8-hn..]，
// 一次写出（要求 payload 前有 ≥18B 预留）
esp_err_t net_ws_send_frame(uint8_t *payload, size_t len);
bool net_ws_writable(void);      // select(write, 0)：false 就别发，计 stall
void net_ws_close(void);         // 标记会话结束（任务会自己收尾）
bool net_server_ws_busy(void);   // 浏览器占用中（/config 用它告诉前端为什么被拒）
