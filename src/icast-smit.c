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

static struct icast_fe_config icast_config = {
	.dtmb = 0,
};

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

/* 唤醒序列 "FE 00 10 00" 只对"刚枚举出来、固件还没被初始化过"的设备有效。
 *
 * 实测：设备被上一轮驱动实例初始化过之后（rmmod 再 insmod），不只是唤醒包，
 * 连后续所有命令包的 IN 应答都超时（-ETIMEDOUT）——芯片根本不回话，
 * 只有 smit_reset() 的硬件复位能把它救回来，代价是设备重新枚举 6~7 秒。
 * 这就是"每次 insmod 第一个 probe 必失败"的来源。
 *
 * 所以：
 *   - 卸载时（smit_disconnect）就复位，把设备留成"刚插上"的状态，
 *     这样下一次 insmod 的第一次 probe 就是干净的；
 *   - probe 里在 6s 预算内重试唤醒包（开机时芯片固件比 USB 枚举慢），
 *     全部超时才复位返回，不再白跑一遍 smit_init（约 12s）；
 *   - 只有 -EPIPE / -ETIMEDOUT 值得重试。
 */
/* 唤醒总预算：开机时设备和固件的启动速度不一样 —— USB 枚举很快，
 * 而芯片从 SPI flash 载入固件要 1~3s。probe 在枚举完成的瞬间就跑了，
 * 这时发 "FE 00 10 00" 没人应答（-ETIMEDOUT），老代码直接判死并复位，
 * 白白多绕一次 7s 的重新枚举。
 * 所以这里给一个总时间预算内反复试；只有明确不是"还没准备好"
 * 的错误（-ENODEV/-ESHUTDOWN/...）才立刻放弃。 */
#define SMIT_WAKE_BUDGET_MS	6000
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

	ret = smit_init(udev, &icast_config.dtmb);
	if (smit_is_transport_err(ret)) {
		dev_err(&udev->dev, "init failed at USB level: %d\n", ret);
		smit_reset(udev);
		return ret;
	}
	if (ret)
		/* 协议级错误（设备回了怪数据）上游一直是忽略的，
		 * 这里只记录，免得把本来能用的设备判死 */
		dev_info(&udev->dev, "init sequence reported %d, continuing\n", ret);

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
	int ret, i;

	usb_get_dev(udev);	/* 下面的轮询期间保证 udev 不被释放 */

	dvb_usb_device_exit(intf);

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

static int __init smit_module_init(void)
{
	int result = 0;
	if ((result = usb_register(&smit_driver))) {
		err("usb_register failed. Error number %d", result);
	}
	return result;
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

