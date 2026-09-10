#!/bin/sh
# sskvm: 解绑 P4 HID 接口的内核 usbhid 驱动（udev 规则调用；$1 = 1-6:1.0 之类）。
#
# 只在 /run/sskvm-hid-isolate 存在时解绑：那个文件是「HID 压测模式」的开关
# （容器里的 libusb 要独占接口）。平时宿主机就是被控机，键鼠必须由内核驱动。
#   sudo touch /run/sskvm-hid-isolate      # 进压测模式（下次枚举不再绑 usbhid）
#   sudo rm /run/sskvm-hid-isolate         # 退出（手动 rebind，见 tools/hid-isolate.sh）
[ -e /run/sskvm-hid-isolate ] || exit 0

i=0
while [ $i -lt 10 ]; do
    if echo "$1" > /sys/bus/usb/drivers/usbhid/unbind 2>/dev/null; then
        exit 0
    fi
    i=$((i+1))
    sleep 0.3
done
exit 1
