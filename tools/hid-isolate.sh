#!/bin/sh
# 切换 sskvm HID 压测模式（宿主机，需要 root）。
#
#   sudo tools/hid-isolate.sh on     # 解绑 P4 键鼠接口，供容器 libusb 独占
#   sudo tools/hid-isolate.sh off    # 交还宿主机内核（键鼠正常打进本机）
#
# 依赖已安装的 /etc/udev/rules.d/90-sskvm-hid.rules + /usr/local/sbin/sskvm-hid-isolate
# （规则只在 /run/sskvm-hid-isolate 存在时生效，所以 on/off 就是建/删这个文件）。
set -e

IFS=$'\n'
ifaces=$(ls -d /sys/bus/usb/devices/*/ 2>/dev/null | while read -r d; do
    [ "$(cat "$d/bInterfaceClass" 2>/dev/null)" = "03" ] || continue
    v=$(cat "$d/../idVendor" 2>/dev/null)
    [ "$v" = "303a" ] || continue
    basename "$d"
done)

case "${1:-}" in
on)
    touch /run/sskvm-hid-isolate
    for i in $ifaces; do
        echo "$i" > /sys/bus/usb/drivers/usbhid/unbind 2>/dev/null || true
    done
    echo "isolated: $ifaces"
    ;;
off)
    rm -f /run/sskvm-hid-isolate
    for i in $ifaces; do
        echo "$i" > /sys/bus/usb/drivers/usbhid/bind 2>/dev/null || true
    done
    sleep 1
    ls /dev/input/by-id/ 2>/dev/null | sed 's/^/  /'
    echo "host input restored"
    ;;
*)
    echo "usage: $0 on|off" >&2
    exit 2
    ;;
esac
