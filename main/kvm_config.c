// 设备配置（NVS）。
//
// 为什么主机名要带 MAC 后缀：局域网里 N 台 KVM 如果都叫 sskvm.local 必然冲突，
// mDNS 的冲突改名又会让名字不稳定（这次是 sskvm-2，重启变 sskvm-3）。
// 出厂默认 sskvm-<MAC 后 2 字节> 零配置且唯一，用户可以再改成 kvm-office 这种好记的。
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "kvm_config.h"

static const char *TAG = "config";

#define NS          "sskvm"
#define K_HOSTNAME  "hostname"
#define K_TOKEN     "token"
#define K_IP        "static_ip"
#define K_GW        "static_gw"
#define K_MASK      "static_mask"
#define K_RES       "res"
#define K_WOL       "wol_mac"

static char s_hostname[33];
static char s_token[65];
static char s_ip[16], s_gw[16], s_mask[16];
static char s_wol[18];
static uint8_t s_res;

#if defined(CONFIG_SSKVM_RES_480P)
#define DEFAULT_RES 0
#elif defined(CONFIG_SSKVM_RES_1080P)
#define DEFAULT_RES 2
#else
#define DEFAULT_RES 1
#endif

static void load_str(nvs_handle_t h, const char *key, char *out, size_t cap, const char *def)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        snprintf(out, cap, "%s", def);
    }
}

esp_err_t kvm_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init");

    // 出厂默认主机名：sskvm-<以太网 MAC 后 2 字节>
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_ETH);
    char def_host[33];
    snprintf(def_host, sizeof(def_host), "sskvm-%02x%02x", mac[4], mac[5]);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        snprintf(s_hostname, sizeof(s_hostname), "%s", def_host);
        snprintf(s_token, sizeof(s_token), "%s", CONFIG_SSKVM_AUTH_TOKEN);
        s_res = DEFAULT_RES;
        s_ip[0] = s_gw[0] = s_mask[0] = 0;
        s_wol[0] = 0;
        ESP_LOGI(TAG, "first boot: hostname=%s", s_hostname);
        return ESP_OK;
    }
    load_str(h, K_HOSTNAME, s_hostname, sizeof(s_hostname), def_host);
    load_str(h, K_TOKEN, s_token, sizeof(s_token), CONFIG_SSKVM_AUTH_TOKEN);
    load_str(h, K_IP, s_ip, sizeof(s_ip), "");
    load_str(h, K_GW, s_gw, sizeof(s_gw), "");
    load_str(h, K_MASK, s_mask, sizeof(s_mask), "");
    load_str(h, K_WOL, s_wol, sizeof(s_wol), "");
    s_res = DEFAULT_RES;
    nvs_get_u8(h, K_RES, &s_res);
    nvs_close(h);

    ESP_LOGI(TAG, "hostname=%s net=%s", s_hostname, s_ip[0] ? s_ip : "dhcp");
    return ESP_OK;
}

static esp_err_t save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_str(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint8_t kvm_config_res(void) { return s_res; }

esp_err_t kvm_config_set_res(uint8_t idx)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(h, K_RES, idx);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_res = idx;
    }
    return err;
}

const char *kvm_config_hostname(void) { return s_hostname; }
const char *kvm_config_token(void) { return s_token; }
const char *kvm_config_static_ip(void) { return s_ip; }
const char *kvm_config_static_gw(void) { return s_gw; }
const char *kvm_config_static_mask(void) { return s_mask; }

// 只允许字母数字和 '-'，1..32 字符（mDNS label 上限 63，留点余量）
static bool valid_hostname(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 32) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

esp_err_t kvm_config_set_hostname(const char *name)
{
    ESP_RETURN_ON_FALSE(valid_hostname(name), ESP_ERR_INVALID_ARG, TAG,
                        "hostname: 1..32 chars, [a-z0-9-] only");
    ESP_RETURN_ON_ERROR(save_str(K_HOSTNAME, name), TAG, "save");
    snprintf(s_hostname, sizeof(s_hostname), "%s", name);
    ESP_LOGI(TAG, "hostname -> %s", s_hostname);
    return ESP_OK;
}

esp_err_t kvm_config_set_token(const char *token)
{
    ESP_RETURN_ON_FALSE(strlen(token) < sizeof(s_token), ESP_ERR_INVALID_ARG, TAG, "token too long");
    ESP_RETURN_ON_ERROR(save_str(K_TOKEN, token), TAG, "save");
    snprintf(s_token, sizeof(s_token), "%s", token);
    ESP_LOGI(TAG, "token %s", s_token[0] ? "set" : "cleared");
    return ESP_OK;
}

// MAC 文本 → 6 字节：冒号/连字符分隔（每段 2 个 hex）或裸 12 hex，大小写随意。
// 分隔必须整齐（全有 5 个或全无），且恰好 6 字节
bool kvm_config_parse_mac(const char *s, uint8_t out[6])
{
    uint8_t m[6];
    int n = 0, cnt = 0, v = 0, seps = 0;
    bool prev_sep = true;
    for (const char *p = s; *p; p++) {
        if (*p == ':' || *p == '-') {
            if (prev_sep || cnt != 0) {          // 连续/开头分隔符、半段就分
                return false;
            }
            seps++;
            prev_sep = true;
            continue;
        }
        int d;
        if (*p >= '0' && *p <= '9') {
            d = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            d = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            d = *p - 'A' + 10;
        } else {
            return false;
        }
        prev_sep = false;
        v = (v << 4) | d;
        if (++cnt == 2) {
            if (n >= 6) {
                return false;
            }
            m[n++] = (uint8_t)v;
            v = 0;
            cnt = 0;
        }
    }
    if (cnt != 0 || n != 6 || (seps != 0 && seps != 5)) {
        return false;
    }
    memcpy(out, m, sizeof(m));
    return true;
}

const char *kvm_config_wol_mac(void) { return s_wol; }

esp_err_t kvm_config_set_wol_mac(const char *mac)
{
    if (mac[0] == 0) {                       // 空串 = 清除
        ESP_RETURN_ON_ERROR(save_str(K_WOL, ""), TAG, "save");
        s_wol[0] = 0;
        ESP_LOGI(TAG, "wol_mac cleared");
        return ESP_OK;
    }
    uint8_t m[6];
    ESP_RETURN_ON_FALSE(kvm_config_parse_mac(mac, m), ESP_ERR_INVALID_ARG, TAG,
                        "bad mac '%s'", mac);
    char norm[18];
    snprintf(norm, sizeof(norm), "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    ESP_RETURN_ON_ERROR(save_str(K_WOL, norm), TAG, "save");
    snprintf(s_wol, sizeof(s_wol), "%s", norm);
    ESP_LOGI(TAG, "wol_mac -> %s", s_wol);
    return ESP_OK;
}
