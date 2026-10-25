#!/usr/bin/env bash
# 把 OpenWrt 的 generic + target 两份 delta 配置合并成完整 .config，
# 再把关键项对齐到设备内核（vermagic / 模块化程度 / 结构体布局相关开关）。
#
# 用法:  KERNEL_DIR=/path/to/linux-6.18.55 ./scripts/ci-prep-config.sh
set -euo pipefail

KERNEL_DIR="${KERNEL_DIR:?set KERNEL_DIR}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$KERNEL_DIR"

echo "== 1. 合并 generic + target（后者覆盖前者）=="
python3 - "$REPO_ROOT/configs" <<'PY'
import os, re, sys
cfgdir = sys.argv[1]
order, val = [], {}
for name in ('generic-config-6.18', 'airoha-an7581-config-6.18'):
    for line in open(os.path.join(cfgdir, name)):
        line = line.rstrip('\n')
        m = re.match(r'^(?:# )?(CONFIG_[A-Za-z0-9_]+)(.*)$', line)
        if not m:
            continue
        k, rest = m.group(1), m.group(2)
        if rest.startswith(' is not set'):
            val[k] = '# %s is not set' % k
        elif rest.startswith('='):
            val[k] = k + rest
        else:
            continue
        if k not in order:
            order.append(k)
open('.config', 'w').write('\n'.join(val[k] for k in order) + '\n')
print('   merged entries:', len(order))
PY

echo "== 2. 覆盖关键项 =="
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
    # ---- 签名 / 调试 / BTF：避免额外依赖 ----
    ('MODULE_SIG', None),
    ('MODULE_SIG_ALL', None),
    ('MODULE_COMPRESS', None),
    ('DEBUG_INFO', None),
    ('DEBUG_INFO_BTF', None),
    ('DEBUG_INFO_DWARF5', None),
    ('GDB_SCRIPTS', None),
    ('SYSTEM_TRUSTED_KEYRING', None),
    # ---- USB：设备上 usbcore.ko 是模块，必须同为 m ----
    ('USB_SUPPORT', 'y'),
    ('USB', 'm'),
    ('USB_COMMON', 'm'),
    ('USB_XHCI_HCD', 'm'),
    ('USB_XHCI_PLATFORM', 'm'),
    ('USB_XHCI_MTK', 'm'),
    ('USB_XHCI_PCI', None),
    ('USB_XHCI_PCI_RENESAS', None),
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
    # ---- 其它 ----
    ('TRIM_UNUSED_KSYMS', None),
    ('UNUSED_KSYMS_WHITELIST', '""'),
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

echo "== 3. olddefconfig =="
make ARCH=arm64 olddefconfig >/dev/null

echo "== 4. 关键项核对 =="
for k in LOCALVERSION SMP MODULE_UNLOAD MODVERSIONS PREEMPT_NONE PREEMPT_DYNAMIC \
         USB USB_COMMON USB_XHCI_HCD USB_XHCI_PLATFORM USB_XHCI_MTK \
         MEDIA_SUPPORT MEDIA_CONTROLLER DVB_CORE DVB_USB DVB_MMAP RC_CORE \
         MODULE_SIG MODULE_SIG_FORCE DEBUG_INFO_BTF TRIM_UNUSED_KSYMS; do
    v=$(grep -E "^(CONFIG_${k}=|# CONFIG_${k} is not set)" .config | head -1 || true)
    printf "   %-26s %s\n" "$k" "${v:-<未出现>}"
done
