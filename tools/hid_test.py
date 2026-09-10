#!/usr/bin/env python3
"""HID 压测（容器内运行，宿主机 udev 已 authorized=0 隔离）。

以 libusb 直连 P4 复合设备，像被控机 OS 一样消费 HID 报表：
  - 键盘 EP 0x81（8B boot 报表）/ 鼠标 EP 0x82（6B 绝对坐标报表）
  - 校验：击键按下/抬起按运行段配对、结束时全零（不卡键）；鼠标按键运行段配对、
    结束时无按住；绝对坐标在 0..32767 全程合法且覆盖扫描区间；控制传输（LED）正常

用法（在宿主机上执行，事件全部进容器，宿主机桌面不受影响）：
  docker run --rm --privileged -v /dev/bus/usb:/dev/bus/usb -v "$PWD":/project \\
      espressif/idf:v6.0.3 bash -c "pip install -q pyusb && python /project/sskvm/tools/hid_test.py [秒]"

P4 侧配合：串口 console 执行 `hid <ms>`（如 `hid 10` ≈ 100 组/秒；`hid 2` = 洪泛）。
"""
import sys
import time

import usb.core
import usb.util

VID, PIDS = 0x303A, (0x400A, 0x4008)  # 400a=旧 sdkconfig 定制，4008=esp_tinyusb 默认
KBD_EP, MOUSE_EP = 0x81, 0x82
KBD_IF, MOUSE_IF = 0, 1
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
GRACE = 2.0  # 结束后等设备排空队列并归零的时间


def find_dev():
    for pid in PIDS:
        d = usb.core.find(idVendor=VID, idProduct=pid)
        if d is not None:
            return d
    return None


def main():
    dev = find_dev()
    if dev is None:
        sys.exit(f"FAIL: P4 composite (303a:{'/'.join(hex(p) for p in PIDS)}) not found")
    print(f"device: {hex(dev.idVendor)}:{hex(dev.idProduct)} bus={dev.bus} addr={dev.address}")

    # 安全闸：宿主机内核不得占用接口（udev 规则失效时这里必须炸，而不是把事件打回宿主机）
    for itf in (KBD_IF, MOUSE_IF):
        if dev.is_kernel_driver_active(itf):
            sys.exit(f"FAIL: kernel driver active on interface {itf} — "
                     f"host would receive the events! Fix /etc/udev/rules.d/90-sskvm-hid.rules")
    for attempt in range(3):
        try:
            dev.set_configuration()
            break
        except usb.core.USBError as e:
            if attempt == 2:
                raise
            print(f"set_configuration retry ({e})")
            time.sleep(1)
    usb.util.claim_interface(dev, KBD_IF)
    usb.util.claim_interface(dev, MOUSE_IF)

    # ---- 控制传输 ----
    # 热身：claim 后第一条控制传输可能撞上内核 usbhid 绑定/解绑窗口（udev 延迟解绑），
    # 先发一条已知稳定的 Get_Idle 把控制管道理顺。
    time.sleep(0.5)
    try:
        dev.ctrl_transfer(0xA1, 0x02, 0, KBD_IF, 1, timeout=3000)
    except usb.core.USBError:
        pass
    CTRL_TIMEOUT = 5000
    dev.ctrl_transfer(0x21, 0x0B, 0, KBD_IF, None, timeout=CTRL_TIMEOUT)   # Set_Protocol = boot
    proto = None
    for attempt in range(2):
        try:
            proto = dev.ctrl_transfer(0xA1, 0x0B, 0, KBD_IF, 1, timeout=CTRL_TIMEOUT)
            break
        except usb.core.USBError as e:
            print(f"Get_Protocol retry ({e})")
            time.sleep(1)
    if proto is not None:
        print(f"kbd protocol: {proto[0]} (0=boot)")
    else:
        print("Get_Protocol: 两次超时（不阻断，真实 OS 不读此项）")
    for led in (0x02, 0x00):                                 # Set_Report: CapsLock 亮/灭
        dev.ctrl_transfer(0x21, 0x09, 0x0200, KBD_IF, bytes([led]), timeout=CTRL_TIMEOUT)
        time.sleep(0.05)
    print("Set_Report LED on/off: ok (P4 串口应打出两行 kbd LED)")

    # ---- 报表消费循环 ----
    def read_loop(until, stats):
        while time.time() < until:
            for ep, size in ((KBD_EP, 8), (MOUSE_EP, 6)):
                try:
                    data = bytes(dev.read(ep, size, timeout=20))
                except usb.core.USBError:
                    continue
                if ep == KBD_EP:
                    st = stats["kbd"]
                    st["n"] += 1
                    st["last"] = data
                    down = any(data[2:8])
                    if down and not st["down"]:
                        st["press"] += 1
                    if st["down"] and not down:
                        st["release"] += 1
                    st["down"] = down
                else:
                    st = stats["mouse"]
                    st["n"] += 1
                    st["last"] = data
                    x = data[1] | (data[2] << 8)
                    y = data[3] | (data[4] << 8)
                    btn = data[0]
                    if btn & ~0x07:
                        st["btn_raw_bad"] = data.hex()
                    st["xmin"] = x if st["xmin"] is None else min(st["xmin"], x)
                    st["xmax"] = x if st["xmax"] is None else max(st["xmax"], x)
                    st["ymin"] = y if st["ymin"] is None else min(st["ymin"], y)
                    st["ymax"] = y if st["ymax"] is None else max(st["ymax"], y)
                    if btn & 1 and not st["down"]:
                        st["press"] += 1
                    if st["down"] and not btn & 1:
                        st["release"] += 1
                    st["down"] = bool(btn & 1)
                    st["wheels"].add(data[5] if data[5] < 128 else data[5] - 256)

    stats = {k: {"n": 0, "last": None, "down": False, "press": 0, "release": 0,
                 "xmin": None, "xmax": None, "ymin": None, "ymax": None,
                 "wheels": set(), "btn_raw_bad": None}
             for k in ("kbd", "mouse")}
    err = None
    try:
        read_loop(time.time() + DURATION, stats)
        # 宽限期：等 P4 把队列里剩余的按下/抬起排干净，最后状态必须归零
        read_loop(time.time() + GRACE, stats)
    except Exception as e:  # 设备掉线/复位
        err = repr(e)

    k, m = stats["kbd"], stats["mouse"]
    print(f"\n--- {DURATION:.0f}s(+{GRACE:.0f}s grace) 结果 ---")
    print(f"kbd: {k['n']} reports, press={k['press']} release={k['release']}, last={k['last'].hex() if k['last'] else '-'}")
    print(f"mouse: {m['n']} reports, x=[{m['xmin']},{m['xmax']}] y=[{m['ymin']},{m['ymax']}], "
          f"press={m['press']} release={m['release']}, wheels={sorted(m['wheels'])}")

    fails = []
    if err:
        fails.append(f"USB error: {err}")
    if k["n"] == 0 and m["n"] == 0:
        fails.append("一个报表都没收到（hid 压测没开？）")
    if k["n"]:
        if k["press"] != k["release"]:
            fails.append(f"键盘按下/抬起不配对: {k['press']}/{k['release']}")
        if any(k["last"][2:8]):
            fails.append("键盘结束时未归零（卡键）")
    if m["n"]:
        if m["press"] != m["release"]:
            fails.append(f"鼠标按下/抬起不配对: {m['press']}/{m['release']}")
        if m["down"]:
            fails.append("鼠标结束时仍按住（卡键）")
        if not (m["xmin"] == 0 and m["xmax"] > 20000 and m["ymin"] == 0 and m["ymax"] > 20000):
            fails.append("坐标扫描未覆盖 0..32767")
        if m["btn_raw_bad"]:
            fails.append("按钮字节异常: " + m["btn_raw_bad"])
        if 1 not in m["wheels"] or -1 not in m["wheels"]:
            fails.append("滚轮双向未出现")
    if fails:
        print("FAIL: " + "; ".join(fails))
        sys.exit(1)
    print(f"PASS: {k['n']} kbd + {m['n']} mouse reports, 配对完整、结束归零、坐标/滚轮/控制传输正常")


if __name__ == "__main__":
    main()
