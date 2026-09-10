#!/usr/bin/env python3
"""无头 WebSocket 客户端：验收传输层（不依赖第三方库，标准库手搓 WS 帧）。

  python tools/ws_client.py <ip> [seconds] [--token T] [--busy-check]

输出 fps / 码率 / 端到端延时 / 关键帧 / pong，并在结束时给 PASS/FAIL：
  PASS = 全程收到视频帧、连接没断、延时 p95 < 300ms（单帧 IDR 尖峰不算）
"""
import base64
import hashlib
import os
import socket
import ssl
import struct
import sys
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WS:
    def __init__(self, ip, token=None, timeout=5):
        self.sock = socket.create_connection((ip, 443), timeout=timeout)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        self.s = ctx.wrap_socket(self.sock, server_hostname=ip)
        self.buf = b""
        key = base64.b64encode(os.urandom(16)).decode()
        path = "/ws" + ("?token=" + token if token else "")
        req = (f"GET {path} HTTP/1.1\r\nHost: {ip}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n")
        self.s.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            d = self.s.recv(4096)
            if not d:
                raise RuntimeError("closed during handshake")
            head += d
        if b"101" not in head.split(b"\r\n")[0]:
            raise RuntimeError("handshake failed: " + head.split(b"\r\n")[0].decode())
        want = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        if want.encode() not in head:
            raise RuntimeError("bad Sec-WebSocket-Accept")
        # 101 之后可能已经跟着状态文本/首帧，不能丢（否则帧边界全错位）
        self.buf = head[head.index(b"\r\n\r\n") + 4:]

    def send(self, payload, opcode=0x2):
        mask = os.urandom(4)
        n = len(payload)
        if n <= 125:
            hdr = bytes([0x80 | opcode, 0x80 | n])
        elif n <= 0xFFFF:
            hdr = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr = bytes([0x80 | opcode, 0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.s.sendall(hdr + mask + masked)

    def _recv_exact(self, n, timeout):
        self.s.settimeout(timeout)
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise ConnectionError("closed")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self, timeout=5):
        """返回 (opcode, payload)；超时返回 (None, b'')。"""
        try:
            b = self._recv_exact(2, timeout)
        except socket.timeout:
            return None, b""
        op, ln = b[0] & 0x0F, b[1] & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", self._recv_exact(2, timeout))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", self._recv_exact(8, timeout))[0]
        return op, self._recv_exact(ln, timeout)

    def close(self):
        try:
            self.send(b"", 0x8)
        except OSError:
            pass
        try:
            self.s.close()
        except OSError:
            pass


def busy_check(ip, token):
    """占用中应该拒绝第二个 WS：期望握手阶段直接失败（HTTP 409）。"""
    try:
        w = WS(ip, token)
        w.close()
        return False, "second WS was accepted"
    except Exception as e:
        return True, str(e)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ip = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
    token = None
    if "--token" in sys.argv:
        token = sys.argv[sys.argv.index("--token") + 1]

    w = WS(ip, token)
    t0 = time.time()
    frames = keys = 0
    lats = []
    win = []
    last_rx = time.time()
    last_report = t0
    last_frames = 0
    pongs = 0
    next_ping = t0 + 5
    first = None
    err = None
    # 排他检查：在第一个连接还活着的时候开第二个
    busy = None
    if "--busy-check" in sys.argv:
        busy, why = busy_check(ip, token)
        print(("PASS" if busy else "FAIL") + f" busy-check: {why}", flush=True)
    try:
        while time.time() - t0 < secs:
            if time.time() >= next_ping:
                w.send(b"\x05")
                next_ping += 5
            op, pl = w.recv(timeout=3)
            if op is None:
                if time.time() - last_rx > 12:
                    raise ConnectionError("no data for 12s")
                continue
            last_rx = time.time()
            if op == 0x1:                                   # 文本：状态 / pong
                if b"pong" in pl:
                    pongs += 1
                continue
            if op in (0x8, 0x9, 0xA):
                continue
            if len(pl) < 9:
                continue
            pts, ww, hh = struct.unpack_from("<IHH", pl, 0)
            # 第一个 NAL 类型：跳过 00 00 [00] 01 起始码
            nal = 0
            for i in range(8, min(len(pl), 48)):
                if pl[i] == 1 and pl[i - 1] == 0 and pl[i - 2] == 0:
                    nal = pl[i + 1] & 0x1F
                    break
            key = nal in (5, 7)
            frames += 1
            keys += key
            if first is None:
                first = (ww, hh)
            now = time.time() * 1000
            off = now - pts
            # 滚动窗口最小值：主机 NTP 步进/漂移不会永久污染基线
            win.append(off)
            if len(win) > 300:
                win.pop(0)
            lats.append(off - min(win))
            if now - last_report >= 2000:
                dt = (now - last_report) / 1000
                print(f"  {frames} frames, {ww}x{hh}, {(frames - last_frames) / dt:5.1f} fps, "
                      f"lat {sorted(lats)[len(lats) // 2]:.0f}ms, keys {keys}, pongs {pongs}")
                last_report = now
                last_frames = frames
    except Exception as e:
        err = e
    w.close()

    dur = time.time() - t0
    if lats:
        p95 = sorted(lats)[int(len(lats) * 0.95)]
    else:
        p95 = 9999
    print(f"frames={frames} keys={keys} fps={frames / dur:.1f} p95={p95:.0f}ms pongs={pongs} res={first}")
    ok = err is None and frames > 0 and p95 < 300 and (busy is not False)
    print(("PASS" if ok else "FAIL") + (f" ({err})" if err else ""))

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
