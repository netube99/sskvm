// KVM 对外接口：一个端口上的页面 + 状态 + 视频/输入 WS + agent HTTP。
//
// 浏览器用一条 WS 拿视频、发键鼠、切分辨率；agent 走 HTTP（可 keep-alive 连续拉图）。
// 视频帧的路径：编码任务直接写双输出缓冲 → publish 记指针（零拷贝）→ WS 会话任务
// 把 latest 标 busy 发送。编码任务永远不碰 socket。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t kvm_api_start(void);

// 视频管线启动时注册两块编码输出缓冲的载荷指针（各预留 ≥18B 头部空间：
// 帧头 8B + WS 头 ≤10B 都写在载荷前面，帧数据本身零拷贝）。
void kvm_api_out_register(uint8_t *p0, uint8_t *p1, size_t cap);

// 编码任务调用：取一块没在发送中的输出缓冲（直接把 H264 写进去）。
// 都在发送中返回 NULL（调用方隔 1ms 重试，这就是背压：发送不归一帧内不编下一帧）。
uint8_t *kvm_api_out_try_acquire(void);

// 编码任务调用：发布刚编完的一帧（data 必须是 try_acquire 返回的指针）。
// 不拷贝：在 data[-8..) 写 [pts u32][w u16][h u16] 帧头，记录 latest。
void kvm_api_publish_frame(const uint8_t *data, size_t len, uint32_t pts_ms, bool key, void *ctx);
