// 视频管线：UVC 原始帧（驱动层已剔 header）→ JPEG 硬解 → YUV420 → H264 硬编 → 双输出缓冲
//
// 四个任务流水并行，帧数据零拷贝：
//   strip 任务（core1）：收 UVC 帧 → 推引用（帧数据不动；仅 agent 拉图时拷一份截图影子）
//   decode 任务（core0）：JPEG 硬解（直接读驱动帧缓冲）→ 还帧 → YUV 队列
//   encode 任务（core0）：YUV → H264 直接写输出缓冲   ← 与前两级并行
//   WS 会话任务：latest 缓冲标 busy 发送（零拷贝）    ← 与编码并行
// 背压靠池深度：YUV 两块（decode 跟着 encode 走）、输出两块（编码跟着发送走）、
// 原始帧引用深度 1（strip 跟着 decode 走，UVC 驱动侧 newest-wins）。
//
// 采集在 uvc_capture，这里只做编解码与流水调度。
// 分辨率是运行时变量（480P/720P/1080P），切换时重建编码器、重开 UVC 流。
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/jpeg_decode.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"
#include "uvc_capture.h"
#include "kvm_api.h"
#include "video_pipeline.h"

static const char *TAG = "video";

#define YUV_BUFS        2       // 解/编各一：池深度就是背压，decode 不会跑到 encode 前面白耗带宽
#define OUT_BUFS        2       // 编码输出 ping-pong：一块在发、一块在编（publish 零拷贝）
#define OUT_HDR_PAD     64      // 载荷前预留：帧头 8B + WS 头 ≤10B（cache line 对齐的缓冲基址）
#define SHADOW_BUF_SIZE (2 * 1024 * 1024)   // 截图影子缓冲：1080p 高细节帧可达 ~1.5MB
#define SNAPSHOT_FRESH_US 300000  // 影子比这旧就视为不新鲜：回 404 并预热影子，调用者重试
#define SNAPSHOT_GAP_US   100000  // 影子更新最小间隔（~10/s）：agent 拉 ~8/s，别让 60fps 每帧都拷
#define VIDEO_BITRATE   CONFIG_SSKVM_VIDEO_BITRATE

#define STATS_N         150
#define STRIP_TASK_PRIO 13      // 与旧 decode 相同：低于 USB host(15/16)，高于编码(10)
#define DECODE_TASK_PRIO 11     // JPEG 解码内部阻塞等硬件（非自旋），core0 上不会饿死编码(10)
#define UVC_STALL_MS    10000   // 流开着但这么久没帧 → 关流重开（自愈）

// 可选分辨率。fps 是管线实测能跑到的帧率，只用于 H264 码控：
// RC 每帧预算 = bitrate / fps，填 60 会让 720P 实际只拿到 ~2.3Mbps（见 §码率）
static const struct {
    uint16_t w, h;
    uint8_t fps;
    const char *name;
} RES[] = {
    { 640,  480,  60, "480P"  },
    { 1280, 720,  46, "720P"  },
    { 1920, 1080, 21, "1080P" },
};
#define RES_N (sizeof(RES) / sizeof(RES[0]))

#if CONFIG_SSKVM_RES_480P
#define DEFAULT_RES 0
#elif CONFIG_SSKVM_RES_1080P
#define DEFAULT_RES 2
#else
#define DEFAULT_RES 1
#endif

// P4 H264 硬编输入是 "packed YUV420": 偶数行 u y y u y y...，奇数行 v y y v y y...
// JPEG 硬解输出 JPEG_DECODE_OUT_FORMAT_YUV420 用的是同一个 fourcc，可直接对接。
_Static_assert((uint32_t)JPEG_DECODE_OUT_FORMAT_YUV420 == (uint32_t)ESP_H264_RAW_FMT_O_UYY_E_VYY,
               "YUV420 fourcc mismatch");

typedef struct {
    uint8_t *buf;
    uint32_t pts;
    int64_t capture_us;     // UVC 帧回调时刻，用于统计端到端延时
} yuv_frame_t;

typedef struct {
    uvc_frame_t f;          // 持有的驱动帧（必须恰好 uvc_capture_release 一次）
} raw_ref_t;

static int s_res = DEFAULT_RES;         // 当前分辨率索引
static uint16_t s_w, s_h, s_align_h;
static uint32_t s_yuv_size;

static QueueHandle_t s_free_yuv_q;      // 空闲 YUV 缓冲
static QueueHandle_t s_ready_yuv_q;     // 待编码 YUV 帧（深度 1，永远只留最新一帧）
static QueueHandle_t s_raw_q;           // 待硬解的原始帧引用（深度 1，只留最新）
static uint8_t *s_shadow;               // 截图影子缓冲（最近一帧 JPEG 的拷贝，仅 agent 拉图时更新）
static jpeg_decoder_handle_t s_jpeg;
static esp_h264_enc_handle_t s_enc;
static esp_h264_enc_param_hw_handle_t s_enc_param;
static uint8_t *s_enc_buf[OUT_BUFS];    // 编码输出 ping-pong；载荷从 s_enc_buf[i]+OUT_HDR_PAD 开始
#define MAX_YUV_SIZE (1920 * 1088 * 3 / 2)   // 1080P（高度 16 对齐）
#define YUV1080_PAYLOAD (1920 * 1080 * 3 / 2) // 解码器实际写满的部分（1080 行）
static uint8_t *s_yuv[YUV_BUFS];         // 按最大尺寸一次分配，所有分辨率共用
static SemaphoreHandle_t s_snap_mu;      // 保护 s_shadow/s_jpeg_last_len（strip vs 截图）
static size_t s_jpeg_last_len;
static int64_t s_jpeg_us;                // 影子更新时刻（0 = 从未）
static volatile int64_t s_snap_hot_until;// agent 在拉图：strip 任务保持影子更新到这个时刻
static SemaphoreHandle_t s_dec_mu;       // decode 任务持帧期间持有；分辨率切换要等它
static video_frame_cb_t s_frame_cb;
static void *s_frame_cb_ctx;
static volatile bool s_force_idr;
static volatile bool s_enc_quit, s_enc_stopped;
static volatile int s_pending_res = -1;
static volatile bool s_res_busy;
static volatile uint32_t s_enc_frames;      // 编码成功计数
static volatile uint32_t s_idr_frames;      // IDR 帧计数
static int64_t s_gate_us;                   // 等传输层消化上一帧的时间（编码任务空转）
static int64_t s_last_idr_us;
static int64_t s_enc_us, s_dec_us, s_wait_us, s_lat_us, s_lat_max;
static int64_t s_bufwait_us, s_strip_us;
static uint32_t s_jpeg_bytes, s_enc_bytes;
static uint32_t s_dec_fail;                 // 连续解码失败计数（日志限速用）

int video_pipeline_res(void) { return s_res; }
int video_pipeline_res_count(void) { return RES_N; }
const char *video_pipeline_res_name(int idx)
{
    return (idx >= 0 && idx < (int)RES_N) ? RES[idx].name : "?";
}
void video_pipeline_res_dims(int idx, uint16_t *w, uint16_t *h)
{
    if (idx < 0 || idx >= (int)RES_N) {
        idx = 1;
    }
    *w = RES[idx].w;
    *h = RES[idx].h;
}

uint16_t video_pipeline_width(void) { return s_w; }
uint16_t video_pipeline_height(void) { return s_h; }
bool video_pipeline_res_busy(void) { return s_res_busy; }
uint32_t video_pipeline_encoded(void) { return s_enc_frames; }
uint32_t video_pipeline_idr(void) { return s_idr_frames; }

void video_pipeline_set_frame_cb(video_frame_cb_t cb, void *ctx)
{
    s_frame_cb = cb;
    s_frame_cb_ctx = ctx;
}

bool video_pipeline_snapshot_begin(const uint8_t **buf, size_t *len)
{
    if (s_snap_mu == NULL || xSemaphoreTake(s_snap_mu, pdMS_TO_TICKS(300)) != pdTRUE) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (s_jpeg_us == 0 || now - s_jpeg_us > SNAPSHOT_FRESH_US) {
        // 影子不新鲜（agent 闲置了一阵）：预热后回 404，调用者重试就能拿到新帧
        s_snap_hot_until = now + 3000000;
        xSemaphoreGive(s_snap_mu);
        return false;
    }
    *buf = s_shadow;
    *len = s_jpeg_last_len;
    return true;    // 调用者持锁拷贝，拷完 end() 解锁
}

void video_pipeline_snapshot_end(void)
{
    xSemaphoreGive(s_snap_mu);
}

void video_pipeline_request_rebuild(void)
{
    s_res_busy = true;
    s_pending_res = s_res;      // 让 decode 任务按当前分辨率整套重建
}

void video_pipeline_force_idr(void)
{
    s_force_idr = true;
}

// lat = 采集时刻 → H264 帧编完，端到端管内延时（不含采集卡/USB 传输与接收端）
static void stats_log(void)
{
    static uint32_t n;
    if (++n % STATS_N) {
        return;
    }
    ESP_LOGI(TAG, "%s lat=%.1f/%.1fms (wait=%.1f buf=%.1f strip=%.1f dec=%.1f enc=%.1f) jpeg=%.0fKB h264=%.1fKB idr=%lu gate=%.1fms",
             RES[s_res].name,
             s_lat_us / 1000.0 / STATS_N, s_lat_max / 1000.0,
             s_wait_us / 1000.0 / STATS_N, s_bufwait_us / 1000.0 / STATS_N,
             s_strip_us / 1000.0 / STATS_N, s_dec_us / 1000.0 / STATS_N,
             s_enc_us / 1000.0 / STATS_N,
             s_jpeg_bytes / 1024.0 / STATS_N, s_enc_bytes / 1024.0 / STATS_N,
             (unsigned long)s_idr_frames, s_gate_us / 1000.0 / STATS_N);
    s_lat_us = s_wait_us = s_bufwait_us = s_strip_us = s_dec_us = s_enc_us = 0;
    s_jpeg_bytes = s_enc_bytes = s_lat_max = 0;
    s_gate_us = 0;
}

// ---------------- 阶段3：YUV → H264 → 双输出缓冲（会话任务取去发） ----------------
static void encode_task(void *arg)
{
    while (1) {
        yuv_frame_t yf;
        if (xQueueReceive(s_ready_yuv_q, &yf, pdMS_TO_TICKS(50)) != pdPASS) {
            if (s_enc_quit) {
                s_enc_stopped = true;
                vTaskDelete(NULL);
            }
            continue;
        }
        if (s_enc_quit) {                   // 分辨率切换：退出，等重建
            if (yf.buf) {                   // 手里的帧必须还：不然每次切换漏 3MB（1080P），
                xQueueSend(s_free_yuv_q, &yf.buf, 0);   // 几次就把 PSRAM 吃光→codec_alloc 失败→重启
            }
            s_enc_stopped = true;
            vTaskDelete(NULL);
        }
        // 上一帧被会话取走才编下一帧（写另一块缓冲）：编码与发送并行，
        // 在途不超过一帧。发送不归 → 无缓冲可取 → 在这里等（这就是背压），
        // 等待在取帧之后：手里帧不还给解码，decode 自然限速，不白耗 PSRAM 带宽
        int64_t t_gate = esp_timer_get_time();
        uint8_t *out_buf;
        while ((out_buf = kvm_api_out_try_acquire()) == NULL) {
            if (s_enc_quit) {
                xQueueSend(s_free_yuv_q, &yf.buf, 0);
                s_enc_stopped = true;
                vTaskDelete(NULL);
            }
            vTaskDelay(1);
        }
        s_gate_us += esp_timer_get_time() - t_gate;

        int64_t t0 = esp_timer_get_time();
        // 刚发过 IDR（500ms 内）先不重复强制，但请求要留着（清了就丢了，
        // 新接入的浏览器会干等到下一个 GOP IDR）
        if (s_force_idr && t0 - s_last_idr_us > 500000) {
            s_force_idr = false;
            esp_h264_enc_force_idr(&s_enc_param->base);
        }
        esp_h264_enc_in_frame_t in = {
            .raw_data = { .buffer = yf.buf, .len = s_yuv_size },
            .pts = yf.pts,
        };
        esp_h264_enc_out_frame_t out = {
            .raw_data = { .buffer = out_buf, .len = MAX_YUV_SIZE },
        };
        esp_h264_err_t eret = esp_h264_enc_process(s_enc, &in, &out);
        if (out.frame_type == ESP_H264_FRAME_TYPE_IDR) {
            s_last_idr_us = esp_timer_get_time();
            s_idr_frames++;
        }
        // 编码器已读完输入帧，先还缓冲，解码任务能早 ~3ms 开工
        xQueueSend(s_free_yuv_q, &yf.buf, 0);
        if (eret == ESP_H264_ERR_OK) {
            int64_t t1 = esp_timer_get_time();
            s_enc_us += t1 - t0;
            s_enc_bytes += out.length;
            s_enc_frames++;
            if (s_frame_cb) {
                // pts 用采集时刻：前端算出来的是真正的端到端延时（采集→显示）
                s_frame_cb(out_buf, out.length, (uint32_t)(yf.capture_us / 1000),
                           out.frame_type == ESP_H264_FRAME_TYPE_IDR, s_frame_cb_ctx);
            }
            int64_t lat = esp_timer_get_time() - yf.capture_us;
            if (lat > 150000) {   // 常开满负荷下 720P 稳态 ~52ms，150ms 以上才是真尖峰（IDR/重开）
                ESP_LOGW(TAG, "slow frame: lat=%.1fms enc=%.1f h264=%uKB", lat / 1000.0,
                         (t1 - t0) / 1000.0, (unsigned)(out.length / 1024));
            }
            s_lat_us += lat;
            if (lat > s_lat_max) {
                s_lat_max = lat;
            }
            stats_log();
        } else {
            ESP_LOGW(TAG, "h264 encode failed");
        }
    }
}

// ---------------- 分辨率相关的分配/释放 ----------------
static void set_res(int idx)
{
    s_res = idx;
    s_w = RES[idx].w;
    s_h = RES[idx].h;
    s_align_h = (s_h + 15) & ~15;       // H264 硬编要求 16 对齐
    s_yuv_size = s_w * s_align_h * 3 / 2;
}

static void codec_free(void)
{
    // PSRAM 缓冲不释放（见 codec_init）：只丢队列引用、关掉编码器
    uint8_t *buf;
    while (xQueueReceive(s_free_yuv_q, &buf, 0) == pdPASS) {
    }
    yuv_frame_t yf;
    while (xQueueReceive(s_ready_yuv_q, &yf, 0) == pdPASS) {
    }
    if (s_enc) {
        esp_h264_enc_close(s_enc);
        esp_h264_enc_del(s_enc);
        s_enc = NULL;
        s_enc_param = NULL;
    }
}

static esp_err_t codec_alloc(void)
{
    // 缓冲按最大尺寸一次分配、永不释放：快速切分辨率时反复 alloc/free 3MB 会把
    // PSRAM 切碎，实测 600+ 次切换后 enc buf 分配失败（还有 14MB 空闲）→ 重启
    for (int i = 0; i < YUV_BUFS; i++) {
        uint8_t *b = s_yuv[i];
        xQueueSend(s_free_yuv_q, &b, 0);
    }

    esp_h264_enc_cfg_hw_t ecfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = RES[s_res].fps * 2,      // 2s
        .fps = RES[s_res].fps,          // 码控每帧预算 = bitrate/fps，必须贴近实际帧率
        .res = { .width = s_w, .height = s_h },
        .rc = { .bitrate = VIDEO_BITRATE, .qp_min = 18, .qp_max = 42 },
    };
    ESP_RETURN_ON_FALSE(esp_h264_enc_hw_new(&ecfg, &s_enc) == ESP_H264_ERR_OK, ESP_FAIL, TAG, "h264 new");
    ESP_RETURN_ON_FALSE(esp_h264_enc_open(s_enc) == ESP_H264_ERR_OK, ESP_FAIL, TAG, "h264 open");
    ESP_RETURN_ON_FALSE(esp_h264_enc_hw_get_param_hd(s_enc, &s_enc_param) == ESP_H264_ERR_OK,
                        ESP_FAIL, TAG, "h264 param");

    ESP_LOGI(TAG, "codec ready: %s %dx%d, %d Mbps @%dfps", RES[s_res].name, s_w, s_h,
             VIDEO_BITRATE / 1000000, RES[s_res].fps);
    return ESP_OK;
}

static esp_err_t codec_init(void)
{
    jpeg_decode_engine_cfg_t jcfg = { .timeout_ms = 100 };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&jcfg, &s_jpeg), TAG, "jpeg engine");

    s_free_yuv_q = xQueueCreate(YUV_BUFS, sizeof(uint8_t *));
    s_ready_yuv_q = xQueueCreate(1, sizeof(yuv_frame_t));
    s_raw_q = xQueueCreate(1, sizeof(raw_ref_t));
    s_dec_mu = xSemaphoreCreateMutex();
    s_snap_mu = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_free_yuv_q && s_ready_yuv_q && s_raw_q && s_dec_mu && s_snap_mu,
                        ESP_ERR_NO_MEM, TAG, "queues");

    s_shadow = (uint8_t *)heap_caps_malloc(SHADOW_BUF_SIZE, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_shadow, ESP_ERR_NO_MEM, TAG, "shadow buf");

    // YUV/编码输出缓冲按 1080P 上限一次分配，所有分辨率共用（小的写不满，无害）
    jpeg_decode_memory_alloc_cfg_t mcfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    for (int i = 0; i < YUV_BUFS; i++) {
        size_t actual = 0;
        s_yuv[i] = (uint8_t *)jpeg_alloc_decoder_mem(MAX_YUV_SIZE, &mcfg, &actual);
        ESP_RETURN_ON_FALSE(s_yuv[i] != NULL, ESP_ERR_NO_MEM, TAG, "yuv alloc");
        // 1080P 时解码器只写 1080 行，编码器却按 16 对齐的 1088 行读（最后 8 行是
        // 裁剪掉的 padding）。这段永远没人写，一次性清零，免得残留上一分辨率的旧像素。
        memset(s_yuv[i] + YUV1080_PAYLOAD, 0, MAX_YUV_SIZE - YUV1080_PAYLOAD);
    }
    // H264 输出要 cache line 对齐（DMA msync 要求），esp_h264_aligned_* 会强制 64B；
    // 载荷前预留 64B 给 帧头+WS头，publish/发送零拷贝
    for (int i = 0; i < OUT_BUFS; i++) {
        uint32_t actual = 0;
        s_enc_buf[i] = (uint8_t *)esp_h264_aligned_malloc(16, 1, OUT_HDR_PAD + MAX_YUV_SIZE,
                                                          &actual, ESP_H264_MEM_SPIRAM);
        ESP_RETURN_ON_FALSE(s_enc_buf[i] != NULL, ESP_ERR_NO_MEM, TAG, "enc buf");
        memset(s_enc_buf[i], 0, OUT_HDR_PAD);
    }
    ESP_LOGI(TAG, "buffers: %uKB shadow + %ux%uKB yuv + %ux%uKB enc (psram %u KB free)",
             (unsigned)(SHADOW_BUF_SIZE / 1024), YUV_BUFS, (unsigned)(MAX_YUV_SIZE / 1024),
             OUT_BUFS, (unsigned)((OUT_HDR_PAD + MAX_YUV_SIZE) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return ESP_OK;
}

esp_err_t video_pipeline_set_resolution(int idx)
{
    ESP_RETURN_ON_FALSE(idx >= 0 && idx < (int)RES_N, ESP_ERR_INVALID_ARG, TAG, "bad res");
    if (idx != s_res) {
        s_pending_res = idx;    // 由 decode 任务在安全点执行重建
        s_res_busy = true;
    }
    return ESP_OK;
}

// 调用方必须持有 s_dec_mu：把待硬解的原始帧还给驱动。关流/换格式前必须先做，
// 否则驱动发现“还有帧没还”就拒绝释放接口（`EP already allocated`，画面再也不回来）。
static void raw_drain_locked(void)
{
    raw_ref_t j;
    while (xQueueReceive(s_raw_q, &j, 0) == pdPASS) {
        uvc_capture_release(&j.f);
    }
}

// 在 strip 任务上下文重建（它持有 UVC 流与收帧；decode 被 s_dec_mu 挡在外面空手，
// 编码任务经 s_enc_quit 退出——两个阻塞点都有短超时，不需要 poison 帧）
static void pipeline_reconfigure(int idx)
{
    ESP_LOGW(TAG, "resolution -> %s (psram %u KB, iram %u KB largest %u KB)", RES[idx].name,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    s_res_busy = true;

    xSemaphoreTake(s_dec_mu, portMAX_DELAY);
    raw_drain_locked();                     // 旧分辨率的待解码帧退回驱动（新尺寸下解了也没用）
    s_enc_quit = true;
    while (!s_enc_stopped) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_enc_quit = s_enc_stopped = false;

    codec_free();
    set_res(idx);
    if (codec_alloc() != ESP_OK) {
        ESP_LOGE(TAG, "codec alloc failed (psram %u KB, iram %u KB largest %u KB), rebooting",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        esp_restart();
    }
    if (xTaskCreatePinnedToCore(encode_task, "encode", 4096, NULL, 10, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "encode task create failed");
    }
    xSemaphoreGive(s_dec_mu);
    // 不关流：驱动自己 停流→重新协商格式→重启，避开 USB 设备关闭/端点释放的竞态。
    // 此刻 strip 任务手里无帧（循环顶），驱动 format_select 里的全帧归还断言能过
    if (uvc_capture_reconfigure(s_w, s_h) != ESP_OK) {
        uvc_capture_close();
        uvc_capture_set_format(s_w, s_h);   // 下次 open 用新尺寸
    }
    video_pipeline_force_idr();
    s_res_busy = false;
}

// ---------------- 合成测试图案（不接采集卡时验证编码路径） ----------------
#if CONFIG_SSKVM_VIDEO_TEST_PATTERN
static void fill_test_pattern(uint8_t *buf, uint32_t frame_idx)
{
    const uint32_t row_bytes = s_w * 3 / 2;
    for (uint32_t y = 0; y < s_align_h; y++) {
        uint32_t *p = (uint32_t *)(buf + (size_t)y * row_bytes);
        uint8_t chroma = (y & 1) ? 160 : 96;        // 偶行 U / 奇行 V
        uint8_t l = (uint8_t)(y + frame_idx * 2);
        uint32_t v = (uint32_t)chroma | ((uint32_t)l << 8) | ((uint32_t)l << 16) | ((uint32_t)chroma << 24);
        for (uint32_t i = 0; i < row_bytes / 4; i++) {
            p[i] = v;
        }
    }
}

static void test_pattern_task(void *arg)
{
    uint32_t i = 0;
    TickType_t last = xTaskGetTickCount();
    while (1) {
        uint8_t *yuv = NULL;
        if (xQueueReceive(s_free_yuv_q, &yuv, pdMS_TO_TICKS(50)) == pdPASS) {
            fill_test_pattern(yuv, i);
            yuv_frame_t yf = { .buf = yuv, .pts = i * 16, .capture_us = esp_timer_get_time() };
            if (xQueueSend(s_ready_yuv_q, &yf, 0) != pdPASS) {
                xQueueSend(s_free_yuv_q, &yuv, 0);
            }
        }
        i++;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(16));
    }
}
#endif // CONFIG_SSKVM_VIDEO_TEST_PATTERN

// ---------------- 阶段1：UVC → 剔 header → 待解码队列（strip 任务） ----------------
#if !CONFIG_SSKVM_VIDEO_TEST_PATTERN
static void strip_task(void *arg)
{
    int64_t last_frame_us = esp_timer_get_time();
    while (1) {
        if (uvc_capture_open() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        last_frame_us = esp_timer_get_time();
        video_pipeline_force_idr();     // 新流的头几帧可能是断的/旧相位：从关键帧重新起步
        while (uvc_capture_connected()) {
            if (uvc_capture_take_restart_request()) {
                ESP_LOGW(TAG, "capture restart requested");
                break;                  // 外层关流→按当前分辨率重开
            }
            int idx = s_pending_res;        // 分辨率切换：安全点（手里无帧）重建
            if (idx >= 0) {
                s_pending_res = -1;
                pipeline_reconfigure(idx);
                if (!uvc_capture_connected()) {
                    break;                  // 流没了，外层重开
                }
                continue;                   // 流还在（format_select 成功），继续收帧
            }
            if (esp_timer_get_time() - last_frame_us > UVC_STALL_MS * 1000) {
                // 流开着却不出帧（采集卡抽风）：关流重开一次
                ESP_LOGW(TAG, "UVC stream stalled %d s, reopening", UVC_STALL_MS / 1000);
                break;
            }
            uvc_frame_t f;
            if (!uvc_capture_receive(&f, 200)) {
                continue;
            }
            last_frame_us = esp_timer_get_time();
            s_bufwait_us += esp_timer_get_time() - f.capture_us;

            // 截图影子：仅 agent 在拉图（热）时拷贝。冷时不拷 → 帧数据零拷贝。
            // 热时限速 ~10/s：agent 拉 ~8/s，管线 60fps——不节流就是每帧拷一整张
            // 1080P JPEG（strip+8ms, enc 被带宽挤慢），1080P 没有这点余量，实测
            // 21→17fps。100ms 间隔仍满足 /snapshot.jpg 的 300ms 新鲜度契约。
            int64_t now_us = esp_timer_get_time();
            if (now_us < s_snap_hot_until && now_us - s_jpeg_us > SNAPSHOT_GAP_US) {
                xSemaphoreTake(s_snap_mu, portMAX_DELAY);
                memcpy(s_shadow, f.data, f.len);
                s_jpeg_last_len = f.len;
                s_jpeg_us = now_us;
                xSemaphoreGive(s_snap_mu);
            }
            s_strip_us += esp_timer_get_time() - now_us;

            raw_ref_t j = { .f = f };
            if (xQueueSend(s_raw_q, &j, 0) != pdPASS) {
                // 只留最新：decode 还没取走上一帧就丢上一帧（它在解更早那帧）
                raw_ref_t old;
                if (xQueueReceive(s_raw_q, &old, 0) == pdPASS) {
                    uvc_capture_release(&old.f);
                    xQueueSend(s_raw_q, &j, 0);
                } else {
                    uvc_capture_release(&j.f);
                }
            }
        }
        // 关流前保证没有任何帧在外面（decode 持帧时握着 s_dec_mu），否则 close 失败、
        // 接口不释放（见 raw_drain_locked）
        xSemaphoreTake(s_dec_mu, portMAX_DELAY);
        raw_drain_locked();
        xSemaphoreGive(s_dec_mu);
        uvc_capture_close();
    }
}

// ---------------- 阶段2：JPEG 硬解 → YUV（decode 任务，与 strip 并行） ----------------
// 直接读驱动帧缓冲：驱动装配时已剔 header（uvc_bulk.c sskvm patch），零拷贝
static void decode_task(void *arg)
{
    while (1) {
        // dec_mu 持有期间不跨任何“拿缓冲/还缓冲”边界之外的东西：
        // 分辨率切换拿它即可保证此时 decode 空手、YUV/帧引用账目干净
        xSemaphoreTake(s_dec_mu, portMAX_DELAY);
        uint8_t *yuv;
        // 先等 YUV 再取帧：YUV 池就是背压，decode 跟着 encode 的节奏走；
        // 若反过来，编码慢时弹出的帧会白白丢掉
        if (xQueueReceive(s_free_yuv_q, &yuv, pdMS_TO_TICKS(50)) != pdPASS) {
            xSemaphoreGive(s_dec_mu);
            continue;
        }
        raw_ref_t j;
        if (xQueueReceive(s_raw_q, &j, pdMS_TO_TICKS(2)) != pdPASS) {
            xQueueSend(s_free_yuv_q, &yuv, 0);
            xSemaphoreGive(s_dec_mu);
            vTaskDelay(pdMS_TO_TICKS(2));   // 没活干，别把 dec_mu 锁死
            continue;
        }

        jpeg_decode_cfg_t dcfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_YUV420,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t out_size = 0;
        int64_t t0 = esp_timer_get_time();
        esp_err_t jerr = jpeg_decoder_process(s_jpeg, &dcfg, j.f.data, j.f.len, yuv, s_yuv_size, &out_size);
        s_wait_us += t0 - j.f.capture_us;
        s_dec_us += esp_timer_get_time() - t0;
        uvc_capture_release(&j.f);
        if (jerr != ESP_OK) {
            s_dec_fail++;                      // 限速日志：115200 串口刷爆会反噬 USB 回调时序
            if (s_dec_fail == 1 || s_dec_fail % 100 == 0) {
                ESP_LOGW(TAG, "jpeg decode failed: %s (%u B) n=%lu", esp_err_to_name(jerr),
                         (unsigned)j.f.len, (unsigned long)s_dec_fail);
            }
            xQueueSend(s_free_yuv_q, &yuv, 0);
            xSemaphoreGive(s_dec_mu);
            continue;
        }
        // ready 队列只留最新一帧；YUV 池（2 块）已让 decode 跟着 encode 的节奏走，
        // 正常情况下这里不会真丢帧（丢帧白白耗一次解码的 PSRAM 带宽）
        yuv_frame_t old;
        if (xQueueReceive(s_ready_yuv_q, &old, 0) == pdPASS && old.buf) {
            xQueueSend(s_free_yuv_q, &old.buf, 0);
        }
        yuv_frame_t yf = { .buf = yuv, .pts = (uint32_t)(j.f.capture_us / 1000), .capture_us = j.f.capture_us };
        if (xQueueSend(s_ready_yuv_q, &yf, 0) != pdPASS) {
            xQueueSend(s_free_yuv_q, &yuv, 0);
        }
        s_dec_fail = 0;
        xSemaphoreGive(s_dec_mu);
    }
}
#endif // !CONFIG_SSKVM_VIDEO_TEST_PATTERN

esp_err_t video_pipeline_start(void)
{
    set_res(DEFAULT_RES);
    ESP_RETURN_ON_ERROR(codec_init(), TAG, "codec_init");
    ESP_RETURN_ON_ERROR(codec_alloc(), TAG, "codec_alloc");
    kvm_api_out_register(s_enc_buf[0] + OUT_HDR_PAD, s_enc_buf[1] + OUT_HDR_PAD, MAX_YUV_SIZE);
    if (xTaskCreatePinnedToCore(encode_task, "encode", 4096, NULL, 10, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_SSKVM_VIDEO_TEST_PATTERN
    ESP_LOGW(TAG, "TEST PATTERN mode (no UVC)");
    if (xTaskCreatePinnedToCore(test_pattern_task, "pattern", 4096, NULL, 10, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#else
    ESP_RETURN_ON_ERROR(uvc_capture_init(), TAG, "uvc_capture_init");
    uvc_capture_set_format(s_w, s_h);
    if (xTaskCreatePinnedToCore(strip_task, "strip", 4096, NULL, STRIP_TASK_PRIO, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(decode_task, "decode", 4096, NULL, DECODE_TASK_PRIO, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}
