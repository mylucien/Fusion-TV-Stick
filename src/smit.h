// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SMIT iCast DTMB/DVC driver
 *
 * Copyright (C) 2026 Xiaodong Ni <nxiaodong520@gmail.com>
 */

#ifndef SMIT_H
#define SMIT_H

#define SMIT_BULK_ENDPOINT_CMD_SEND				0x01
#define SMIT_BULK_ENDPOINT_CMD_RECEIVE		0x82
#define SMIT_BULK_ENDPOINT_STREAM					0x84

/* 协议级错误码一律小于 -1000（-1003 tag 不符、-1004、-1006、
 * -(1000+checkcount) 重试用尽）；而 USB 传输层的错误是内核 errno，
 * 都在 -1..-200 之间。据此可以区分"设备回了怪数据"和"USB 传输挂了"。 */
static inline int smit_is_transport_err(int ret)
{
	return ret < 0 && ret > -1000;
}

/* 调试开关，定义在 icast-smit.c，供本文件打点用 */
extern bool smit_trace;

int smit_reset(struct usb_device *udev);
int smit_set(struct usb_device *udev, u8 req, u8 *data, u8 len, int need_complete_flag);
int smit_tuner_stat(struct usb_device *udev, u8 *out, int checkcount);
int smit_init(struct usb_device *udev, u8 *dtmb);

#endif