// HID 报表描述符 + TinyUSB HID 回调（esp_tinyusb 的这三个回调不是 weak，app 必须实现）
#pragma once
#include <stdint.h>

// 报表描述符长度（usb_device.c 组装配置描述符用；usb_hid.c 有 _Static_assert 兜底）
#define USB_HID_REPORT_KBD_LEN      65
#define USB_HID_REPORT_MOUSE_LEN    64

// 空函数：让链接器把本文件（tud_hid_* 回调）拉进 main 归档
void usb_hid_init(void);
