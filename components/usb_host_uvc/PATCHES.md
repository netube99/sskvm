# 本地改动（相对上游 2.5.2）

本目录是 **espressif/usb_host_uvc 2.5.2 的 vendored 副本 + 本地补丁**（Apache-2.0，
见 `LICENSE`）。上游在快速分辨率切换 / 这颗采集卡上有几处会 assert 或丢数据的问题，
本地已修。升级上游时请逐条核对，别让补丁丢失。

## uvc_bulk.c

1. **SoF 分支的 NULL 检查**（`uvc_frame_reset(current_frame)`）
   停流后 `current_frame` 可能为 NULL，原代码直接解引用 → assert。
   加 NULL 判断后跳过。

2. **SoF 分配帧与 `uvc_host_stream_pause()` 的竞态**
   pause 把 `current_frame` 还回帧队列后，bulk 回调又给 `current_frame` 赋新帧，
   这帧永远回不来 → 之后 `close` 拒绝释放接口（`EP already allocated`）。
   现在分配后**再查一次 streaming 状态**，已停就立刻把帧还回去。

3. **DATA 态就地剥 12B UVC payload header**（这颗采集卡的兼容补丁）
   该卡每个 bulk payload（每个 URB，16384B）都带 12B header，stock 状态机只在
   SoF/EoF 剥 → 中间的 header 会混进 JPEG（以前只能在事后对整帧做一次 memcpy 剔除）。
   现在在装配帧、urb→frame 拷贝时按签名（`0x0C, 0x8C/0x8D`）+ 校验就地剥掉，零额外拷贝。
   三个约定：
   - `same_urb_cont`（见 `uvc_types_priv.h`）：防止 SoF 落进 DATA 态时二次剥；
   - **DMA 缓冲字段只读一次**：完成中断与最后几个字节落地有竞态，条件判断里读一次、
     `-=` 时再读一次会拿到撕裂值（实测会出现 ~189B 的洞）；
   - 字节读取在 `payload_data_len >= 2` 长度检查之后（先验长度再解引用）。

## 协议层要求（不是补丁，但依赖它）

`uvc_host_stream_format_select()` 内部的 `uvc_format_save()` 会断言所有帧都已归还。
调用方必须先把 `s_frame_q` 和 `current_frame` 清空（`uvc_capture_reconfigure()` 先 drain），
否则快速切换会打 assert。
