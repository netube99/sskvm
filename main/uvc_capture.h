// UVC 采集：USB HS host + 采集卡流，把原始帧交给上层（含 newest-wins 丢旧帧）。
//
// 帧数据已经是干净的 MJPEG：驱动层（uvc_bulk.c sskvm patch）在每个 bulk payload
// 装配进帧缓冲时就地剥掉采集卡的 12B payload header，无需再剔。
// 流的生命周期由消费者驱动：uvc_capture_open() → receive/release 循环 → close()，
// 这样帧缓冲的归属清晰（消费者持帧期间不会有人关流）。
// 所有权：receive() 把帧交给调用者（驱动不再碰它），调用者必须且只能
// release(frame) 一次；receive 内部丢掉的旧帧由它自己归还。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const uint8_t *data;    // UVC 原始数据（干净 MJPEG）
    size_t         len;
    int64_t        capture_us;
    const void    *frame;   // 驱动帧句柄，还帧时传回 uvc_capture_release()
} uvc_frame_t;

esp_err_t uvc_capture_init(void);                       // USB host + UVC 驱动 + 事件任务
esp_err_t uvc_capture_open(void);                       // 打开并启动流
void      uvc_capture_set_format(uint16_t w, uint16_t h);  // 下次 open 生效（帧率用卡的默认/最高）
// 运行时换分辨率：不关设备，只停流→重新协商格式→重启（避免 close/open 的一堆驱动竞态）。
// 调用前必须已归还所有 receive() 拿到的帧（驱动断言全帧已归还）
esp_err_t uvc_capture_reconfigure(uint16_t w, uint16_t h);
void      uvc_capture_close(void);
// 请消费者（strip 任务）在安全点关流重开：拔插/抽风后状态不干净时的软件复位
void      uvc_capture_request_restart(void);
bool      uvc_capture_take_restart_request(void);       // 取出并清除请求（strip 任务专用）
bool      uvc_capture_connected(void);
bool      uvc_capture_receive(uvc_frame_t *out, uint32_t timeout_ms);  // 内部丢弃更旧的帧
void      uvc_capture_release(const uvc_frame_t *f);    // 归还帧给驱动（每帧恰一次）

// 诊断：收到的帧计数（不动 = 流卡了）
uint32_t  uvc_capture_frames(void);

// 诊断：把 USB 主机上的设备列出来（VID/PID/class），相机找不到时用
void      uvc_capture_dump_devices(void);
