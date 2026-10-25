// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SMIT iCast DTMB/DVC driver
 *
 * Copyright (C) 2026 Xiaodong Ni <nxiaodong520@gmail.com>
 */

#include "dvb-usb.h"
#include "smit.h"

struct st_smit_pack {
	char tag;
	char byte2;
	char req;
	char datalen;
	char data[1000];
} typedef SMIT_PACK;

int smit_usb_bulk_msg(struct usb_device *usb_dev, unsigned int pipe, void *data, int len, int *actual_length, int timeout)
{
  void *buf;
  int ret;

  buf = (void *)kmalloc(len, GFP_KERNEL);
  if ( !buf )
    return -12;
  memcpy(buf, data, len);
  ret = usb_bulk_msg(usb_dev, pipe, buf, (unsigned int)len, actual_length, timeout);
  if ( ret >= 0 )
  {
    if ( pipe & 0x80 )
      memcpy(data, buf, len);
  }
  kfree(buf);
  return ret;
}

int smit_reset(struct usb_device *udev)
{
  return usb_control_msg(udev, usb_sndctrlpipe(udev, 0), 160, 64, 0, 0, 0, 0, 500);
}

int smit_check(struct usb_device *udev, int checkcount)
{
  int cnt;
  int ret;
  int actual_length;

  cnt = 0;

  while ( 1 )
  {
    SMIT_PACK smit = {.tag = 0x01, .byte2 = 0x0, .req = 0xA0, .datalen = 0x01, .data[0] = 0x01};
    ret = smit_usb_bulk_msg(udev, usb_sndbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_SEND), &smit, 5, &actual_length, 500);
    if ( ret )
    {
      return ret;
    }
    ret = smit_usb_bulk_msg(udev, usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE), &smit, 1000, &actual_length, 500);
    if ( ret )
    {
      return ret;
    }
    if ( smit.tag != 1 )
    {
      return -1003;
    }
    if ( smit.data[1] == 0x80 )
    {
      return 0;
    }
    if (smit.data[1] )
    {
      return -1006;
    }
    ++cnt;
    if ( checkcount <= 0 )
      return 0;
    if ( cnt > checkcount )
      break;
    msleep((unsigned int)(cnt / 2 + 3));
  }
  return -checkcount;
}

int smit_set(struct usb_device *udev, u8 req, u8 *data, u8 len, int need_complete_flag)
{
  int ret, actual_length;
  SMIT_PACK smit = {.tag = 0x01, .byte2 = 0x0, .req = req, .datalen = len};

  actual_length = 0;
  
  memcpy(smit.data, data, len);
  
  ret = smit_usb_bulk_msg(udev, usb_sndbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_SEND), &smit, len + 4, &actual_length, 500);
  if ( !ret )
  {
    ret = smit_usb_bulk_msg(udev, usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE), &smit, 1000, &actual_length, 500);
    if ( ret )
    {
      return ret;
    }
    if (smit.tag != 1 )
    {
      return -1004;
    }
    if (smit.req == 0x80 && smit.data[1] == 0x80 )
    {
        return 0;
    }
    if ( (smit.datalen + 4) >= actual_length || smit.data[(u8)smit.datalen] != 0x80 || smit.data[(u8)smit.datalen + 3] != 0x80 )
    {
      if ( need_complete_flag )
      {
        msleep(100);
        return smit_check(udev, need_complete_flag);
      }
    }
    return 0;
  }
  return ret;
}

int smit_tuner_stat(struct usb_device *udev, u8 *out, int checkcount)
{
  int ret;
  int actual_length = 0;
  SMIT_PACK smit = {.tag = 0x01, .byte2 = 0x0, .req = 0x81, .datalen = 0x01, .data[0] = 0x01};

  ret = smit_usb_bulk_msg(udev, usb_sndbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_SEND), &smit, 5, &actual_length, 400);
  if ( ret )
  {
    return ret;
  }

  ret = smit_usb_bulk_msg(udev, usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE), &smit, 900, &actual_length, 400);
  if ( ret )
  {
//    smit_reset(udev);
    return ret;
  }
  if ( smit.tag != 1 )
  {
    return -1003;
  }
  
  ret = (u8)smit.datalen;
  memcpy(out, smit.data, ret);
  
  if ( ( ret >= actual_length || smit.data[ret] != 0x80 || smit.data[ret + 3] != 0x80 ) && checkcount > 0)
  {
      smit_check(udev, checkcount);
  }
 
  return ret;
}

/* 下面这一串初始化命令的返回值上游一直是静默忽略的，设备回怪数据
 * （-1003/-1004/-1006）也能继续跑。为了让 probe 能区分"设备回了怪数据"
 * 和"USB 传输本身挂了"，这里把第一个负返回值记下来带出去，
 * 但流程照旧全部执行，不提前退出。 */
#define SMIT_DO(expr) do { int __r = (expr); if (__r < 0 && err == 0) err = __r; } while (0)

int smit_init(struct usb_device *udev, u8 *dtmb)
{
  int err = 0;
  u8 cmd0[1]  = { 0x01 };
  u8 cmd3[10] = { 0x01, 0x92, 0x07, 0x00, 0x00, 0x01, 0x00, 0x41, 0x00, 0x01};
  u8 cmd4[9]  = { 0x01, 0x90, 0x02, 0x00, 0x01, 0x9F, 0x80, 0x10, 0x00 };
  u8 cmd5[37] = { 0x01, 0x90, 0x02, 0x00, 0x01, 0x9F, 0x80, 0x11, 0x1C, 0x00, 0x01, 0x00, 0x41, 0x00, 0x02, 0x00,
                  0x41, 0x00, 0x03, 0x00, 0x41, 0x00, 0x24, 0x00, 0x41, 0x00, 0x40, 0x00, 0x41, 0x00, 0x96, 0x10,
                  0x01, 0x00, 0x20, 0x00, 0x41 };
  u8 cmd6[10] = { 0x01, 0x92, 0x07, 0x00, 0x00, 0x96, 0x10, 0x01, 0x00, 0x03 };
  u8 cmd7[17] = { 0x01, 0x90, 0x02, 0x00, 0x03, 0x9F, 0x9A, 0x00, 0x08, 0x53, 0x4D, 0x69, 0x54, 0x5A, 0x42, 0x4A, 0x4C };
  u8 cmd8[10] = { 0x01, 0x92, 0x07, 0x00, 0x00, 0x96, 0x10, 0x01, 0x00, 0x04 };
  u8 cmd10[10] = { 0x01, 0x92, 0x07, 0x00, 0x00, 0x03, 0x00, 0x41, 0x00, 0x04 };
  u8 cmd11[9] = { 0x01, 0x90, 0x02, 0x00, 0x04, 0x9F, 0x80, 0x30, 0x00 };
  u8 cmd12[10] = { 0x01, 0x92, 0x07, 0x00, 0x00, 0x96, 0x10, 0x01, 0x00, 0x05 };
  u8 cmd9[32] = {
  	0x01, 0x90, 0x02, 0x00, 0x03, 0x9F, 0x9A, 0x07,
  	0x17,	0x00, 0x00, 0x14, 0x00, 0x03, 0x00, 0x10,
  	0xB8,	0xF3, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
  	0x00,	0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
  }; 
  u8 cmd14[20] = { 0x01, 0x90, 0x02, 0x00, 0x03, 0x9F, 0x9A, 0x07, 0x0B, 0x07, 0x00, 0x08, 0x10, 0x07, 0x00, 0x04, 0x47, 0x44, 0x48, 0x57};
  u8 buf[1000];

  SMIT_DO(smit_set(udev, 0x82, cmd0, 1, 100));
  SMIT_DO(smit_tuner_stat(udev, buf, 0));

  SMIT_DO(smit_set(udev, 0xA0, cmd3, 10, 0));

  SMIT_DO(smit_set(udev, 0xA0, cmd4, 9, 100));
  SMIT_DO(smit_tuner_stat(udev, buf, 0));

  cmd4[7] = 0x12;
  SMIT_DO(smit_set(udev, 0xA0, cmd4, 9, 100));
  SMIT_DO(smit_tuner_stat(udev, buf, 0));

  SMIT_DO(smit_set(udev, 0xA0, cmd5, 37, 100));
  SMIT_DO(smit_tuner_stat(udev, buf, 0));

  cmd3[5] = 0x02;
  cmd3[9] = 0x02;
  SMIT_DO(smit_set(udev, 0xA0, cmd3, 10, 0));

  cmd4[4] = 0x02;
  cmd4[7] = 0x20;
  SMIT_DO(smit_set(udev, 0xA0, cmd4, 9, 100));
  SMIT_DO(smit_tuner_stat(udev, buf, 0));
  SMIT_DO(smit_tuner_stat(udev, buf, 0));
  
  SMIT_DO(smit_set(udev, 0xA0, cmd6, 10, 1));

  SMIT_DO(smit_set(udev, 0xA0, cmd7, 17, 100));
  SMIT_DO(smit_tuner_stat(udev, buf, 100));
  SMIT_DO(smit_tuner_stat(udev, buf, 0));

  SMIT_DO(smit_set(udev, 0xA0, cmd8, 10, 300));
  SMIT_DO(smit_tuner_stat(udev, buf, 0));

  SMIT_DO(smit_set(udev, 0xA0, cmd14, 20, 100));
  SMIT_DO(smit_tuner_stat(udev, buf, 0));
  
  *dtmb = strstr(buf + 18, "DTMB:Y") != NULL;

  if (!*dtmb)
  {
    SMIT_DO(smit_set(udev, 0xA0, cmd10, 10, 0));
    
    SMIT_DO(smit_set(udev, 0xA0, cmd11, 9, 0));
    SMIT_DO(smit_tuner_stat(udev, buf, 0));
    SMIT_DO(smit_tuner_stat(udev, buf, 0));
    
    SMIT_DO(smit_set(udev, 0xA0, cmd12, 10, 100));
    SMIT_DO(smit_tuner_stat(udev, buf, 0));
  }

  SMIT_DO(smit_set(udev, 0xA0, cmd9, 32, 100));
  SMIT_DO(smit_tuner_stat(udev, buf, 20));
  
  return err;
}

#undef SMIT_DO
