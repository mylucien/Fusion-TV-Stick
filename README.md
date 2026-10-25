# dvb-usb-smit for airoha/an7581 (Nokia XG-040G-MD)

在 CI 里交叉编译 SMIT iCast DTMB/DVBC USB dongle 驱动（VID:PID `29df:0001`），
目标设备内核 **6.18.55 / aarch64_cortex-a53 / SMP mod_unload aarch64**（PonWrt r0-03c1345）。

## 状态

| 项 | 结果 |
|---|---|
| 交叉编译（GitHub Actions, ubuntu-24.04, gcc-13 aarch64） | ✅ |
| `vermagic` = `6.18.55 SMP mod_unload aarch64` | ✅ 三个 .ko 全部一致 |
| `.gnu.linkonce.this_module` = 704B（与设备内核 `struct module` 一致） | ✅ |
| 设备上 `insmod dvb-core.ko` / `dvb-usb.ko` / `dvb-usb-smit.ko` | ✅ 全部 rc=0 |
| `usbcore: registered new interface driver dvb_usb_smit` | ✅ |
| 插上 dongle 后 probe / 出 `/dev/dvb/adapter0` | ⏳ 待插硬件验证 |

设备上实测：

```
# lsmod | grep dvb
dvb_core              106496  1 dvb_usb
dvb_usb                20480  1 dvb_usb_smit
dvb_usb_smit           20480  0
usbcore               208896 13 dvb_usb_smit,dvb_usb,ch341,uvcvideo,...

# dmesg
usbcore: registered new interface driver dvb_usb_smit
```

## 为什么需要自己编

固件里**没有**任何 DVB / RC / INPUT 的 kmod，所以 dvb-core / dvb-usb 也得自己带。

USB 主机侧不用管：设备已经加载了 `xhci_hcd` / `xhci_mtk_hcd` / `xhci_plat_hcd`
（`lsmod` 可见，`usbcore` 的引用计数里有它们），插 U 盘就能枚举。

## 构建方式

- 内核源码：kernel.org 的 **linux-6.18.55**（与设备 `uname -r` 严格一致）
- `.config`：`arm64 defconfig` 打底 → 叠加 `configs/` 下 OpenWrt(PonWrt) 的
  generic + target 两份 delta → 显式覆盖 vermagic / 结构体布局相关项 → `olddefconfig`
- USB / DVB 全部 `=m`，与设备现有的 `usbcore.ko` / `mc.ko` / `videodev.ko` 一致
- `DVB_MMAP=n`：避免拖入 `videobuf2` 依赖
- `NO_RC=1`：驱动 `properties` 里没有 `.rc`，遥控走空实现，行为等价但少一个 `rc-core.ko`
- `KBUILD_MODPOST_WARN=1`：没有内核的 `Module.symvers`，未解析符号是预期的

## 关键坑：`struct module` 大小必须逐字匹配

`insmod` 会校验 `.gnu.linkonce.this_module` 段的字节数是否等于内核里
`sizeof(struct module)`，不一致直接拒绝：

```
module dvb_core: .gnu.linkonce.this_module section size must match
                 the kernel's built struct module size at run time
```

设备侧基准怎么来的：读任意一个原厂模块的该段大小即可（原厂 `.ko` 就是按设备内核编的）：

```
# 132 个原厂模块全是 704
size .gnu.linkonce.this_module = 704 (0x2c0), init 在 +0x138, exit 在 +0x298
```

本仓库第一次编出来是 **1088**，差的 392 字节全部来自
`CONFIG_MODULES_TREE_LOOKUP`：它给 `struct module_memory` 加了一个
`struct mod_tree_node mtn`（56B），而 6.18 的 `mem[MOD_MEM_NUM_TYPES]` 是 7 项，
7 × 56 = **392**。1088 − 392 = 696，再按 64 字节对齐 = **704**，严丝合缝。

该符号在 Kconfig 里是

```
config MODULES_TREE_LOOKUP
	def_bool y
	depends on PERF_EVENTS || TRACING || CFI
```

**没有 prompt，不能直接写 `=n`**，只能把三个依赖都关掉：

```
# CONFIG_PERF_EVENTS is not set
# CONFIG_TRACING is not set
# CONFIG_CFI is not set
```

`scripts/ci-prep-config.sh` 已经这么做了，CI 里还有一道硬断言，
段大小不等于 704 直接 fail，避免又生成一个装不上的包。

## 目录

```
configs/   OpenWrt generic + target 内核配置 delta
src/       smit.c(协议) icast-fe.c(前端) icast-smit.c(桥接) + 头文件
scripts/   ci-prep-config.sh（合成 .config） prepare-src.sh（抠 DVB 源码到 kern/）
Makefile   kbuild 包装：dvb-core / dvb-usb / dvb-usb-smit
```

驱动源码来自 `github.com/nxdong520/iCast`（GPL-2.0），做了三处移植改动：

1. 合并成单模块，前端直接调 `icast_fe_attach()`，不再走 `dvb_attach()`/`symbol_request()`
2. `dvb-usb-remote.c` 换成空实现（`src/dvb-usb-remote-stub.c`）；
   注意 6.18 起 `dvb_usb_disable_rc_polling` 已由 `dvb-usb-init.c` 无条件定义，
   stub 里不能再定义一次，否则 `ld -r` 报 multiple definition
3. `dvb-usb.h` 里 `#include "dvb-pll.h"`，6.18 已把 `dvb-pll.h` 挪到
   `drivers/media/dvb-frontends/`，`prepare-src.sh` 会复制一份进来

## 产物校验

artifact 里每个 `.ko` 应满足：

```
vermagic=6.18.55 SMP mod_unload aarch64
ELF64, AArch64
.gnu.linkonce.this_module = 704
```

## 设备上安装

```sh
# 模块放到 /lib/modules/$(uname -r)/
cp dvb-core.ko dvb-usb.ko dvb-usb-smit.ko /lib/modules/$(uname -r)/

# 开机自动加载（kmodloader 按文件名字典序处理，文件内按行顺序加载）
printf 'dvb-core\ndvb-usb\ndvb-usb-smit\n' > /etc/modules.d/90-dvb-smit

# 或者手工
insmod /lib/modules/$(uname -r)/dvb-core.ko
insmod /lib/modules/$(uname -r)/dvb-usb.ko
insmod /lib/modules/$(uname -r)/dvb-usb-smit.ko
```

插上 dongle 后应该看到：

```
dmesg | grep -i -E 'dvb|smit'
ls /dev/dvb/adapter0/
```

## 已知限制

- 设备没有 `/proc/config.gz`，`.config` 是从 PonWrt 目标的 delta 反推的。
  vermagic 和 `struct module` 尺寸这两个硬门槛已经对齐；**其它**会改变结构体布局的
  选项（`CONFIG_PM`、`CONFIG_USB_OTG`、`CONFIG_MEDIA_CONTROLLER` 等）如果与固件真配置
  不同，模块能 load，但访问结构体字段会错位 —— 表现为 probe 时直接 oops。
  真出问题时，按 `dmesg` 崩在哪一行，去对相应结构体。
- 目前没有实测 probe：测试机（192.168.0.114）上没插 `29df:0001` 的 dongle。
