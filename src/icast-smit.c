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
 */
bool smit_trace;
static bool smit_skip_wake;
static bool smit_reset_on_exit;

module_param_named(trace, smit_trace, bool, 0644);
MODULE_PARM_DESC(trace, "print per-command timing of the init sequence");
module_param_named(skip_wake, smit_skip_wake, bool, 0644);
MODULE_PARM_DESC(skip_wake, "skip the state probe and always run the init sequence");
module_param_named(reset_on_exit, smit_reset_on_exit, bool, 0644);
MODULE_PARM_DESC(reset_on_exit, "hardware-reset the chip on disconnect (forces a cold next probe)");

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
#define SMIT_WAKE_BUDGET_MS	1000
#define SMIT_WAKE_TIMEOUT_MS	500
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

		msleep(200);
	}

	dev_info(&udev->dev,
		 "wake gave up after %d attempt(s) / %u ms, last error %d\n",
		 attempt, jiffies_to_msecs(jiffies - t0), ret);
	kfree(buf);
	return ret;
}

/* ---- 芯片状态判定（两个遗留问题的共同解）----------------------------
 *
 * 实测结论（2026-10-25，真机对照实验）：
 *
 *   热芯片发唤醒包 FE 00 10 00        -> 无应答，且之后所有命令全超时
 *   热芯片重跑初始化序列              -> cmd0/cmd3 都应答，但 cmd4（写寄存器
 *                                        0x9F/0x80/0x10）之后固件彻底不回话
 *   热芯片只发一条命令（不唤醒不 init）-> 立即正常应答（0ms，返回数据）
 *
 * 也就是说芯片初始化过一次之后，命令通道一直是活的；把它打死的恰恰是
 * 我们自己的"重新初始化"。原方案（复位 -> 重新枚举 7s -> 再 init 7s）
 * 既慢又完全没必要，只是没人试过"直接问一句"。
 *
 * 因此 probe 分两条路：
 *   探测有应答 -> 芯片在应用模式，跳过唤醒和初始化，直接注册前端（约 0ms）
 *   探测无应答 -> 冷芯片，走唤醒 + 完整初始化（插拔 dongle、整机断电后）
 */
static int smit_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	u8 cmd0[1] = { 0x01 };
	int ret;

	if (smit_skip_wake) {
		/* 实验路径：先打一发纯命令探测，看热芯片的命令通道还通不通。
		 * 0x81 就是前端 read_status 用的那条命令，芯片若在应用模式应当应答。 */
		u8 pbuf[900];

		ret = smit_tuner_stat(udev, pbuf, 0);
		dev_info(&udev->dev,
			 "skip_wake: raw tuner_stat probe -> %d (buf[0]=%02x buf[1]=%02x)\n",
			 ret, pbuf[0], pbuf[1]);
		goto do_init;
	}

	/* 初始化序列的第一条命令本来就是 req 0x82 的只读查询，拿它当探针：
	 * 有应答（0 或协议级错误码）说明芯片在应用模式，USB 层错误码说明是冷芯片。 */
	ret = smit_set(udev, 0x82, cmd0, 1, 0);
	if (!smit_is_transport_err(ret)) {
		dev_info(&udev->dev,
			 "chip is already initialized (probe reply %d), skipping wake + init\n",
			 ret);
		goto register_device;
	}

	dev_info(&udev->dev, "cold chip (probe %d), waking it up\n", ret);
	ret = smit_wake(udev);
	if (ret) {
		/* 唤醒包也没人理：说明芯片卡在既不在应用模式、又听不懂唤醒的
		 * 中间态。只有硬件复位能救，代价是重新枚举 6~7 秒。 */
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
	int result;

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

