#!/usr/bin/env python3
"""sskvm agent 客户端（只用标准库）。

    from kvm import Kvm
    k = Kvm("192.168.1.50", token="")
    open("shot.jpg", "wb").write(k.screenshot())
    k.click(12000, 8000)
    k.type("hello world")

命令行：
    ./kvm.py <host> shot [out.jpg]
    ./kvm.py <host> click <x> <y> [button]     # button: 1=左 2=右 4=中
    ./kvm.py <host> move <x> <y>
    ./kvm.py <host> wheel <n>                  # 正=向上滚（HID/evdev 惯例）
    ./kvm.py <host> type <text>
    ./kvm.py <host> key <hid_usage>            # 0x28=Enter, 0x04='a'
    ./kvm.py <host> wol [mac]                  # 网络唤醒（mac 缺省用设备记住的）

token 也可以放环境变量 SSKVM_TOKEN。
"""
import http.client
import os
import ssl
import struct
import sys
import time

__all__ = ["Kvm"]


class Kvm:
    """一台被控机的输入/截图接口。坐标 0..32767 映射整屏。"""

    def __init__(self, host, port=443, token="", timeout=5):
        self.host = host
        self.port = port
        self.token = token or os.environ.get("SSKVM_TOKEN", "")
        self.timeout = timeout
        self._ctx = ssl.create_default_context()
        self._ctx.check_hostname = False
        self._ctx.verify_mode = ssl.CERT_NONE      # 自签证书

    # ---- 底层 ----
    def _q(self, path):
        return path + (f"?token={self.token}" if self.token else "")

    def _conn(self):
        return http.client.HTTPSConnection(self.host, self.port, context=self._ctx,
                                           timeout=self.timeout)

    def _get(self, path):
        c = self._conn()
        c.request("GET", self._q(path))
        r = c.getresponse()
        data = r.read()
        c.close()
        if r.status != 200:
            raise RuntimeError(f"GET {path}: HTTP {r.status} {data[:80]!r}")
        return data

    def _post(self, path, body, ctype="application/octet-stream"):
        c = self._conn()
        c.request("POST", self._q(path), body=body, headers={"Content-Type": ctype})
        r = c.getresponse()
        out = r.read()
        c.close()
        if r.status != 200:
            raise RuntimeError(f"POST {path}: HTTP {r.status} {out[:80]!r}")
        return out

    def _event(self, ev):
        self._post("/input", ev)

    # ---- 公开 API ----
    def screenshot(self, path=None):
        """取采集卡原始 JPEG。给了 path 就顺便存盘。"""
        data = self._get("/snapshot.jpg")
        if path:
            with open(path, "wb") as f:
                f.write(data)
        return data

    def move(self, x, y):
        self._event(struct.pack("<BBHHb", 0x02, 0, int(x), int(y), 0))

    def click(self, x, y, button=1, hold=0.05):
        self._event(struct.pack("<BBHHb", 0x02, button, int(x), int(y), 0))
        time.sleep(hold)
        self._event(struct.pack("<BBHHb", 0x02, 0, int(x), int(y), 0))

    def wheel(self, n):
        n = max(-127, min(127, int(n)))
        self._event(struct.pack("<BBHHb", 0x02, 0, 0, 0, n))

    def key(self, usage, shift=False, hold=0.03):
        """单个 HID usage（0x04='a' … 0x28=Enter）。"""
        self._event(bytes([0x01, 0x02 if shift else 0, usage, 0, 0, 0, 0, 0]))
        time.sleep(hold)
        self._event(bytes([0x01, 0, 0, 0, 0, 0, 0, 0]))

    def wol(self, mac=""):
        """网络唤醒：设备对目标 MAC 发 UDP 广播。mac 空 = 用设备记住的那个。"""
        return self._post("/wol", mac.encode(), "text/plain").decode()

    def type(self, text):
        """整串丢给 P4（8ms/字符）。P4 队列满会只收下前面一部分，这里续传剩下的。"""
        data = text.encode()
        off, deadline, r = 0, time.time() + 10, "typed 0"
        while off < len(data):
            r = self._post("/type", data[off:], "text/plain; charset=utf-8").decode()
            n = int(r.rsplit(" ", 1)[-1]) if r.startswith("typed ") else 0
            if n == 0:
                if time.time() > deadline:
                    raise RuntimeError(f"type timeout at {off}/{len(data)} chars")
                time.sleep(0.02)
                continue
            off += n
        return r


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    host, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    k = Kvm(host)
    if cmd == "shot":
        data = k.screenshot(args[0] if args else None)
        print(f"{len(data)} bytes" + (f" -> {args[0]}" if args else ""))
    elif cmd == "click":
        k.click(int(args[0]), int(args[1]), int(args[2]) if len(args) > 2 else 1)
        print(f"clicked ({args[0]},{args[1]})")
    elif cmd == "move":
        k.move(int(args[0]), int(args[1]))
    elif cmd == "wheel":
        k.wheel(int(args[0]))
    elif cmd == "type":
        print(k.type(" ".join(args)))
    elif cmd == "key":
        k.key(int(args[0], 0))
    elif cmd == "wol":
        print(k.wol(args[0] if args else ""))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
