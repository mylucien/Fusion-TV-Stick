// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SMIT iCast DTMB/DVC driver
 *
 * Copyright (C) 2026 Xiaodong Ni <nxiaodong520@gmail.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/usb.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
#include <dvb_frontend.h>
#else
#include <media/dvb_frontend.h>
#endif

#include "dvb-usb.h"
#include "icast-fe.h"
#include "smit.h"

//#define CONFIG_DYNAMIC_DEBUG

#define USB_VID_SMIT							0x29DF
#define USB_PID_SMIT_ICAST				0x0001

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

/* 热芯片跳过初始化时拿不到 tuner 信息，dtmb 只能取默认值。
 * 必须默认 1（双模 ops）：它同时声明 SYS_DVBT + SYS_DVBC_ANNEX_A，
 * 是单模 ops 的超集，取错也不会少功能。
 * 冷启动路径会被 smit_init 里的实际检测覆盖。 */
static struct icast_fe_config icast_config = {
	.dtmb = 1,
};

/* ---- 调试开关（默认全关，不影响正常行为） ----------------------------
 *
 * trace         : 打印 smit_init 每条命令的耗时
 * skip_wake     : probe 时跳过状态判定，直接跑初始化序列
 *                 （用来复现"热芯片重跑 init 会把自己打死"）
 * reset_on_exit : 卸载时做硬件复位，强制下一次 probe 走冷启动路径
 * diag_probe    : probe 时打印设备描述符和 vendor control 请求的应答
 */
bool smit_trace;
static bool smit_skip_wake;
static bool smit_reset_on_exit;
static bool smit_diag_probe;

module_param_named(trace, smit_trace, bool, 0644);
MODULE_PARM_DESC(trace, "print per-command timing of the init sequence");
module_param_named(skip_wake, smit_skip_wake, bool, 0644);
MODULE_PARM_DESC(skip_wake, "skip the wake-up and run the init sequence directly");
module_param_named(reset_on_exit, smit_reset_on_exit, bool, 0644);
MODULE_PARM_DESC(reset_on_exit, "hardware-reset the chip on disconnect (forces a cold next probe)");
module_param_named(diag_probe, smit_diag_probe, bool, 0644);
MODULE_PARM_DESC(diag_probe, "dump descriptors and vendor control replies during probe");

static int icast_frontend_attach(struct dvb_usb_adapter *adap)
{
	/* Frontend and bridge live in the same module, so call it directly
	 * instead of dvb_attach()/symbol_request(). One less .ko to load. */
	if ((adap->fe_adap[0].fe = icast_fe_attach(adap, &icast_config)) != NULL) {
		return 0;
	}
	info("not attached iCast usb device");
	return -ENODEV;
}

static struct usb_device_id icast_id_table[] = {
	{USB_DEVICE(USB_VID_SMIT, USB_PID_SMIT_ICAST)},
	{}
};

MODULE_DEVICE_TABLE(usb, icast_id_table);

static struct dvb_usb_device_properties icast_properties = {
	.num_adapters = 1,

	.adapter = {
		{
		.num_frontends = 1,
		.fe = {{
			.caps = DVB_USB_ADAP_HAS_PID_FILTER,
			.frontend_attach  = icast_frontend_attach,
			.stream = {
				.type = USB_BULK,
				.count = 8,
				.endpoint = SMIT_BULK_ENDPOINT_STREAM,
				.u = {
					.bulk = {
						.buffersize = 8192,
					}
				}
			},
		}},	
		},
	},
	.num_device_descs = 1,
	.devices = {
		{
			.name = "iCast DVB USB DONGLE adapter",
		  .warm_ids = {&icast_id_table[0], NULL},
		},
	}
};

/* 唤醒序列 "FE 00 10 00" 只对"刚上电、固件还停在等待握手状态"的冷芯片有效。
 *
 * 必须只在确认为冷芯片之后才发：真机对照实验里，热芯片（已经初始化过一次）
 * 收到这个包不但不应答，还会让之后的命令全部超时 —— 它本身就是毒药。
 * 所以调用顺序是先 smit_set(0x82) 探活，探不到才允许发唤醒包。
 *
 * 冷芯片唤醒实测：第一次就应答（0 ms）；热芯片重试 9 次 / 6270 ms 全超时，
 * 所以 1s 预算足够，没必要拉长。
 */
#define SMIT_WAKE_BUDGET_MS	4000
#define SMIT_WAKE_TIMEOUT_MS	300
#define SMIT_WAKE_BUF_SIZE	512

static int smit_wake(struct usb_device *udev)
{
	const u8 wake[4] = { 0xFE, 0x00, 0x10, 0x00 };
	unsigned long t0 = jiffies, deadline;
	int size, ret = 0, attempt = 0;
	u8 *buf;

	buf = kmalloc(SMIT_WAKE_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	deadline = t0 + msecs_to_jiffies(SMIT_WAKE_BUDGET_MS);

	for (;;) {
		attempt++;

		/* 清掉可能残留的 stall，同时把 data toggle 复位 */
		usb_clear_halt(udev,
			usb_sndbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_SEND));
		usb_clear_halt(udev,
			usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE));

		memcpy(buf, wake, sizeof(wake));
		ret = usb_bulk_msg(udev,
			usb_sndbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_SEND),
			buf, sizeof(wake), &size, SMIT_WAKE_TIMEOUT_MS);
		if (!ret) {
			ret = usb_bulk_msg(udev,
				usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE),
				buf, SMIT_WAKE_BUF_SIZE, &size,
				SMIT_WAKE_TIMEOUT_MS);
			/* 回包长度不确定：设备给的数据比缓冲区多只会报
			 * EOVERFLOW，这时包其实已经收到了，不算失败 */
			if (ret == -EOVERFLOW)
				ret = 0;
		}

		if (!ret) {
			dev_info(&udev->dev,
				 "wake answered after %d attempt(s), %u ms\n",
				 attempt, jiffies_to_msecs(jiffies - t0));
			kfree(buf);
			return 0;
		}

		/* 端点 stall 或超时都可能是"还没准备好"；
		 * 其它错误（设备没了/被关掉了）重试没有意义 */
		if (ret != -EPIPE && ret != -ETIMEDOUT)
			break;
		if (time_after_eq(jiffies, deadline))
			break;

		msleep(100);
	}

	dev_info(&udev->dev,
		 "wake gave up after %d attempt(s) / %u ms, last error %d\n",
		 attempt, jiffies_to_msecs(jiffies - t0), ret);
	kfree(buf);
	return ret;
}

/* 采集冷/热芯片的描述符与 vendor control 应答（diag_probe=1）。
 * 实测两者完全一致，没有区分度，留着只为以后换芯片/固件时快速复测。 */
static void smit_diag(struct usb_device *udev)
{
	const struct usb_device_descriptor *d = &udev->descriptor;
	static const struct { u8 type; u8 req; u16 val; u16 idx; u16 len; } t[] = {
		{ 0xC0, 0xA0, 0x0000, 0, 64 },
		{ 0xC0, 0x01, 0x0000, 0, 8  },
		{ 0xC0, 0x81, 0x0000, 0, 64 },
		{ 0xC0, 0x82, 0x0000, 0, 64 },
		{ 0xC0, 0xFF, 0x0000, 0, 64 },
	};
	u8 buf[64];
	int i, ret;

	dev_info(&udev->dev,
		 "diag: bcdUSB=%04x bcdDevice=%04x id=%04x:%04x cfg=%u cls=%02x/%02x/%02x iMan=%u iProd=%u iSer=%u speed=%d\n",
		 d->bcdUSB, d->bcdDevice, d->idVendor, d->idProduct,
		 d->bNumConfigurations, d->bDeviceClass, d->bDeviceSubClass,
		 d->bDeviceProtocol, d->iManufacturer, d->iProduct,
		 d->iSerialNumber, udev->speed);

	for (i = 0; i < ARRAY_SIZE(t); i++) {
		int n = 0;

		memset(buf, 0, sizeof(buf));
		ret = usb_control_msg(udev,
			usb_rcvctrlpipe(udev, 0), t[i].req, t[i].type,
			t[i].val, t[i].idx, buf, t[i].len, 300);
		if (ret > 0)
			n = min(ret, 16);
		dev_info(&udev->dev, "diag: ctrl req=%02x val=%04x -> %d %*ph\n",
			 t[i].req, t[i].val, ret, n, buf);
	}

	if (d->iProduct) {
		ret = usb_string(udev, d->iProduct, buf, sizeof(buf) - 1);
		if (ret > 0) {
			buf[ret] = 0;
			dev_info(&udev->dev, "diag: iProduct=\"%s\"\n", buf);
		}
	}
}

/* 探针：初始化序列的第一条命令本来就是 "req 0x82 读 1 字节" 的只读查询。
 * 热芯片 0ms 就回 0，冷芯片必然超时（-ETIMEDOUT）。
 *
 * 必须走 smit_set() 而不是手搓 usb_bulk_msg：直接拿栈缓冲区调 usb_bulk_msg
 * 实测会返回 -EAGAIN(-11)，看起来像错误其实根本不是设备给的答案，
 * 会把热芯片误判成冷芯片。smit_set 内部用 kmalloc 缓冲区，行为可靠。
 */
static int smit_ping(struct usb_device *udev)
{
	u8 cmd0[1] = { 0x01 };

	return smit_set(udev, 0x82, cmd0, 1, 0);
}

/* 模块加载时在总线上找到的、确认已初始化的芯片的 devnum。
 * devnum 每次重新枚举都会变，所以它同时充当"这个芯片还是不是当年那个"的凭据。 */
static int smit_warm_devnum = -1;

/* ---- 芯片状态判定（两个遗留问题的共同解）-----------------------------
 *
 * 目标：热芯片（已经初始化过一次）不要走"复位 -> 重新枚举 7s -> 再 init 7s"。
 *
 * 已被证伪的手段：
 *   - 设备描述符 / 字符串 / vendor control 请求：冷热完全一样
 *     （bcdDevice=0200、iProduct="DONGLE"、几个 0xC0 请求一律 -11），零区分度。
 *   - 在 probe 里用 bulk 命令探活：冷芯片收不到应答时，探针的 5 个字节已经
 *     留在它的 OUT FIFO 里，紧跟着的唤醒包 FE 00 10 00 会被拼成畸形命令，
 *     握手必然失败（实测 4s / 10 次重发都冲不掉）-> probe 失败 -> 复位 ->
 *     重枚举 -> 再探测 -> 再污染，无限循环。
 *
 * 所以探活必须发生在 probe **之外**：模块初始化时、usb_register 之前先问一句。
 *   - 有应答 -> 记下 devnum，probe 时看到同一个 devnum 就直接注册（~0s）
 *   - 无应答 -> 是冷芯片，但探针已经弄脏了 FIFO。就地复位让它重新枚举成
 *              干净的冷芯片，之后 usb_register 触发的 probe 走正常唤醒+初始化。
 * 因为这时还没有注册 usb_driver，复位不会引起任何 probe，也就不可能循环。
 */
static int smit_pick_device(struct usb_device *udev, void *data)
{
	struct usb_device **found = data;

	if (udev->descriptor.idVendor == USB_VID_SMIT &&
	    udev->descriptor.idProduct == USB_PID_SMIT_ICAST) {
		*found = usb_get_dev(udev);
		return 1;	/* 找到就停 */
	}
	return 0;
}

static int smit_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	int ret;

	if (smit_diag_probe)
		smit_diag(udev);

	if (smit_warm_devnum == (int)udev->devnum) {
		dev_info(&udev->dev,
			 "chip is already initialized (devnum %d), skipping wake + init\n",
			 udev->devnum);
		smit_warm_devnum = -1;	/* 只复用一次 */
		goto register_device;
	}

	if (smit_skip_wake)
		goto do_init;

	ret = smit_wake(udev);
	if (ret) {
		/* 唤醒包也不理：芯片卡在既不在应用模式、又听不懂唤醒的中间态，
		 * 或者干脆是热芯片（唤醒包对它就是毒药）。只有硬件复位能救。 */
		dev_info(&udev->dev,
			 "device not responding (%d), resetting to re-enumerate\n",
			 ret);
		smit_reset(udev);
		return ret;
	}
do_init:
	ret = smit_init(udev, &icast_config.dtmb);
	if (smit_is_transport_err(ret)) {
		dev_err(&udev->dev, "init failed at USB level: %d\n", ret);
		/* 实验模式下不要复位：否则同一次 insmod 里会不断
		 * 复位→重枚举→再失败，变成死循环 */
		if (!smit_skip_wake)
			smit_reset(udev);
		return ret;
	}
	if (ret)
		/* 协议级错误（设备回了怪数据）上游一直是忽略的，
		 * 这里只记录，免得把本来能用的设备判死 */
		dev_info(&udev->dev, "init sequence reported %d, continuing\n", ret);

register_device:
	ret = dvb_usb_device_init(intf, &icast_properties, THIS_MODULE, NULL,
				  adapter_nr);
	if (ret == 0)
		return 0;

	dev_err(&udev->dev, "dvb_usb_device_init failed: %d\n", ret);
	smit_reset(udev);
	return ret;
}

static void smit_disconnect(struct usb_interface *intf)
{
	struct usb_device *udev = interface_to_usbdev(intf);

	dvb_usb_device_exit(intf);

	/* 默认不复位芯片。
	 * 以前这里复位是为了"让下一次 insmod 的第一次 probe 是干净的"，代价是
	 * 设备重新枚举 7s；现在 probe 会先探活，热芯片直接复用，重新枚举纯属浪费。
	 * 需要验证冷启动路径时用 reset_on_exit=1 手动复位。 */
	if (smit_reset_on_exit)
		dev_info(&udev->dev, "reset on exit: %d\n", smit_reset(udev));
}

static struct usb_driver smit_driver = {
	.name = KBUILD_MODNAME,
	.probe = smit_probe,
	.disconnect = smit_disconnect,
	.id_table = icast_id_table,
};

/* 重启/关机不再需要任何动作：probe 探测到热芯片会直接复用，不再有
 * "开机第一次 probe 失败"这回事，reboot_notifier 已删除。 */

static int __init smit_module_init(void)
{
	struct usb_device *udev = NULL;
	int result, ret;

	/* 加载时总线上可能已经挂着 dongle（insmod 重载、开机时开机脚本加载）。
	 * 趁还没注册 usb_driver 先问一次芯片状态，见 smit_probe 上面的说明：
	 *   - 热芯片：记下 devnum，probe 直接注册，不再复位重枚举
	 *   - 冷芯片：探针弄脏了 FIFO，就地复位（此时没有 driver，不会引发 probe） */
	usb_for_each_dev(&udev, smit_pick_device);
	if (udev) {
		ret = smit_ping(udev);
		if (!smit_is_transport_err(ret)) {
			smit_warm_devnum = udev->devnum;
			pr_info("dvb_usb_smit: chip already initialized (devnum %d), will reuse it\n",
				udev->devnum);
		} else {
			pr_info("dvb_usb_smit: cold chip (ping %d), resetting it for a clean probe\n",
				ret);
			smit_reset(udev);
		}
		usb_put_dev(udev);
	}

	result = usb_register(&smit_driver);
	if (result) {
		err("usb_register failed. Error number %d", result);
		return result;
	}
	return 0;
}

static void __exit smit_module_exit(void)
{
	usb_deregister(&smit_driver);
}

module_init(smit_module_init);
module_exit(smit_module_exit);

MODULE_DESCRIPTION("SMIT iCast dvb driver");
MODULE_AUTHOR("Xiaodong Ni <nxiaodong520@gmail.com>");
MODULE_LICENSE("GPL");

