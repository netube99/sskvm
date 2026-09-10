#include <string.h>
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_netif_sntp.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "kvm_config.h"
#include "eth_net.h"

static const char *TAG = "eth";
static EventGroupHandle_t s_eg;
static esp_netif_t *s_netif;
static char s_ip[16] = "0.0.0.0";
static char s_mac[18];
#define GOT_IP_BIT BIT0

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_eth_handle_t eth = *(esp_eth_handle_t *)data;
    switch (id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac[6];
        esp_eth_ioctl(eth, ETH_CMD_G_MAC_ADDR, mac);
        snprintf(s_mac, sizeof(s_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        esp_eth_ioctl(eth, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
        ESP_LOGI(TAG, "link up, MAC %s, %s %s-duplex", s_mac,
                 speed == ETH_SPEED_100M ? "100M" : "10M",
                 duplex == ETH_DUPLEX_FULL ? "full" : "half");
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link down");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const esp_netif_ip_info_t *ip = &((ip_event_got_ip_t *)data)->ip_info;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip->ip));
    ESP_LOGI(TAG, "%s got IP %s, gw " IPSTR, kvm_config_hostname(), s_ip, IP2STR(&ip->gw));
    xEventGroupSetBits(s_eg, GOT_IP_BIT);
}

esp_err_t eth_net_start(void)
{
    s_eg = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_eg, ESP_ERR_NO_MEM, TAG, "no mem");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // MAC: P4 默认 RMII 配置（含全部数据引脚 + CLK 输入 GPIO50）
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;            // 原理图 AD0 上拉 5.1K / AD3 下拉 5.1K
    phy_cfg.reset_gpio_num = 51;     // PHY RESET 接 GPIO51
    // IDF v6 把 ip101 驱动移到了 managed component，标准寄存器操作 generic 足够
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_ERR_NO_MEM, TAG, "mac/phy alloc");

    esp_eth_handle_t eth = NULL;
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    // DHCP option 12：路由器客户端列表里显示 sskvm-xxxx（比 mDNS 通用）
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_netif, kvm_config_hostname()));

    // 配了静态 IP 就不用 DHCP
    if (kvm_config_static_ip()[0]) {
        esp_netif_ip_info_t info = { 0 };
        esp_netif_dhcpc_stop(s_netif);      // 可能本来就没启动，忽略返回值
        info.ip.addr = inet_addr(kvm_config_static_ip());
        info.gw.addr = inet_addr(kvm_config_static_gw()[0] ? kvm_config_static_gw()
                                                           : kvm_config_static_ip());
        info.netmask.addr = inet_addr(kvm_config_static_mask()[0] ? kvm_config_static_mask()
                                                                  : "255.255.255.0");
        ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &info));
        snprintf(s_ip, sizeof(s_ip), "%s", kvm_config_static_ip());
        ESP_LOGI(TAG, "static IP %s", s_ip);
    }

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth);
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, glue));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler, NULL));
    ESP_RETURN_ON_ERROR(esp_eth_start(eth), TAG, "eth_start");

    if (s_ip[0] != '0') {                   // 静态 IP：直接算已就绪
        xEventGroupSetBits(s_eg, GOT_IP_BIT);
    }
    return ESP_OK;
}

esp_err_t eth_net_wait_ip(TickType_t timeout, char *ip4, size_t ip4_len)
{
    EventBits_t bits = xEventGroupWaitBits(s_eg, GOT_IP_BIT, pdFALSE, pdTRUE, timeout);
    if (!(bits & GOT_IP_BIT)) {
        ESP_LOGW(TAG, "no IP yet: 主机名 %s / MAC %s —— 去路由器客户端列表找它",
                 kvm_config_hostname(), s_mac[0] ? s_mac : "?");
        return ESP_ERR_TIMEOUT;
    }
    esp_netif_ip_info_t info;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_netif, &info), TAG, "get ip info");
    if (ip4 && ip4_len) {
        snprintf(ip4, ip4_len, IPSTR, IP2STR(&info.ip));
    }
    return ESP_OK;
}

const char *eth_net_ip(void) { return s_ip; }
const char *eth_net_mac(void) { return s_mac; }

void eth_net_sntp_start(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_SSKVM_SNTP_SERVER);
    cfg.start = true;
    cfg.smooth_sync = false;
    esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "SNTP started (%s)", CONFIG_SSKVM_SNTP_SERVER);
}
