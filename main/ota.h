// 网页固件升级（POST /ota，body = idf.py build 的 sskvm.bin 原样上传）。
//
// 全程用 IDF 官方 API，不自造：
//   esp_ota_begin/write/end（esp_partition + 镜像校验）→ esp_ota_set_boot_partition → 重启
// 异常自动回退靠 bootloader 的 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE（sdkconfig）：
// 新固件以 ESP_OTA_IMG_PENDING_VERIFY 状态启动，app_main 在全部模块起来后
// 3s 调 esp_ota_mark_app_valid_cancel_rollback()；期间崩溃/断电/挂死重启，
// bootloader 自动回退到另一个槽的旧固件。
#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "net_server.h"

// 流式接收整包并调度重启。total = Content-Length；read/rd_ctx 由 net_server 提供
//（返回 >0 字节且 ≤ cap；0 = 对端超时；-1 = 断开；total 读满后不得再调）。
// HTTP 结果写进 resp。
esp_err_t ota_upload(size_t total, net_stream_read_t read, void *rd_ctx, net_resp_t *resp);

// 开机自确认：PENDING_VERIFY 状态下撑过启动（网络/USB/视频都起来了）即取消回退
void ota_confirm_if_pending(void);
