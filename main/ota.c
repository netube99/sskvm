#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota.h"

static const char *TAG = "ota";

// 小于这个不可能是本项目的固件（实测 ~910KB）：垃圾请求直接拒，别把分区白擦一遍
#define OTA_MIN_BYTES   (128 * 1024)

static void reply(net_resp_t *resp, int status, const char *msg)
{
    resp->status = status;
    resp->data = (const uint8_t *)msg;
    resp->len = strlen(msg);
}

// 200 应答写完、TLS 正常关闭之后再动手（这个一次性任务随后重启设备）
static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

esp_err_t ota_upload(size_t total, net_stream_read_t read, void *rd_ctx, net_resp_t *resp)
{
    static bool s_busy;
    if (__atomic_test_and_set(&s_busy, __ATOMIC_RELAXED)) {     // 同时只许一个升级会话
        reply(resp, 409, "another upload is running");
        return ESP_OK;
    }
    bool ok = false;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    // 会话任务栈只有 8KB：4KB 缓冲放静态（s_busy 已保证同时只有一个升级会话）
    static uint8_t buf[4096];

    if (!part) {
        reply(resp, 500, "no ota partition");
        goto out;
    }
    if (total < OTA_MIN_BYTES || total > part->size) {
        reply(resp, 400, "size is not firmware-like");
        goto out;
    }

    esp_ota_handle_t h = 0;                             // handle 从 1 起：0 = begin 没给出来
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = esp_ota_begin(part, total, &h);       // 擦分区，~1-2s
    if (e != ESP_OK) {
        if (h) {
            esp_ota_abort(h);   // IDF 的 begin 在擦除失败时不自清 entry，手动兜住
        }
        ESP_LOGE(TAG, "esp_ota_begin %s: %d", part->label, e);
        reply(resp, 500, "erase failed");
        goto out;
    }

    size_t got = 0;
    while (got < total) {
        size_t want = total - got < sizeof(buf) ? total - got : sizeof(buf);
        int n = read(buf, want, rd_ctx);
        if (n <= 0) {
            esp_ota_abort(h);
            ESP_LOGW(TAG, "upload stalled at %u/%uKB", (unsigned)(got / 1024),
                     (unsigned)(total / 1024));
            reply(resp, 400, "upload interrupted");
            goto out;
        }
        e = esp_ota_write(h, buf, n);
        if (e != ESP_OK) {
            esp_ota_abort(h);
            // 镜像头非法（magic 0xE9 检查在首块写入时做）按坏镜像回 400，别冤枉 flash
            if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
                reply(resp, 400, "not a valid app image");
            } else {
                ESP_LOGE(TAG, "esp_ota_write: %d", e);
                reply(resp, 500, "flash write failed");
            }
            goto out;
        }
        got += n;
        if ((got >> 16) != ((got - n) >> 16)) {         // 每 64KB 一条进度
            ESP_LOGI(TAG, "receiving %u/%uKB", (unsigned)(got / 1024),
                     (unsigned)(total / 1024));
        }
    }

    e = esp_ota_end(h);                                 // 镜像完整性校验（sha256）
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %d", e);
        reply(resp, 400, "not a valid app image");
        goto out;
    }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %d", e);
        reply(resp, 500, "set boot failed");
        goto out;
    }
    ESP_LOGI(TAG, "ota done: %uKB in %dms -> %s, restarting",
             (unsigned)(total / 1024), (int)((esp_timer_get_time() - t0) / 1000),
             part->label);
    if (xTaskCreate(reboot_task, "ota_rbt", 2048, NULL, 3, NULL) != pdPASS) {
        esp_restart();          // 起不来就立刻重启：宁可断掉应答，不能停在"改了 boot 却不重启"
    }
    ok = true;
    reply(resp, 200, "rebooting");
out:
    if (!ok) {
        __atomic_clear(&s_busy, __ATOMIC_RELAXED);      // 成功路径马上重启，不用还
    }
    return ESP_OK;
}

void ota_confirm_if_pending(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "OTA image survived boot: confirmed, rollback cancelled");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}
