# dvb-usb-smit for airoha/an7581 (Nokia XG-040G-MD)

在 CI 里交叉编译 SMIT iCast DTMB/DVBC USB dongle 驱动（VID:PID `29df:0001`），
目标设备内核 **6.18.55 / aarch64_cortex-a53 / SMP mod_unload aarch64**。

## 为什么需要自己编

设备固件（PonWrt r0-03c1345）**没有编 USB 主机控制器**：
`/proc/kallsyms` 里 `xhci_*` 符号数为 0，`/sys/class/usb_host` 不存在，
dmesg 零条 xhci 记录。设备树里两个 `mediatek,mtk-xhci` 节点 `status=okay`、
`airoha-usb-phy` 已加载 —— 硬件在，驱动没编。

同时固件也没有任何 DVB/RC/INPUT 的 kmod。

所以产物是 5 个模块，加载顺序：

```
xhci-hcd.ko
  xhci-plat-hcd.ko   (含 CONFIG_USB_XHCI_MTK，才能绑定 mediatek,mtk-xhci)
     ↓
dvb-core.ko  →  dvb-usb.ko  →  dvb-usb-smit.ko
```

## 构建方式

- 内核源码：kernel.org 的 **linux-6.18.55**（与设备 `uname -r` 一致）
- `.config`：由 `configs/` 下 OpenWrt(PonWrt) 的 generic + target 两份 delta 配置合并，
  再显式对齐 vermagic 相关项（`LOCALVERSION=""`、`SMP=y`、`MODULE_UNLOAD=y`、
  `MODVERSIONS=n`、`PREEMPT_NONE`）
- USB / DVB 全部按 `=m` 编，与设备现有的 `usbcore.ko` / `mc.ko` / `videodev.ko` 保持一致
- `DVB_MMAP=n`：避免拖入 `videobuf2` 依赖
- `NO_RC=1`：驱动 `properties` 里没有 `.rc`，远端遥控走空实现，行为等价但少一个 `rc-core.ko`

## 目录

```
configs/   OpenWrt generic + target 内核配置 delta
src/       smit.c(协议) icast-fe.c(前端) icast-smit.c(桥接) + 头文件
scripts/   ci-prep-config.sh（合成 .config） prepare-src.sh（抠 DVB 源码到 kern/）
Makefile   kbuild 包装：dvb-core / dvb-usb / dvb-usb-smit
```

驱动源码来自 `github.com/nxdong520/iCast`（GPL-2.0），已做三处移植改动：

1. 合并成单模块，前端直接调 `icast_fe_attach()`，不再走 `dvb_attach()`/`symbol_request()`
2. `dvb-usb-remote.c` 换成空实现（`src/dvb-usb-remote-stub.c`）
3. 去掉各文件的 `MODULE_*` 重复声明

## 产物校验

artifact 里每个 `.ko` 应满足：

```
vermagic=6.18.55 SMP mod_unload aarch64
ELF64, AArch64
```

## 设备上安装

```sh
insmod xhci-hcd.ko
insmod xhci-plat-hcd.ko        # dmesg 应出现 xhci-mtk ... 并出现 /sys/class/usb_host
insmod dvb-core.ko
insmod dvb-usb.ko
insmod dvb-usb-smit.ko         # 插上 dongle 后应枚举 29df:0001
```

## 风险

`.config` 是从 PonWrt 目标的 delta 配置反推的，不是设备固件里那一份真配置
（设备没有 `/proc/config.gz`）。vermagic 能保证一致，但如果固件在
`CONFIG_PM` / `CONFIG_USB_OTG` / `CONFIG_MEDIA_CONTROLLER` 这类
**会改变结构体布局**的选项上与反推结果不同，模块能 load 但访问结构体字段会错位。
真出问题时的定位方法：`insmod` 后立刻 `dmesg`，看是在哪一步崩的。
