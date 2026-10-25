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
| 热 reload（`rmmod` + `insmod`）出节点 | ✅ **0.11s**（热芯片直接复用，不复位不重枚举） |
| 冷启动（插拔 / 复位后） | ✅ 10s（7s USB 枚举 + 2.7s init），一次成功 |
| 整机重启后开机 | ✅ 一次 probe 成功，`probe_failed=0` |
| 真机重启验证 | ✅ 重启后 `demux0 dvr0 frontend0 net0` 齐全 |

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

## 关键坑 3：热芯片怎么处理（`-22` / `-110` / reload 慢）

最初的症状：每轮 `insmod dvb-usb-smit` 的第一个 probe 必然失败，随后 dongle 自己断开
重新枚举（`devnum` +1），第二次才成功，肉眼看到的是"插了要等十几秒才出节点"。开机也一样。

第一版的判断是"热芯片不应答任何命令，只有硬件复位能救"，于是加了
disconnect 复位 + reboot notifier，把 reload 时间定格在 14s。**这个判断是错的。**

真机对照实验（`keep_warm=1` 卸载不复位，然后 `skip_wake=1` 直接跑初始化序列）
推翻了它：

| 热芯片上的操作 | 结果 |
|---|---|
| 发唤醒包 `FE 00 10 00` | 无应答，**且之后所有命令全 `-110`** |
| 只发一条命令（不唤醒不 init） | 立即正常应答：`raw tuner_stat probe -> 34`、`cmd0 0ms r=0`、`cmd3 0ms r=0` |
| 完整重跑 `smit_init()` | `cmd0`/`cmd3` 都应答，**`cmd4`（写寄存器 `0x9F/0x80/0x10`）之后固件彻底不回话** |

所以芯片初始化过一次之后命令通道一直是活的，把它打死的恰恰是我们自己的
"重新初始化"和"唤醒包"。正确做法是**热芯片直接复用，根本不要碰它**。

判定"冷还是热"试过三条路，前两条都失败，记下来免得再踩：

1. **设备描述符 / 字符串 / vendor control 请求** —— 冷热完全一致
   （`bcdDevice=0200`、`iProduct="DONGLE"`、几个 `0xC0` 请求一律 `-11`），零区分度。
2. **在 `probe` 里用 bulk 命令探活** —— 冷芯片收不到应答时，探针的 5 个字节
   已经留在它的 OUT FIFO 里，紧跟着的唤醒包被拼成畸形命令，握手必失败
   （加大到 4s / 10 次重发也冲不掉）。probe 失败又触发复位 → 重枚举 → 再探测 →
   再污染，**实测无限循环**。
   另外探针别手搓 `usb_bulk_msg` + 栈缓冲区，那种写法实测返回 `-11 (EAGAIN)`，
   看起来像错误但根本不是设备给的答案，会把热芯片误判成冷芯片。走 `smit_set()`
   （内部 `kmalloc` 缓冲区）才可靠。
3. **在 `module_init` 里探活（当前方案）** —— 此时还没 `usb_register`，
   复位不会引发任何 probe，污染可以就地清掉，也就不可能循环。

最终架构：

```
module_init（usb_register 之前）
  总线查找 29df:0001
    ├─ ping 有应答 -> 记下 devnum，等 probe 复用
    └─ ping 超时   -> 是冷芯片但 FIFO 被探针弄脏
                      smit_reset() 清干净，并轮询 udev->state 直到 NOTATTACHED
                      （约 0.8s）再 usb_register，避免 probe 撞上正在重启的设备
probe
    devnum == 记录值 -> 直接 dvb_usb_device_init()，不唤醒不 init（~0s）
    否则             -> 唤醒 + smit_init + 注册
```

`devnum` 每次重新枚举都会变，顺便充当"还是不是当年那个芯片"的凭据，拔插后不会误复用。

实测（真机，`/dev/kmsg` 取的）：

| 场景 | 改前 | 改后 |
|---|---|---|
| `rmmod` + `insmod`（热芯片） | 14s | **0.11s**，连测 3 次稳定 |
| 冷启动（dongle 复位/重插） | 10s | 10s（7s USB 枚举 + 2.7s init），一次成功 |
| 整机重启后开机 | 第一次 probe 失败、7s 自愈 | 一次 probe 成功，`probe_failed=0` |
| `smit_init()` 序列本身 | 7610ms | **2730ms** |

`init` 那 5s 的优化来自两处：`smit_set()` 里无条件的 `msleep(100)`（全序列 10 处 = 1s）
改为直接进 `smit_check()` 的退避轮询；序列末尾那条 `cmd9` 是调谐命令，初始化阶段
频点全 0，芯片既不调谐也不回完成标志，实测白等 **4390ms** 拿 `-1100`，改成只发不收完成标志。

因为 probe 现在能正确处理热芯片，`reboot_notifier` 已经删掉 —— 重启前不需要做任何事。

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
- 冷启动那 10s 里有 7s 是芯片固件重启后的 USB 枚举，主机侧压不掉
  （插拔 dongle、`reset_on_exit=1` 强制复位都会走这条路）。
- 热芯片复用的前提是"上一次初始化是完整的"。如果芯片处于半死状态（例如上一次
  `smit_init()` 中途被拔线），`module_init` 的 ping 会超时，走复位重建，不会卡死。
- `FE_READ_BER` / `FE_READ_UNCORRECTED_BLOCKS` 返回 `ENOTSUPP`，驱动没实现；
  `dvr0` 在未设置 demux 输出前 open 会返回 `ENOTSUP`，设好 PID 过滤后可读。

## 调试参数

```
insmod dvb-usb-smit.ko trace=1          # 打印 smit_init 每条命令的耗时
insmod dvb-usb-smit.ko diag_probe=1     # 打印描述符 + 5 个 vendor control 请求的应答
insmod dvb-usb-smit.ko reset_on_exit=1  # 卸载时复位芯片，强制下一次 probe 走冷启动
insmod dvb-usb-smit.ko skip_wake=1      # 跳过唤醒直接跑初始化（复现"热芯片被 init 打死"）
```

