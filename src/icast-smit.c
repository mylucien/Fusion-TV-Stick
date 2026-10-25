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
#include <linux/reboot.h>

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

/* ---- 调试开关（默认全关，不影响正常行为） ----------------------------
 *
 * trace      : 打印 smit_init 每条命令的耗时
 * skip_wake  : probe 时跳过唤醒包，直接跑初始化序列
 *              （用来判定"热芯片到底还不还听得懂命令"）
 * keep_warm  : 卸载时不做硬件复位，把芯片留在"已初始化"的热状态
 *              （配合 skip_wake 做对照实验）
 */
bool smit_trace;
static bool smit_skip_wake;
static bool smit_keep_warm;

module_param_named(trace, smit_trace, bool, 0644);
MODULE_PARM_DESC(trace, "print per-command timing of the init sequence");
module_param_named(skip_wake, smit_skip_wake, bool, 0644);
MODULE_PARM_DESC(skip_wake, "skip the wake-up packet and go straight to init");
module_param_named(keep_warm, smit_keep_warm, bool, 0644);
MODULE_PARM_DESC(keep_warm, "do not hardware-reset the chip on disconnect");

static struct icast_fe_config icast_config = {
	.dtmb = 0,
};

/* 当前已成功 probe 的设备，只给重启通知用（重启时把芯片复位回冷状态） */
static struct usb_device *smit_udev;

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

/* 唤醒序列 "FE 00 10 00" 只对"刚上电/刚复位、固件还没被初始化过"的芯片有效。
 *
 * 实测：设备被任何一轮驱动实例初始化过之后，不只是唤醒包，连后续所有命令包的
 * IN 应答都超时（-ETIMEDOUT）——芯片根本不回话，只有 smit_reset() 的硬件复位
 * 能把它救回来，代价是设备重新枚举 6~7 秒。这就是"首次 probe 必失败"的来源。
 *
 * 冷/热芯片的实测对比：复位重新枚举之后 probe，唤醒包第一次就应答（0 ms）；
 * 热芯片则 9 次 / 6270 ms 全超时。所以不需要长重试预算。
 *
 * 处理办法：
 *   - 卸载时（smit_disconnect）复位，让下一次 insmod 的第一次 probe 是干净的；
 *   - 重启/关机时（smit_reboot_notify）也复位，否则下次开机的第一次 probe
 *     会撞上热芯片（路由器重启并不切断 dongle 的状态）；
 *   - probe 里唤醒包只给 1s 预算，不应答就复位返回，不再白跑 smit_init（约 12s）；
 *   - 只有 -EPIPE / -ETIMEDOUT 值得重试。
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

static int smit_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
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

	ret = smit_wake(udev);
	if (ret) {
		/* 固件已经初始化过 / 端点坏了：芯片不会应答任何命令，
		 * 只能硬件复位让它重新枚举，以干净状态再来一次。
		 * 这里不要再去试 smit_init —— 那是 20 多条命令 × 500ms 超时。 */
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

	ret = dvb_usb_device_init(intf, &icast_properties, THIS_MODULE, NULL,
				  adapter_nr);
	if (ret == 0) {
		usb_get_dev(udev);	/* 重启通知里要用，防止 udev 提前释放 */
		smit_udev = udev;
		return 0;
	}

	dev_err(&udev->dev, "dvb_usb_device_init failed: %d\n", ret);
	smit_reset(udev);
	return ret;
}

static void smit_disconnect(struct usb_interface *intf)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	int ret, i;

	if (smit_udev == udev) {
		smit_udev = NULL;
		usb_put_dev(udev);	/* 配平 probe 里的 usb_get_dev */
	}

	usb_get_dev(udev);	/* 下面的轮询期间保证 udev 不被释放 */

	dvb_usb_device_exit(intf);

	if (smit_keep_warm) {
		dev_info(&udev->dev, "keep_warm: leaving chip initialized\n");
		usb_put_dev(udev);
		return;
	}

	/* 把芯片复位回"刚插上"的状态。否则下一次 insmod 时固件还在跑，
	 * 唤醒包和命令包全都超时，probe 必然先失败一次再靠复位自愈。
	 * 设备已经被拔掉时这个控制请求会返回 -ENODEV，无害。 */
	ret = smit_reset(udev);
	dev_info(&udev->dev, "reset on disconnect: %d\n", ret);

	/* 芯片复位是"发完命令才重启"，实测要 ~0.8s 才真正从总线上消失。
	 * 如果这里立刻返回，紧跟着的 insmod 会 probe 到一个正在重启的设备，
	 * 必然 -ETIMEDOUT 失败一次。所以等它掉线再走，上限 1.5s。 */
	for (i = 0; i < 30; i++) {
		if (udev->state == USB_STATE_NOTATTACHED)
			break;
		msleep(50);
	}
	if (i)
		dev_info(&udev->dev, "device detached after %d ms\n", i * 50);

	usb_put_dev(udev);
}

static struct usb_driver smit_driver = {
	.name = KBUILD_MODNAME,
	.probe = smit_probe,
	.disconnect = smit_disconnect,
	.id_table = icast_id_table,
};

/* 重启/关机时把芯片复位回冷状态。
 * 路由器重启并不会切断 dongle 的供电，芯片会带着"已初始化"的状态活到下一次开机，
 * 于是开机时的第一次 probe 拿到的是热芯片：唤醒包和命令包全部超时，
 * 只能靠复位重新枚举兜底（多绕一次 6~7s 的断连）。
 * 在这里提前复位，下次开机就是冷芯片，第一次 probe 直接成功。 */
static int smit_reboot_notify(struct notifier_block *nb,
			      unsigned long action, void *data)
{
	struct usb_device *udev = smit_udev;

	if (udev)
		pr_info("dvb_usb_smit: reset chip for clean next boot: %d\n",
			smit_reset(udev));
	return NOTIFY_DONE;
}

static struct notifier_block smit_reboot_nb = {
	.notifier_call = smit_reboot_notify,
};

static int __init smit_module_init(void)
{
	int result;

	result = usb_register(&smit_driver);
	if (result) {
		err("usb_register failed. Error number %d", result);
		return result;
	}
	register_reboot_notifier(&smit_reboot_nb);
	return 0;
}

static void __exit smit_module_exit(void)
{
	unregister_reboot_notifier(&smit_reboot_nb);
	usb_deregister(&smit_driver);
}

module_init(smit_module_init);
module_exit(smit_module_exit);

MODULE_DESCRIPTION("SMIT iCast dvb driver");
MODULE_AUTHOR("Xiaodong Ni <nxiaodong520@gmail.com>");
MODULE_LICENSE("GPL");

