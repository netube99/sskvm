// 复合 USB 设备：HID 键盘 + HID 绝对鼠标 + MSC，跑在 OTG_FS 的 FS_PHY1（GPIO24/25）
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t usb_device_init(void);

// 主机是否已完成枚举（/config 的 usb 字段，诊断用）
bool usb_device_mounted(void);
