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
| 冷启动（插拔 / 复位后） | ✅ 10s（6.7s USB 枚举 + 3.1s 唤醒/init），一次成功 |
| 整机重启后开机 | ✅ 一次 probe 成功，`probe_failed=0` |
| 真机重启验证 | ✅ 重启后 `demux0 dvr0 frontend0 net0` 齐全 |
| 命令通道并发压测（4 线程同一 fd，30s） | ✅ 1488 次 `FE_READ_STATUS`，0 错误 |
| 长时间持续读状态 | ✅ 5 分钟 4 线程 14412 次，0 超时 / 0 stall / 0 错位 |
| 通道卡死后自愈（注入故障验证） | ✅ 自动复位重新枚举，节点自己回来，**不用拔插** |
| 自愈（热复位）总耗时 | **9.8s**（6.7s 枚举 + 3.1s 唤醒/init），两次测量误差 <1ms |
| 冷启动连做 3 轮 | ✅ 3/3 成功，`init` 2730 / 2730 / 2760 ms |
| 全量代码审查（4 P0 / 5 P1 / 3 P2） | ✅ 12 处全部修复，回归复测见「关键坑 5」 |
| 审查后冷启动 ×3 | ✅ 8.0 / 8.0 / 8.1s，`timeout=0 resync=0 recover=0` |

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

1. **设备描述符 / 字符串 / vendor control 请求** —— 描述符和字符串确实一样
   （`bcdDevice=0200`、`iProduct="DONGLE"`），但**"vendor 请求零区分度"这个结论是假的**。
   那几个 `0xC0` 请求的 `-11` 根本不是设备的答复：`diag_probe=1` 当时把栈上的
   `u8 buf[64]` 直接交给了 `usb_control_msg`，`hcd_map_urb_for_dma()` 对栈缓冲区
   `WARN_ONCE("transfer buffer is on stack")` 并返回 `-EAGAIN`，**请求一个字节都没发出去**。
   修正前的真机日志：
   ```
   usb 1-1: diag: bcdUSB=0200 bcdDevice=0200 id=29df:0001 ... speed=3
   transfer buffer is on stack
   usb 1-1: diag: ctrl req=a0 val=0000 -> -11
   usb 1-1: diag: ctrl req=01 val=0000 -> -11
   ...
   ```
   现在 `smit_diag()` 改用 `kmalloc` 缓冲区。也就是说这条路**从来没被真正测过**，
   想找更简单的冷热判据，值得用它重测一遍。
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

## 关键坑 4：跑一段时间"不定时挂死"，必须拔插才能恢复

这是实际使用中最影响体验的问题：驱动加载正常、能出节点、能锁台，但跑几个小时
（有时几分钟）之后就不动了，画面冻结、状态不再更新，`rmmod`/`insmod` 都没用，
**只有拔掉 USB 重新插**才恢复。

根因不在"稳定性跑久了劣化"，而在命令通道（`0x01` 写 + `0x82` 读）的四个结构性缺陷：

1. ~~两步事务没有互斥~~ —— **这条归因是错的，已撤回。**
   `dvb_frontend_do_ioctl()` 在 `fepriv->sem` 下调 `dvb_frontend_handle_ioctl()`，
   状态线程（每 100ms 的 `read_status`）也在同一把信号量下（`dvb_frontend.c`），
   而且这把信号量是 per-frontend 而不是 per-fd。**所有前端操作本来就完全串行**，
   同一个 OUT 包被两个调用者交错在结构上就不可能发生。
   → `smit_cmd_lock` 仍然保留：它唯一真正起作用的是 `probe`/自愈复位路径，
   那条路径不受 `fepriv->sem` 保护，确实可能和 ioctl 撞上。但别再把它当成
   "挂死的解药"——**以后再出现挂死，不要因为"已经加了锁"就把并发排除**。
   当年那个"4 线程同 fd 压 20s，0 错误"的测试，测的其实是核心的 `fepriv->sem`，
   不是这把锁。真正有效的是下面第 2/3/4 条加上自愈复位。

2. **一次 IN 超时就永久错位。** 这是"不定时"的真正来源，不需要任何并发：某次读应答
   超时（芯片正好在忙），那条应答还躺在 IN 管道里，下一条命令读到的是它的**上一条**
   应答。之后整条通道稳定地偏移一格，`read_status` 永远拿到旧数据 —— 表现就是
   "状态不再更新、画面冻结"，而且自己好不了。
   → 超时 / `tag` 错位后置 `resync_pending`，下次事务前 `smit_drain_in()` 把 IN 管道
   抽干，从干净边界重新开始。注意这里**只在真 stall 时**才 `clear_halt`（见第 3 条）。

3. **端点 stall 之后从不清 halt。** 一旦某次传输拿到 `-EPIPE`，之后每一次传输都立刻
   返回 `-EPIPE`，主机侧没有任何东西能让它恢复。
   → 遇到 `-EPIPE` 先 `usb_clear_halt()` 再重试一次。但代价要算清楚：
   `usb_clear_halt()` 是一条 `CLEAR_FEATURE` 控制请求（不管端点是否真 halt 都会发），
   超时 `USB_CTRL_SET_TIMEOUT = 5s`。所以①只在**确实还会再重试**时才清，
   最后一次失败后再清纯属白等；②只清**出错的那个端点**，不要收发一起清。

4. **重试只有次数上限、没有时间上限。** `smit_check()` 原来只判 `cnt > checkcount`，
   每次尝试还含两个 500ms 超时；`checkcount=300` 时一条 ioctl 最坏能卡几十秒，
   应用整个卡在 ioctl 里。
   → 预算改成 `1s + checkcount*10ms`（上限 6s），且在循环头上就判，别等跑完一轮。

还有一个"看起来像挂死"的坑在前端层：`icast_fe_read_status()` 读失败时直接
`return 0`，**没有写 `*status`**。上层 `dvb_frontend_ioctl()` 里那个变量是栈上的
未初始化值，随便带着 `FE_HAS_LOCK` 位，于是一次读失败之后播放器会一直认为
"还锁着"，对着一动不动的画面永远不恢复。→ 进函数先 `*status = 0`。

最后加了一层**自愈**兜底：连续 3 次事务在传输层失败就认为芯片卡死，
在 workqueue 里发硬件复位让芯片重新枚举（不能在 ioctl 上下文里同步做，那等于
在还没退出时拆自己的设备），`probe` 因此走完整冷启动，节点自己回来 —— 不需要拔插。
复位本身带死循环保护：连续 3 次复位都没换来一次成功 probe 就停手
（`reset_attempts` 在每次 probe 成功时清零）。

实测（`/sys/module/dvb_usb_smit/parameters/stats` 计数）：

| 场景 | 结果 |
|---|---|
| 4 线程同 fd 压 `FE_READ_STATUS` 30s | 1488 次，0 错误，`timeout=0 epipe=0 resync=0` |
| 同上跑 5 分钟 | 14412 次，0 错误，最大延迟 120ms，计数全 0 |
| 注入 3 次传输层失败（`fault_test=3`） | `recover=1`，`devnum 3 -> 4`，复位后 `wake answered 1 attempt(s) 0 ms`，节点自动回来 |

`stats` 是这个问题的诊断入口，以后再挂死先读它：

```
# cat /sys/module/dvb_usb_smit/parameters/stats
ok=2932 timeout=0 epipe=0 resync=0 recover=1 err_streak=0
```

`timeout` 涨 → 芯片不再应答（业务层）；`epipe` 涨 → 端点 stall 过但被 `clear_halt`
救回来了；`resync` 涨 → 出现过错位并被冲刷修正；`recover` 涨 → 触发过自愈复位。
四个都不动而画面临时断 → 问题在 TS 数据流（`0x84`），不在这条命令通道。

### 自愈（热复位）要多久

`recover=1` 之后是一次完整的冷启动：`vendor reset` → 芯片固件重启 → USB 重新枚举 →
`probe` → 唤醒 → `smit_init`。真机用 `tools/recover_time.py` 量的，两次结果完全一致：

| 阶段 | 耗时 | 说明 |
|---|---|---|
| 连续 3 次失败 → 发出复位 | ~0 ms | `schedule_work`，异步，不阻塞 ioctl |
| 复位 → 设备从总线消失 | 0.01s | 命令发完即重启 |
| 掉线 → 重新枚举完成 | **6.72s** | 芯片固件启动 + USB 枚举，主机侧压不掉 |
| 枚举 → 前端注册完成 | **3.09s** | 唤醒（0ms）+ `settle_ms`(300) + `smit_init`(2760ms) |
| **合计（复位 → 节点可用）** | **9.8s** | 两次测量：9.811s / 9.812s |

应用侧看到的时间更长一点：从第一条 error 到节点恢复约 **11.5s**，多出来的是
应用自己的轮询间隔和恢复后第一次 `open`+`FE_READ_STATUS` 的 1.6s 开销
（`dvb_frontend_open` 会跑一遍前端 init）。这段时间节点是**不存在**的，
播放器需要能容忍 `ENOENT` 并重试 `open`；一次 open 失败就放弃的播放器得手动重启它。

6.72s 是硬成本（芯片固件启动），想再压只能让芯片不重启，也就是不走复位这条路。

### 顺带修掉的两个会把自愈拖崩的缺陷

这两个是压测自愈时才暴露的，跟"复位要多久"直接相关：

1. **`smit_drain_in()` 开头无条件 `usb_clear_halt()`** —— 芯片不应答时它一定走满
   `USB_CTRL_GET_TIMEOUT`（5s）。实测 `init` 里 `cmd3` 花 5580ms、`cmd4` 花 10630ms，
   正好是「1 次 `clear_halt` + 一次 560ms 的 IN 超时」和「2 次 + 560ms」。
   触发 `resync` 的原因是**传输超时**，端点并没有 halt，清 halt 纯属自找 5s。
   后果很严重：芯片死掉时整套 `init` 跑了 **283 秒**（`init sequence took 283340ms`），
   期间 `rmmod` 完全卡在 D 状态，谁也动不了。→ `drain` 不再 `clear_halt`
   （真 stall 的路径在 `smit_bulk_in/out` 里单独处理）。

2. **唤醒包应答 ≠ 固件就绪。** 实测撞过一次：`wake answered after 1 attempt(s), 0 ms`
   之后 560ms 内 `cmd0` 仍然超时，整套 `init` 全 `-110` → 触发自愈复位 → 再撞 →
   连撞 3 次后 `3 resets without a successful probe, giving up`，设备直接躺那儿不动。
   唤醒包只是让芯片从 bootloader 跳到应用固件，它回的 ACK 只代表"这条命令收到了"。
   固件启动时间在软复位后尤其不稳。→ 唤醒成功后 `settle_ms`（默认 300ms）再发第一条命令。

另外 `SMIT_DO` 现在遇到传输层错误就跳过剩余命令：原来芯片已经死了还要把二十多条
命令一条条发完，每条白等 560ms（十几秒）才让 `probe` 失败。

改完实测：链路坏时单条命令 **10.6s → 0.56s**，`init` 失败从十几秒降到 **<1s**；
连续 3 轮冷启动 `init` 全部成功（`2730 / 2730 / 2760 ms`），没有再撞上空窗。

## 关键坑 5：全量代码审查找到的隐患

驱动跑通之后又从头到尾读了一遍，找出 12 处问题。其中 4 处是会真出事的。

### P0

**1. `smit_init()` 的 `u8 buf[1000]` 从来没初始化**

```c
u8 buf[1000];
...
SMIT_DO(smit_tuner_stat(udev, buf, 0));
*dtmb = strstr(buf + 18, "DTMB:Y") != NULL;
```

`strstr` 要求 NUL 结尾。设备正常最多回 255 字节（`datalen` 是 u8），可一旦
`smit_tuner_stat` 全部失败，这块栈数组就是未初始化的垃圾，`strstr` 会一路读到
1000 字节之外 —— **内核栈越界读**，轻则把随机内存当字符串（误判单/双模），
重则踩到 guard page 直接 oops。改成 `= {0,}`，最坏也只是退化成单模。

**2. `probe` / `init` 期间会触发自愈复位**

这是上一轮加自愈时自己埋的雷。冷启动的 `init` 有二十多条命令，芯片启动慢一点就会
连续几条超时 → 达到 `SMIT_RECOVER_STREAK` → workqueue 在设备**还没注册完**时复位 →
后续命令全 `-ENODEV` → `probe` 失败 → 又复位一次 → 循环。

之前用 `settle_ms` 掩盖了这个现象，但根因一直在。现在加 `smit_in_probe` 标志，
`smit_note()` 看到就拒绝触发；`probe` 改用统一出口（`goto out`）清标志。

**3. `disconnect` 不清 `smit_warm_devnum`**

`devnum` 是 USB 核心回收复用的。留着它，下一次插上来的冷芯片完全可能拿到同一个号，
`probe` 就误判成"热芯片"、跳过唤醒和 `init` —— 节点出来了但前端全哑（每条命令都
超时），再靠自愈绕一圈才恢复。自愈路径自己会清，**外部拔插不会**，所以必须在这里清。

**4. `smit_wake()` 每轮无条件 `usb_clear_halt()` × 2**

`usb_clear_halt` 内部是 `CLEAR_FEATURE` 控制请求，超时用 `USB_CTRL_GET_TIMEOUT`（5s）。
芯片连 control 都不应答时，每轮固定烧 10s，**1s 的预算形同废纸**。这和
`smit_drain_in` 是同一类错误，只是当时漏改了这一处。改为只在真 `-EPIPE` 时清 ——
超时的时候端点并没有 halt。

### P1

| # | 问题 | 后果 |
|---|---|---|
| 5 | `(buf[19] << 24)`：`u8` 提升成有符号 `int` 后移进符号位 | 未定义行为，UBSAN 直接报 `shift-out-of-bounds`。位模式碰巧对，编译器一优化就没保证了 |
| 6 | `stats` 五个计数器是普通 `++` | 多线程并发 ioctl 时丢更新，而这几个数正是排查挂死层次的唯一依据 → 改 `atomic_long_t` |
| 7 | `SMIT_PACK` 用 `char` 而非 `u8` | aarch64 上 `char` 无符号所以碰巧对；x86 上有符号会让 `data[i] == 0x80` 恒假、完成标志判断失效 |
| 8 | `init` 传输层失败时没有 `-ENODEV` 豁免 | 设备正在消失还会补一刀复位，把 7s 重枚举又拖一遍（`wake` 分支早就有这个豁免） |
| 9 | `module_init` 里 `smit_reset` 的注释写"计入尝试次数"，实际没走 `smit_reset_allowed()` | 注释骗人，会让人以为死循环保护少一次预算 |

P2 三处：`__smit_set` 增加 `len > sizeof(data)` 校验（它是导出接口，当前最大 37
但没有任何防线）；`DWORD_TO_BYTES` 宏加 `do-while(0)` 和参数括号；
`read_status` 读失败时把 `priv->lock` 置 0（否则 `read_signal_strength`/`read_snr`
会继续返回旧强度，看起来像信号还在）。

### 修完的回归数据

| 场景 | 结果 |
|---|---|
| 热 reload | **0.12s**，`devnum` 不变，四节点齐全 |
| 冷启动 ×3 | **8.0 / 8.0 / 8.1s** 全部成功，`timeout=0 resync=0 recover=0` |
| 4 线程同 fd 压 20s | 975 次，0 错误，最大延迟 100ms，计数全 0 |
| 注入 3 次故障验证自愈 | `recover=1`、`devnum` +1、节点自动回来、前端正常 |
| probe 期间自愈抑制 | 3 轮冷启动 `recover` 始终为 0，没有被误触发 |

`ok` 计数从 3 涨到 1953，正好等于 975 次 `read_status` × 每条 2 条命令 —— 原子计数准确。

## 关键坑 6：第二轮审查（频率缓存被污染 / 两条归因被推翻）

第二轮是纯代码 + 内核源码对照的方式做的，没有再靠真机撞。核心收获是
**"我们以为的根因"里有一条是错的，而"看着最无害的一行"反而是最严重的 bug**。

### P0-1 频率缓存被设备单位污染，失锁后再也调不回来

`icast_fe_get_frontend()` 无视 `props` 参数，直接写 `fe->dtv_property_cache`
（上游原版就是这样，我们照抄了）。核对 6.18 内核后确认两条路径待遇完全不同：

- `FE_GET_FRONTEND` → `dvb_get_frontend()` 先 `memcpy` 一份副本再交给驱动，
  注释写明是防"驱动乱写缓存"。驱动绕过副本，结果**用户读到的是旧值，
  真实缓存却被改写**。
- `dvb_frontend_add_event()` → 直接把真实 `&fe->dtv_property_cache` 传进来，
  而且**每次锁定状态变化都会调**，不需要应用参与。

而 `priv->frequency` 有两个写入方、两种单位：`set_frontend` 存 Hz（下发时 `/1000`），
`read_status` 锁定时用设备回报值覆盖（同一结构的回读字段，几乎必然是 kHz）。
于是：锁定 → `read_status` 把 `priv->frequency` 改成 474000 → `add_event` 调
`get_frontend` 把它写进 `cache.frequency` → 失锁后 `swzigzag_autotune` 以
`c->frequency` 为基准下发 → 往 **474 kHz** 调，永远锁不上。
这正好解释了"画面卡住不恢复"。

修法：`get_frontend` 只写 `props`；`read_status` 不再覆盖 `priv->frequency`；
`priv->frequency` 只保留一个单位（Hz）、一个写入方（`set_frontend`），
新增 `tuned` 标志确保只在真的调谐成功过之后才上报。

> 真机确认单位需要能收台的频点。不过**修复不依赖这个结论**：
> 只要干掉"两个单位混在一个字段"，无论设备回报的是 Hz 还是 kHz 都不会再污染缓存。

### P0-2 DTMB/DVB-C 判定会静默退化成"仅 DVB-C"

`strstr(buf + 18, "DTMB:Y")` 用的 `buf` 是整个 init 序列复用的同一个数组。
cmd14 之后的 `smit_tuner_stat` 一旦被跳过或失败，里面留着的是**上一条（cmd8）
的应答**，必然匹配不到 → 双模芯片被判成单模，probe 只打一句 `continuing`，
没有任何提示。

修法：先 `memset`，且只认明确读到的内容 —— `"DTMB:N"` 降级、`"DTMB:Y"` 升双模、
**读不到就保持传入的默认值**（双模 ops 是单模的超集，判错代价最小）。

### P0-3 传输层错误被协议错误掩盖

`SMIT_DO` 原来只记"第一个负返回值"。如果先来一条协议错误（`-1003`），
后面链路彻底断了（一串 `-110`）也不会覆盖 `err`，于是"跳过剩余命令"的逻辑
永远不触发：六条命令白跑，返回 `-1003`，probe 判成"非传输错误，继续"，
**注册一个已经死掉的设备**。修法：传输层错误无条件覆盖。

### P1 四条

| 问题 | 后果 | 修法 |
|---|---|---|
| `-EOVERFLOW` 当成功但没把数据拷回 | 调用方拿到**自己发出去的请求包**当应答解析（`usb_start_wait_urb` 在 EOVERFLOW 时已经回填 `actual_length`，数据也在缓冲区里） | `ret == -EOVERFLOW` 也 `memcpy`；`pipe & 0x80` 换成 `usb_pipein()` |
| `-EPIPE` 重试里最后一次失败后仍 `clear_halt` | 收发两端各清一次，芯片不应答时白烧 2×5s | 只在还会重试时才清，且只清出错的那个端点 |
| `smit_diag()` 用栈缓冲区 | `usb_control_msg` 全部 `-EAGAIN`，**请求根本没发出去**；"vendor 请求零区分度"的结论因此不成立 | 改 `kmalloc` |
| `module_exit` 先 `cancel_work_sync` 再 `usb_deregister` | 被取消的 work 不会跑，它持有的 `usb_get_dev` 永不释放；且 deregister 之前还可能排进新 work | 换顺序 + 取消时补 `usb_put_dev` |

### P2 六条

- `set_frontend` 不看返回值、永远 `return 0` —— 链路死了应用也看到"调谐成功"，
  `priv` 还会被更新成假状态。改成失败上抛且不更新。
- 信号强度用魔数换算、没有 clamp —— 实测算过：`x=101` 得 654、`x=255` 得 36042
  （`x*65535/100` 超出 16 位被截断），表现为"信号一下掉到几乎没有"。加 clamp。
- `__smit_check()` 的返回值在 `__smit_tuner_stat()` 里被丢弃 —— 完成标志轮询
  在传输层挂掉时不算失败，自愈不会启动。
- `smit_note()` 把 `-ENOMEM` 也算链路故障 —— 连续几次内存紧张就能把好芯片复位；
  `module_init` 的 ping 返回 `-ENOMEM` 还会被当成冷芯片去复位。
- `len > sizeof(smit.data)` 这条校验永远为假（`len` 是 `u8`，上限 255）。
  改成 `unsigned int` 参数 + 同时挡 `> 0xFF`（datalen 字段是 `u8`）。
- `icast_fe_init` 不清 `priv->lock`；`read_status` 只报 `SIGNAL|LOCK`（缺
  `CARRIER/SYNC/VITERBI`，个别应用会认为没锁）；`settle_ms` 可写负数（→ 巨大
  `msleep`）；`smit_warm_devnum` 只比 devnum 不比 busnum；声明了
  `HAS_PID_FILTER` 却没实现 `.pid_filter` 回调（Full-speed 下 `max_feed_count=0`，
  demux 一个 filter 都没有）。全部修掉，`pid_filter` 那个直接去掉声明。

### 一条被撤回的归因

"跑一阵子挂死 = 没有互斥导致 OUT 包交错"——**错**。详见「关键坑 4」第 1 条。
前端操作被核心的 `fepriv->sem` 完全串行化，那次 4 线程压测测的是核心的信号量。
锁留着有用（护 `probe`/自愈路径），但它不是解药。

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
- 冷启动那 10s 里有 6.7s 是芯片固件重启后的 USB 枚举，主机侧压不掉。
- 自愈复位（命令通道卡死时自动触发）同样要 ~9.8s：6.7s 枚举 + 3.1s 唤醒/init。
  这段时间节点是**不存在**的，播放器要能容忍 `ENOENT` 并重试 `open`。
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
insmod dvb-usb-smit.ko autorecover=0    # 关掉命令通道卡死时的自动复位（默认开）
insmod dvb-usb-smit.ko settle_ms=0      # 唤醒后不等固件就绪直接发命令（复现空窗竞态）
```

运行时可直接读写（不用重新 insmod）：

```sh
# 链路健康度，排查"挂着不动"先看这个
cat /sys/module/dvb_usb_smit/parameters/stats

# 故障注入：让接下来 N 条命令直接返回 -ETIMEDOUT，验证自愈链路
echo 3 > /sys/module/dvb_usb_smit/parameters/fault_test
```

