// UVC 采集 → JPEG 硬解 → H264 硬编（分辨率可运行时切换，输出回调给传输层）
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t video_pipeline_start(void);

// 每编码出一帧就回调（在编码任务上下文：实现必须不阻塞、不碰 socket）。
// data 指向编码输出缓冲里的 H264 载荷（前 8B 是预留头部空间，publish 者写帧头用）。
typedef void (*video_frame_cb_t)(const uint8_t *data, size_t len, uint32_t pts_ms,
                                 bool key, void *ctx);
void video_pipeline_set_frame_cb(video_frame_cb_t cb, void *ctx);

// 截图：最近一帧 stripped JPEG。begin 持锁返回指针，调用者拷走后 end 解锁。
bool video_pipeline_snapshot_begin(const uint8_t **buf, size_t *len);
void video_pipeline_snapshot_end(void);

// 请求编码器下一个帧编 IDR（新接入 / 解码器报错时用）
void video_pipeline_force_idr(void);

// 分辨率：索引 0=480P 1=720P 2=1080P（表在 video_pipeline.c）
int video_pipeline_res(void);
int video_pipeline_res_count(void);
const char *video_pipeline_res_name(int idx);
void video_pipeline_res_dims(int idx, uint16_t *w, uint16_t *h);
uint16_t video_pipeline_width(void);
uint16_t video_pipeline_height(void);
// 切换分辨率：异步执行（重建编码器/缓冲、重开 UVC 流）
esp_err_t video_pipeline_set_resolution(int idx);

// 分辨率是否正在重建；编码成功帧数；IDR 帧数；让管线整套重建（编码任务卡死时）
bool video_pipeline_res_busy(void);
uint32_t video_pipeline_encoded(void);
uint32_t video_pipeline_idr(void);
void video_pipeline_request_rebuild(void);

#ifdef __cplusplus
}
#endif
