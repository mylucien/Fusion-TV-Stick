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

/* 唤醒序列 "FE 00 10 00" 只对"刚枚举出来、固件还没被初始化过"的设备有效。
 *
 * 如果设备已经被上一轮驱动实例初始化过（rmmod 再 insmod、或者 modprobe 两次），
 * 芯片已经在跑固件了，这个包的 IN 应答会超时（-ETIMEDOUT），probe 于是失败，
 * 只能靠 smit_reset() 把芯片复位、设备重新枚举（约 6~7s）之后第二次 probe 才成功。
 * 这就是"每次 insmod 第一个 probe 必失败"的来源。
 *
 * 处理办法：
 *   - 端点被 stall（-EPIPE）才值得 clear_halt 重试；
 *   - 超时说明是"热设备"，不再浪费重试时间，交给调用方按热状态继续初始化；
 *   - 其它错误（-ENODEV/-ESHUTDOWN 等）才真正失败。 */
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
			buf, sizeof(wake), &size, 500);
		if (ret)
			goto out;

		ret = usb_bulk_msg(udev,
			usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE),
			buf, SMIT_WAKE_BUF_SIZE, &size, 500);
		/* 回包长度不确定：设备给的数据比缓冲区多只会报 EOVERFLOW，
		 * 这时包其实已经收到了，不算失败 */
		if (ret == -EOVERFLOW)
			ret = 0;
		if (!ret) {
			kfree(buf);
			return 0;
		}
		/* 只有 stall 才值得重试；超时是"固件不应答"，再试也一样 */
		if (ret != -EPIPE)
			break;
		msleep(50 * attempt);
	}

out:
	kfree(buf);
	return ret;
}

static int smit_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	int ret;

	ret = smit_wake(udev);
	if (ret)
		/* 热设备：芯片已经在跑固件，唤醒包本来就没人应答。
		 * 下面重发一遍同样的寄存器配置没有副作用，还省掉一次
		 * 6~7s 的复位重新枚举。真要是设备没了，smit_init 会以
		 * 传输层错误失败，再走复位兜底。 */
		dev_info(&udev->dev,
			 "wake not answered (%d), assuming device already initialized\n",
			 ret);

	ret = smit_init(udev, &icast_config.dtmb);
	if (smit_is_transport_err(ret)) {
		dev_err(&udev->dev, "init failed at USB level: %d\n", ret);
		goto reset_device;
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

