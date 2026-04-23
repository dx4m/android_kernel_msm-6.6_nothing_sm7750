// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * leds-aw20144.c   aw20144 led module
 *
 * Version: v0.3.0
 *
 * Copyright (c) 2020 Shanghai Awinic Technology Co., Ltd. All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation;either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/miscdevice.h>
#include <linux/completion.h>
#include <linux/mman.h>
#include <linux/poll.h>
#include <asm/uaccess.h>
#include <linux/leds.h>
#include <linux/firmware.h>
#include <asm/cpufeature.h>
#include <linux/workqueue.h>
#include <linux/rtc.h>
#include <linux/timekeeping.h>

#include "leds-aw20144.h"
#include "leds-aw20144_effect.h"

#define AW20144_I2C_NAME		"matrix-leds"
#define AW20144_DRIVER_VERSION		"v0.4.0"
#define AW20144_READ_CHIPID_RETRIES	5
#define AW_I2C_READ_RETRIES		5
#define AW_I2C_WRITE_RETRIES		5
#define AW_I2C_RETRIES 2
#define AW_I2C_RETRY_DELAY 1
#define AW20144_EFFECT_CNT		5
#define ALL_CHANNEL 144
#define VALID_CHANNEL 137
#define TORCH_BRIGHTNESS	182
#define TORCH_TIMING_CFG	10

#define DRV_TAG "[aw20144] "

static int log_level = 0;
static int device_detected = 0;

#define LOG_INFO(fmt, ...) \
	do { \
		if (log_level == 0 || log_level == 0xFF) \
			pr_info(DRV_TAG fmt, ##__VA_ARGS__); \
	} while (0)

#define LOG_ERR(fmt, ...) \
	do { \
		if (log_level == 0 || log_level == 1 || log_level == 0xFF) \
			pr_err(DRV_TAG fmt, ##__VA_ARGS__); \
	} while (0)

#define LOG_DBG(fmt, ...) \
	do { \
		if (log_level == 0xFF) \
			pr_info(DRV_TAG fmt, ##__VA_ARGS__); \
	} while (0)

struct awcfgdata aw20144_cfg_array[] = {
	{aw20144_all_rgb_on, sizeof(aw20144_all_rgb_on)},
	{aw20144_all_rgb_off, sizeof(aw20144_all_rgb_off)},
	{aw20144_red_ok_on, sizeof(aw20144_red_ok_on)},
	{aw20144_red_ok_blink, sizeof(aw20144_red_ok_blink)},
	{aw20144_red_blink_off, sizeof(aw20144_red_blink_off)},
};

#define AW20144_CFG_NAME_MAX		32
static char aw20144_cfg_bin[][32] = {
	"aw20144_all_rgb_on.bin",
	"aw20144_all_rgb_off.bin",
	"aw20144_red_ok_on.bin",
	"aw20144_red_ok_blink.bin",
	"aw20144_red_blink_off.bin",
};

#define POWER_SAVE_MODE

#define AW20144_CFG_FREQ_MAX		8
static char hw_ver[4] = "T0";
static char dev_color[6] = "WHITE";
static char vendor_id0[] = "LITEON";
static char vendor_id1[] = "Fantasy";
static char *freqs[] = {
	"62.5kHz",
	"32.25kHz",
	"15.6kHz",
	"7.8kHz",
	"3.9kHz",
	"1.95kHz",
	"977Hz",
	"488Hz"
};

#define AW20144_BLACK_IMAX AW20144_IMAX_80mA
#define AW20144_WHITE_IMAX AW20144_IMAX_80mA

static DECLARE_WAIT_QUEUE_HEAD(aw20144_waitq);
int ev_happen;
char ev_code = '0';
struct aw20144 *g_aw20144;

static int aw20144_hw_enable(struct aw20144 *aw20144);
static int aw20144_led_init(struct aw20144 *aw20144);

/*******************************************************************************
 *
 * aw20144 i2c read/write
 *
 ******************************************************************************/

static int
aw20144_i2c_read(struct aw20144 *aw20144, unsigned char reg_addr, unsigned char *reg_data)
{
	int ret = -1;
	unsigned char cnt = 0;

	while (cnt < AW_I2C_READ_RETRIES) {
		ret = i2c_smbus_read_byte_data(aw20144->client, reg_addr);
		if (ret < 0) {
			LOG_ERR("%s: i2c read cnt=%d, error=%d\n", __func__, cnt, ret);
		} else {
			*reg_data = ret;
			break;
		}
		cnt++;
		usleep_range(2000, 2500);
	}

	return ret;
}

static int
aw20144_i2c_write(struct aw20144 *aw20144, unsigned char reg_addr, unsigned char reg_data)
{
	int ret = -1;
	unsigned char cnt = 0;

	while (cnt < AW_I2C_WRITE_RETRIES) {
		ret = i2c_smbus_write_byte_data(aw20144->client, reg_addr, reg_data);
		if (ret < 0)
			LOG_ERR("%s: i2c write cnt=%d, error=%d\n", __func__, cnt, ret);
		else
			break;

		cnt++;
		usleep_range(2000, 2500);
	}

	return ret;
}

static int aw20144_i2c_write_bit(struct aw20144 *aw20144,
				unsigned char reg_addr, unsigned int mask,
				unsigned char reg_data)
{
	unsigned char reg_val = 0;

	aw20144_i2c_read(aw20144, reg_addr, &reg_val);
	reg_val &= mask;
	reg_val |= (reg_data & (~mask));
	aw20144_i2c_write(aw20144, reg_addr, reg_val);

	return 0;
}

static int aw20144_set_page(struct aw20144 *aw20144, unsigned char reg_data)
{
	return aw20144_i2c_write(aw20144, AWPAGEADDR, reg_data);
}

static int aw20144_imax_cfg(struct aw20144 *aw20144, unsigned int imax)
{
	int ret = 0;

	if (imax > 0xFF)
		imax = 0xFF;

	aw20144->imax = imax;
	aw20144->imax_target = aw20144->imax;
	LOG_INFO("%s: imax %d -> %d opMode %d\n", __func__, aw20144->imax, imax, aw20144->operating_mode);
	ret = aw20144_set_page(aw20144, AWPAGE0);
	if (ret < 0) {
		return ret;
	}
	ret = aw20144_i2c_write(aw20144, REG_GCCR, aw20144->imax);
	if (ret < 0) {
		return ret;
	}

	return ret;
}

static int aw20144_i2c_write_block(struct aw20144 *aw20144,
					unsigned char reg_addr, u8 length, unsigned char *reg_data)
{
	int ret = -1;
	unsigned char cnt = 0;
	u8 buf[256] = {0};

	struct i2c_msg msg = {
		.addr = aw20144->client->addr,
		.flags = 0,
		.buf = buf,
		.len = length + 1
	};

	/* Copy Register Address. */
	buf[0] = reg_addr;

	/* Copy Register Data. */
	memcpy(&buf[1], reg_data, length);

	while (cnt < AW_I2C_RETRIES) {
		ret =
			i2c_transfer(aw20144->client->adapter, &msg, 1);
		if (ret < 0) {
			LOG_ERR("%s: i2c_write cnt=%d error=%d\n", __func__, cnt,
				ret);
		} else {
			break;
		}
		cnt++;
		msleep(AW_I2C_RETRY_DELAY);
	}

	return ret;
}

static int aw20144_rgbcolor_config(struct aw20144 *aw20144)
{
	int ret = 0;
	/*
	led0-36 dim cfg
	R1C1,  R2C1, ...R12C1,
	R1C2,  R2C2, ...R12C2,
	R1C3,  R2C3, ...R12C3,
	*/
	unsigned char aw20144_rgb_color_cfg_black[36] = {
		0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x14, 0x14, 0x14, 0x14, 0x14,
		0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x14, 0x14, 0x14, 0x14, 0x14,
		0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
	};
	unsigned char aw20144_rgb_color_cfg_white[36] = {
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x04, 0x04, 0x04, 0x04,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x04, 0x04, 0x04, 0x04,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04
	};

	ret = aw20144_set_page(aw20144, AWPAGE2);
	if (ret < 0) {
		return ret;
	}


	if (strncmp(dev_color, "BLACK", 5) == 0) {
		LOG_INFO("%s: device dim for black\n", __func__);
		ret = aw20144_i2c_write_block(aw20144, 0x00, 36, aw20144_rgb_color_cfg_black);
	} else {
		LOG_INFO("%s: device dim for white\n", __func__);
		ret = aw20144_i2c_write_block(aw20144, 0x00, 36, aw20144_rgb_color_cfg_white);
	}


	if (ret < 0) {
		return ret;
	}

	return ret;
}

static void aw20144_clean_buf(struct aw20144 *aw20144, int status)
{
	struct mmap_buf_format *opbuf = aw20144->start_buf;
	int i = 0;

	for (i = 0; i < LED_MMAP_BUF_SUM; i++) {
		memset(opbuf->data, 0, LED_MMAP_BUF_SIZE*2);
		opbuf->status = status;
		opbuf = opbuf->kernel_next;
	}
}

#define MAX_BRIGHTNESS 4095
static void aw20144_leds_effect_work(struct work_struct *work)
{
	struct aw20144 *aw20144 = container_of(work, struct aw20144,
			leds_effect_work);
	uint32_t retry = 0;
	unsigned char brightness[ALL_CHANNEL] = {0};
	ktime_t start, runtime, delay;
	int i = 0, j = 0, num = 0;
	int flag = 0; //This flag is used when start_buf status is MMAP_BUF_DATA_FINISHED
	int mapping_channels[VALID_CHANNEL] = {
			0, 18, 36, 54, 72, 1, 19, 37,
			55, 73, 91, 109, 127, 142, 12, 2,
			20, 38, 56, 74, 92, 110, 128, 124,
			106, 30, 3, 21, 39, 57, 75, 93,
			111, 129, 140, 141, 13, 48, 4, 22,
			40, 58, 76, 94, 112, 130, 122, 123,
			88, 31, 66, 5, 23, 41, 59, 77,
			95, 113, 131, 104, 105, 70, 49, 84,
			6, 24, 42, 60, 78, 96, 114, 132,
			86, 87, 52, 67, 102, 7, 25, 43,
			61, 79, 97, 115, 133, 68, 69, 34,
			85, 120, 8, 26, 44, 62, 80, 98,
			116, 134, 50, 51, 16, 138, 9, 27,
			45, 63, 81, 99, 117, 135, 32, 33,
			139, 10, 28, 46, 64, 82, 100, 118,
			136, 14, 15, 121, 11, 29, 47, 65,
			83, 101, 119, 137, 103, 17, 35, 53,
			71 };

	pm_stay_awake(aw20144->dev);
	aw20144->curr_buf = aw20144->start_buf;
	LOG_INFO("effect working status IdxBuf@%d status 0x%x length %d sec_num %d stream_mode %d\n",
		aw20144->curr_buf->bit, aw20144->curr_buf->status, aw20144->curr_buf->length,
		aw20144->sec_num, aw20144->stream_mode);

	if (aw20144->start_buf->status == MMAP_BUF_DATA_FINISHED) {
		flag = 1;
		aw20144->start_buf->status = MMAP_BUF_DATA_VALID;
		LOG_INFO("effect working IdxBuf@%d Ignore the MMAP_BUF_DATA_FINISHED judgment once!", aw20144->curr_buf->bit);
	}
	do {
		if (aw20144->curr_buf->status == MMAP_BUF_DATA_VALID) {
			if (flag == 1) {
				aw20144->start_buf->status = MMAP_BUF_DATA_FINISHED;
				flag = 0;
				LOG_INFO("effect working Restore start_buf to MMAP_BUF_DATA_FINISHED");
			}
			if (aw20144->sec_num == VALID_CHANNEL) {
				LOG_INFO("effect working IdxBuf@%d length %d br %d\n",
					aw20144->curr_buf->bit, aw20144->curr_buf->length, aw20144->curr_buf->brightness);
				for (i = 0; i < aw20144->curr_buf->length; i += VALID_CHANNEL) {
					start = ktime_get();
					memset(brightness, 0, ALL_CHANNEL);
					for (j = 0; j < VALID_CHANNEL; j++) {
						num = mapping_channels[j];
						brightness[num] = aw20144->curr_buf->data[i+j]*aw20144->curr_buf->brightness/MAX_BRIGHTNESS;
					}
					if (aw20144->stream_mode == 0) {
						LOG_INFO("effect working break before play stream_mode@%d\n", aw20144->stream_mode);
						break;
					}
					aw20144_set_page(aw20144, AWPAGE1);
					aw20144_i2c_write_block(aw20144, 0x00, ALL_CHANNEL, brightness); /*led(0-143)*/
					if (aw20144->stream_mode == 0) {
						LOG_INFO("effect working break after play stream_mode@%d\n", aw20144->stream_mode);
						break;
					}
					runtime = ktime_sub(ktime_get(), start);
					delay = 16666*1000 - runtime;//ns
					if (delay > 0) {
						//LOG_INFO("%s delay:%d\n", __func__, delay/1000);
						usleep_range(delay/1000, delay/1000);
					}
					if (aw20144->stream_mode == 0) {
						LOG_INFO("effect working break after next frame\n");
						break;
					}
				}
				if (aw20144->stream_mode == 0) {
					LOG_INFO("effect working break length@%d stream_mode@%d\n", aw20144->curr_buf->length, aw20144->stream_mode);
					break;
				}
			}
			aw20144->curr_buf->status = MMAP_BUF_DATA_INVALID;
			aw20144->curr_buf->length = 0;
			aw20144->curr_buf = aw20144->curr_buf->kernel_next;
			retry = 0;
		} else if (aw20144->curr_buf->status == MMAP_BUF_DATA_FINISHED) {
			LOG_INFO("effect working IdxBuf@%d status @finish\n", aw20144->curr_buf->bit);
			break;
		} else {
			if (aw20144->stream_mode == 0) {
				LOG_INFO("effect working break stream_mode@%d\n", aw20144->stream_mode);
				break;
			}
			LOG_INFO("effect working IdxBuf@%d status 0x%X waiting\n", aw20144->curr_buf->bit, aw20144->curr_buf->status);
			msleep(1);
		}
		if (aw20144->stream_mode == 0) {
			LOG_INFO("effect working break stream_mode@%d retry@%d\n", aw20144->stream_mode, retry);
			break;
		}
	} while (retry++ < 30);

	/*
	* Reset the buffer status
	*/
	aw20144_clean_buf(aw20144, MMAP_BUF_DATA_FINISHED);

	memset(brightness, 0, ALL_CHANNEL);
	aw20144_set_page(aw20144, AWPAGE1);
	aw20144_i2c_write_block(aw20144, 0x00, ALL_CHANNEL, brightness); /*led(0-143)*/

	ev_happen = 1;
	ev_code = '1';
	LOG_INFO("ev_happen:%d ev_code:%c\n", ev_happen, ev_code);
	wake_up_interruptible(&aw20144_waitq);
	pm_relax(aw20144->dev);
}

static int justOpenOnce;
static int aw20144_open(struct inode *inode, struct file *filp)
{
	LOG_INFO("%s enter\n", __func__);
	if (justOpenOnce == 0) {
		justOpenOnce++;
	} else {
		LOG_INFO("%s err\n", __func__);
	}

	return 0;

}
static int aw20144_release(struct inode *inode, struct file *filp)
{
	if (justOpenOnce > 0) {
		LOG_INFO("Now the led_strips has been closed!\n");
		justOpenOnce = 0;
	} else {
		LOG_INFO("The the led_strips has already been closed!\n");
	}

	return 0;
}

static ssize_t aw20144_read(struct file *file, char __user *user, size_t size, loff_t *ppos)
{
	struct aw20144 *aw20144 = g_aw20144;
	int ret = 0;

	LOG_INFO("%s stream_mode@%d\n", __func__, aw20144->stream_mode);
	if (size != 1)
		return -EINVAL;

	wait_event_interruptible(aw20144_waitq, ev_happen);
	ret = copy_to_user(user, &ev_code, 1);
	if (ret == 0) {
		ev_code = '0';
		ev_happen = 0;
		return 1;
	}
	ev_code = '0';
	ev_happen = 0;

	return ret;
}

static inline unsigned long nt_arch_calc_vm_flag_bits(unsigned long flags)
{
	/*
	 * Only allow MTE on anonymous mappings as these are guaranteed to be
	 * backed by tags-capable memory. The vm_flags may be overridden by a
	 * filesystem supporting MTE (RAM-based).
	 */
	if (system_supports_mte() && (flags & MAP_ANONYMOUS))
		return VM_MTE_ALLOWED;

	return 0;
}
static inline unsigned long
__nt_calc_vm_flag_bits(unsigned long flags)
{
	return _calc_vm_trans(flags, MAP_GROWSDOWN, VM_GROWSDOWN) |
		_calc_vm_trans(flags, MAP_LOCKED, VM_LOCKED) |
		_calc_vm_trans(flags, MAP_SYNC, VM_SYNC) |
		nt_arch_calc_vm_flag_bits(flags);
}

static int aw20144_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct aw20144 *aw20144 = g_aw20144;
	unsigned long phys;
	int ret = 0;

#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 7, 0)
	vm_flags_t vm_flags = calc_vm_prot_bits(PROT_READ|PROT_WRITE, 0) |
					__nt_calc_vm_flag_bits(MAP_SHARED);

	vm_flags |= current->mm->def_flags | VM_MAYREAD | VM_MAYWRITE |
			VM_MAYEXEC | VM_SHARED | VM_MAYSHARE;

	if (vma && (pgprot_val(vma->vm_page_prot) !=
			pgprot_val(vm_get_page_prot(vm_flags)))) {
		LOG_ERR("vm_page_prot error!");
		return -EPERM;
	}

	if (vma && ((vma->vm_end - vma->vm_start) !=
			(PAGE_SIZE << LED_MMAP_PAGE_ORDER))) {
		LOG_ERR("mmap size check err!");
		return -EPERM;
	}
#endif

	LOG_INFO("%s stream_mode@%d\n", __func__, aw20144->stream_mode);

	phys = virt_to_phys(aw20144->start_buf);

	ret = remap_pfn_range(vma, vma->vm_start, (phys >> PAGE_SHIFT), (vma->vm_end - vma->vm_start), vma->vm_page_prot);
	if (ret) {
		dev_err(aw20144->dev, "Error mmap failed\n");
		return ret;
	}

	return ret;
}

static unsigned int aw20144_poll(struct file *file, poll_table *wait)
{
	struct aw20144 *aw20144 = g_aw20144;
	unsigned int mask = 0;

	LOG_INFO("%s stream_mode@%d ev_happen %d\n", __func__, aw20144->stream_mode, ev_happen);

	poll_wait(file, &aw20144_waitq, wait);
	if (ev_happen == 1) {
		mask |= POLLIN | POLLRDNORM;
	}
	LOG_INFO("%s stream_mode@%d ev_happen %d done\n", __func__, aw20144->stream_mode, ev_happen);

	return mask;
}

long aw20144_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct aw20144 *aw20144 = g_aw20144;

	void __user *argp = (void __user *)arg;
	unsigned char buf;

	switch (cmd) {
	case LED_STRIPS_STREAM_MODE:
		if (copy_from_user(&aw20144->sec_num, argp, 1)) {
			return -EFAULT;
		}
		LOG_INFO("%s stream_mode @1 opMode %d sec_num:%d to start\n", __func__, aw20144->operating_mode, aw20144->sec_num);
		aw20144->stream_mode = 1;
		if ((aw20144->operating_mode == 1)
				&& (aw20144->imax != aw20144->imax_target)) {
			LOG_INFO("%s active and reconfig imax %d -> %d\n", __func__, aw20144->imax, aw20144->imax_target);
			aw20144->imax = aw20144->imax_target;
			aw20144_hw_enable(aw20144);
			aw20144_led_init(aw20144);
		#ifdef POWER_SAVE_MODE
			aw20144_set_page(aw20144, AWPAGE0);
			aw20144_i2c_write_bit(aw20144, REG_GCR, BIT_CHIPEN_DISABLE, BIT_CHIPEN_ENABLE);
		#endif
		}
		queue_work(aw20144->leds_workqueue, &aw20144->leds_effect_work);
		break;
	case LED_STRIPS_STOP_MODE:
		LOG_INFO("%s stream_mode @0 to stop\n", __func__);
		aw20144->stream_mode = 0;
		aw20144_clean_buf(aw20144, MMAP_BUF_DATA_FINISHED);
		break;
	case LED_STRIPS_ALWAYS_ON:
		if (copy_from_user(&aw20144->always_on, argp, 1)) {
			return -EFAULT;
		}
		LOG_INFO("%s always_on @%d\n", __func__, aw20144->always_on);
		break;
	case LED_STRIPS_FREQ_SET:
		if (copy_from_user(&buf, argp, 1)) {
			return -EFAULT;
		}
		LOG_INFO("%s freq set %d\n", __func__, buf);
		aw20144_set_page(aw20144, AWPAGE0);
		if (buf == 0) {
			aw20144_i2c_write_bit(aw20144, REG_PCCR, PCCR_PWMFRQ_MSK, PCCR_PWMFRQ_VAL);
		} else if (buf == 1) {
			aw20144_i2c_write_bit(aw20144, REG_PCCR, PCCR_PWMFRQ_MSK, PCCR_PWMFRQ_VAL);
		}
		break;
	default:
		LOG_INFO("%s:No match Mode.\n", __func__);
		break;
	}

	return 0;
}

static const struct file_operations aw20144_ioctl_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = aw20144_ioctl,
	.open = aw20144_open,
	.read = aw20144_read,
	.mmap = aw20144_mmap,
	.poll = aw20144_poll,
	.release = aw20144_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl = aw20144_ioctl,
#endif
};

static struct miscdevice led_strips_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "led_strips",
	.fops = &aw20144_ioctl_fops,
};

/*******************************************************************************
 *
 * aw20144 led init
 *
 ******************************************************************************/

static int aw20144_led_init(struct aw20144 *aw20144)
{
	int i = 0;

	LOG_INFO("%s: opMode %d imax %d(%d)\n", __func__, aw20144->operating_mode, aw20144->imax, aw20144->imax_target);
	if (aw20144->imax != aw20144->imax_target)
		aw20144->imax = aw20144->imax_target;

	/* enter page0 */
	aw20144_set_page(aw20144, AWPAGE0);
	/* set SW active number */
	aw20144_i2c_write_bit(aw20144, REG_GCR, GCR_SWSEL_MSK, GCR_SWSEL_VAL << GCR_SWSEL_POS);
	/* set global current */
	aw20144_i2c_write(aw20144, REG_GCCR, aw20144->imax);
	/* set PWM frequency */
	aw20144_i2c_write(aw20144, REG_PCCR, 0xC0);
	/* set constant current */
	aw20144_set_page(aw20144, AWPAGE2);
	for (i = 0; i <= AW20144_CFG_CNT_PAGE2; i++)
		aw20144_i2c_write(aw20144, REG_SL0 + i, aw20144->sl_current);

	return 0;
}

/*******************************************************************************
 *
 * aw20144 brightness work
 *
 ******************************************************************************/

static void aw20144_brightness_work(struct work_struct *work)
{
	struct aw20144 *aw20144 = container_of(work, struct aw20144, brightness_work);
	unsigned char reg_page1_pwm[AW20144_CFG_CNT_PAGE1];

	aw20144_set_page(aw20144, AWPAGE1);

	if (aw20144->cdev.brightness > aw20144->cdev.max_brightness)
		aw20144->cdev.brightness = aw20144->cdev.max_brightness;

	if (aw20144->cdev.brightness > 0) {
		/* set all led brightness */
		memset(reg_page1_pwm, aw20144->cdev.brightness, sizeof(reg_page1_pwm));
		aw20144_i2c_write_block(aw20144, 0x00, AW20144_CFG_CNT_PAGE1, reg_page1_pwm);
		/* set chip enable */
		aw20144_set_page(aw20144, AWPAGE0);
		aw20144_i2c_write_bit(aw20144, REG_GCR, BIT_CHIPEN_DISABLE, BIT_CHIPEN_ENABLE);
	} else {
		/* clear all led brightness */
		memset(reg_page1_pwm, 0, sizeof(reg_page1_pwm));
		aw20144_i2c_write_block(aw20144, 0x00, AW20144_CFG_CNT_PAGE1, reg_page1_pwm);
	}
}

static void aw20144_torch_work(struct work_struct *pwork)
{
	unsigned int i, num;
	unsigned char brightness[ALL_CHANNEL] = {0};
	struct delayed_work *pd = (struct delayed_work*)
		container_of(pwork, struct delayed_work, work);

	struct aw20144 *aw20144 = (struct aw20144 *)
		container_of(pd, struct aw20144, torch_work);
	int mapping_channels[VALID_CHANNEL] = {
				0, 18, 36, 54, 72, 1, 19, 37,
				55, 73, 91, 109, 127, 142, 12, 2,
				20, 38, 56, 74, 92, 110, 128, 124,
				106, 30, 3, 21, 39, 57, 75, 93,
				111, 129, 140, 141, 13, 48, 4, 22,
				40, 58, 76, 94, 112, 130, 122, 123,
				88, 31, 66, 5, 23, 41, 59, 77,
				95, 113, 131, 104, 105, 70, 49, 84,
				6, 24, 42, 60, 78, 96, 114, 132,
				86, 87, 52, 67, 102, 7, 25, 43,
				61, 79, 97, 115, 133, 68, 69, 34,
				85, 120, 8, 26, 44, 62, 80, 98,
				116, 134, 50, 51, 16, 138, 9, 27,
				45, 63, 81, 99, 117, 135, 32, 33,
				139, 10, 28, 46, 64, 82, 100, 118,
				136, 14, 15, 121, 11, 29, 47, 65,
				83, 101, 119, 137, 103, 17, 35, 53,
				71 };

	aw20144->torch_pwm = TORCH_BRIGHTNESS;
	LOG_INFO("%s enter set torch pwm val:%d\n", __func__, aw20144->torch_pwm);

	for (i = 0; i < VALID_CHANNEL; i++) {
		num = mapping_channels[i];
		brightness[num] = aw20144->torch_pwm;
	}
	/* enter page 1 */
	aw20144_set_page(aw20144, AWPAGE1);
	/* set all pwm value */
	aw20144_i2c_write_block(aw20144, 0x00, AW20144_CFG_CNT_PAGE1, brightness);

}

static void aw20144_set_brightness(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct aw20144 *aw20144 = container_of(cdev, struct aw20144, cdev);

	aw20144->cdev.brightness = brightness;
	schedule_work(&aw20144->brightness_work);
}

static void aw20144_rgbblink_cfg(struct aw20144 *aw20144, unsigned int *databuf)
{
	/* enter page0 */
	aw20144_set_page(aw20144, AWPAGE0);
	/* set PWMH0/PWML0 */
	aw20144->max_rgbblink = (databuf[1] & 0x0000ff00) >> 8;
	aw20144_i2c_write(aw20144, REG_PWMH0, aw20144->max_rgbblink);
	aw20144->min_rgbblink = (databuf[1] & 0x000000ff);
	aw20144_i2c_write(aw20144, REG_PWML0, aw20144->min_rgbblink);
	/* set rise/hold/fall/off time */
	aw20144->time_rise_hold = (databuf[2] & 0x0000ff00) >> 8;
	aw20144_i2c_write(aw20144, REG_PAT0T0, aw20144->time_rise_hold);
	aw20144->time_fall_off = (databuf[2] & 0x000000ff);
	aw20144_i2c_write(aw20144, REG_PAT0T1, aw20144->time_fall_off);
	/* set chip enable */
	aw20144_i2c_write_bit(aw20144, REG_GCR, BIT_CHIPEN_DISABLE, BIT_CHIPEN_ENABLE);
	/* set auto breath mode */
	aw20144_i2c_write_bit(aw20144, REG_PAT0CFG, BIT_PATMD_MANUAL, BIT_PATMD_AUTO);
	/* enable auto breath */
	aw20144_i2c_write_bit(aw20144, REG_PAT0CFG, BIT_PATEN_DISABLE, BIT_PATEN_ENABLE);
	/* run clear */
	aw20144_i2c_write_bit(aw20144, REG_PATGO, BIT_RUN0_DISABLE, BIT_RUN0_DISABLE);
	/* run auto breath */
	aw20144_i2c_write_bit(aw20144, REG_PATGO, BIT_RUN0_DISABLE, BIT_RUN0_ENABLE);
}

static void aw20144_cfg_bin_loaded(const struct firmware *cont, void *context)
{
	struct aw20144 *aw20144 = context;

	int i = 0;
	unsigned char page = 0;
	unsigned char reg_addr = 0;
	unsigned char reg_val = 0;

	if (!cont) {
		dev_err(aw20144->dev, "%s: no bin file found\n", __func__);
		release_firmware(cont);
		return;
	}

	for (i = 0; i < cont->size; i += 2) {
		if (*(cont->data + i) == AWPAGEADDR) {
			page = *(cont->data + i + 1);
			LOG_INFO("%s: enter page %x\n", __func__, page);
		}
		aw20144_i2c_write(aw20144, *(cont->data + i), *(cont->data + i + 1));

		reg_addr = *(cont->data + i);
		reg_val = *(cont->data + i + 1);
		if ((page == AWPAGE0) && (reg_addr == REG_RSTN) && (reg_val == AWREG_SWRST)) {
			usleep_range(5000, 5500);
			LOG_INFO("%s: software reset complete\n", __func__);
			aw20144_led_init(aw20144);
			LOG_INFO("%s: led init complete\n", __func__);
		}
	}

	release_firmware(cont);
	LOG_INFO("%s: config bin load complete\n", __func__);
}

static int aw20144_cfg_update_bin(struct aw20144 *aw20144)
{
	return request_firmware_nowait(THIS_MODULE, FW_ACTION_UEVENT,
				aw20144_cfg_bin[aw20144->designeffect],
				aw20144->dev, GFP_KERNEL, aw20144,
				aw20144_cfg_bin_loaded);
}

static int
aw20144_cfg_update_array(struct aw20144 *aw20144, unsigned char *cfg_data, unsigned int cfg_size)
{
	unsigned int i = 0;
	unsigned char page = 0;
	unsigned char reg_addr = 0;
	unsigned char reg_val = 0;

	for (i = 0; i < cfg_size; i += 2) {
		if (cfg_data[i] == AWPAGEADDR) {
			page = cfg_data[i + 1];
			LOG_INFO("%s: enter page %x\n", __func__, page);
		}

		aw20144_i2c_write(aw20144, cfg_data[i], cfg_data[i + 1]);

		reg_addr = cfg_data[i];
		reg_val = cfg_data[i + 1];
		if ((page == AWPAGE0) && (reg_addr == REG_RSTN) && (reg_val == AWREG_SWRST)) {
			usleep_range(5000, 5500);
			LOG_INFO("%s: software reset complete\n", __func__);
			aw20144_led_init(aw20144);
			LOG_INFO("%s: led init complete\n", __func__);
		}
	}

	LOG_INFO("%s: config array load complete\n", __func__);

	return 0;
}

/*******************************************************************************
 *
 * sysfs attribute group: design effect store
 *
 ******************************************************************************/

static ssize_t
designeffect_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	unsigned int databuf[1];
	int ret = -1;

	ret = kstrtou32(buf, 0, &databuf[0]);
	if (ret < 0) {
		dev_err(aw20144->dev, "%s: input data invalid!", __func__);
		return ret;
	}

	aw20144->designeffect = databuf[0];
	if (aw20144->effect_bin) {
		if (aw20144->designeffect < AW20144_EFFECT_CNT) {
			aw20144_cfg_update_bin(aw20144);
		} else {
			dev_err(aw20144->dev, "%s: input data out of range!\n", __func__);
			return -EAGAIN;
		}
	} else {
		if (aw20144->designeffect < AW20144_EFFECT_CNT) {
			aw20144_cfg_update_array(aw20144,
			aw20144_cfg_array[aw20144->designeffect].cfg_data,
			aw20144_cfg_array[aw20144->designeffect].cfg_size);
		} else {
			dev_err(aw20144->dev, "%s: input data out of range!\n", __func__);
			return -EAGAIN;
		}
	}

	return len;
}

static ssize_t imax_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	unsigned int databuf[1];
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	sscanf(buf, "%d", &databuf[0]);
	LOG_INFO("%s: imax %d opMode %d\n", __func__, databuf[0], aw20144->operating_mode);
	if (databuf[0] > 0xFF)
		databuf[0] = 0xFF;
	aw20144->imax_target = databuf[0];
	/*
	* imax is suggest to valid when led init.
	* aw20144_imax_cfg(aw20144, databuf[0]);
	*/

	return len;
}

static ssize_t imax_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	return sprintf(buf, "current imax = %d\n", aw20144->imax);
}

static ssize_t torch_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	unsigned int val = 0;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	sscanf(buf, "%d", &val);
	LOG_INFO("%s: set torch timing val:%d\n", __func__, val);

	if(val)
		schedule_delayed_work(&aw20144->torch_work, msecs_to_jiffies(1000*60*TORCH_TIMING_CFG));
	else {
		aw20144->torch_pwm = 0;
		cancel_delayed_work_sync(&aw20144->torch_work);
	}

	return len;
}

static ssize_t torch_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
    struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	return sprintf(buf, "%d\n", aw20144->torch_pwm);
}

static ssize_t effect_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	unsigned int i;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	if (aw20144->effect_bin) {
		for (i = 0; i < sizeof(aw20144_cfg_bin) / AW20144_CFG_NAME_MAX; i++) {
			len +=
				snprintf(buf + len, PAGE_SIZE - len, "cfg[%x] = %s\n", i,
					 aw20144_cfg_bin[i]);
		}
		len +=
			snprintf(buf + len, PAGE_SIZE - len, "current cfg = %s\n",
				 aw20144_cfg_bin[aw20144->effect]);
	} else {
		for (i = 0; i < sizeof(aw20144_cfg_array) / sizeof(struct awcfgdata);
			 i++) {
			len +=
				snprintf(buf + len, PAGE_SIZE - len, "cfg[%x] = %pf\n", i,
					 aw20144_cfg_array[i].cfg_data);
		}
		len +=
			snprintf(buf + len, PAGE_SIZE - len, "current cfg = %pf\n",
				 aw20144_cfg_array[aw20144->effect].cfg_data);
	}

	return len;
}

static ssize_t effect_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	unsigned int databuf[1];
	int ret = -1;

	ret = kstrtou32(buf, 0, &databuf[0]);
	if (ret < 0) {
		dev_err(aw20144->dev, "%s: input data invalid!", __func__);
		return ret;
	}

	aw20144->designeffect = databuf[0];
	if (aw20144->effect_bin) {
		if (aw20144->designeffect < AW20144_EFFECT_CNT) {
			aw20144_cfg_update_bin(aw20144);
		} else {
			dev_err(aw20144->dev, "%s: input data out of range!\n", __func__);
			return -EAGAIN;
		}
	} else {
		if (aw20144->designeffect < AW20144_EFFECT_CNT) {
			aw20144_cfg_update_array(aw20144,
			aw20144_cfg_array[aw20144->designeffect].cfg_data,
			aw20144_cfg_array[aw20144->designeffect].cfg_size);
		} else {
			dev_err(aw20144->dev, "%s: input data out of range!\n", __func__);
			return -EAGAIN;
		}
	}

	return len;
}

static ssize_t frame_brightness_store(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	char ch = ' ';
	const char *p;
	int val = 0;
	unsigned int frame_brightness[ALL_CHANNEL];
	int frame_num = 0;
	unsigned int i, num;
	unsigned char brightness[ALL_CHANNEL] = {0};

	pm_stay_awake(aw20144->dev);
	frame_num = 0;
	if (sscanf(buf, "%d", &val) == 1) {
		frame_brightness[frame_num] = val;
		p = strchr(buf, ch);
		while (p) {
			p = p + 1;
			if (sscanf(p, "%d", &val) == 1) {
				frame_num++;
				frame_brightness[frame_num] = val;
				p = strchr(p, ch);
			} else {
				break;
			}
		}
		frame_num++;
	}

	LOG_INFO("frame_num %d\n", frame_num);

	/*frame interface for 24111*/
	if (frame_num == ALL_CHANNEL) {
		int led_all[ALL_CHANNEL] = {18, 6, 29, 17, 5, 28, 16, 4, 27, 15, 3, 26, 14, 2, 25, 13, 1, 24, 12, 0, 8, 9, 21, 33, 32, 34, 10, 22, 11, 23, 35, 20, 31, 30, 19, 7};
		for (i = 0; i < ALL_CHANNEL; i++) {
			num = led_all[i];
			brightness[num] = frame_brightness[i];
		}
	} else if (frame_num == 20) {
		int led0[20] = {18, 6, 29, 17, 5, 28, 16, 4, 27, 15, 3, 26, 14, 2, 25, 13, 1, 24, 12, 0};
		for (i = 0; i < 20; i++) {
			num = led0[i];
			brightness[num] = frame_brightness[i];
		}
	} else if (frame_num == 5) {
		int led1[5] = {20, 31, 30, 19, 7};
		for (i = 0; i < 5; i++) {
			num = led1[i];
			brightness[num] = frame_brightness[i];
		}
	} else if (frame_num == 11) {
		int led2[11] = {8, 9, 21, 33, 32, 34, 10, 22, 11, 23, 35};
		for (i = 0; i < 5; i++) {
			num = led2[i];
			brightness[num] = frame_brightness[i];
		}
	} else if (frame_num == VALID_CHANNEL) {
		int mapping_channels[VALID_CHANNEL] = {
				0, 18, 36, 54, 72, 1, 19, 37,
				55, 73, 91, 109, 127, 142, 12, 2,
				20, 38, 56, 74, 92, 110, 128, 124,
				106, 30, 3, 21, 39, 57, 75, 93,
				111, 129, 140, 141, 13, 48, 4, 22,
				40, 58, 76, 94, 112, 130, 122, 123,
				88, 31, 66, 5, 23, 41, 59, 77,
				95, 113, 131, 104, 105, 70, 49, 84,
				6, 24, 42, 60, 78, 96, 114, 132,
				86, 87, 52, 67, 102, 7, 25, 43,
				61, 79, 97, 115, 133, 68, 69, 34,
				85, 120, 8, 26, 44, 62, 80, 98,
				116, 134, 50, 51, 16, 138, 9, 27,
				45, 63, 81, 99, 117, 135, 32, 33,
				139, 10, 28, 46, 64, 82, 100, 118,
				136, 14, 15, 121, 11, 29, 47, 65,
				83, 101, 119, 137, 103, 17, 35, 53,
				71 };
		for (i = 0; i < VALID_CHANNEL; i++) {
			num = mapping_channels[i];
			brightness[num] = frame_brightness[i];
		}
	}

	aw20144_set_page(aw20144, AWPAGE1);
	aw20144_i2c_write_block(aw20144, 0x00, ALL_CHANNEL, brightness); /*led(0-143)*/
	pm_relax(aw20144->dev);

	return len;
}

/*******************************************************************************
 *
 * hardware enable/off
 *
 ******************************************************************************/

static int aw20144_hw_enable(struct aw20144 *aw20144)
{
	if (aw20144 && gpio_is_valid(aw20144->enable_gpio)) {
		gpio_set_value_cansleep(aw20144->enable_gpio, 0);
		usleep_range(2000, 2500);

		gpio_set_value_cansleep(aw20144->enable_gpio, 1);
		usleep_range(3000, 3500);
		LOG_INFO("%s: set gpio high\n", __func__);
	} else {
		LOG_ERR("%s: aw20144 or gpio unavailable", __func__);
		return -EIO;
	}

	return 0;
}

static int aw20144_hw_off(struct aw20144 *aw20144)
{
	if (aw20144 && gpio_is_valid(aw20144->enable_gpio)) {
		gpio_set_value_cansleep(aw20144->enable_gpio, 0);
		usleep_range(2000, 2500);
		LOG_INFO("%s: set gpio low\n", __func__);
	} else {
		LOG_ERR("%s: aw20144 or gpio unavailable\n", __func__);
		return -EIO;
	}

	return 0;
}

static ssize_t operating_mode_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	return sprintf(buf, "%d\n", aw20144->operating_mode);
}

static ssize_t operating_mode_store(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t len)
{
	unsigned int val;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	sscanf(buf, "%d", &val);

	LOG_INFO("%s: opMode %d -> %d imax %d(%d)\n", __func__, aw20144->operating_mode, val, aw20144->imax, aw20144->imax_target);

	if (val == 1) {/*active*/
		if (aw20144->operating_mode == 0) {
			aw20144_hw_enable(aw20144);
			aw20144_led_init(aw20144);
		#ifdef POWER_SAVE_MODE
			aw20144_set_page(aw20144, AWPAGE0);
			aw20144_i2c_write_bit(aw20144, REG_GCR, BIT_CHIPEN_DISABLE, BIT_CHIPEN_ENABLE);
			//aw20144_rgbcolor_config(aw20144);
		#endif
			aw20144->operating_mode = 1;
		} else if (aw20144->operating_mode == 2) {
			aw20144_set_page(aw20144, AWPAGE0);
			aw20144_i2c_write_bit(aw20144, REG_GCR, BIT_CHIPEN_DISABLE, BIT_CHIPEN_ENABLE);
			//aw20144_rgbcolor_config(aw20144);
			aw20144->operating_mode = 1;
		} else if ((aw20144->operating_mode == 1) && (aw20144->imax != aw20144->imax_target)) {
			aw20144_hw_enable(aw20144);
			aw20144_led_init(aw20144);
		#ifdef POWER_SAVE_MODE
			aw20144_set_page(aw20144, AWPAGE0);
			aw20144_i2c_write_bit(aw20144, REG_GCR, BIT_CHIPEN_DISABLE, BIT_CHIPEN_ENABLE);
			//aw20144_rgbcolor_config(aw20144);
		#endif
		}
	} else if (val == 2) {/*stand-by*/
		if (aw20144->operating_mode == 0) {
			aw20144_hw_enable(aw20144);
			aw20144->operating_mode = 2;
		} else if (aw20144->operating_mode == 1) {
			aw20144_set_page(aw20144, AWPAGE0);
			aw20144_i2c_write_bit(aw20144, REG_GCR, BIT_CHIPEN_DISABLE, BIT_CHIPEN_DISABLE);
			aw20144->operating_mode = 2;
		}
	} else if (val == 0) {/*shut down*/
		aw20144_hw_off(aw20144);
		aw20144->operating_mode = 0;
	}

	return len;
}

static ssize_t hwid_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	LOG_INFO("%s\n", __func__);

	return sprintf(buf, "%s\n", hw_ver);
}

static ssize_t hwid_store(struct device *dev,
						  struct device_attribute *attr,
						  const char *buf, size_t len)
{
	//struct led_classdev *led_cdev = dev_get_drvdata(dev);
	//struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	LOG_INFO("%s %s %ld\n", __func__, buf, len);

	if (len > sizeof(hw_ver)) {
		LOG_INFO("%s: invalid hwid \n", __func__);
		return len;
	}
	memset(hw_ver, 0, sizeof(hw_ver));
	strcpy(hw_ver, buf);

	return len;
}

static ssize_t dev_color_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	LOG_INFO("%s\n", __func__);

	return sprintf(buf, "%s\n", dev_color);
}

static ssize_t dev_color_store(struct device *dev,
									struct device_attribute *attr,
									const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	LOG_INFO("%s %s %ld\n", __func__, buf, len);

	if (len > sizeof(dev_color)) {
		LOG_INFO("%s: invalid hwid \n", __func__);
		return len;
	}
	memset(dev_color, 0, sizeof(dev_color));
	strcpy(dev_color, buf);
	if (strncmp(dev_color, "BLACK", 5) == 0) {
		LOG_INFO("%s: device color is black\n", __func__);
		aw20144_imax_cfg(aw20144, AW20144_BLACK_IMAX);
	} else {
		aw20144_imax_cfg(aw20144, AW20144_WHITE_IMAX);//default is white
	}
	aw20144_rgbcolor_config(aw20144);

	return len;
}

unsigned int factory_led;
static ssize_t factory_test_show(struct device *dev,
								struct device_attribute *attr, char *buf)
{
	LOG_INFO("%s\n", __func__);

	return sprintf(buf, "%d\n", factory_led);
}

static ssize_t factory_test_store(struct device *dev,
						  struct device_attribute *attr,
						  const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	int ret = 0;
	int led_num = 0;
	unsigned char reg_page1_pwm[AW20144_CFG_CNT_PAGE1];
	unsigned int databuf[1] = { 0 };

	ret = kstrtou32(buf, 0, &databuf[0]);
	if (ret < 0) {
		dev_err(aw20144->dev, "%s: input data invalid!", __func__);
		return ret;
	}

	LOG_INFO("%s enter val:%d\n", __func__, databuf[0]);
	/* enter page 1 */
	aw20144_set_page(aw20144, AWPAGE1);

	for (led_num = 0; led_num < AW20144_CFG_CNT_PAGE1; led_num++) {
		reg_page1_pwm[led_num] = databuf[0];
	}
	/* set all pwm value */
	aw20144_i2c_write_block(aw20144, 0x00, AW20144_CFG_CNT_PAGE1, reg_page1_pwm);

#ifdef POWER_SAVE_MODE
	/* set chip enable */
	aw20144_set_page(aw20144, AWPAGE0);
	aw20144_i2c_write_bit(aw20144, REG_GCR, BIT_CHIPEN_DISABLE, BIT_CHIPEN_ENABLE);
	aw20144->operating_mode =1;
#endif

	factory_led = databuf[0];

	return len;
}

static ssize_t always_on_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	LOG_INFO("%s aw20144->always_on:%d\n", __func__, aw20144->always_on);

	return sprintf(buf, "%d\n", aw20144->always_on);
}

static ssize_t always_on_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	unsigned int val;

	sscanf(buf, "%d", &val);

	LOG_INFO("%s val:%d\n", __func__, val);

	aw20144->always_on = val;

	return len;
}

static ssize_t vendor_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	int len = 0;

	LOG_INFO("%s\n", __func__);

	if (0x20 == aw20144->addr)
		len += snprintf(buf + len, PAGE_SIZE - len, "%s\n", vendor_id0);
	else if (0x21 == aw20144->addr)
		len += snprintf(buf + len, PAGE_SIZE - len, "%s\n", vendor_id1);
	else
		len += snprintf(buf + len, PAGE_SIZE - len, "%s\n", "Unknow vendor id");

	return len;
}

static ssize_t light_id_info_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	int32_t var1 = 0, var2 = 0;
	struct timespec64 ts;
	struct rtc_time tm;

	if (sscanf(buf, "%d %d", &var1, &var2) == 2) {
		ktime_get_real_ts64(&ts);
		rtc_time64_to_tm(ts.tv_sec, &tm);
		LOG_INFO("LightID: %d on %d %d-%02d-%02d %02d:%02d:%02d.%09lu UTC\n",
			var1, var2, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
	}

	return len;
}

static ssize_t single_brightness_store(struct device *dev,
							struct device_attribute *attr,
							const char *buf, size_t len)
{
	unsigned int databuf[2] = { 0, 0 };
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	if (sscanf(buf, "%d %d", &databuf[0], &databuf[1]) == 2) {
		aw20144_set_page(aw20144, AWPAGE1);
		aw20144_i2c_write(aw20144, databuf[0], databuf[1]);
	}

	return len;
}

static ssize_t all_white_brightness_store(struct device *dev,
							struct device_attribute *attr,
							const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	unsigned int i, num;
	unsigned char brightness[ALL_CHANNEL] = {0};
	unsigned int databuf[1] = { 0 };
	int ret = -1;

	ret = kstrtou32(buf, 0, &databuf[0]);
	if (ret < 0) {
		dev_err(aw20144->dev, "%s: input data invalid!", __func__);
		return ret;
	}

	int mapping_channels[VALID_CHANNEL] = {
				0, 18, 36, 54, 72, 1, 19, 37,
				55, 73, 91, 109, 127, 142, 12, 2,
				20, 38, 56, 74, 92, 110, 128, 124,
				106, 30, 3, 21, 39, 57, 75, 93,
				111, 129, 140, 141, 13, 48, 4, 22,
				40, 58, 76, 94, 112, 130, 122, 123,
				88, 31, 66, 5, 23, 41, 59, 77,
				95, 113, 131, 104, 105, 70, 49, 84,
				6, 24, 42, 60, 78, 96, 114, 132,
				86, 87, 52, 67, 102, 7, 25, 43,
				61, 79, 97, 115, 133, 68, 69, 34,
				85, 120, 8, 26, 44, 62, 80, 98,
				116, 134, 50, 51, 16, 138, 9, 27,
				45, 63, 81, 99, 117, 135, 32, 33,
				139, 10, 28, 46, 64, 82, 100, 118,
				136, 14, 15, 121, 11, 29, 47, 65,
				83, 101, 119, 137, 103, 17, 35, 53,
				71 };

	LOG_INFO("%s enter set val:%d\n", __func__, databuf[0]);

	for (i = 0; i < VALID_CHANNEL; i++) {
		num = mapping_channels[i];
		brightness[num] = databuf[0];
	}
	/* enter page 1 */
	aw20144_set_page(aw20144, AWPAGE1);
	/* set all pwm value */
	aw20144_i2c_write_block(aw20144, 0x00, AW20144_CFG_CNT_PAGE1, brightness);

	return len;
}

/*******************************************************************************
 *
 * sysfs attribute group: allrgbblink store
 *
 ******************************************************************************/

static ssize_t
allrgbblink_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	unsigned char reg_page3_pwm[AW20144_CFG_CNT_PAGE3];
	unsigned int databuf[3] = { 0, 0, 0 };

	/* enter page 3 */
	aw20144_set_page(aw20144, AWPAGE3);

	if (sscanf(buf, "%x %x %x", &databuf[0], &databuf[1], &databuf[2]) == 3) {
		/* set rgb blink value */
		aw20144->rgb_color = (databuf[0] & 0x000000ff);
		memset(reg_page3_pwm, aw20144->rgb_color, sizeof(reg_page3_pwm));
		aw20144_i2c_write_block(aw20144, 0x00, AW20144_CFG_CNT_PAGE3, reg_page3_pwm);
		/* blink parameter configuration */
		aw20144_rgbblink_cfg(aw20144, databuf);
	} else {
		dev_err(aw20144->dev, "%s: input data invalid!", __func__);
		return -EAGAIN;
	}

	return len;
}

/*******************************************************************************
 *
 * sysfs attribute group: onergbblink store
 *
 ******************************************************************************/

static ssize_t onergbblink_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	unsigned int databuf[3] = { 0, 0, 0 };

	if (sscanf(buf, "%x %x %x", &databuf[0], &databuf[1], &databuf[2]) == 3) {
		/* enter page 3 */
		aw20144_set_page(aw20144, AWPAGE3);
		/* select rgb and color */
		aw20144->rgb_num = (databuf[0] & 0x0000ff00) >> 8;
		aw20144->rgb_color = (databuf[0] & 0x000000ff);
		if (aw20144->rgb_num <= AW20144_RGB_NUM) {
			aw20144_i2c_write(aw20144, aw20144->rgb_num, aw20144->rgb_color);
			/* blink parameter configuration */
			aw20144_rgbblink_cfg(aw20144, databuf);
		} else {
			dev_err(aw20144->dev, "%s: rgb number invalid!", __func__);
			return -EAGAIN;
		}
	} else {
		dev_err(aw20144->dev, "%s: input data invalid!", __func__);
		return -EAGAIN;
	}

	return len;
}

/*******************************************************************************
 *
 * sysfs attribute group: allrgbbrightness store
 *
 ******************************************************************************/

static ssize_t allrgbbrightness_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	int led_num = 0;
	unsigned char reg_page1_pwm[AW20144_CFG_CNT_PAGE1];
	unsigned int databuf[1] = { 0 };
	int ret = -1;

	ret = kstrtou32(buf, 0, &databuf[0]);
	if (ret < 0) {
		dev_err(aw20144->dev, "%s: input data invalid!", __func__);
		return ret;
	}

	/* enter page 1 */
	aw20144_set_page(aw20144, AWPAGE1);

	for (led_num = 0; led_num < AW20144_CFG_CNT_PAGE1; led_num += 3) {
		reg_page1_pwm[led_num] = (databuf[0] & 0x00ff0000) >> 16;
		reg_page1_pwm[led_num + 1] = (databuf[0] & 0x0000ff00) >> 8;
		reg_page1_pwm[led_num + 2] = (databuf[0] & 0x000000ff);
	}
	/* set all pwm value */
	aw20144_i2c_write_block(aw20144, 0x00, AW20144_CFG_CNT_PAGE1, reg_page1_pwm);

	/* set chip enable */
	aw20144_set_page(aw20144, AWPAGE0);
	aw20144_i2c_write_bit(aw20144, REG_GCR, BIT_CHIPEN_DISABLE, BIT_CHIPEN_ENABLE);

	return len;
}

/*******************************************************************************
 *
 * sysfs attribute group: onerrgbbrightness store
 *
 ******************************************************************************/

static ssize_t onergbbrightness_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	unsigned int databuf[2] = { 0, 0 };

	if (sscanf(buf, "%x %x", &databuf[0], &databuf[1]) == 2) {
		/* enter page 1 */
		aw20144_set_page(aw20144, AWPAGE1);
		if (databuf[0] <= AW20144_RGB_NUM) {
			aw20144->rgbbrightness = (databuf[1] & 0x00ff0000) >> 16;
			aw20144_i2c_write(aw20144, databuf[0] * 3, aw20144->rgbbrightness);
			aw20144->rgbbrightness = (databuf[1] & 0x0000ff00) >> 8;
			aw20144_i2c_write(aw20144, (databuf[0] * 3 + 1), aw20144->rgbbrightness);
			aw20144->rgbbrightness = (databuf[1] & 0x000000ff);
			aw20144_i2c_write(aw20144, (databuf[0] * 3 + 2), aw20144->rgbbrightness);
			/* enter page 0 */
			aw20144_set_page(aw20144, AWPAGE0);
			/* set chip enable */
			aw20144_i2c_write_bit(aw20144, REG_GCR,
				BIT_CHIPEN_DISABLE, BIT_CHIPEN_ENABLE);
		} else {
			dev_err(aw20144->dev, "%s: rgb number invalid!", __func__);
			return -EAGAIN;
		}
	} else {
		dev_err(aw20144->dev, "%s: input data invalid!", __func__);
		return -EAGAIN;
	}

	return len;
}

static ssize_t opendetect_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	uint8_t val = 0, err_num = 0;
	uint8_t i, j = 0;
	ssize_t len = 0;

	/* 1. enable chipen and set SW number and enable open detect */
	/* enter page0 */
	aw20144_set_page(aw20144, AWPAGE0);
	/* set SW active number */
	aw20144_i2c_write_bit(aw20144, REG_GCR, GCR_SWSEL_MSK, GCR_SWSEL_VAL << GCR_SWSEL_POS);
	aw20144_i2c_write_bit(aw20144, REG_GCR, GCR_OSDE_MSK, GCR_OSDE_OPEN_VAL << GCR_OSDE_POS);
	aw20144_i2c_write(aw20144, REG_SRCR, 0x22);//0x2B 0x22
	aw20144_i2c_write(aw20144, REG_DGCR, 0xC9);//0x02 0xC9
	mdelay(100);

	for (i = 0; i < AW20144_OSR_REG_NUM; i++) {
		aw20144_i2c_read(aw20144, REG_OSR0 + i, &val);
		for (j = 0; j < 6; j++) {
			if (val & 0x01) {
				LOG_INFO("%s : channel %d open detected\n",
						__func__, 6 * i + j);
				len += snprintf(buf + len, PAGE_SIZE - len,
						"Open detected channel: %d\n",
						6 * i + j);
				err_num++;
			}
			val = val >> 1;
		}
	}
	if (err_num == 0) {
		LOG_INFO("%s : open detected pass\n", __func__);
		len += snprintf(buf + len, PAGE_SIZE - len,
				"Open detected Pass\n");
	} else {
		LOG_INFO("%s : open detected fail\n", __func__);
		len += snprintf(buf + len, PAGE_SIZE - len,
				"Open detected Fail\n");
	}

	return len;
}

static ssize_t shortdetect_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	uint8_t val = 0, err_num = 0;
	uint8_t i, j = 0;
	ssize_t len = 0;

	/* 1. enable chipen and set SW number and enable open detect */
	/* enter page0 */
	aw20144_set_page(aw20144, AWPAGE0);
	/* set SW active number */
	aw20144_i2c_write_bit(aw20144, REG_GCR, GCR_SWSEL_MSK, GCR_SWSEL_VAL << GCR_SWSEL_POS);
	aw20144_i2c_write_bit(aw20144, REG_GCR, GCR_OSDE_MSK, GCR_OSDE_SHORT_VAL << GCR_OSDE_POS);
	mdelay(100);

	for (i = 0; i < AW20144_OSR_REG_NUM; i++) {
		aw20144_i2c_read(aw20144, REG_OSR0 + i, &val);
		for (j = 0; j < 6; j++) {
			if (val & 0x01) {
				LOG_INFO("%s channel %d short detected\n",
						__func__, 6 * i + j);
				len += snprintf(buf + len, PAGE_SIZE - len,
						"Short detected channel: %d\n",
						6 * i + j);
				err_num++;
			}
			val = val >> 1;
		}
	}
	if (err_num == 0) {
		LOG_INFO("%s : short detected pass\n", __func__);
		len += snprintf(buf + len, PAGE_SIZE - len,
				"Short detected Pass\n");
	} else {
		LOG_INFO("%s : short detected fail\n", __func__);
		len += snprintf(buf + len, PAGE_SIZE - len,
				"Short detected Fail\n");
	}

	return len;
}

/*******************************************************************************
 *
 * sysfs attribute group: reg store/show
 *
 ******************************************************************************/

static ssize_t
reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	ssize_t len = 0;
	unsigned char i = 0;
	unsigned char reg_val = 0;

	/* enter page 0 */
	aw20144_set_page(aw20144, AWPAGE0);
	for (i = 0; i < AW20144_REG_PAGE0_MAX; i++) {
		if (!(aw20144_reg_page0_access[i] & REG_RD_ACCESS))
			continue;
		aw20144_i2c_read(aw20144, i, &reg_val);
		len += snprintf(buf + len, PAGE_SIZE - len,
				"PAGE0 reg: 0x%02x = 0x%02x\n", i, reg_val);
	}

	return len;
}

static ssize_t
reg_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);

	unsigned int databuf[3] = { 0, 0, 0 };

	if (sscanf(buf, "%x %x %x",
			&databuf[0], &databuf[1], &databuf[2]) == 3) {
		if (databuf[0] == AWPAGE0) {
			/* select page */
			aw20144_set_page(aw20144, databuf[0]);
			/* write value in address */
			aw20144_i2c_write(aw20144, databuf[1], databuf[2]);
		} else {
			dev_err(aw20144->dev, "%s: input reg page invalid!\n", __func__);
		}
	} else {
		dev_err(aw20144->dev, "%s: input reg data format err\n", __func__);
	}

	return len;
}

/*******************************************************************************
 *
 * sysfs attribute group: soft reset store/show
 *
 ******************************************************************************/

static ssize_t
swrst_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	unsigned int databuf[3] = { 0, 0, 0 };

	if (sscanf(buf, "%x %x %x", &databuf[0], &databuf[1], &databuf[2]) == 3) {
		/* select page */
		aw20144_set_page(aw20144, databuf[0]);
		/* software reset  */
		aw20144_i2c_write(aw20144, databuf[1], databuf[2]);
		usleep_range(8000, 85000);
		LOG_INFO("%s: software reset complete\n", __func__);
		aw20144_led_init(aw20144);
		LOG_INFO("%s: led init complete\n", __func__);
	} else {
		dev_err(aw20144->dev,
			"%s: input reg data format err\n", __func__);
	}

	return len;
}

/*******************************************************************************
 *
 * sysfs attribute group: hwen store/show
 *
 ******************************************************************************/

static ssize_t
hwen_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	unsigned int databuf[1] = { 0 };
	int ret = -1;

	ret = kstrtou32(buf, 0, &databuf[0]);
	if (ret < 0) {
		dev_err(aw20144->dev, "%s: input data invalid!", __func__);
		return ret;
	}

	if (databuf[0] == 1) {
		aw20144_hw_enable(aw20144);
		LOG_INFO("%s: hw enable complete\n", __func__);
	} else {
		aw20144_hw_off(aw20144);
		LOG_INFO("%s: hw off complete\n", __func__);
	}

	return len;
}

static ssize_t
hwen_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	ssize_t len = 0;

	len +=
	snprintf(buf + len, PAGE_SIZE - len, "hwen = %d\n", gpio_get_value(aw20144->enable_gpio));

	return len;
}

static ssize_t freq_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	unsigned int freq = 0;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	int ret = 0;

	sscanf(buf, "%d", &freq);

	if (freq > 7)
		freq = 7;

	ret = aw20144_set_page(aw20144, AWPAGE0);
	if (ret < 0) {
		return ret;
	}

	ret = aw20144_i2c_write(aw20144, REG_PCCR, freq << 5);
	if (ret < 0) {
		return ret;
	}

	return len;
}

static ssize_t freq_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	unsigned char freq = 0;
	int i,ret = 0;
	ssize_t len = 0;

	ret = aw20144_set_page(aw20144, AWPAGE0);
	if (ret < 0) {
		return ret;
	}

	ret = aw20144_i2c_read(aw20144, REG_PCCR, &freq);
	if (ret < 0) {
		return ret;
	}

	freq >>= 5;
	for(i = 0; i < AW20144_CFG_FREQ_MAX; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len,
				"freq[%d] = %s\n", i, freqs[i]);
	}

	len += snprintf(buf + len, PAGE_SIZE - len,
		"\ncurrent freq[%d] = %s\n", freq, freqs[freq]);

	return len;
}

static ssize_t chipid_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw20144 *aw20144 = container_of(led_cdev, struct aw20144, cdev);
	int ret = -1;
	unsigned char cnt = 0;
	unsigned char reg_val = 0;
	ssize_t len = 0;

	/* hardware enable */
	ret = aw20144_hw_enable(aw20144);
	if (ret)
		dev_err(aw20144->dev, "%s: chipid show hardware enable failed", __func__);
	usleep_range(2000, 2500);
	/*  enter page0 */
	aw20144_set_page(aw20144, AWPAGE0);
	while (cnt < AW20144_READ_CHIPID_RETRIES) {
		ret = aw20144_i2c_read(aw20144, REG_RSTN, &reg_val);
		LOG_INFO("chipid show is 0x%x\n", reg_val);
		if ((reg_val == AW20144_CHIPID) || (reg_val == AW20144_CHIPID_A2)) {
			LOG_INFO("chipid show successful\n");
			len +=
			snprintf(buf + len, PAGE_SIZE - len, "0x%x\n", reg_val);
			return len;
		} else {
			dev_err(aw20144->dev, "chipid show failed, err=%d\n", ret);
			cnt++;
			usleep_range(1000, 1500);
		}
	}

	return len;
}

static DEVICE_ATTR_WO(designeffect);
static DEVICE_ATTR_WO(allrgbblink);
static DEVICE_ATTR_WO(onergbblink);
static DEVICE_ATTR_WO(allrgbbrightness);
static DEVICE_ATTR_WO(onergbbrightness);
static DEVICE_ATTR_RO(opendetect);
static DEVICE_ATTR_RO(shortdetect);
static DEVICE_ATTR_RW(reg);
static DEVICE_ATTR_WO(swrst);
static DEVICE_ATTR_RW(hwen);
static DEVICE_ATTR_RW(imax);
static DEVICE_ATTR_RW(torch);
static DEVICE_ATTR_RW(effect);
static DEVICE_ATTR_WO(frame_brightness);
static DEVICE_ATTR_RW(operating_mode);
static DEVICE_ATTR_RW(hwid);
static DEVICE_ATTR_RW(dev_color);
static DEVICE_ATTR_RW(factory_test);
static DEVICE_ATTR_RW(always_on);
static DEVICE_ATTR_WO(all_white_brightness);
static DEVICE_ATTR_WO(single_brightness);
static DEVICE_ATTR_RO(vendor);
static DEVICE_ATTR_WO(light_id_info);
static DEVICE_ATTR_RW(freq);
static DEVICE_ATTR_RO(chipid);

static struct attribute *aw20144_led_attributes[] = {
	&dev_attr_opendetect.attr,
	&dev_attr_shortdetect.attr,
	&dev_attr_reg.attr,
	&dev_attr_hwen.attr,
	&dev_attr_imax.attr,
	&dev_attr_torch.attr,
	&dev_attr_effect.attr,
	&dev_attr_swrst.attr,
	&dev_attr_onergbbrightness.attr,
	&dev_attr_allrgbbrightness.attr,
	&dev_attr_onergbblink.attr,
	&dev_attr_allrgbblink.attr,
	&dev_attr_designeffect.attr,
	&dev_attr_frame_brightness.attr,
	&dev_attr_operating_mode.attr,
	&dev_attr_hwid.attr,
	&dev_attr_dev_color.attr,
	&dev_attr_factory_test.attr,
	&dev_attr_always_on.attr,
	&dev_attr_all_white_brightness.attr,
	&dev_attr_single_brightness.attr,
	&dev_attr_vendor.attr,
	&dev_attr_light_id_info.attr,
	&dev_attr_freq.attr,
	&dev_attr_chipid.attr,
	NULL
};

static struct attribute_group aw20144_attribute_group = {
	.attrs = aw20144_led_attributes
};

/*******************************************************************************
 *
 * read chip id
 *
 ******************************************************************************/

static int aw20144_read_chipid(struct aw20144 *aw20144)
{
	int ret = -1;
	unsigned char cnt = 0;
	unsigned char reg_val = 0;

	/* hardware enable */
	ret = aw20144_hw_enable(aw20144);
	if (ret)
		dev_err(aw20144->dev, "%s: hardware enable failed", __func__);

	while (cnt < AW20144_READ_CHIPID_RETRIES) {
		ret = aw20144_i2c_read(aw20144, REG_RSTN, &reg_val);
		LOG_INFO("AW20144 chip id is %0x\n", reg_val);
		if ((reg_val == AW20144_CHIPID) || (reg_val == AW20144_CHIPID_A2)) {
			LOG_INFO("read aw20144 chipid successful\n");
			return 0;
		}

		dev_err(aw20144->dev, "read aw20144 id failed, err=%d\n", ret);
		cnt++;
		usleep_range(1000, 1500);
	}

	return ret;
}

/*******************************************************************************
 *
 * parse device tree
 *
 ******************************************************************************/

static int aw20144_parse_dts(struct aw20144 *aw20144, struct device_node *np)
{
	int ret = -1;

	aw20144->enable_gpio = of_get_named_gpio(np, "enable-gpio", 0);
	if (gpio_is_valid(aw20144->enable_gpio)) {
		dev_info(aw20144->dev, "%s: enable gpio available\n", __func__);
	} else {
		dev_err(aw20144->dev, "%s: enable gpio unavailable\n", __func__);
		return -EIO;
	}

	ret = of_property_read_u32(np, "aw20144,imax", &aw20144->imax);
	if (ret < 0) {
		dev_err(aw20144->dev, "%s: parse imax err, ret = %d\n", __func__, ret);
		return ret;
	}
	aw20144->imax_target = aw20144->imax;
	LOG_INFO("%s: led imax = 0x%x\n", __func__, aw20144->imax);

	ret = of_property_read_u32(np, "aw20144,sl_current", &aw20144->sl_current);
	if (ret < 0) {
		dev_err(aw20144->dev, "%s: parse sl_current err, ret = %d\n", __func__, ret);
		return ret;
	}
	LOG_INFO("%s: led sl_current = 0x%x\n", __func__, aw20144->sl_current);

	ret = of_property_read_u32(np, "aw20144,max_brightness", &aw20144->cdev.max_brightness);
	if (ret < 0) {
		dev_err(aw20144->dev,
		"%s: parse max-brightness err, ret = %d\n", __func__, ret);
		return ret;
	}
	LOG_INFO("%s: led max brightness = 0x%x\n", __func__, aw20144->cdev.max_brightness);

	aw20144->effect_bin = of_property_read_bool(np, "aw20144,effect-bin");
	if (aw20144->effect_bin)
		LOG_INFO("%s: led effect use bin\n", __func__);

	return 0;
}

/*******************************************************************************
 *
 * i2c driver probe
 *
 ******************************************************************************/

static int aw20144_i2c_probe(struct i2c_client *client)
{
	struct aw20144 *aw20144;
	struct device_node *np = client->dev.of_node;
	int ret = -1;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "%s: check i2c error\n", __func__);
		return -ENODEV;
	}

	aw20144 = devm_kzalloc(&client->dev, sizeof(struct aw20144), GFP_KERNEL);
	if (aw20144 == NULL) {
		ret = -ENOMEM;
		goto err_devm_kzalloc;
	}

	aw20144->cdev.name = AW20144_I2C_NAME;
	aw20144->client = client;
	aw20144->dev = &client->dev;
	aw20144->addr = client->addr;
	dev_info(&client->dev, "%s: i2c address is 0x%x\n", __func__, aw20144->addr);

	/* be used in aw20144_i2c_remove */
	i2c_set_clientdata(client, aw20144);

	/* parse device tree */
	if (np) {
		ret = aw20144_parse_dts(aw20144, np);
		if (ret) {
			dev_err(&client->dev, "%s: parse dts failed\n", __func__);
			goto err_parse_dts;
		} else {
			LOG_INFO("%s: parse dts successful\n", __func__);
		}
	} else {
		dev_err(&client->dev, "%s: np is NULL\n", __func__);
		goto err_np_null;
	}

	/* init enable gpio */
	if (gpio_is_valid(aw20144->enable_gpio)) {
		ret = devm_gpio_request_one(&client->dev,
						aw20144->enable_gpio,
						GPIOF_OUT_INIT_LOW,
						"aw20144_enable_gpio");
		if (ret) {
			dev_err(&client->dev,
				"%s: gpio request failed\n", __func__);
			if(!device_detected) ret = -EPROBE_DEFER;
			goto err_gpio_request;
		}
	}

	/* read chip id */
	ret = aw20144_read_chipid(aw20144);
	if (ret < 0) {
		dev_err(&client->dev, "%s: read chipid error\n", __func__);
		goto err_read_chipid;
	}

	aw20144_led_init(aw20144);

	INIT_WORK(&aw20144->brightness_work, aw20144_brightness_work);
	aw20144->cdev.brightness_set = aw20144_set_brightness;

	INIT_DELAYED_WORK(&aw20144->torch_work, aw20144_torch_work);

	ret = led_classdev_register(aw20144->dev, &aw20144->cdev);
	if (ret) {
		dev_err(aw20144->dev,
			"%s: classdev register failed, ret = %d\n", __func__, ret);
		goto err_register_class;
	} else {
		LOG_INFO("%s: classdev register successful\n", __func__);
	}

	ret = sysfs_create_group(&aw20144->cdev.dev->kobj, &aw20144_attribute_group);
	if (ret) {
		dev_err(aw20144->dev,
			"%s: sysfs creat group failed, ret = %d\n", __func__, ret);
		goto error_create_group;
	} else {
		LOG_INFO("%s: sysfs creat group successful\n", __func__);
	}

	aw20144->start_buf = (struct mmap_buf_format *)__get_free_pages(GFP_KERNEL, LED_MMAP_PAGE_ORDER);
	if (aw20144->start_buf == NULL) {
		dev_err(&client->dev, "Error __get_free_pages failed\n");
	}
	SetPageReserved(virt_to_page(aw20144->start_buf));
	{
		struct mmap_buf_format *temp;
		uint32_t i = 0;
		temp = aw20144->start_buf;
		for (i = 1; i < LED_MMAP_BUF_SUM; i++) {
			temp->kernel_next = (aw20144->start_buf + i);
			temp = temp->kernel_next;
		}
		temp->kernel_next = aw20144->start_buf;

		temp = aw20144->start_buf ;
		for (i = 0; i < LED_MMAP_BUF_SUM; i++) {
			temp->bit = i;
			temp = temp->kernel_next;
		}
	}

	LOG_INFO("%s leds_workqueue\n", __func__);
	aw20144->leds_workqueue = create_singlethread_workqueue("leds_wq");
	if (!aw20144->leds_workqueue) {
			dev_err(&client->dev, "%s create leds workqueue fail\n", __func__);
	} else {
		LOG_INFO("%s creat work\n", __func__);
		INIT_WORK(&aw20144->leds_effect_work, aw20144_leds_effect_work);
	}

	init_completion(&aw20144->completion);
	ret = misc_register(&led_strips_dev);
	if (ret) {
		dev_err(&client->dev, "%s: misc_register failed\n", __func__);
	}
	g_aw20144 = aw20144;

	LOG_INFO("%s device_init_wakeup\n", __func__);
	device_init_wakeup(aw20144->dev, true);
	LOG_INFO("%s probe completed successfully!\n", __func__);

	device_detected = 1;

	return 0;

error_create_group:
	led_classdev_unregister(&aw20144->cdev);
err_register_class:
err_read_chipid:
	gpio_free(aw20144->enable_gpio);
err_gpio_request:
err_np_null:
err_parse_dts:
	devm_kfree(&client->dev, aw20144);
err_devm_kzalloc:

	return ret;
}

/*******************************************************************************
 *
 * i2c driver remove
 *
 ******************************************************************************/

static void aw20144_i2c_remove(struct i2c_client *client)
{
	struct aw20144 *aw20144 = i2c_get_clientdata(client);

	cancel_work_sync(&aw20144->brightness_work);

	if (gpio_is_valid(aw20144->enable_gpio))
		gpio_free(aw20144->enable_gpio);

	devm_kfree(&client->dev, aw20144);
}

static const struct i2c_device_id aw20144_i2c_id[] = {
	{AW20144_I2C_NAME, 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, aw20144_i2c_id);

static const struct of_device_id aw20144_dt_match[] = {
	{.compatible = "awinic,aw20144_led"},
	{},
};

static struct i2c_driver aw20144_i2c_driver = {
	.driver = {
		.name = AW20144_I2C_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(aw20144_dt_match),
	},
	.probe = aw20144_i2c_probe,
	.remove = aw20144_i2c_remove,
	.id_table = aw20144_i2c_id,
};

static int __init aw20144_i2c_init(void)
{
	int ret = -1;

	LOG_INFO("driver version %s\n", AW20144_DRIVER_VERSION);

	ret = i2c_add_driver(&aw20144_i2c_driver);
	if (ret) {
		LOG_ERR("add aw20144 driver failed\n");
		return ret;
	}

	return 0;
}

module_init(aw20144_i2c_init);

static void __exit aw20144_i2c_exit(void)
{
	i2c_del_driver(&aw20144_i2c_driver);
}

module_exit(aw20144_i2c_exit);

MODULE_DESCRIPTION("AW20144 LED Driver");
MODULE_LICENSE("GPL v2");
