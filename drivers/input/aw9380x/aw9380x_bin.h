/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AW9380X_BIN_H__
#define __AW9380X_BIN_H__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/regmap.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/string.h>
#define AWINIC_CODE_VERSION "V0.0.7-V1.0.4"

#define DEBUG_LOG_LEVEL
#ifdef DEBUG_LOG_LEVEL
#define DBG(fmt, arg...) pr_info("AWINIC_BIN %s,line= %d,"fmt, __func__, __LINE__, ##arg)

#define DBG_ERR(fmt, arg...)  pr_err("AWINIC_BIN_ERR %s,line= %d,"fmt, __func__, __LINE__, ##arg)

#else
#define DBG(fmt, arg...) do {} while (0)
#define DBG_ERR(fmt, arg...) do {} while (0)
#endif

#define printing_data_code

#define BigLittleSwap16(A)	((((unsigned short int)(A) & 0xff00) >> 8) | \
				(((unsigned short int)(A) & 0x00ff) << 8))

#define BigLittleSwap32(A)	((((unsigned long)(A) & 0xff000000) >> 24) | \
				(((unsigned long)(A) & 0x00ff0000) >> 8) | \
				(((unsigned long)(A) & 0x0000ff00) << 8) | \
				(((unsigned long)(A) & 0x000000ff) << 24))

#define AW_CAP_BIN_NUM_MAX   100
#define AW_CAP_HEADER_LEN    60

#define AW_CAP_ABS(x)							(((x) > 0) ? (x) : (-(x)))
#define AW_CAP_GET_32_DATA(w, x, y, z)			((unsigned int)(((w) << 24) | ((x) << 16) | ((y) << 8) | (z)))
#define AW_CAP_GET_16_DATA(a, b)			(((uint32_t)(a) << (8)) | ((uint32_t)(b) << (0)))

enum bin_header_version_enum {
	HEADER_VERSION_1_0_0 = 0x01000000,
};

enum data_type_enum {
	DATA_TYPE_REGISTER = 0x00000000,
	DATA_TYPE_DSP_REG = 0x00000010,
	DATA_TYPE_DSP_CFG = 0x00000011,
	DATA_TYPE_SOC_REG = 0x00000020,
	DATA_TYPE_SOC_APP = 0x00000021,
	DATA_TYPE_MULTI_BINS = 0x00002000,
};

struct bin_header_info {
	unsigned int header_len; /* Frame header length */
	unsigned int check_sum; /* Frame header information-Checksum */
	unsigned int header_ver; /* Frame header information-Frame header version */
	unsigned int bin_data_type; /* Frame header information-Data type */
	unsigned int bin_data_ver; /* Frame header information-Data version */
	unsigned int bin_data_len; /* Frame header information-Data length */
	unsigned int ui_ver; /* Frame header information-ui version */
	unsigned char chip_type[8]; /* Frame header information-chip type */
	unsigned int reg_byte_len; /* Frame header information-reg byte len */
	unsigned int data_byte_len; /* Frame header information-data byte len */
	unsigned int device_addr; /* Frame header information-device addr */
	unsigned int valid_data_len; /* Length of valid data obtained after parsing */
	unsigned int valid_data_addr; /* The offset address of the valid data obtained after parsing relative to info */

	unsigned int reg_num; /* The number of registers obtained after parsing */
	unsigned int reg_data_byte_len; /* The byte length of the register obtained after parsing */
	unsigned int download_addr; /* The starting address or download address obtained after parsing */
	unsigned int app_version; /* The software version number obtained after parsing */
};

struct bin_container {
	unsigned int len; /* The size of the bin file obtained from the firmware */
	unsigned char data[]; /* Store the bin file obtained from the firmware */
};

struct aw_bin {
	char *p_addr; /* Offset pointer (backward offset pointer to obtain frame header information and important information) */
	unsigned int all_bin_parse_num; /* The number of all bin files */
	unsigned int multi_bin_parse_num; /* The number of single bin files */
	unsigned int single_bin_parse_num; /* The number of multiple bin files */
	struct bin_header_info header_info[AW_CAP_BIN_NUM_MAX]; /* Frame header information and other important data obtained after parsing */
	struct bin_container info; /* Obtained bin file data that needs to be parsed */
};

int aw_parsing_bin_file(struct aw_bin *bin);
#endif
