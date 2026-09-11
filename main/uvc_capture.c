// UVC 采集实现。详见 uvc_capture.h。
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "uvc_capture.h"

static const char *TAG = "uvc";

#define USB_HOST_PRIO    15
#define FRAME_QUEUE_LEN  3
#define UVC_FRAME_SIZE   (2 * 1024 * 1024)   // 1080p 高细节帧实测可达 ~1.5MB（别用 4MB）

typedef struct {
    const uvc_host_frame_t *frame;
    int64_t capture_us;
} raw_frame_t;

static QueueHandle_t s_frame_q;
static SemaphoreHandle_t s_frame_mu;    // 串行化 frame_cb 与关流时的回收
static volatile bool s_closing;
static volatile bool s_connected;
static volatile bool s_restart;    // 请求关流重开（设置页按钮 / 拔插后自愈）
static uint8_t s_cap_fps;           // 请求的采集帧率（0 = 卡的默认/最高）
static uvc_host_stream_hdl_t s_stream;
static uint8_t s_dev_addr, s_uvc_idx;
static volatile uint32_t s_frames;      // 收到的帧计数（监督任务用）

static void driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    if (event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        s_dev_addr = event->device_connected.dev_addr;
        s_uvc_idx = event->device_connected.uvc_stream_index;
        ESP_LOGI(TAG, "camera connected: addr=%d, %d formats",
                 s_dev_addr, event->device_connected.frame_info_num);
    }
}

static bool frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    QueueHandle_t q = *(QueueHandle_t *)user_ctx;
    raw_frame_t rf = { .frame = frame, .capture_us = esp_timer_get_time() };
    // 关流时的竞态：stop 返回后仍可能有回调在飞。若回调在 drain 之后才入队，
    // 这一帧就永远回不了驱动 → "Not all frames are returned" → 接口不释放 →
    // 下次 open 报 EP already allocated。用 take(0) 的锁串行化：关流中直接还帧。
    if (!s_frame_mu || xSemaphoreTake(s_frame_mu, 0) != pdTRUE) {
        return true;
    }
    bool keep = !s_closing && xQueueSend(q, &rf, 0) == pdPASS;
    xSemaphoreGive(s_frame_mu);
    return !keep;
}

static void stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "USB transfer error %i", event->transfer_error.error);
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "camera disconnected");
        s_connected = false;
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGD(TAG, "frame buffer overflow");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGD(TAG, "frame buffer underflow");
        break;
    default:
        break;
    }
}

static uvc_host_stream_config_t s_stream_cfg = {
    .event_cb = stream_event_cb,
    .frame_cb = frame_cb,
    .user_ctx = &s_frame_q,
    .usb = {
        .vid = UVC_HOST_ANY_VID,
        .pid = UVC_HOST_ANY_PID,
    },
    .vs_format = {
        .format = UVC_VS_FORMAT_MJPEG,     // 分辨率/帧率由 uvc_capture_set_format() 运行时填
    },
    .advanced = {
        .number_of_frame_buffers = FRAME_QUEUE_LEN,
        .frame_size = UVC_FRAME_SIZE,
        .frame_heap_caps = MALLOC_CAP_SPIRAM,
        .number_of_urbs = 3,
        .urb_size = 16384,
    },
};

static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}


esp_err_t uvc_capture_init(void)
{
    s_frame_q = xQueueCreate(FRAME_QUEUE_LEN, sizeof(raw_frame_t));
    s_frame_mu = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_frame_q != NULL && s_frame_mu != NULL, ESP_ERR_NO_MEM, TAG, "frame q");

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL,
                                                USB_HOST_PRIO, NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "usb_lib task");

    const uvc_host_driver_config_t drv_cfg = {
        .event_cb = driver_event_cb,
        .driver_task_stack_size = 5 * 1024,
        .driver_task_priority = USB_HOST_PRIO + 1,
        .xCoreID = 0,
        .create_background_task = true,
    };
    return uvc_host_install(&drv_cfg);
}

void uvc_capture_set_format(uint16_t w, uint16_t h, uint8_t fps)
{
    s_stream_cfg.vs_format.h_res = w;
    s_stream_cfg.vs_format.v_res = h;
    s_stream_cfg.vs_format.fps = fps;           // 0 = 用采集卡默认（最高）帧率
    s_cap_fps = fps;
}

void uvc_capture_request_restart(void)
{
    s_restart = true;
}

bool uvc_capture_take_restart_request(void)
{
    if (!s_restart) {
        return false;
    }
    s_restart = false;
    return true;
}

// 运行时换分辨率：驱动自己会 停流→probe/commit→重启，不关设备。
// 用 close+open 的话，快速切换会撞上 USB 设备关闭/端点释放的竞态
// （驱动 assert: usbh_dev_close num_ctrl_xfers_inflight / EP already allocated）。
esp_err_t uvc_capture_reconfigure(uint16_t w, uint16_t h)
{
    if (!s_stream) {
        return ESP_ERR_INVALID_STATE;
    }
    // 记住目标格式：断线/抽风重开时 open 用的是它。忘了这一步，切到 1080P 后
    // 拔插采集卡会用开机时的 720P 重开 —— 卡的 JPEG 是 720p、编码器按 1080p 读
    // YUV，画面就成斜切的错位花屏（见 AGENTS §7）。
    uvc_capture_set_format(w, h, s_cap_fps);
    uvc_host_stream_format_t fmt = {
        .format = UVC_VS_FORMAT_MJPEG,
        .h_res = w,
        .v_res = h,
        .fps = s_cap_fps,                  // 切分辨率也得带卡帧率，否则又协商回卡的最高帧率
    };
    // 1) 先把我们手里的帧全还给驱动。format_select 里的 uvc_format_save()
    //    断言所有帧都已在空队列里，手里还留着帧就会 assert 重启。
    //    （调用方——strip 任务——在循环顶拿不到帧；decode 侧的帧由 pipeline_reconfigure
    //    通过 dec_mu 保证已归还；驱动队列里的帧在这里还。）
    xSemaphoreTake(s_frame_mu, portMAX_DELAY);
    s_closing = true;
    raw_frame_t rf;
    while (xQueueReceive(s_frame_q, &rf, 0) == pdPASS) {
        uvc_host_frame_return(s_stream, (uvc_host_frame_t *)rf.frame);
    }
    xSemaphoreGive(s_frame_mu);

    // 2) 换格式（驱动内部 stop→probe/commit→start；stop 会把 current_frame 还回来）
    esp_err_t err = uvc_host_stream_format_select(s_stream, &fmt);

    // 3) 重新放行 frame_cb
    xSemaphoreTake(s_frame_mu, portMAX_DELAY);
    s_closing = false;
    xSemaphoreGive(s_frame_mu);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "format_select %ux%u: %s", w, h, esp_err_to_name(err));
        s_connected = false;
    }
    return err;
}

esp_err_t uvc_capture_open(void)
{
    uvc_host_stream_hdl_t stream = NULL;
    esp_err_t err = uvc_host_stream_open(&s_stream_cfg, pdMS_TO_TICKS(5000), &stream);
    if (err != ESP_OK && s_cap_fps) {
        // 卡没有这个帧率档（驱动用 fabsf 精确匹配 descriptor）→ 退回卡的默认帧率，
        // 别把流丢在打不开的状态（否则 5s 重试循环里永远出不了画面）
        ESP_LOGW(TAG, "no %u fps mode (%s), retry at card default", s_cap_fps, esp_err_to_name(err));
        uvc_capture_set_format(s_stream_cfg.vs_format.h_res, s_stream_cfg.vs_format.v_res, 0);
        err = uvc_host_stream_open(&s_stream_cfg, pdMS_TO_TICKS(5000), &stream);
    }
    if (err != ESP_OK) {
        usb_host_lib_info_t info;
        if (usb_host_lib_info(&info) == ESP_OK) {
            ESP_LOGW(TAG, "no UVC camera (%s), usb devices=%d", esp_err_to_name(err), info.num_devices);
        }
        return err;
    }
    s_stream = stream;
    s_connected = true;
    err = uvc_host_stream_start(stream);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream start failed: %s", esp_err_to_name(err));
        uvc_capture_close();        // 别把开着的流丢了，否则端点一直占着，下次 open 直接失败
        return err;
    }
    ESP_LOGI(TAG, "UVC opened");
    return ESP_OK;
}

void uvc_capture_close(void)
{
    if (s_stream == NULL) {
        return;
    }
    // 先停流再回收帧！stop 之前流还在跑（60fps），只 drain 一次会漏帧，
    // 而 uvc_host_stream_close 发现还有帧没还就跳过接口释放（只警告一句），
    // 端点 0x83 就一直占着，下次 open 报 "EP already allocated"、画面永远回不来。
    // 调用方（strip 任务）此时手里无帧（循环顶），消费者侧的帧由管线保证已归还。
    uvc_host_stream_stop(s_stream);
    xSemaphoreTake(s_frame_mu, portMAX_DELAY);   // 之后到达的回调直接还帧（见 frame_cb）
    s_closing = true;
    raw_frame_t rf;
    while (xQueueReceive(s_frame_q, &rf, 0) == pdPASS) {
        uvc_host_frame_return(s_stream, (uvc_host_frame_t *)rf.frame);
    }
    xSemaphoreGive(s_frame_mu);
    esp_err_t err = uvc_host_stream_close(s_stream);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream close: %s", esp_err_to_name(err));
    }
    s_stream = NULL;
    s_connected = false;
    s_closing = false;
}

bool uvc_capture_connected(void)
{
    return s_connected;
}

uint32_t uvc_capture_frames(void)
{
    return s_frames;
}

void uvc_capture_dump_devices(void)
{
    usb_host_client_config_t ccfg = { .is_synchronous = true, .max_num_event_msg = 4 };
    usb_host_client_handle_t client = NULL;
    esp_err_t err = usb_host_client_register(&ccfg, &client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dump: client_register %s", esp_err_to_name(err));
        return;
    }
    uint8_t addrs[8];
    int num = 8;
    err = usb_host_device_addr_list_fill(num, addrs, &num);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dump: addr_list_fill %s", esp_err_to_name(err));
        usb_host_client_deregister(client);
        return;
    }
    ESP_LOGW(TAG, "%d USB device(s) on the bus:", num);
    for (int i = 0; i < num; i++) {
        usb_device_handle_t dev = NULL;
        err = usb_host_device_open(client, addrs[i], &dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "  addr %d: open %s", addrs[i], esp_err_to_name(err));
            continue;
        }
        const usb_device_desc_t *dd = NULL;
        usb_host_get_device_descriptor(dev, &dd);
        ESP_LOGW(TAG, "  addr %d vid=%04x pid=%04x class=%02x", addrs[i],
                 dd ? dd->idVendor : 0, dd ? dd->idProduct : 0, dd ? dd->bDeviceClass : 0);
        usb_host_device_close(client, dev);
    }
    usb_host_client_deregister(client);
}

bool uvc_capture_receive(uvc_frame_t *out, uint32_t timeout_ms)
{
    raw_frame_t rf;
    if (xQueueReceive(s_frame_q, &rf, pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        return false;
    }
    // 只保留最新帧：管线落后时处理旧帧只会把延时越积越高。
    // 丢掉的旧帧当场归还（它们从未交给上层，无所有权问题）
    raw_frame_t newer;
    while (xQueueReceive(s_frame_q, &newer, 0) == pdPASS) {
        uvc_host_frame_return(s_stream, (uvc_host_frame_t *)rf.frame);
        rf = newer;
    }
    s_frames++;
    out->data = rf.frame->data;
    out->len = rf.frame->data_len;      // 归还前记住，return 会把 data_len 清 0
    out->capture_us = rf.capture_us;
    out->frame = rf.frame;
    return true;
}

void uvc_capture_release(const uvc_frame_t *f)
{
    if (f && f->frame && s_stream) {
        uvc_host_frame_return(s_stream, (uvc_host_frame_t *)f->frame);
    }
}
