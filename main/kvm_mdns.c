// mDNS 宣告。
//
// 主机名默认 sskvm-<MAC 后 2 字节>（见 kvm_config），零配置且不会和别的 KVM 撞名；
// 同时广播 _sskvm._tcp，TXT 里带 name/mac/ver/res，agent 用 zeroconf 就能列出局域网里所有 KVM。
//
// 注意 mDNS 不是万能入口：Windows/Android 浏览器对 .local 支持不稳，
// 所以还配合了 DHCP option 12（见 eth_net）和网页上显示的 IP。
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "mdns.h"
#include "eth_net.h"
#include "kvm_config.h"
#include "kvm_mdns.h"
#include "video_pipeline.h"

static const char *TAG = "mdns";
static char s_res[16];

esp_err_t kvm_mdns_start(void)
{
    uint16_t w = 0, h = 0;
    video_pipeline_res_dims(kvm_config_res(), &w, &h);
    snprintf(s_res, sizeof(s_res), "%ux%u", w, h);

    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(kvm_config_hostname()), TAG, "hostname_set");
    mdns_instance_name_set(kvm_config_hostname());

    mdns_txt_item_t txt[] = {
        { "name", kvm_config_hostname() },
        { "mac",  eth_net_mac() },
        { "ver",  esp_app_get_description()->version },
        { "res",  s_res },
    };
    ESP_RETURN_ON_ERROR(mdns_service_add(NULL, "_sskvm", "_tcp", 443, txt, 4), TAG, "service_add");
    ESP_LOGI(TAG, "%s.local + _sskvm._tcp advertised", kvm_config_hostname());
    return ESP_OK;
}

void kvm_mdns_set_hostname(const char *name)
{
    mdns_hostname_set(name);
    mdns_instance_name_set(name);
    mdns_service_txt_item_set("_sskvm", "_tcp", "name", name);
}
