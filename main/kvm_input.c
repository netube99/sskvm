// 上层输入事件 → HID 报表。所有网络入口（WS 会话任务 / HTTP /input /type）都灌到这里。
//
// 不在收包回调里直接 tud_hid_n_report()：主机 1ms 轮询一次，tud_hid_n_ready()
// 经常是 false，直接发会丢。这里只更新"最新状态"，2ms 任务在 ready 时下发。
// 绝对鼠标天然适合"只发最新"（相对鼠标要累加，这是选绝对的另一个理由）。
// 文本输入（/type 整串敲键）也走这里，见 TEXT_Q_LEN。
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "kvm_input.h"

// 文本输入（agent /type）：整串进环形队列，2ms 任务按 按下→保持→抬起→间隔 的节奏敲，
// 不用每个字符走一次网络往返。
#define TEXT_Q_LEN   256
#define TEXT_HOLD_US 5000
#define TEXT_GAP_US  3000       // 8ms/字符 ≈ 125 字符/秒

// kbd 报表: [0]=modifiers [1]=0 [2..7]=keycode[6]；mouse 报表: buttons,x,y,wheel
//
// 键盘用 8 深队列而不是"最新快照"：快照会在按下+抬起落在同一次 2ms 轮询时
// 吞掉一整次击键。真人按键至少几十 ms 碰不到，但自动化客户端（CDP/Playwright）
// 的瞬时点按一按一个准——实测 Enter 就这么丢过。鼠标仍然只存最新（绝对坐标天然
// 如此，相对量在发送端已经累加）。
#define KBD_Q_LEN   8
static uint8_t s_kbd_q[KBD_Q_LEN][8];
static volatile int s_kbd_head, s_kbd_tail;    // head=写，tail=读
static uint8_t s_kbd_last[8];                  // 最后一次入队的键盘报表（GET_REPORT 用）
static uint8_t s_mouse[6];
static bool s_mouse_dirty;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// 文本队列。注意：生产者不止一个（WS 会话任务 / HTTP /type 任务），消费者是 hid 任务，
// release_all 又会清队列 —— head/tail/state 的每一次读写都必须在 s_lock 里，
// 否则 release_all 与 hid 任务/生产者交错会把队列搞倒挂（幽灵打字 + 键盘锁死）。
typedef struct {
    uint8_t usage;
    uint8_t mods;       // 0 或 0x02(左 Shift)
} key_t;
static key_t s_text_q[TEXT_Q_LEN];
static volatile int s_text_head, s_text_tail;
static int s_text_state;            // 0=空闲 1=按住
static int64_t s_text_next_us;      // 只被 hid 任务读写，可以放锁外

// ASCII(US 布局) → HID keycode + Shift；0 表示不支持（非 ASCII 跳过）
static key_t map_char(char c)
{
    key_t k = { 0, 0 };
    if (c >= 'a' && c <= 'z') {
        k.usage = 0x04 + (c - 'a');
    } else if (c >= 'A' && c <= 'Z') {
        k.usage = 0x04 + (c - 'A');
        k.mods = 0x02;
    } else if (c >= '1' && c <= '9') {
        k.usage = 0x1E + (c - '1');
    } else {
        switch (c) {
        case '0': k.usage = 0x27; break;
        case '\n': case '\r': k.usage = 0x28; break;
        case '\t': k.usage = 0x2B; break;
        case '\b': k.usage = 0x2A; break;
        case 0x1B: k.usage = 0x29; break;
        case ' ': k.usage = 0x2C; break;
        case '-': k.usage = 0x2D; break;
        case '=': k.usage = 0x2E; break;
        case '[': k.usage = 0x2F; break;
        case ']': k.usage = 0x30; break;
        case '\\': k.usage = 0x31; break;
        case ';': k.usage = 0x33; break;
        case '\'': k.usage = 0x34; break;
        case '`': k.usage = 0x35; break;
        case ',': k.usage = 0x36; break;
        case '.': k.usage = 0x37; break;
        case '/': k.usage = 0x38; break;
        case '!': k.usage = 0x1E; k.mods = 0x02; break;
        case '@': k.usage = 0x1F; k.mods = 0x02; break;
        case '#': k.usage = 0x20; k.mods = 0x02; break;
        case '$': k.usage = 0x21; k.mods = 0x02; break;
        case '%': k.usage = 0x22; k.mods = 0x02; break;
        case '^': k.usage = 0x23; k.mods = 0x02; break;
        case '&': k.usage = 0x24; k.mods = 0x02; break;
        case '*': k.usage = 0x25; k.mods = 0x02; break;
        case '(': k.usage = 0x26; k.mods = 0x02; break;
        case ')': k.usage = 0x27; k.mods = 0x02; break;
        case '_': k.usage = 0x2D; k.mods = 0x02; break;
        case '+': k.usage = 0x2E; k.mods = 0x02; break;
        case '{': k.usage = 0x2F; k.mods = 0x02; break;
        case '}': k.usage = 0x30; k.mods = 0x02; break;
        case '|': k.usage = 0x31; k.mods = 0x02; break;
        case ':': k.usage = 0x33; k.mods = 0x02; break;
        case '"': k.usage = 0x34; k.mods = 0x02; break;
        case '~': k.usage = 0x35; k.mods = 0x02; break;
        case '<': k.usage = 0x36; k.mods = 0x02; break;
        case '>': k.usage = 0x37; k.mods = 0x02; break;
        case '?': k.usage = 0x38; k.mods = 0x02; break;
        default: break;
        }
    }
    return k;
}

int kvm_input_text(const char *s, int len)
{
    int n = 0;
    for (; n < len; n++) {
        key_t k = map_char(s[n]);
        if (k.usage == 0) {
            continue;                       // 不支持的字符跳过
        }
        portENTER_CRITICAL(&s_lock);
        int next = (s_text_head + 1) % TEXT_Q_LEN;
        if (next == s_text_tail) {
            portEXIT_CRITICAL(&s_lock);
            break;                          // 满：返回已吃下的字符数，调用者续传
        }
        s_text_q[s_text_head] = k;
        s_text_head = next;
        portEXIT_CRITICAL(&s_lock);
    }
    return n;
}

// 入队一条键盘报表（调用者已持 s_lock）。队列满就并进队尾（保留最新状态），
// 不丢头部——消费者每 2ms 就排空一次，满只可能在自动化连击时出现。
static void kbd_push(const uint8_t rep[8])
{
    int next = (s_kbd_head + 1) % KBD_Q_LEN;
    if (next == s_kbd_tail) {
        memcpy(s_kbd_q[(s_kbd_head + KBD_Q_LEN - 1) % KBD_Q_LEN], rep, 8);
        return;
    }
    memcpy(s_kbd_q[s_kbd_head], rep, 8);
    s_kbd_head = next;
}

void kvm_input_release_all(void)
{
    portENTER_CRITICAL(&s_lock);
    s_kbd_head = s_kbd_tail = 0;                // 丢掉排队中的旧状态
    s_text_head = s_text_tail = 0;              // 文本队列也清掉
    s_text_state = 0;
    static const uint8_t zero[8];
    kbd_push(zero);
    s_mouse[0] = 0;                             // buttons
    s_mouse[5] = 0;                             // wheel
    s_mouse_dirty = true;
    portEXIT_CRITICAL(&s_lock);
}

void kvm_input_handle(const uint8_t *d, int len)
{
    if (len < 1) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    switch (d[0]) {
    case 0x01:      // 键盘：全量状态，入队（见 kbd_push 的说明）
        if (len >= 8) {
            uint8_t rep[8] = { d[1], 0, d[2], d[3], d[4], d[5], d[6], d[7] };
            kbd_push(rep);
            memcpy(s_kbd_last, rep, sizeof(s_kbd_last));
        }
        break;
    case 0x02:      // 绝对鼠标：buttons + x/y(0..32767) + wheel
        if (len >= 7) {
            memcpy(s_mouse, d + 1, 6);
            s_mouse_dirty = true;
        }
        break;
    default:
        break;
    }
    portEXIT_CRITICAL(&s_lock);
}

// GET_REPORT（部分 BIOS/主机初始化时探测）：返回最后一次入队的报表快照
int kvm_input_get_report(uint8_t inst, uint8_t *out, size_t cap)
{
    int n = 0;
    portENTER_CRITICAL(&s_lock);
    if (inst == 0 && cap >= sizeof(s_kbd_last)) {
        memcpy(out, s_kbd_last, sizeof(s_kbd_last));
        n = (int)sizeof(s_kbd_last);
    } else if (inst == 1 && cap >= sizeof(s_mouse)) {
        memcpy(out, s_mouse, sizeof(s_mouse));
        n = (int)sizeof(s_mouse);
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

static void hid_task(void *arg)
{
    // 鼠标只存最新快照（绝对坐标）；键盘走 8 深队列，见 KBD_Q_LEN。
    while (1) {
        int64_t now = esp_timer_get_time();

        // ---- 文本状态机优先 ----
        // 按下/抬起与队列推进全部在锁内变更（生产者/release_all 会并发清队），
        // tud_* 发送在锁外。按下失败不丢字符（留在队头重试），抬起失败保持
        // 按住重发抬起 —— 键盘不能发出去就永远按下。
        bool press = false, release = false;
        key_t tk = { 0, 0 };
        portENTER_CRITICAL(&s_lock);
        if (s_text_state == 0) {
            if (s_text_tail != s_text_head && now >= s_text_next_us) {
                tk = s_text_q[s_text_tail];     // 只拷贝不出队：抬起成功才算敲完
                press = true;
                s_text_state = 1;
                s_text_next_us = now + TEXT_HOLD_US;
            }
        } else if (now >= s_text_next_us) {
            release = true;
            s_text_state = 0;
            s_text_next_us = now + TEXT_GAP_US;
        }
        portEXIT_CRITICAL(&s_lock);
        if (press) {
            uint8_t kc[6] = { tk.usage, 0, 0, 0, 0, 0 };
            bool sent = tud_hid_n_ready(0) && tud_hid_n_keyboard_report(0, 0, tk.mods, kc);
            if (!sent) {
                portENTER_CRITICAL(&s_lock);
                s_text_state = 0;
                s_text_next_us = now + 2000;    // 端点忙：稍后重试同一字符
                portEXIT_CRITICAL(&s_lock);
            }
        } else if (release) {
            bool sent = tud_hid_n_ready(0) && tud_hid_n_keyboard_report(0, 0, 0, NULL);
            if (sent) {
                portENTER_CRITICAL(&s_lock);
                if (s_text_tail != s_text_head) {   // release_all 清过就不再推进
                    s_text_tail = (s_text_tail + 1) % TEXT_Q_LEN;
                }
                portEXIT_CRITICAL(&s_lock);
            } else {
                portENTER_CRITICAL(&s_lock);
                s_text_state = 1;               // 松开没发出去：保持按住，下一轮重发抬起
                s_text_next_us = now + 2000;
                portEXIT_CRITICAL(&s_lock);
            }
        }

        // ---- 物理键鼠（文本忙时让路，避免两边抢键盘）----
        // 键盘"取件即出队"（拷贝+推进同一临界区）：release_all 之后压入的全零
        // 释放报文只会排在本条之后，永远不会被跳过 —— 旧行为（先发后推进）
        // 和 release_all 竞态时会把全零包跳过，断线后按键永久卡死。
        portENTER_CRITICAL(&s_lock);
        bool text_busy = (s_text_state != 0) || (s_text_tail != s_text_head);
        bool kd = s_kbd_head != s_kbd_tail && !text_busy;
        bool md = s_mouse_dirty;
        uint8_t kbd[8], mouse[6];
        if (kd) {
            memcpy(kbd, s_kbd_q[s_kbd_tail], sizeof(kbd));
            s_kbd_tail = (s_kbd_tail + 1) % KBD_Q_LEN;
        }
        memcpy(mouse, s_mouse, sizeof(mouse));
        portEXIT_CRITICAL(&s_lock);

        if (kd) {
            if (tud_hid_n_ready(0)) {
                // 发送失败就丢这一条：报表是全量快照，后续报文会覆盖最终状态
                tud_hid_n_keyboard_report(0, 0, kbd[0], &kbd[2]);
            }
        }
        if (md) {
            // 端点忙时不能当发过了：丢掉的是"松开"那一条，被控机就永远按着鼠标键。
            // 真发出去了才清 dirty，且清之前确认期间没有更新状态（否则会把新状态一起吞掉）。
            if (tud_hid_n_ready(1) && tud_hid_n_report(1, 0, mouse, sizeof(mouse))) {
                portENTER_CRITICAL(&s_lock);
                if (memcmp(s_mouse, mouse, sizeof(mouse)) == 0) {
                    s_mouse_dirty = false;
                }
                portEXIT_CRITICAL(&s_lock);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void kvm_input_start(void)
{
    // prio 11 / core0：低于 USB 任务（15/16）与 TinyUSB（12），高于编码（10）；
    // 运行时间极短（µs 级），不挤占视频管线，但不会被满载拖出 ~1ms 的输入延时
    if (xTaskCreatePinnedToCore(hid_task, "hid", 3072, NULL, 11, NULL, 0) != pdPASS) {
        ESP_LOGE("kvm_input", "hid task create failed");
    }
}
