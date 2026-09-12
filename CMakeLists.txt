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

# esp_h264 是 managed component（重新下载会覆盖源码），所以每次 configure 都补一次。
# 两层补丁，都针对"1080P 参考帧要 ~135KB 连续内部 RAM"：
# ① PSRAM 兜底：TLS 会话缓冲把内部堆切碎、ref 分不到时不再 codec_alloc 失败重启，
#    而是退到 PSRAM（H264 DMA 能读 PSRAM，同函数 db 缓冲本来就是这么回退的）。
# ② ref/db 块跨 del/new 缓存复用（sskvm_cached_take，不真正 free）：第一次 open
#    （开机、堆最干净时）就按 1080P 上限整块预留，之后任何分辨率复用同一块、
#    永不释放，显存不可能被零散小块（TLS 会话 ~30KB）切碎；开机小分辨率之后
#    带观众切上 1080P 的路径也随之消失。
set(_h264_file "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_hw_param.c")
if(EXISTS "${_h264_file}")
    file(READ "${_h264_file}" _h264_src)
    if(NOT _h264_src MATCHES "sskvm ref")
        string(REPLACE
            "    ESP_H264_GOTO_ON_FALSE(param->ref, ESP_H264_ERR_MEM, __exit__, TAG, \"No memory for reference frame\");"
            "    if (param->ref == NULL) { /* sskvm ref fallback */\n        param->ref = (uint8_t *)esp_h264_aligned_malloc(16, 1, max_refame_buffer_size(param->mb_width), &actual_size, ESP_H264_MEM_SPIRAM);\n    }\n    ESP_H264_GOTO_ON_FALSE(param->ref, ESP_H264_ERR_MEM, __exit__, TAG, \"No memory for reference frame\");"
            _h264_src "${_h264_src}")
        file(WRITE "${_h264_file}" "${_h264_src}")
        message(STATUS "esp_h264: applied sskvm ref-frame PSRAM fallback patch")
    endif()
    file(READ "${_h264_file}" _h264_src)
    if(NOT _h264_src MATCHES "sskvm ref cache")
        set(_h264_helpers [=[
/* sskvm ref cache: 内部 RAM 的 ref/db 块跨 del/new 缓存复用（不真正 free）。
   第一次 open（开机、堆最干净时）就按 1080P 上限整块预留"显存"：
   SSKVM_H264_MB_W/H_MAX = 1920/16、1088/16。之后任何分辨率都复用同一块，
   永远不再有第二次分配 —— TLS 会话等零散小块只碰剩余堆，显存不可能被切碎。
   （背景：有观众时切分辨率，TLS 块（~30KB）会落进已释放的编码器区域中段，
   切回 1080P 时最大连续块 ~132KB < 135KB，ref 静默退 PSRAM，
   P 帧 enc 44.5 -> 76ms、22 -> 13fps，实测。） */
#define SSKVM_H264_MB_W_MAX 120     /* 1920 / 16 */
#define SSKVM_H264_MB_H_MAX 68     /* 1088 / 16 */
typedef struct {
    uint8_t *p;
    uint32_t size;
    bool busy;
} sskvm_h264_cached_buf_t;
static sskvm_h264_cached_buf_t sskvm_ref_cache;
static sskvm_h264_cached_buf_t sskvm_db_cache;

static uint8_t *sskvm_cached_take(sskvm_h264_cached_buf_t *c, uint32_t need,
                                  uint32_t reserve, uint32_t *actual_size)
{
    if (c->p && !c->busy && c->size >= need) {
        c->busy = true;
        *actual_size = c->size;
        memset(c->p, 0, need);      /* 保持上游 open 拿到清零缓冲的语义 */
        return c->p;
    }
    uint8_t *p = (uint8_t *)esp_h264_aligned_malloc(16, 1, need > reserve ? need : reserve,
                                                    actual_size, ESP_H264_MEM_INTERNAL);
    if (p) {
        if (c->p) {
            esp_h264_free(c->p);    /* 小分辨率换大分辨率：旧的小块还给堆 */
        }
        c->p = p;
        c->size = *actual_size;
        c->busy = true;
    }
    return p;
}

static int max_refame_buffer_size(int16_t mb_width)]=])
        string(REPLACE "static int max_refame_buffer_size(int16_t mb_width)" "${_h264_helpers}" _h264_src "${_h264_src}")
        string(REPLACE [=[
    param->ref = (uint8_t *)esp_h264_aligned_malloc(16, 1, max_refame_buffer_size(param->mb_width), &actual_size, ESP_H264_MEM_INTERNAL);
    if (param->ref == NULL) { /* sskvm ref fallback */
        param->ref = (uint8_t *)esp_h264_aligned_malloc(16, 1, max_refame_buffer_size(param->mb_width), &actual_size, ESP_H264_MEM_SPIRAM);
    }]=]
            [=[
    param->ref = sskvm_cached_take(&sskvm_ref_cache, max_refame_buffer_size(param->mb_width),
                                   max_refame_buffer_size(SSKVM_H264_MB_W_MAX), &actual_size); /* sskvm ref cache */
    if (param->ref == NULL) {
        param->ref = (uint8_t *)esp_h264_aligned_malloc(16, 1, max_refame_buffer_size(param->mb_width), &actual_size, ESP_H264_MEM_SPIRAM);
    }]=]
            _h264_src "${_h264_src}")
        string(REPLACE
            [=[    param->db = (uint8_t *)esp_h264_malloc_prefer(1, max_db_buffer_size(param->mb_width, param->mb_height), &actual_size, ESP_H264_MEM_INTERNAL, ESP_H264_MEM_SPIRAM);]=]
            [=[    param->db = sskvm_cached_take(&sskvm_db_cache, max_db_buffer_size(param->mb_width, param->mb_height),
                                  max_db_buffer_size(SSKVM_H264_MB_W_MAX, SSKVM_H264_MB_H_MAX), &actual_size); /* sskvm ref cache */
    if (param->db == NULL) {
        param->db = (uint8_t *)esp_h264_malloc_prefer(1, max_db_buffer_size(param->mb_width, param->mb_height), &actual_size, ESP_H264_MEM_SPIRAM, ESP_H264_MEM_SPIRAM);
    }]=]
            _h264_src "${_h264_src}")
        string(REPLACE [=[
        if (param->ref) {
            esp_h264_free(param->ref);
        }
        if (param->db) {
            esp_h264_free(param->db);
        }]=]
            [=[
        if (param->ref) { /* sskvm ref cache: 块留给下一次 open 复用 */
            if (param->ref != sskvm_ref_cache.p) {
                esp_h264_free(param->ref);
            }
            sskvm_ref_cache.busy = false;
        }
        if (param->db) {
            if (param->db != sskvm_db_cache.p) {
                esp_h264_free(param->db);
            }
            sskvm_db_cache.busy = false;
        }]=]
            _h264_src "${_h264_src}")
        file(WRITE "${_h264_file}" "${_h264_src}")
        message(STATUS "esp_h264: applied sskvm ref/db sticky-cache patch")
    endif()
    # string(REPLACE) 在上游源码变化时会静默 no-op：没打上 = 1080P 掉回 13fps 的坑
    # 重新挖开，所以 configure 时硬性断言三处补丁全在，缺任何一处直接 FAIL
    file(READ "${_h264_file}" _h264_verify)
    string(FIND "${_h264_verify}" "sskvm_cached_take(&sskvm_ref_cache" _h264_p1)
    string(FIND "${_h264_verify}" "sskvm_cached_take(&sskvm_db_cache" _h264_p2)
    string(FIND "${_h264_verify}" "块留给下一次 open 复用" _h264_p3)
    if(_h264_p1 EQUAL -1 OR _h264_p2 EQUAL -1 OR _h264_p3 EQUAL -1)
        message(FATAL_ERROR "esp_h264 sskvm 补丁没打全（上游源码变了？）—— 先核对 REPLACE 目标串再升级组件")
    endif()
endif()
