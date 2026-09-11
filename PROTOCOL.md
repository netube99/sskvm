# sskvm 接口协议

一个端口（443，HTTPS/WSS）。鉴权用 token（`?token=` 或 `X-Auth-Token`，空 = 不鉴权）。

## HTTP

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/` | 否 | 单文件前端页面（不鉴权，token 错了页面才有机会提示输入） |
| GET | `/config` | 是 | 设备状态 JSON |
| POST | `/config` | 是 | `res=0/1/2`、`hostname=<名字>` 或 `uvc=restart`（重启采集卡流） |
| GET | `/snapshot.jpg` | 是 | 采集卡原始 JPEG（最新一帧，给 agent） |
| POST | `/input` | 是 | body = 二进制键鼠事件（见下） |
| POST | `/type` | 是 | body = UTF-8 文本，P4 拆成 HID 按键；队列满返回已收字符数 |
| GET | `/ws` | 是 | WebSocket 升级（排他：已有一个浏览器时返回 `409 {"error":"busy"}`） |

`/config` 响应：

```json
{"hostname":"sskvm-xxxx","ip":"192.168.1.50","mac":"aa:bb:cc:dd:ee:ff",
 "version":"0.1.0","res":1,"resBusy":0,"usb":1,"signal":1,"wsBusy":0}
```

`res` 0=480P 1=720P 2=1080P；`signal`=0 表示采集卡没在出帧；`wsBusy`=1 表示浏览器已占用。
`res=` 写入会落 NVS（重启后保持）；首次开机用 Kconfig 默认值，之后一直是最后一次设的值。
HTTP 响应支持 keep-alive（agent 连续拉图复用一条 TLS 连接），空闲 15s 自动关闭。

`POST /config` 的 `uvc=restart`：关掉采集流并按**当前分辨率**重新协商、重开（~1s，WS 不断），
用于采集卡拔插/抽风后画面异常的软件复位；成功回 `restarting`（异步）。

## WebSocket `/ws`

**下行（设备 → 浏览器）**

| 类型 | 内容 |
|---|---|
| binary | `[u32 pts_ms LE][u16 width LE][u16 height LE][Annex-B AU]`（丢帧式：只发最新一帧） |
| text | `{"t":"state","res":1,"resBusy":0,"usb":1,"signal":1}` / `{"t":"pong","d":<设备ms>}` |

- 一帧一条 WS 消息；`pts` 是**采集时刻**的设备单调毫秒，前端配合 pong 里的 `d` 估算真实单向延时
- 连接建立后立即推一条全量 state；之后只在状态变化时推
- 新连接会触发一个 IDR（关键帧）
- 上行 `0x06` 切分辨率与 `POST /config res=` 等价，两者都会落 NVS

**上行（浏览器 → 设备）**，全部是 binary，设备端不解析 JSON：

| 首字节 | 长度 | 含义 |
|---|---|---|
| 0x01 | 8 | 键盘：`[mods][0][keycode×6]`（boot 键盘） |
| 0x02 | 7 | 绝对鼠标：`[buttons][x u16][y u16][wheel i8]`（x/y 0..32767） |
| 0x04 | 1 | 请求 IDR（解码器重建 / 落后时） |
| 0x05 | 1 | 心跳，设备回 `{"t":"pong","d":<ms>}` |
| 0x06 | 2 | 切分辨率：`[0x06][0/1/2]` |

`POST /input` 的 body 用同一套 0x01/0x02 编码；`/type` 用文本（ASCII/美式布局）。

## 断线与排他

- 浏览器：断线指数退避重连（0.5s→8s，带抖动）；`document.hidden` 时主动断开
- 设备：15s 收不到任何上行数据（心跳 5s 一次）或连续 ~0.5s 写不出去 → 踢掉该会话
- 同一时刻只允许一个 WS；被占用时第二个浏览器拿到 HTTP 409
