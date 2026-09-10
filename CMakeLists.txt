# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sskvm VERSION 0.1.0)

# lwIP 默认不开 SO_SNDTIMEO（LWIP_SO_SNDTIMEO=0）→ setsockopt 静默失败，send() 对
# 已经消失的客户端会一直阻塞（TCP 重传要几分钟）。WS 会话任务一旦卡在 send 里，
# 就永远占着排他锁，浏览器再也连不上（实测 Chrome 被 kill 后必现）。
# 全局打开：send 超时返回 EWOULDBLOCK，mbedtls 返回 WANT_WRITE，我们随即关会话。
idf_build_set_property(COMPILE_DEFINITIONS "LWIP_SO_SNDTIMEO=1" APPEND)

# esp_h264 是 managed component（重新下载会覆盖源码），所以每次 configure 都补一次：
# 1080P 参考帧要 ~135KB 连续**内部** RAM，TLS 会话缓冲会把内部堆切碎，分不到就
# codec_alloc 失败 → 重启（实测：4 个 TLS 会话时必现）。补丁：内部 RAM 分不到就退到
# PSRAM（H264 DMA 能读 PSRAM，同一个函数里的 db 缓冲本来就是这么回退的）。
set(_h264_file "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_hw_param.c")
if(EXISTS "${_h264_file}")
    file(READ "${_h264_file}" _h264_src)
    if(NOT _h264_src MATCHES "sskvm ref fallback")
        string(REPLACE
            "    ESP_H264_GOTO_ON_FALSE(param->ref, ESP_H264_ERR_MEM, __exit__, TAG, \"No memory for reference frame\");"
            "    if (param->ref == NULL) { /* sskvm ref fallback */\n        param->ref = (uint8_t *)esp_h264_aligned_malloc(16, 1, max_refame_buffer_size(param->mb_width), &actual_size, ESP_H264_MEM_SPIRAM);\n    }\n    ESP_H264_GOTO_ON_FALSE(param->ref, ESP_H264_ERR_MEM, __exit__, TAG, \"No memory for reference frame\");"
            _h264_src "${_h264_src}")
        file(WRITE "${_h264_file}" "${_h264_src}")
        message(STATUS "esp_h264: applied sskvm ref-frame PSRAM fallback patch")
    endif()
endif()
