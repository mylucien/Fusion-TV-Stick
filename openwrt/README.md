# 在 OpenWrt / PonWrt 源码树里编译

## 结论先说

**这个仓库原本不能直接丢进 OpenWrt 编译**，缺的是「包」，不是驱动代码。
现在补上了 `openwrt/Makefile`，按下面的步骤放进去就能用 buildroot 编成 `.ipk`。

三个原来挡住它的东西：

| 现象 | 原因 |
|---|---|
| buildroot 根本看不见这个包 | OpenWrt 只扫 `package/**/Makefile` 里 `KernelPackage` / `KernelModule` 定义的包，根目录那个 `Makefile` 是普通的 kbuild 包装，没人会去找它 |
| 包的编译流程接不上 | 根 `Makefile` 是「外部模块」模式：需要 `KERNEL_DIR=<准备好的内核树>`，且 `kern/` 得先由 `scripts/prepare-src.sh` 从内核源码里抠出来 |
| 编出来装不上 | `insmod` 校验 `.gnu.linkonce.this_module` 的字节数必须等于内核的 `sizeof(struct module)`。这个值完全由目标内核 `.config` 决定（本设备是 **704**），在别的 target / 别的配置下编出来就是另一个数，直接 `Invalid module format` |

最后一条最关键：**必须用本设备（airoha/an7581，内核 6.18.55）那套配置的源码树**。
官方 OpenWrt 别的机型的树编出来是装不上的 —— 不是驱动的问题，是内核 ABI 不同。

## 步骤

以源码树根目录为当前目录（`make menuconfig` 能跑的那个目录）：

```sh
mkdir -p package/kernel/dvb-usb-smit

# 1) 包定义
cp /path/to/Fusion-TV-Stick/openwrt/Makefile package/kernel/dvb-usb-smit/Makefile

# 2) 驱动源码（整个仓库放进来，Makefile 里的 DVB_SMIT_SRC 默认就指这里）
git clone https://github.com/mylucien/Fusion-TV-Stick.git \
	package/kernel/dvb-usb-smit/repo

# 3) 选中
make menuconfig
#   Kernel modules  --->  Video Support  --->  <M> kmod-dvb-usb-smit

# 4) 编
make package/kernel/dvb-usb-smit/compile V=s
# 或者整编：make -j$(nproc)
```

产物在 `bin/targets/*/*/packages/kmod-dvb-usb-smit_*.ipk`，装到设备上就是三个模块：

```
/lib/modules/<kver>/dvb-core.ko
/lib/modules/<kver>/dvb-usb.ko
/lib/modules/<kver>/dvb-usb-smit.ko
/etc/modules.d/30-dvb-usb-smit        # 开机按 dvb-core -> dvb-usb -> dvb-usb-smit 加载
```

只想拿 `.ko` 手动 insmod 也行，编完直接去 `build_dir/target-*/linux-*/dvb-usb-smit/` 取。

## 这个包做了什么

`Build/Prepare`：把 `repo/` 整个拷进 `PKG_BUILD_DIR`，然后

```
KERNEL_DIR=$(LINUX_DIR) bash scripts/prepare-src.sh
```

也就是从 `$(LINUX_DIR)/drivers/media/` 里把 `dvb-core` / `dvb-usb` 源码抠到 `kern/`
（外加 `dvb-pll.h`），再核对文件清单和 `Makefile` 里的 `obj-m` 是否对得上。
**没有复用内核里已有的 `CONFIG_DVB_CORE`**，原因：设备固件里压根没有 DVB 栈，
`CONFIG_MEDIA_SUPPORT` / `CONFIG_DVB_CORE` 都是关的，只能自带一份。

`Build/Compile`：调仓库自带的 kbuild 包装

```
make -C $(PKG_BUILD_DIR) KERNEL_DIR=$(LINUX_DIR) ARCH=arm64 \
     CROSS_COMPILE=$(TARGET_CROSS) NO_RC=1 KBUILD_MODPOST_WARN=1 all
```

`KBUILD_MODPOST_WARN=1` 是必须的：`dvb-core` / `dvb-usb` / `dvb-usb-smit` 是同一批
编出来的外部模块，内核的 `Module.symvers` 里没有它们的符号，modpost 报未解析属预期。

编完还会跑 `openwrt/postcheck.sh`，把每个 `.ko` 的 vermagic 和
`this_module` 大小打出来。**看到 704 才对**，看到别的大小就说明这套树的
内核配置跟设备不一样。

## 注意事项

1. **不要同时选 OpenWrt 自带的 `kmod-dvb-core`**。两个包各装一个同名
   `dvb-core.ko`，后装的会把先装的覆盖掉，符号对不上就加载失败。
2. **内核源码要完整**。`prepare-src.sh` 需要 `$(LINUX_DIR)/drivers/media/dvb-core`
   和 `.../usb/dvb-usb`。OpenWrt 不会裁剪这些目录，但如果你的树打了「精简内核源码」
   的补丁就得自己放回来。
3. **`NO_RC` 默认 1**（不编 `rc-core`）。驱动没有 `.rc`，遥控走空实现。
   想要 `rc-core.ko` 就 `make ... NO_RC=0`，前提是内核开了 `CONFIG_INPUT`。
4. **开机自动加载**会走 `module_init` 的探活：芯片是冷的就地复位，等 7s 左右固件
   启动（`/dev/dvb/adapter0/*` 在这段时间不存在，播放器要能重试 open）。
   这就是设计里的冷启动路径，实测 8.06s。
5. **内核开了 `CONFIG_MODVERSIONS` 也没关系** —— 在真树里编，`Module.symvers`
   是齐全的，比 CI 那套「vanilla 内核 + 仿 OpenWrt 配置」更准。设备固件是
   `CONFIG_MODVERSIONS=n`，所以只要 vermagic 和 `struct module` 对上就能装。
6. `DVB_SMIT_SRC` 可以覆盖源码位置，不想 clone 到包目录里就：

   ```sh
   make package/kernel/dvb-usb-smit/compile \
        DVB_SMIT_SRC=/home/me/src/Fusion-TV-Stick
   ```

## 和现在 GitHub Actions 产物的关系

CI 那条路是「下载 kernel.org 的 linux-6.18.55 + 用 `configs/` 里的 OpenWrt delta
拼一份仿真 `.config` + 树外编译」，好处是**不用装一整套 buildroot**，一分钟出三个 `.ko`，
坏处是那份 `.config` 只是模仿，得靠 `struct module = 704`、`i_rdev = 68` 两条断言
反向确认没偏。真在设备同源的树里编，这两个数天然就对，不用再猜。
两条路可以并行存在：日常迭代用 CI，要出固件集成包（`.ipk`）就用这里的包。
