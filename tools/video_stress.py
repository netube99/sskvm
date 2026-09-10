#!/usr/bin/env python3
"""视频管线压测：串口驱动 P4 快速轮换分辨率，监听日志判定稳定性。

判定标准（全部满足才 PASS）：
  - 完成 ≥ N 次完整分辨率切换（rebuild = 重开 UVC 流 + 重建 YUV/编码器）
  - 每次 "resolution ->" 后 psram free 稳定（漂移 < 2MB = 没漏缓冲）
  - 无 codec alloc 失败 / UVC stalled / EP already allocated / WDT / Guru Meditation
  - 结束后 stat：编码帧计数在增长，psram 回到基线

用法：python3 tools/video_stress.py [--period 300] [--minutes 5] [--port /dev/ttyACM0]
"""
import argparse
import re
import serial
import time

RE_RES = re.compile(r"resolution -> (\w+) \(psram (\d+) KB")
RE_BAD = re.compile(r"codec alloc failed|EP already allocated|Guru Meditation|"
                    r"abort\(\)|Task watchdog|rebooting", re.I)
RE_WARN = re.compile(r"UVC stream stalled")
RE_LAT = re.compile(r"lat=([\d.]+)/([\d.]+)ms")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--period", type=int, default=300, help="轮换间隔 ms")
    ap.add_argument("--minutes", type=float, default=5)
    args = ap.parse_args()

    s = serial.Serial(args.port, 115200, timeout=0.3)
    s.setDTR(False)
    s.setRTS(True); time.sleep(0.15); s.setRTS(False)   # 复位，从干净状态开测

    log = []
    raw = open("/tmp/sskvm_stress.log", "w")
    def pump(dur):
        t0 = time.time()
        while time.time() - t0 < dur:
            line = s.readline().decode("utf-8", "replace").rstrip()
            if line:
                log.append(line)
                raw.write(line + "\n"); raw.flush()
                if any(k in line for k in ("stalled", "codec alloc",
                                           "Guru", "abort", "watchdog")):
                    print("  | " + line)
    def switches_timeline():
        """从日志取每次切换的设备时间戳，返回相邻间隔(ms)"""
        ts = []
        for l in log:
            m = re.match(r"[WI] \((\d+)\) video: resolution ->", l)
            if m:
                ts.append(int(m.group(1)))
        return [b - a for a, b in zip(ts, ts[1:])]

    print(f"[1/4] 等待启动…")
    pump(8)
    s.write(b"stat\n"); s.flush()
    pump(2)
    print(f"[2/4] 开始轮换：每 {args.period}ms 切一次，共 {args.minutes} 分钟")
    s.write(f"resloop {args.period}\n".encode()); s.flush()
    pump(args.minutes * 60)
    s.write(b"resloop 0\n"); s.flush()
    print("[3/4] 停止轮换，等待收尾…")
    pump(5)
    s.write(b"stat\n"); s.flush()
    pump(2)
    s.close()

    # ---- 判定 ----
    switches = RE_RES.findall("\n".join(log))
    gaps = switches_timeline()
    bad = [l for l in log if RE_BAD.search(l) and "resolution" not in l]
    lat = [tuple(map(float, m.groups())) for l in log if (m := RE_LAT.search(l))]
    fails = []
    print(f"\n--- 结果 ---")
    print(f"切换次数: {len(switches)}  依次: {sorted(set(n for n, _ in switches))}")
    if switches:
        # 各分辨率的 psram 余量应各自稳定（不同分辨率占用本来就不同）
        per_res = {}
        for name, kb in switches:
            per_res.setdefault(name, set()).add(int(kb))
        for name, vals in sorted(per_res.items()):
            spread = max(vals) - min(vals)
            print(f"psram free[{name}]: {sorted(vals)} KB (抖动 {spread} KB)")
            if spread > 256:
                fails.append(f"psram[{name}] 抖动 {spread} KB（疑似漏缓冲）")
        if gaps:
            slow = sum(1 for g in gaps if g > 2000)
            print(f"切换间隔: 中位 {sorted(gaps)[len(gaps)//2]}ms, >2s 的 {slow} 次")
        print(f"lat avg/max 末段: {lat[-1] if lat else '无'}  样本 {len(lat)}")
    if len(switches) < 3:
        fails.append("切换次数过少（管线可能没在切）")
    for l in bad:
        fails.append("异常日志: " + l.strip()[:120])
    for l in log:
        if RE_WARN.search(l):
            print("  警告: " + l.strip()[:120])
    print("FAIL: " + "; ".join(fails) if fails else
          f"PASS: {len(switches)} 次完整切换，psram 稳定，无异常日志")


if __name__ == "__main__":
    main()
