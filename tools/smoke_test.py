#!/usr/bin/env python3
"""sskvm 冒烟测试：一条命令验证整套链路。

    ./smoke_test.py [host] [--token TOKEN]

检查项：
  1. HTTPS 页面 200 且包含 WebCodecs 逻辑
  2. GET /snapshot.jpg 是合法 JPEG（SOI/EOI 标记）
  3. POST /type 返回 typed N
  4. POST /input 点击不报错
  5. 单次 write 的 POST（头和 body 同一段）：body 首字节不能被吃掉
  6. wss /ws 能收到帧且是 Annex-B（8B 帧头之后是起始码 00 00 01）
"""
import base64
import hashlib
import json
import os
import socket
import ssl
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kvm import Kvm  # noqa: E402

if len(sys.argv) < 2 or sys.argv[1].startswith("--"):
    sys.exit(__doc__)
HOST = sys.argv[1]
TOKEN = sys.argv[sys.argv.index("--token") + 1] if "--token" in sys.argv else ""
FAIL = []


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))
    if not ok:
        FAIL.append(name)


def ws_h264_frames(host, token, seconds=3):
    """极简 WebSocket 客户端，数一下 H264 帧。"""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    s = ctx.wrap_socket(socket.create_connection((host, 443), timeout=5), server_hostname=host)
    key = base64.b64encode(os.urandom(16)).decode()
    path = "/ws" + (f"?token={token}" if token else "")
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: {host}:443\r\nUpgrade: websocket\r\n"
              f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
              f"Sec-WebSocket-Version: 13\r\n\r\n".encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    if b" 101 " not in buf.split(b"\r\n")[0]:
        return -1, b""
    rbuf = buf.split(b"\r\n\r\n", 1)[1]

    def recv(n):
        nonlocal rbuf
        while len(rbuf) < n:
            d = s.recv(1 << 16)
            if not d:
                raise ConnectionError
            rbuf += d
        out, rbuf = rbuf[:n], rbuf[n:]
        return out

    def frame():
        """读一条完整 WS 消息（自动拼分片），返回 (opcode, payload)"""
        payload = b""
        while True:
            h = recv(2)
            op = h[0] & 0x0F
            fin = h[0] & 0x80
            ln = h[1] & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", recv(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", recv(8))[0]
            payload += recv(ln) if ln else b""
            if fin:
                return op, payload

    n, first = 0, b""
    t0 = time.time()
    while time.time() - t0 < seconds:
        op, msg = frame()
        if op != 0x2:                       # 文本 = 状态/pong，不算视频帧
            continue
        if n == 0:
            first = msg
        n += 1
    return n, first


def post_one_write(host, path, body, token=""):
    """头和 body 放同一次 write（curl/fetch 的行为）。
    回归：http_read_request 曾先给头部打 NUL 再拷 body，把 body 首字节写成 0。"""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    s = ctx.wrap_socket(socket.create_connection((host, 443), timeout=5), server_hostname=host)
    b = body.encode() if isinstance(body, str) else body
    hdr = (f"POST {path} HTTP/1.1\r\nHost: {host}:443\r\nContent-Length: {len(b)}\r\n"
           + (f"X-Auth-Token: {token}\r\n" if token else "") + "\r\n").encode()
    s.sendall(hdr + b)
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(4096)
    head, rest = resp.split(b"\r\n\r\n", 1)
    cl = int([l.split(b":", 1)[1] for l in head.split(b"\r\n")
              if l.lower().startswith(b"content-length")][0])
    while len(rest) < cl:
        rest += s.recv(4096)
    s.close()
    return head.split(b"\r\n")[0], rest[:cl]


def main():
    k = Kvm(HOST, token=TOKEN)

    html = k._get("/")
    check("页面 200 + WebCodecs", b"VideoDecoder" in html and b"/ws" in html,
          f"{len(html)}B")

    jpg = None
    for _ in range(20):                     # 刚上电时 UVC 还没出帧，等一等
        try:
            jpg = k.screenshot()
            break
        except RuntimeError as e:
            if "no frame yet" not in str(e):
                raise
            time.sleep(1)
    check("snapshot 是 JPEG", jpg is not None and jpg[:2] == b"\xff\xd8" and jpg[-2:] == b"\xff\xd9",
          f"{len(jpg) if jpg else 0}B {jpg[:4].hex() if jpg else ''}")

    r = k.type("ok")
    check("POST /type", r.startswith("typed "), r.strip())
    # 擦掉自己敲的字符：测试别在人家屏幕上留垃圾（按实际敲进去的个数退格）
    n_typed = int(r.rsplit(" ", 1)[-1]) if r.startswith("typed ") else 0
    for _ in range(n_typed):
        k.key(0x2a)

    try:
        k.click(16384, 16384)
        check("POST /input 点击", True)
    except Exception as e:
        check("POST /input 点击", False, str(e))

    cur = json.loads(k._get("/config"))["res"]
    st, body = post_one_write(HOST, "/config", f"res={cur}", TOKEN)
    check("POST 一次写完 body", b"200" in st and body == b"ok",
          f"{st.decode(errors='replace')} {body!r}")

    n, first = ws_h264_frames(HOST, TOKEN)
    check("wss /ws 有帧", n > 0, f"{n} 帧")
    if first:
        # 帧头 [u32 pts][u16 w][u16 h] 之后是 Annex-B
        body = first[8:] if len(first) > 8 else b""
        check("H264 Annex-B 起始码", body[:4] == b"\x00\x00\x00\x01", body[:9].hex())

    print()
    print("ALL PASS" if not FAIL else f"FAILED: {', '.join(FAIL)}")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
