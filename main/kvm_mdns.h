// mDNS：<hostname>.local + _sskvm._tcp 服务发现
#pragma once
#include "esp_err.h"

esp_err_t kvm_mdns_start(void);

// 改名后重新宣告（kvm_config_set_hostname 之后调用）
void kvm_mdns_set_hostname(const char *name);
