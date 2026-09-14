idf_component_register(SRCS "app_main.c"
                            "console.c"
                            "usb_device.c" "usb_hid.c"
                            "uvc_capture.c" "video_pipeline.c"
                            "kvm_input.c"
                            "net_server.c" "kvm_api.c" "ota.c"
                            "eth_net.c" "kvm_config.c" "kvm_mdns.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES usb_host_uvc esp_hal_usb esp_timer esp_driver_jpeg esp_driver_uart
                                     esp_eth esp_netif esp_event mbedtls esp-tls mdns
                                     nvs_flash esp_app_format app_update)

# 自签证书：不提交进仓库，首次 configure 时用 openssl 生成到 main/certs/（已 gitignore）。
# 想要浏览器不报警告跑 tools/gen_cert.sh <ip>（本地 CA 签，SAN 带 IP）。
set(CERT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/certs)
if(NOT EXISTS ${CERT_DIR}/servercert.pem OR NOT EXISTS ${CERT_DIR}/prvtkey.pem)
    file(MAKE_DIRECTORY ${CERT_DIR})
    message(STATUS "generating self-signed cert in ${CERT_DIR} (tools/gen_cert.sh <ip> for a browser-trusted one)")
    execute_process(COMMAND openssl ecparam -name prime256v1 -genkey -noout -out ${CERT_DIR}/prvtkey.pem)
    execute_process(COMMAND openssl req -x509 -new -key ${CERT_DIR}/prvtkey.pem -out ${CERT_DIR}/servercert.pem
                    -days 3650 -subj /CN=sskvm)
endif()

# 前端页面 + 证书打进固件
target_add_binary_data(${COMPONENT_LIB} "index.html" TEXT)
target_add_binary_data(${COMPONENT_LIB} "certs/servercert.pem" TEXT)
target_add_binary_data(${COMPONENT_LIB} "certs/prvtkey.pem" TEXT)
