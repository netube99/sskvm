// 复合 USB 设备：boot 键盘 + 绝对鼠标的复合配置描述符 + TinyUSB 安装。
//
// 跑在 OTG_FS 的 FS_PHY1（GPIO24/25，板上 "USB1.1 Type-C" 口），
// PHY 软切换（usb_wrap_ll_phy_select）等效 eFuse=1 但可逆、不烧熔丝。
//
// 接口固定不变。描述符固定，绝不运行时换（换 = 重新枚举 = 键盘鼠标一起掉）。
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "tusb.h"
#include "hal/usb_wrap_ll.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "usb_device.h"
#include "usb_hid.h"
#include "kvm_input.h"

static const char *TAG = "usb_dev";

// IF0 kbd / IF1 mouse
enum {
    ITF_NUM_HID_KBD = 0,
    ITF_NUM_HID_MOUSE,
    ITF_NUM_TOTAL,
};

#define EPNUM_HID_KBD    1      // EP1 IN
#define EPNUM_HID_MOUSE  2      // EP2 IN
#define HID_POLL_MS      1      // 1ms 轮询：键鼠延时 ~1ms（HZ 必须 >= 1000 才排得开）

#define CFG_DESC_LEN          (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)

static const uint8_t desc_cfg[] = {
    // 不声明 REMOTE_WAKEUP：没有实现（从未调 tud_remote_wakeup），别对主机做虚假承诺
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CFG_DESC_LEN, 0, 100),
    // 接口号, 字符串索引(0=无), boot 协议, 报表描述符长度, EP IN, EP 大小, 轮询间隔
    TUD_HID_DESCRIPTOR(ITF_NUM_HID_KBD, 0, HID_ITF_PROTOCOL_KEYBOARD, USB_HID_REPORT_KBD_LEN,
                       0x80 | EPNUM_HID_KBD, CFG_TUD_HID_EP_BUFSIZE, HID_POLL_MS),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID_MOUSE, 0, HID_ITF_PROTOCOL_NONE, USB_HID_REPORT_MOUSE_LEN,
                       0x80 | EPNUM_HID_MOUSE, CFG_TUD_HID_EP_BUFSIZE, HID_POLL_MS),
};

static volatile bool s_mounted;     // 主机已完成枚举（/config 的 usb 字段，诊断用）

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    bool mounted;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        mounted = true;
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        mounted = false;
        // 主机侧重启/重枚举：不松的话 hid 任务会把断开期间积压的键鼠状态
        // （按住的键/鼠标键）灌给重枚举后的全新主机会话
        kvm_input_release_all();
    } else {
        return;                             // 未来若开 suspend 回调，别误清 mounted
    }
    s_mounted = mounted;
    ESP_LOGI(TAG, "host %s", mounted ? "enumerated" : "disconnected");
}

bool usb_device_mounted(void) { return s_mounted; }

esp_err_t usb_device_init(void)
{
    // 软切换 FS PHY 交叉矩阵: OTG_FS → FS_PHY1(GPIO24/25), USJ → FS_PHY2(未引出)
    usb_wrap_ll_phy_select(&USB_WRAP, 0);

    usb_hid_init();

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;   // P4 默认选 HS 口，必须覆盖
    // 设备任务提到 12（默认 5 会被 encode(10)/USB host(15/16) 挤到饿死：
    // 1ms HID 轮询和控制传输都要及时响应；低于 USB host、高于视频编码）
    tusb_cfg.task = TINYUSB_TASK_CUSTOM(4096, 12, 0);
    tusb_cfg.descriptor.full_speed_config = desc_cfg;
    tusb_cfg.event_cb = usb_event_cb;
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb install");
    ESP_LOGI(TAG, "composite: kbd + mouse on GPIO24/25");
    return ESP_OK;
}
