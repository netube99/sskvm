// KVM 输入：上层事件 → USB HID 报表
// 绝对鼠标（instance 1）+ boot 键盘（instance 0），状态由 2ms 任务按需下发。
#pragma once
#include <stdint.h>

// 二进制事件（未来网络层的上行输入协议，小端）
//   0x01 KBD    [modifiers][keycode[6]]                8B
//   0x02 MOUSE  [buttons][x u16][y u16][wheel i8]      7B
void kvm_input_handle(const uint8_t *data, int len);

// agent 文本输入：ASCII/美式布局拆成按键序列，返回吃下的字符数（队列满就少一些）
int kvm_input_text(const char *s, int len);

// 全部松开：键盘全松、鼠标按键/滚轮归零（坐标不动——归零会把指针拽到左上角）。
// 上层断线时必须调用，否则浏览器断网/关标签时那个全零包发不出去，被控机一直卡着键。
void kvm_input_release_all(void);

void kvm_input_start(void);
