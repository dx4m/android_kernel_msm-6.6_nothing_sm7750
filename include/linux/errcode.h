/*
 * Copyright (C) 2024 Chino-e Inc.
 *
 */
#ifndef _ERRORCODE_H_
#define _ERRORCODE_H_
typedef enum {
	ESD_ERR,
	CMDQ_ERR,
	MIPI_ERR,
	LCM_I2C_ERR,
	FPS_SWITCH_ERR,
	ERR_5,
	ERR_6,
	ERR_7,
	ERR_8,
	ERR_9,
	ERR_10,
	ERR_11,
	LCM_ERR_MAX,
} LCM_ERROR_TYPE;
extern unsigned long errcode_lcd;		//0x701000
extern void set_errorcode_value(unsigned long *code_type, LCM_ERROR_TYPE err);
#define CAM_MODULE1 0x901000
#define CAM_MODULE2 0x902000
#define CAM_MODULE4 0x904000
#define CAM_MODULE5 0x905000
typedef enum {
	ERR_SENSOR_PROBE,
	ERR_SENSOR_POWER,
	ERR_SENSOR_I2C,
	ERR_SENSOR_INIT,
	ERR_CAM_04,
	ERR_CAM_05,
	ERR_CAM_06,
	ERR_CAM_07,
	ERR_CAM_08,
	ERR_CAM_FLASH,
	ERR_CAM_OTP,
	ERR_CAM_AF,
	ERR_CAM_MAX,
} CAM_ERROR_TYPE;
#endif
