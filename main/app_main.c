// sskvm：局域网 IP-KVM 设备端。
//
//   HDMI → UVC 采集卡 → P4（JPEG 硬解 → H264 硬编 → WSS）
//   P4 同时是复合 USB 设备（HID boot 键盘 + 绝对鼠标），插在**被控机**上
//
// 启动顺序：NVS 配置 → 以太网 → 传输层/HTTP 路由 → USB 复合设备 → HID 任务
//           → 视频管线 → 串口 console。
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "kvm_config.h"
#include "eth_net.h"
#include "kvm_mdns.h"
#include "kvm_api.h"
#include "ota.h"
#include "kvm_input.h"
#include "usb_device.h"
#include "video_pipeline.h"
#include "console.h"

static const char *TAG = "sskvm";

void app_main(void)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: %s rev%d.%d, cores=%d", CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100, chip.cores);

    // 1. NVS 配置（主机名/token/静态 IP/分辨率）
    ESP_ERROR_CHECK(kvm_config_init());

    // 2. 以太网 + DHCP/静态 IP + SNTP（TLS 证书有效期要正确时钟）
    ESP_ERROR_CHECK(eth_net_start());
    char ip[16];
    if (eth_net_wait_ip(pdMS_TO_TICKS(15000), ip, sizeof(ip)) == ESP_OK) {
        ESP_LOGI(TAG, "open https://%s/  (hostname %s)", ip, kvm_config_hostname());
    }
    eth_net_sntp_start();
    kvm_mdns_start();

    // 3. 传输层 + 路由（编码任务的帧从这里出去）
    ESP_ERROR_CHECK(kvm_api_start());

    // 4. 复合 USB 设备（插在被控机上）
    ESP_ERROR_CHECK(usb_device_init());
    ESP_LOGI(TAG, "USB composite ready: HID kbd + absolute mouse on GPIO24/25");

    // 5. HID 下发任务（kvm_input_handle 的消费者）
    kvm_input_start();

    // 6. 视频管线（UVC 采集 → JPEG 硬解 → H264 硬编，编码输出回调给 kvm_api）
    video_pipeline_set_frame_cb(kvm_api_publish_frame, NULL);
    ESP_ERROR_CHECK(video_pipeline_start());

    // 7. 串口 console：控制 + 压测驱动（res/idr/stat/hid/resloop）
    ESP_ERROR_CHECK(console_start());

    // OTA 新固件的确认点：全部模块起来后再撑 3s 才取消 bootloader 自动回退；
    // 窗口内崩溃/断电/挂死重启 → 旧固件继续跑（见 ota.c）
    vTaskDelay(pdMS_TO_TICKS(3000));
    ota_confirm_if_pending();

    uint32_t s = 0;
    while (1) {
        if (s % 30 == 0) {
            ESP_LOGI(TAG, "tick %lu | up %lu s | iram %lu KB | psram %lu KB",
                     (unsigned long)s,
                     (unsigned long)(esp_timer_get_time() / 1000000ULL),
                     (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        }
        s++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
