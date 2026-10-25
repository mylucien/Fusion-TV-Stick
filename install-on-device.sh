#!/bin/sh
# 在目标设备（Nokia XG-040G-MD / airoha an7581 / 6.18.55）上安装 dvb-usb-smit
#
# 用法：
#   1. 在电脑上把三个 .ko 放到设备能访问的 HTTP 目录，例如
#        python -m http.server 8899
#   2. 设备上执行：
#        sh install-on-device.sh http://192.168.0.111:8899
#
# 前提：设备上已经加载 xhci_hcd / xhci_mtk_hcd（固件自带）。
set -e

BASE="${1:-http://192.168.0.111:8899}"
KVER="$(uname -r)"
DEST="/lib/modules/$KVER"
TMP="/tmp/dvb-install"

echo ">> kernel: $KVER"
echo ">> base  : $BASE"

mkdir -p "$TMP" "$DEST"

for m in dvb-core dvb-usb dvb-usb-smit; do
	echo ">> fetching $m.ko"
	wget -q -O "$TMP/$m.ko" "$BASE/$m.ko"
done

echo ">> 卸载旧模块（忽略错误）"
rmmod dvb_usb_smit 2>/dev/null || true
rmmod dvb_usb 2>/dev/null || true
rmmod dvb_core 2>/dev/null || true

cp -f "$TMP"/*.ko "$DEST"/

echo ">> 加载（顺序不能乱：dvb-core -> dvb-usb -> dvb-usb-smit）"
for m in dvb-core dvb-usb dvb-usb-smit; do
	insmod "$DEST/$m.ko"
	echo "   $m ok"
done

printf 'dvb-core\ndvb-usb\ndvb-usb-smit\n' > /etc/modules.d/90-dvb-smit
sync

echo ">> 结果"
lsmod | grep -i dvb || true
echo ">> 驱动绑定节点："
ls /sys/bus/usb/drivers/ | grep -i smit || true
echo
echo "插上 dongle(29df:0001) 后：dmesg | grep -i -E 'dvb|smit'; ls /dev/dvb/"
