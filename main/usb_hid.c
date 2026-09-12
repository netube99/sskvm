// HID 报表描述符与回调。
//
// instance 0 = boot 键盘（8B 输入 + 1B LED 输出）
// instance 1 = 绝对鼠标（6B：buttons, X u16, Y u16, wheel i8）——PiKVM 同款，
//              光标跳到点击处，浏览器把指针位置按 0..32767 映射整屏
#include <stdint.h>
#include "esp_log.h"
#include "tinyusb.h"
#include "usb_hid.h"
#include "kvm_input.h"

static const char *TAG = "usb_hid";

_Static_assert(CFG_TUD_HID == 2, "kbd+mouse 双实例是描述符与回调的硬约定（改 Kconfig 会静默断鼠标）");

// 标准 boot 键盘报表
static const uint8_t desc_report_kbd[] = { TUD_HID_REPORT_DESC_KEYBOARD() };

// 绝对鼠标：按键(3bit) + 5bit 填充 + 绝对 X/Y(0..32767) + 相对滚轮
static const uint8_t desc_report_mouse[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x03,        //     Usage Maximum (3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x03,        //     Input (Const,Var,Abs)  -> 1 字节按键
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x16, 0x00, 0x00,  //     Logical Minimum (0)
    0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x02,        //     Input (Data,Var,Abs)   -> 4 字节绝对 X/Y
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data,Var,Rel)   -> 1 字节滚轮
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

_Static_assert(sizeof(desc_report_kbd) == USB_HID_REPORT_KBD_LEN, "kbd descriptor length");
_Static_assert(sizeof(desc_report_mouse) == USB_HID_REPORT_MOUSE_LEN, "mouse descriptor length");

void usb_hid_init(void) {}

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == 0 ? desc_report_kbd : desc_report_mouse;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    // HID 规范对 boot 键盘 GET_REPORT 是强制项；部分主机初始化时会探测，
    // 恒 STALL 可能被拒。返回最后一份报表快照。
    if (report_type != HID_REPORT_TYPE_INPUT) {
        return 0;
    }
    int n = kvm_input_get_report(instance, buffer, reqlen);
    return n > 0 ? (uint16_t)n : 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    static uint8_t s_led_last = 0xFF;
    if (instance == 0 && report_type == HID_REPORT_TYPE_OUTPUT && bufsize > 0) {
        if (buffer[0] != s_led_last) {      // 只在变化时打：CapsLock 连切不会刷爆串口
            s_led_last = buffer[0];
            ESP_LOGI(TAG, "kbd LED = 0x%02X", buffer[0]);
        }
    }
}
