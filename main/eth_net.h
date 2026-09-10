// 以太网（IP101G RMII）+ DHCP/静态 IP + SNTP
#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t eth_net_start(void);

// 等待拿到 IP（静态 IP 时立即返回）
esp_err_t eth_net_wait_ip(TickType_t timeout, char *ip4, size_t ip4_len);

const char *eth_net_ip(void);
const char *eth_net_mac(void);      // "xx:xx:xx:xx:xx:xx"，link up 后才有值

// 启动 SNTP（不阻塞，同步完成后打日志）
void eth_net_sntp_start(void);
