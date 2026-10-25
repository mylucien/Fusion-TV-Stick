// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SMIT iCast DTMB/DVC driver
 *
 * Copyright (C) 2026 Xiaodong Ni <nxiaodong520@gmail.com>
 */

#include "dvb-usb.h"
#include "smit.h"
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/moduleparam.h>
#include <linux/version.h>

/* 单次 bulk 传输的超时。设备正常工作时应答是 0ms 级的，
 * 这个值只在链路出问题时才会被真正等满。 */
#define SMIT_BULK_TIMEOUT_MS	500

/* 冲刷 IN 管道时用的超时：只要读不到东西就认为已经干净了 */
#define SMIT_DRAIN_TIMEOUT_MS	40

/* 完成标志轮询的总预算 = 基础值 + checkcount*10ms，且不超过上限。
 * 原来是纯次数上限（checkcount 最大 300），每次重试含两个 500ms 超时，
 * 链路一坏就能把一个 ioctl 卡几十秒，用户看到的就是"整个 dvb 死住了"。 */
#define SMIT_CHECK_BASE_MS	1000
#define SMIT_CHECK_MAX_MS	6000

/* 连续多少次"整条事务"在传输层失败就认为芯片卡死，触发自愈复位 */
#define SMIT_RECOVER_STREAK	3

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

/* ---- 命令通道 ---------------------------------------------------------
 *
 * 一条业务命令 = "往 0x01 写 5+N 字节，再从 0x82 读回一包"，两步构成一个事务。
 * 这两步必须在一次独占访问里完成：两个调用者一旦并发（比如播放器在轮询
 * FE_READ_STATUS，同时另一个进程在调 FE_SET_FRONTEND），OUT 包会交错，
 * 读回的是对方的应答，tag/长度对不上，之后整条通道就永久错位。
 * 真机上的表现就是"跑一段时间不定时挂死，拔插才好"。
 */
static DEFINE_MUTEX(smit_cmd_lock);

/* 上一次事务在传输层出过问题（超时 / stall / tag 错位），说明 IN 管道里
 * 可能还躺着半包或者上一条命令的旧应答。下一次事务开始前先冲干净，
 * 否则会一直读到错位的数据。 */
static bool smit_resync_pending;

static unsigned smit_err_streak;
static unsigned long smit_stat_ok, smit_stat_timeout, smit_stat_epipe,
		     smit_stat_resync, smit_stat_recover;

int smit_reset(struct usb_device *udev)
{
  return usb_control_msg(udev, usb_sndctrlpipe(udev, 0), 160, 64, 0, 0, 0, 0, 500);
}

/* 端点 stall（-EPIPE）之后必须 clear_halt，否则之后每一次传输都会立刻
 * 返回 -EPIPE，谁也救不回来 —— 这是"驱动挂死只能拔插"最典型的成因。 */
static void smit_clear_halt(struct usb_device *udev)
{
	usb_clear_halt(udev, usb_sndbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_SEND));
	usb_clear_halt(udev, usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE));
}

static int smit_bulk_out(struct usb_device *udev, void *data, int len)
{
	int ret, actual_length = 0, tries = 0;

	do {
		ret = smit_usb_bulk_msg(udev,
			usb_sndbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_SEND),
			data, len, &actual_length, SMIT_BULK_TIMEOUT_MS);
		if (!ret)
			return 0;
		if (ret != -EPIPE)
			break;
		smit_stat_epipe++;
		smit_clear_halt(udev);
	} while (++tries < 2);

	if (ret == -ETIMEDOUT) {
		smit_stat_timeout++;
		smit_resync_pending = true;
	}
	return ret;
}

static int smit_bulk_in(struct usb_device *udev, void *data, int len, int *actual_len)
{
	int ret, actual_length = 0, tries = 0;

	do {
		ret = smit_usb_bulk_msg(udev,
			usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE),
			data, len, &actual_length, SMIT_BULK_TIMEOUT_MS);
		/* 设备给的数据比缓冲区多只会报 EOVERFLOW，包其实已经收到了 */
		if (!ret || ret == -EOVERFLOW) {
			if (actual_len)
				*actual_len = actual_length;
			return 0;
		}
		if (ret != -EPIPE)
			break;
		smit_stat_epipe++;
		smit_clear_halt(udev);
	} while (++tries < 2);

	if (ret == -ETIMEDOUT) {
		smit_stat_timeout++;
		smit_resync_pending = true;
	}
	return ret;
}

/* 把 IN 管道里残留的包全部读掉，让下一条命令从干净的边界开始。
 * 只在事务之间调用（持有 smit_cmd_lock），不会吃掉在飞的应答。 */
static void smit_drain_in(struct usb_device *udev)
{
	u8 *buf;
	int i, actual_length, ret;

	smit_resync_pending = false;
	smit_clear_halt(udev);

	buf = kmalloc(1024, GFP_KERNEL);
	if (!buf)
		return;

	for (i = 0; i < 32; i++) {
		ret = usb_bulk_msg(udev,
			usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE),
			buf, 1024, &actual_length, SMIT_DRAIN_TIMEOUT_MS);
		if (ret == -EOVERFLOW)
			continue;	/* 收到了，继续抽 */
		if (ret)
			break;		/* 超时 / stall：认为已经空了 */
	}
	kfree(buf);
	smit_stat_resync++;
}

static void smit_prepare(struct usb_device *udev)
{
	if (smit_resync_pending)
		smit_drain_in(udev);
}

/* 公共入口统一在这里判定链路健康度：连续 N 次事务在传输层失败，
 * 说明芯片已经卡死，交给上层复位重新枚举（不用拔插）。
 * 协议级错误码（-1xxx，设备回了怪数据）不算链路故障，不触发复位。 */
static void smit_note(struct usb_device *udev, int ret)
{
	if (ret >= 0) {
		smit_stat_ok++;
		smit_err_streak = 0;
		return;
	}
	if (!smit_is_transport_err(ret) || ret == -ENODEV || ret == -ESHUTDOWN)
		return;

	if (++smit_err_streak >= SMIT_RECOVER_STREAK) {
		smit_err_streak = 0;
		smit_stat_recover++;
		smit_schedule_recovery(udev);
	}
}

static int __smit_check(struct usb_device *udev, int checkcount)
{
  unsigned int budget = SMIT_CHECK_BASE_MS + (unsigned int)checkcount * 10;
  unsigned long deadline;
  int cnt;
  int ret;

  if (budget > SMIT_CHECK_MAX_MS)
    budget = SMIT_CHECK_MAX_MS;
  deadline = jiffies + msecs_to_jiffies(budget);

  cnt = 0;

  while ( 1 )
  {
    SMIT_PACK smit = {.tag = 0x01, .byte2 = 0x0, .req = 0xA0, .datalen = 0x01, .data[0] = 0x01};

    /* 每次尝试都可能等满两个超时，所以预算在循环头上判，不能只在末尾判 */
    if ( time_after_eq(jiffies, deadline) )
      break;

    ret = smit_bulk_out(udev, &smit, 5);
    if ( ret )
    {
      return ret;
    }
    ret = smit_bulk_in(udev, &smit, 1000, NULL);
    if ( ret )
    {
      return ret;
    }
    if ( smit.tag != 1 )
    {
      smit_resync_pending = true;
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
  /* 归到协议级错误码区间（< -1000），别和 USB 传输层的 errno 混在一起：
   * 调用方靠这个区间判断是该重试/复位，还是照旧忽略。 */
  return -(1000 + checkcount);
}

static int __smit_set(struct usb_device *udev, u8 req, u8 *data, u8 len, int need_complete_flag)
{
  int ret, actual_length;
  SMIT_PACK smit = {.tag = 0x01, .byte2 = 0x0, .req = req, .datalen = len};

  smit_prepare(udev);

  actual_length = 0;

  memcpy(smit.data, data, len);

  ret = smit_bulk_out(udev, &smit, len + 4);
  if ( !ret )
  {
    ret = smit_bulk_in(udev, &smit, 1000, &actual_length);
    if ( ret )
    {
      return ret;
    }
    if (smit.tag != 1 )
    {
      smit_resync_pending = true;
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
        /* 原先这里先 msleep(100) 再去轮询完成标志，一整套初始化序列要付
         * 10 次 = 1s 的固定开销。smit_check 自己就是从 3ms 起步的退避轮询，
         * 直接进去等更划算，芯片真有活干也等得住。 */
        return __smit_check(udev, need_complete_flag);
      }
    }
    return 0;
  }
  return ret;
}

static int __smit_tuner_stat(struct usb_device *udev, u8 *out, int checkcount)
{
  int ret;
  int actual_length = 0;
  SMIT_PACK smit = {.tag = 0x01, .byte2 = 0x0, .req = 0x81, .datalen = 0x01, .data[0] = 0x01};

  smit_prepare(udev);

  ret = smit_bulk_out(udev, &smit, 5);
  if ( ret )
  {
    return ret;
  }

  ret = smit_bulk_in(udev, &smit, 900, &actual_length);
  if ( ret )
  {
    return ret;
  }
  if ( smit.tag != 1 )
  {
    smit_resync_pending = true;
    return -1003;
  }

  ret = (u8)smit.datalen;
  memcpy(out, smit.data, ret);

  if ( ( ret >= actual_length || smit.data[ret] != 0x80 || smit.data[ret + 3] != 0x80 ) && checkcount > 0)
  {
      __smit_check(udev, checkcount);
  }

  return ret;
}

/* 故障注入，只用来验证自愈链路：>0 时每条命令直接返回 -ETIMEDOUT，不碰硬件。
 * 设成 3 再调两次 FE_READ_STATUS（每次两条命令）就能凑满连续失败阈值，
 * 观察芯片是否会自动复位重新枚举、节点是否自己回来。默认 0，不影响正常行为。 */
static int smit_fault_test;
module_param_named(fault_test, smit_fault_test, int, 0644);
MODULE_PARM_DESC(fault_test, "inject N transport failures to exercise auto recovery");

static bool smit_fault_hit(struct usb_device *udev)
{
	if (smit_fault_test <= 0)
		return false;
	smit_fault_test--;
	smit_note(udev, -ETIMEDOUT);
	return true;
}

int smit_set(struct usb_device *udev, u8 req, u8 *data, u8 len, int need_complete_flag)
{
  int ret;

  if (smit_fault_hit(udev))
    return -ETIMEDOUT;

  mutex_lock(&smit_cmd_lock);
  ret = __smit_set(udev, req, data, len, need_complete_flag);
  mutex_unlock(&smit_cmd_lock);

  smit_note(udev, ret);
  return ret;
}

int smit_tuner_stat(struct usb_device *udev, u8 *out, int checkcount)
{
  int ret;

  if (smit_fault_hit(udev))
    return -ETIMEDOUT;

  mutex_lock(&smit_cmd_lock);
  ret = __smit_tuner_stat(udev, out, checkcount);
  mutex_unlock(&smit_cmd_lock);

  smit_note(udev, ret);
  return ret;
}

/* 下面这一串初始化命令的返回值上游一直是静默忽略的，设备回怪数据
 * （-1003/-1004/-1006）也能继续跑。为了让 probe 能区分"设备回了怪数据"
 * 和"USB 传输本身挂了"，这里把第一个负返回值记下来带出去，
 * 但流程照旧全部执行，不提前退出。 */
#define SMIT_DO(expr) do {						\
	int __r;							\
	unsigned long __t0 = jiffies;					\
	__r = (expr);							\
	if (__r < 0 && err == 0)					\
		err = __r;						\
	if (smit_trace)							\
		pr_info("dvb_usb_smit:  %-46s %5ums r=%d\n", #expr,	\
			jiffies_to_msecs(jiffies - __t0), __r);		\
} while (0)

int smit_init(struct usb_device *udev, u8 *dtmb)
{
  int err = 0;
  unsigned long t_all = jiffies;
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

  /* 这条 cmd9 本身是"调谐"命令，初始化阶段频点全是 0，发出去芯片既不会
   * 调谐也不会回完成标志 —— 实测在这里白等 4390ms 后拿到 -1100（重试用尽）。
   * 上游同样忽略它的结果，所以只发不收完成标志。 */
  SMIT_DO(smit_set(udev, 0xA0, cmd9, 32, 0));
  SMIT_DO(smit_tuner_stat(udev, buf, 20));
  
  if (smit_trace)
    pr_info("dvb_usb_smit:  init sequence took %ums\n",
            jiffies_to_msecs(jiffies - t_all));

  return err;
}

#undef SMIT_DO

/* /sys/module/dvb_usb_smit/parameters/stats —— 链路健康度快照。
 * 以后再出现"不定时挂死"，先读这个就能看出挂在哪一层：
 *   timeout 在涨 -> 芯片不再应答（业务层卡住）
 *   epipe   在涨 -> 端点 stall 过，已被 clear_halt 救回来
 *   resync  在涨 -> 出现过错位，已被冲刷修正
 *   recover 在涨 -> 触发过自愈复位（芯片重新枚举过）
 * 四个都不动而画面临时断 -> 问题在 TS 数据流，不在这条命令通道。
 */
static int smit_stats_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer,
		"ok=%lu timeout=%lu epipe=%lu resync=%lu recover=%lu err_streak=%u\n",
		smit_stat_ok, smit_stat_timeout, smit_stat_epipe,
		smit_stat_resync, smit_stat_recover, smit_err_streak);
}

static const struct kernel_param_ops smit_stats_ops = {
	.get = smit_stats_get,
};

module_param_cb(stats, &smit_stats_ops, NULL, 0444);
MODULE_PARM_DESC(stats, "read-only link health counters");
