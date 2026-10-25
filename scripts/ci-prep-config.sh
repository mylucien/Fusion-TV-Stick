#!/usr/bin/env bash
# 生成 arm64 内核 .config：arm64 defconfig 打底 -> 叠加 OpenWrt generic+target
# 两份 delta -> 强制覆盖 vermagic / 模块化程度相关开关 -> olddefconfig。
#
# 用法:  KERNEL_DIR=/path/to/linux-6.18.55 bash scripts/ci-prep-config.sh
set -euo pipefail

KERNEL_DIR="${KERNEL_DIR:?set KERNEL_DIR}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$KERNEL_DIR"

ARCH=${ARCH:-arm64}

echo "== 0. $ARCH defconfig 打底（保证基础能力齐全）=="
make ARCH="$ARCH" defconfig >/dev/null
echo "   defconfig entries: $(grep -c '^CONFIG_' .config)"

echo "== 1. 叠加 OpenWrt generic + target delta（后者覆盖前者）=="
python3 - "$REPO_ROOT/configs" <<'PY'
import os, re, sys
cfgdir = sys.argv[1]

lines = open('.config').read().split('\n')
if lines and lines[-1] == '':
    lines.pop()

idx = {}
for i, ln in enumerate(lines):
    m = re.match(r'^(?:# )?(CONFIG_[A-Za-z0-9_]+)', ln)
    if m:
        idx[m.group(1)] = i

added = rep = 0
for name in ('generic-config-6.18', 'airoha-an7581-config-6.18'):
    path = os.path.join(cfgdir, name)
    for line in open(path):
        line = line.rstrip('\n')
        m = re.match(r'^(?:# )?(CONFIG_[A-Za-z0-9_]+)(.*)$', line)
        if not m:
            continue
        k, rest = m.group(1), m.group(2)
        if rest.startswith(' is not set'):
            new = '# %s is not set' % k
        elif rest.startswith('='):
            new = k + rest
        else:
            continue
        if k in idx:
            lines[idx[k]] = new
            rep += 1
        else:
            idx[k] = len(lines)
            lines.append(new)
            added += 1

open('.config', 'w').write('\n'.join(lines) + '\n')
print('   delta overrides: %d replaced / %d appended' % (rep, added))
PY

echo "== 2. 强制覆盖关键项 =="
python3 <<'PY'
import re
OVR = [
    # ---- vermagic: 必须与设备 '6.18.55 SMP mod_unload aarch64' 逐字一致 ----
    ('LOCALVERSION', '""'),
    ('LOCALVERSION_AUTO', None),
    ('SMP', 'y'),
    ('MODULE_UNLOAD', 'y'),
    ('MODVERSIONS', None),
    ('PREEMPT', None),
    ('PREEMPT_RT', None),
    ('PREEMPT_VOLUNTARY', None),
    ('PREEMPT_DYNAMIC', None),
    ('PREEMPT_LAZY', None),
    ('PREEMPT_NONE', 'y'),
    # ---- 签名 / 调试 / BTF：避免额外依赖 ----
    ('MODULE_SIG', None),
    ('MODULE_SIG_ALL', None),
    ('MODULE_COMPRESS', None),
    ('MODULE_COMPRESS_ZSTD', None),
    ('MODULE_COMPRESS_XZ', None),
    ('MODULE_COMPRESS_GZIP', None),
    ('DEBUG_INFO', None),
    ('DEBUG_INFO_BTF', None),
    ('DEBUG_INFO_DWARF5', None),
    ('GDB_SCRIPTS', None),
    ('SYSTEM_TRUSTED_KEYRING', None),
    ('SYSTEM_BLACKLIST_KEYRING', None),
    ('KALLSYMS', 'y'),
    # ---- USB：设备上 usbcore.ko 是模块，必须同为 m ----
    ('USB_SUPPORT', 'y'),
    ('USB', 'm'),
    ('USB_COMMON', 'm'),
    ('USB_XHCI_HCD', 'm'),
    ('USB_XHCI_PLATFORM', 'm'),
    ('USB_XHCI_MTK', 'm'),
    ('USB_XHCI_PCI', None),
    ('USB_XHCI_PCI_RENESAS', None),
    ('USB_XHCI_MTK_DEBUGFS', None),
    # ---- Media / DVB：设备上 mc/videodev/videobuf2 是模块，MEDIA_CONTROLLER=y ----
    ('MEDIA_SUPPORT', 'm'),
    ('MEDIA_CONTROLLER', 'y'),
    ('MEDIA_DIGITAL_TV_SUPPORT', 'y'),
    ('MEDIA_USB_SUPPORT', 'y'),
    ('MEDIA_SUBDRV_AUTOSELECT', None),
    ('DVB_CORE', 'm'),
    ('DVB_USB', 'm'),
    ('DVB_MMAP', None),
    ('RC_CORE', None),
    ('I2C', 'y'),
    ('I2C_CORE', 'y'),
    # ---- struct inode 布局：设备 i_rdev @68 ----
    # 设备 kallsyms 里有 posix_acl_init/posix_acl_alloc 等内建全局符号，
    # 说明固件 FS_POSIX_ACL=y（OpenWrt 的 delta 里写的是 not set，不可靠）。
    # 该开关给 struct inode 在 i_flags 之后加 i_acl/i_default_acl 两个指针(+16B)，
    # 使 i_rdev 从 52 挪到 68。布局不一致时 dvb_device_open() 里的 iminor()
    # 会读到 i_mapping 的高半(0xffffff80 -> minor=1048448)，因而返回 ENODEV。
    # 注意 FS_POSIX_ACL 本身是 "def_bool n" 的隐藏符号，没有 prompt，
    # 直接写 =y 会被 olddefconfig 丢掉，必须靠文件系统的 ACL 选项 select 出来。
    ('TMPFS_POSIX_ACL', 'y'),
    # ---- 其它 ----
    ('TRIM_UNUSED_KSYMS', None),
    ('UNUSED_KSYMS_WHITELIST', '""'),
    ('CFI_CLANG', None),
    ('LTO_CLANG', None),
    ('LTO_NONE', 'y'),
    # ---- struct module 布局：设备 sizeof(struct module)=704 ----
    # MODULES_TREE_LOOKUP 是 "def_bool y; depends on PERF_EVENTS || TRACING || CFI"，
    # 没有 prompt 所以不能直接写 n，只能把三个依赖全部关掉。
    # 它是 struct module_memory.mtn(56B) 的来源：mem[7] 共多出 392B，
    # 使 .gnu.linkonce.this_module 从 704 涨到 1088，insmod 报
    # "section size must match the kernel's built struct module size"。
    ('PERF_EVENTS', None),
    ('TRACING', None),
    ('CFI', None),
    ('MODULE_DEBUG', None),
    ('MODULE_STATS', None),
    ('MODULE_UNLOAD_TAINT_TRACKING', None),
]

lines = open('.config').read().split('\n')
idx = {}
for i, ln in enumerate(lines):
    m = re.match(r'^(?:# )?(CONFIG_[A-Za-z0-9_]+)', ln)
    if m:
        idx[m.group(1)] = i

for k, v in OVR:
    new = ('# CONFIG_%s is not set' % k) if v is None else ('CONFIG_%s=%s' % (k, v))
    if k in idx:
        lines[idx[k]] = new
    else:
        idx[k] = len(lines)
        lines.append(new)

open('.config', 'w').write('\n'.join(lines) + '\n')
print('   overrides applied:', len(OVR))
PY

echo "== 3. MEDIA_ATTACH=n（隐藏符号，只能改 Kconfig 默认值）=="
# dvb_frontend_invoke_release() 里有：
#     if (release) { release(fe); dvb_detach(release); }
# 而 dvb_detach() 就是 symbol_put_addr()，是给 dvb_attach() 的 symbol_request()
# 配对用的。本驱动的前端和 bridge 编在同一个 .ko 里，icast_frontend_attach()
# 直接调用 icast_fe_attach()，没有 symbol_request，于是 rmmod 时这个 put
# 会让模块引用计数下溢：
#     WARNING: CPU: 3 PID: ... at module_put+0x40/0x50
#     dvb_frontend_put -> dvb_frontend_detach -> dvb_usb_device_exit
# MEDIA_ATTACH 是 "bool" 没有 prompt（default MODULES），.config 里写 not set
# 会被 olddefconfig 丢掉，所以直接改 Kconfig 的默认值。关掉后 dvb_attach()
# 变成直接函数调用、dvb_detach() 变成空语句 —— 正是我们要的语义。
# 我们只用 dvb-core + dvb-usb 的源码，两者都不含 dvb_attach() 调用，
# 因此不受影响。
python3 - <<'PY'
import re, sys
p = 'drivers/media/Kconfig'
s = open(p).read()
m = re.search(r'^config MEDIA_ATTACH\n(?:[ \t]+[^\n]*\n)+', s, re.M)
if not m:
    sys.exit('!! %s 里找不到 MEDIA_ATTACH 定义' % p)
blk = m.group(0)
if re.search(r'^[ \t]+default n$', blk, re.M):
    print('   already default n')
elif 'default MODULES' in blk:
    open(p, 'w').write(s.replace(blk, blk.replace('default MODULES', 'default n'), 1))
    print('   MEDIA_ATTACH: default MODULES -> n')
else:
    sys.exit('!! MEDIA_ATTACH 定义与预期不符:\n' + blk)
PY

echo "== 4. olddefconfig =="
make ARCH="$ARCH" olddefconfig >/dev/null

echo "== 5. 关键项核对 =="
for k in LOCALVERSION LOCALVERSION_AUTO SMP MODULE_UNLOAD MODVERSIONS \
         PREEMPT_NONE PREEMPT_DYNAMIC PREEMPT_BUILD \
         USB USB_COMMON USB_XHCI_HCD USB_XHCI_PLATFORM USB_XHCI_MTK \
         MEDIA_SUPPORT MEDIA_CONTROLLER MEDIA_ATTACH DVB_CORE DVB_USB DVB_MMAP RC_CORE \
         MODULE_SIG MODULE_SIG_FORCE DEBUG_INFO DEBUG_INFO_BTF TRIM_UNUSED_KSYMS; do
    v=$(grep -E "^(CONFIG_${k}=|# CONFIG_${k} is not set)" .config | head -1 || true)
    printf "   %-24s %s\n" "$k" "${v:-<未出现>}"
done

if grep -q '^CONFIG_MEDIA_ATTACH=y' .config; then
    echo "!! MEDIA_ATTACH 仍为 y，rmmod 时 dvb_detach() 会 module_put 下溢"
    exit 1
fi

echo
echo "== 6. vermagic 预算 =="
make -s ARCH="$ARCH" kernelrelease
