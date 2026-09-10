// 设备配置：NVS 持久化，出厂默认从硬件/编译期推导。
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t kvm_config_init(void);

// 主机名（mDNS / DHCP option 12）。出厂默认 sskvm-<MAC 后 2 字节>，永不重名
const char *kvm_config_hostname(void);
esp_err_t kvm_config_set_hostname(const char *name);   // 非法/超长返回错误

// 静态 IP（空串 = 用 DHCP）
const char *kvm_config_static_ip(void);
const char *kvm_config_static_gw(void);
const char *kvm_config_static_mask(void);

// 分辨率索引（0=480P 1=720P 2=1080P，出厂默认由 Kconfig 决定）
uint8_t kvm_config_res(void);
esp_err_t kvm_config_set_res(uint8_t idx);

// 访问 token（空串 = 不鉴权）；出厂默认取 CONFIG_SSKVM_AUTH_TOKEN
const char *kvm_config_token(void);
esp_err_t kvm_config_set_token(const char *token);
