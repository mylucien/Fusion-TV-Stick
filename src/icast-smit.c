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

/* 唤醒序列 "FE 00 10 00" 在第一次 probe 时经常会失败：
 * 上一轮驱动实例退出后 bulk 端点可能还带着 halt（或 data toggle 没复位），
 * 这时 usb_bulk_msg() 会立刻返回 -EPIPE，probe 随即返回 -EINVAL，
 * usbcore 放弃绑定；只能等 smit_reset() 把设备复位重新枚举之后，
 * 第二次 probe 才成功，代价是十几秒的断连。
 *
 * 所以这里先清 halt 再发唤醒包，失败按 50/100/150ms 退避重试；
 * 三次全挂才退回 reset 兜底（保持原来"总能自愈"的特性）。 */
#define SMIT_WAKE_ATTEMPTS	3
#define SMIT_WAKE_BUF_SIZE	512

static int smit_wake(struct usb_device *udev)
{
	const u8 wake[4] = { 0xFE, 0x00, 0x10, 0x00 };
	int size, ret = 0, attempt;
	u8 *buf;

	buf = kmalloc(SMIT_WAKE_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (attempt = 1; attempt <= SMIT_WAKE_ATTEMPTS; attempt++) {
		/* 清掉可能残留的 stall，同时把 data toggle 复位 */
		usb_clear_halt(udev,
			usb_sndbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_SEND));
		usb_clear_halt(udev,
			usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE));

		memcpy(buf, wake, sizeof(wake));
		ret = usb_bulk_msg(udev,
			usb_sndbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_SEND),
			buf, sizeof(wake), &size, 1000);
		if (ret) {
			dev_warn(&udev->dev,
				 "wake OUT failed: %d (attempt %d/%d)\n",
				 ret, attempt, SMIT_WAKE_ATTEMPTS);
			goto retry;
		}

		ret = usb_bulk_msg(udev,
			usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE),
			buf, SMIT_WAKE_BUF_SIZE, &size, 1000);
		/* 回包长度不确定：设备给的数据比缓冲区多只会报 EOVERFLOW，
		 * 这时包其实已经收到了，不算失败 */
		if (ret == -EOVERFLOW)
			ret = 0;
		if (ret) {
			dev_warn(&udev->dev,
				 "wake IN failed: %d (attempt %d/%d)\n",
				 ret, attempt, SMIT_WAKE_ATTEMPTS);
			goto retry;
		}

		kfree(buf);
		return 0;
retry:
		msleep(50 * attempt);
	}

	dev_err(&udev->dev,
		"wake sequence failed after %d attempts, last error %d\n",
		SMIT_WAKE_ATTEMPTS, ret);
	kfree(buf);
	return ret ? ret : -EIO;
}

static int smit_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	int ret;

	ret = smit_wake(udev);
	if (ret)
		goto reset_device;

	ret = smit_init(udev, &icast_config.dtmb);
	if (ret)
		/* 初始化命令的协议级错误上游一直是忽略的，这里只记录不中断，
		 * 免得把本来能用的设备判死 */
		dev_warn(&udev->dev, "init sequence reported %d, continuing\n", ret);

	ret = dvb_usb_device_init(intf, &icast_properties, THIS_MODULE, NULL,
				  adapter_nr);
	if (ret == 0)
		return 0;

	dev_err(&udev->dev, "dvb_usb_device_init failed: %d\n", ret);

reset_device:
	/* 兜底：让设备复位并重新枚举，下一次 probe 从干净状态开始。
	 * 注意复位之后 udev 很快失效，这里只能立刻返回。 */
	smit_reset(udev);
	return ret ? ret : -EINVAL;
}

static struct usb_driver smit_driver = {
	.name = KBUILD_MODNAME,
	.probe = smit_probe,
	.disconnect = dvb_usb_device_exit,
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

