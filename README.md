# dvb-usb-smit for airoha/an7581 (Nokia XG-040G-MD)

在 CI 里交叉编译 SMIT iCast DTMB/DVBC USB dongle 驱动（VID:PID `29df:0001`），
目标设备内核 **6.18.55 / aarch64_cortex-a53 / SMP mod_unload aarch64**（PonWrt r0-03c1345）。

## 状态

| 项 | 结果 |
|---|---|
| 交叉编译（GitHub Actions, ubuntu-24.04, gcc-13 aarch64） | ✅ |
| `vermagic` = `6.18.55 SMP mod_unload aarch64` | ✅ 三个 .ko 全部一致 |
| `.gnu.linkonce.this_module` = 704B（与设备内核 `struct module` 一致） | ✅ |
| `offsetof(struct inode, i_rdev)` = 68（与设备内核 `struct inode` 一致） | ✅ |
| 设备上 `insmod dvb-core.ko` / `dvb-usb.ko` / `dvb-usb-smit.ko` | ✅ 全部 rc=0 |
| `usbcore: registered new interface driver dvb_usb_smit` | ✅ |
| 插上 dongle 后 probe / 出 `/dev/dvb/adapter0` | ✅ |
| 打开 `frontend0` 读到 `dvb_frontend_info` | ✅ |
| 开机 / reload 后首次 probe | ⚠️ 见下方「已知限制」 |

设备上实测：

```
# lsmod | grep dvb
dvb_core              106496  1 dvb_usb
dvb_usb                20480  1 dvb_usb_smit
dvb_usb_smit           20480  0
usbcore               208896 13 dvb_usb_smit,dvb_usb,ch341,uvcvideo,...

# ls -l /dev/dvb/adapter0/
crw-------  root root  212, 3  frontend0
crw-------  root root  212, 4  demux0
crw-------  root root  212, 5  dvr0
crw-------  root root  212, 7  net0
```

`FE_GET_INFO` 返回：

```
name       : iCast DTMB/DVBC USB demodulator
type       : 2  OFDM（DTMB 在 Linux 里就是 FE_OFDM 类型）
frequency  : 10000000 .. 862000000 Hz (stepsize 10000)
caps       : 0x000b0201
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

## 关键坑 2：`struct inode` 大小必须一致（`i_rdev` @68）

模块能加载、驱动能绑定，但一打开 `/dev/dvb/adapter0/*` 就 `ENODEV`（errno 19）。
根因不在驱动，而在**我们编出来的 `struct inode` 和设备内核的不是同一个形状**。

`dvb_device_open()` 第一件事是 `iminor(inode)` 取次设备号，实际打出来是 `1048448`：

```
dvbdiag: inode=... mode=0x2180 ino=106 rdev=0xd400003 sizeof=584 off=68   # 修好后
```

`1048448 = 0xFFF80` 正好是 aarch64 直接映射区指针高 32 位 `0xffffff80` 的低 20 位
—— 说明读到的不是 `i_rdev`，而是 `i_mapping` 的高半。差 16 字节，来自
`CONFIG_FS_POSIX_ACL`：

```c
#ifdef CONFIG_FS_POSIX_ACL
	struct posix_acl	*i_acl;          /* +8 */
	struct posix_acl	*i_default_acl;  /* +8 */
#endif
```

设备固件是 `FS_POSIX_ACL=y`（`/proc/kallsyms` 里 `posix_acl_init`、`posix_acl_alloc`
等是内建 `T` 符号，这就是判据；OpenWrt 的 delta 配置里写的是 `not set`，不可信）。
`i_rdev` 因此从 52 挪到 68，我们按 52 去读就落到了 `i_mapping` 上。

坑在于 **`FS_POSIX_ACL` 是 `def_bool n` 的隐藏符号，没有 prompt**，直接写
`CONFIG_FS_POSIX_ACL=y` 会被 `olddefconfig` 丢掉。只能打开 select 它的文件系统选项：

```
CONFIG_TMPFS_POSIX_ACL=y     # select FS_POSIX_ACL
```

CI 现在会断言 `offsetof(struct inode, i_rdev) == 68`，不等就 fail。

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
  已经踩到过两个因为「delta 不等于真配置」而错位的内核结构体：
  `struct module`（704B）和 `struct inode`（`i_rdev` @68）。两者现在都有 CI 硬断言。
  **其它**会改变结构体布局的选项（`CONFIG_PM`、`CONFIG_USB_OTG`、`CONFIG_SECURITY` 等）
  如果与固件真配置不同，仍可能出现 `probe` oops 或字段错位 —— 到时候按 `dmesg`
  崩在哪一行，用同样的办法（`pahole` 看我们的偏移 / `/proc/kallsyms` 找内建符号当判据）去对。
- **首次 probe 会失败一次**：每次 `insmod dvb-usb-smit.ko`，第一个 probe 都返回
  `-22 (EINVAL)`，紧接着 USB 设备自己断开重枚举（device number +1），第二次 probe 才成功。

  ```
  dvb_usb_smit 1-1:1.0: probe with driver dvb_usb_smit failed with error -22
  usb 1-1: USB disconnect, device number 3
  usb 1-1: new high-speed USB device number 4 using xhci-mtk
  dvb-usb: found a 'iCast DVB USB DONGLE adapter' in warm state.
  usb 1-1: DVB: registering adapter 0 frontend 0 (...)
  dvb-usb: iCast DVB USB DONGLE adapter successfully initialized and connected.
  ```

  开机自动加载（`/etc/modules.d/90-dvb-smit`）时同样会遇到，**约 15 秒后自动恢复**，
  不会是永久性失败。根因在 `smit_probe` 的初始化时序（第一次的 bulk 命令在设备
  刚枚举、接口还没 settle 时返回 EINVAL），值得后续修掉。
- `FE_READ_BER` / `FE_READ_UNCORRECTED_BLOCKS` 返回 `ENOTSUPP`，驱动没实现；
  `dvr0` 在未设置 demux 输出前 open 会返回 `ENOTSUP`，设好 PID 过滤后可读。
