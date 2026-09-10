// 串口 console：设备控制面 + 压测驱动。
//
// 行式命令（\n 结尾，115200）：
//   res            查询分辨率
//   res <0|1|2>    切换分辨率（0=480P 1=720P 2=1080P）
//   rebuild        按当前分辨率整套重建管线（等价一次切换）
//   idr            强制下一帧编 IDR
//   resloop <ms|0> 每 ms 毫秒自动轮换分辨率（快速切换压测），0 停止
//   hid <ms|0>     每 ms 毫秒注入一轮合成键鼠事件（HID 压测），0 停止
//   stat           打印状态（内存/帧计数/USB 枚举）
//   usbdevs        列 USB host 总线上的设备（诊断采集卡）
//   reboot
//
// 无鉴权、无持久化：它只在本地串口上可用（115200 只能在设备旁接入）。
// 压测命令（resloop/hid）由 tools/video_stress.py、tools/hid_test.py 驱动。
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "usb/usb_host.h"
#include "usb_device.h"
#include "uvc_capture.h"
#include "video_pipeline.h"
#include "kvm_input.h"
#include "console.h"

static const char *TAG = "console";

// ---------------- HID 压测驱动 ----------------
static volatile int s_hid_period_ms;    // 0 = 停
static volatile uint32_t s_hid_cycles;

// 注入一轮合成键鼠事件：1 次击键（'a' down/up）+ 鼠标横扫（40%~80% 区间按住左键）
// 事件走 kvm_input_handle —— 和未来网络层同一条入口，压测的就是真实路径。
static void hid_inject_one_cycle(void)
{
    static const uint8_t kbd_down[8] = { 0x01, 0, 0x04, 0, 0, 0, 0, 0 };   // 0x01=KBD 事件，'a'
    static const uint8_t kbd_up[8] = { 0x01 };
    kvm_input_handle(kbd_down, 8);
    kvm_input_handle(kbd_up, 8);

    static int step;
    int x = step * 32767 / 10, y = step * 32767 / 10;
    uint8_t btn = (step >= 4 && step < 8) ? 1 : 0;
    uint8_t mouse[7] = { 0x02, btn, x & 0xFF, x >> 8, y & 0xFF, y >> 8, 0 };
    if (step == 2) {
        mouse[6] = 1;                   // 滚轮上
    } else if (step == 9) {
        mouse[6] = (uint8_t)-1;         // 滚轮下
    }
    kvm_input_handle(mouse, 7);
    step = (step + 1) % 11;
    s_hid_cycles++;
}

static void hid_stress_task(void *arg)
{
    while (1) {
        if (!s_hid_period_ms) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        hid_inject_one_cycle();
        int64_t el = (esp_timer_get_time() - t0) / 1000;
        vTaskDelay(pdMS_TO_TICKS(s_hid_period_ms > el ? s_hid_period_ms - el : 0));
    }
}

// ---------------- resloop 压测驱动 ----------------
static volatile int s_resloop_ms;       // 0 = 停
static volatile uint32_t s_resloop_count;

static void resloop_task(void *arg)
{
    int idx = 0;
    while (1) {
        if (!s_resloop_ms) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        idx = (idx + 1) % 3;
        if (video_pipeline_set_resolution(idx) == ESP_OK) {
            s_resloop_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(s_resloop_ms));
    }
}

// ---------------- 命令执行 ----------------
static void stat_print(void)
{
    usb_host_lib_info_t lib = {0};
    usb_host_lib_info(&lib);
    ESP_LOGI(TAG, "res=%s busy=%d uvc_frames=%u enc_frames=%u idr=%u hid_cycles=%u resloop=%u usb_mounted=%d usbdevs=%d",
             video_pipeline_res_name(video_pipeline_res()), video_pipeline_res_busy(),
             (unsigned)uvc_capture_frames(), (unsigned)video_pipeline_encoded(),
             (unsigned)video_pipeline_idr(), (unsigned)s_hid_cycles, (unsigned)s_resloop_count,
             usb_device_mounted(), lib.num_devices);
    ESP_LOGI(TAG, "iram %lu KB (largest %lu KB) | psram %lu KB (largest %lu KB)",
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

static void exec(char *line)
{
    char *cmd = strtok(line, " ");
    char *arg = strtok(NULL, " ");
    if (!cmd) {
        return;
    }
    if (!strcmp(cmd, "res")) {
        if (!arg) {
            ESP_LOGI(TAG, "res=%s", video_pipeline_res_name(video_pipeline_res()));
        } else {
            esp_err_t e = video_pipeline_set_resolution(atoi(arg));
            ESP_LOGI(TAG, "res %s: %s", arg, esp_err_to_name(e));
        }
    } else if (!strcmp(cmd, "rebuild")) {
        video_pipeline_request_rebuild();
        ESP_LOGI(TAG, "rebuild requested");
    } else if (!strcmp(cmd, "idr")) {
        video_pipeline_force_idr();
        ESP_LOGI(TAG, "idr requested");
    } else if (!strcmp(cmd, "resloop")) {
        s_resloop_ms = arg ? atoi(arg) : 0;
        if (s_resloop_ms < 0) {
            s_resloop_ms = 0;
        }
        ESP_LOGI(TAG, "resloop=%d ms", s_resloop_ms);
    } else if (!strcmp(cmd, "hid")) {
        s_hid_period_ms = arg ? atoi(arg) : 0;
        if (s_hid_period_ms < 0) {
            s_hid_period_ms = 0;
        }
        if (!s_hid_period_ms) {
            kvm_input_release_all();   // 停止时全松：停止瞬间可能正按着键/鼠标
        }
        ESP_LOGI(TAG, "hid=%d ms", s_hid_period_ms);
    } else if (!strcmp(cmd, "stat")) {
        stat_print();
    } else if (!strcmp(cmd, "usbdevs")) {
        uvc_capture_dump_devices();
    } else if (!strcmp(cmd, "reboot")) {
        esp_restart();
    } else {
        ESP_LOGW(TAG, "unknown cmd: %s (res|rebuild|idr|resloop|hid|stat|usbdevs|reboot)", cmd);
    }
}

static void console_task(void *arg)
{
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &cfg));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));

    char line[64];
    int n = 0;
    while (1) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (n) {
                uart_write_bytes(UART_NUM_0, "\r\n", 2);
                line[n] = 0;
                exec(line);
                n = 0;
            }
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = c;
            uart_write_bytes(UART_NUM_0, &c, 1);    // echo
        }
    }
}

esp_err_t console_start(void)
{
    if (xTaskCreate(console_task, "console", 4096, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(hid_stress_task, "hidstress", 3072, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(resloop_task, "resloop", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
