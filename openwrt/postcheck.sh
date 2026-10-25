#!/bin/sh
# 校验 OpenWrt 编译出来的 .ko 能不能被目标内核 insmod。
#
# 只需一条硬指标：.gnu.linkonce.this_module 段大小 == 内核里 sizeof(struct module)。
# 它完全由目标内核 .config 决定（TRACING / PERF_EVENTS / LIVEPATCH / BPF 等），
# 差一个字节 insmod 就拒绝：
#
#   module dvb_core: .gnu.linkonce.this_module section size must match
#                    the kernel's built struct module size at run time
#
# 在 an7581 这台设备上是 704。换了 target 之后这里会打印别的大小 ——
# 那就说明内核配置不同，编出来的模块只能给那个配置的内核用。
#
# 环境变量：
#   KO_MDIR                .ko 所在目录
#   OBJDUMP                objdump（交叉工具链的，主机上执行）
#   EXPECT_MODULE_SIZE     期望值，默认 704；设成 0 只打印不比对

set -u

KO_MDIR="${KO_MDIR:-.}"
OBJDUMP="${OBJDUMP:-objdump}"
EXPECT="${EXPECT_MODULE_SIZE:-704}"

rc=0

for m in dvb-core dvb-usb dvb-usb-smit; do
	f="$KO_MDIR/$m.ko"
	if [ ! -f "$f" ]; then
		echo "!! 缺少 $m.ko"
		rc=1
		continue
	fi

	# 不依赖 strings(1)：grep -a 直接扫二进制里的 .modinfo
	vm=$(grep -a -o 'vermagic=[0-9A-Za-z._+ ~]*' "$f" 2>/dev/null |
		head -1 | sed 's/^vermagic=//')

	hex=$("$OBJDUMP" -h "$f" 2>/dev/null |
		awk '$2 == ".gnu.linkonce.this_module" { print $3; exit }')

	if [ -n "${hex:-}" ]; then
		size=$((0x$hex))
		printf '   %-14s vermagic="%s"  this_module=%s\n' "$m" "${vm:-?}" "$size"
		if [ "$EXPECT" != "0" ] && [ "$size" != "$EXPECT" ]; then
			echo "   !! $m.ko: struct module = $size，本驱动验证过的目标是 $EXPECT"
			echo "      这个内核的 .config 与目标设备不同，模块装不上。"
		fi
	else
		printf '   %-14s vermagic="%s"  this_module=?\n' "$m" "${vm:-?}"
		echo "   (objdump 读不出段表，跳过大小比对)"
	fi

	case "$vm" in
	*" SMP mod_unload aarch64") ;;
	*) echo "   !! $m.ko: vermagic 与设备内核(6.18.55 SMP mod_unload aarch64)不匹配" ;;
	esac
done

exit $rc
