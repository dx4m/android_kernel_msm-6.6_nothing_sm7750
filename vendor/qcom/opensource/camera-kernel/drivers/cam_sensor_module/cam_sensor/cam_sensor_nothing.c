// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "cam_sensor_nothing.h"
#include "cam_req_mgr_dev.h"
#include "cam_sensor_soc.h"
#include "cam_sensor_core.h"
#include "camera_main.h"
#include "cam_compat.h"
#include <linux/slab.h>

#if IS_ENABLED(CONFIG_OEM_BOOTINFO)
#define CONFIG_CAMERA_BOOT_INFO
#endif
#if IS_ENABLED(CONFIG_OEM_ERRCODE)
#define CONFIG_CAMERA_ERR_INFO
#endif

#ifdef CONFIG_CAMERA_BOOT_INFO
extern char main_camera[32];
extern char sub_front_camera[32];
extern char depth_camera[32];
extern char tele_camera[32];
extern char main_camera_sn[64];
extern char depth_camera_sn[64];
extern char front_camera_sn[64];
extern char tele_camera_sn[64];
extern char main_module_id[64];
#endif

#ifdef CONFIG_CAMERA_ERR_INFO
extern unsigned long  errcode_cam_back;
extern unsigned long  errcode_cam_front;
extern unsigned long  errcode_cam_uw;
extern unsigned long  errcode_cam_tele;
#endif

static struct cam_sensor_ctrl_t *g_sctrl[MAX_SENSOR_NUM];

static struct kobject ext_cam_i2c_kobject;
static struct cam_nt_i2c_info ext_i2c_info;


static ssize_t ext_cam_i2c_object_show(struct kobject *k, struct attribute *attr, char *buf)
{
	struct kobj_attribute *kobj_attr;
	int ret = -EIO;

	kobj_attr = container_of(attr, struct kobj_attribute, attr);

	if (kobj_attr->show)
		ret = kobj_attr->show(k, kobj_attr, buf);

	return ret;
}

static ssize_t ext_cam_i2c_object_store(struct kobject *k, struct attribute *attr, const char *buf, size_t size)
{
	struct kobj_attribute *kobj_attr;
	int ret = -EIO;

	kobj_attr = container_of(attr, struct kobj_attribute, attr);

	if (kobj_attr->store)
		ret = kobj_attr->store(k, kobj_attr, buf, sizeof(buf));

	return size;
}

static const struct sysfs_ops ext_cam_i2c_object_sysfs_ops = {
	.show = ext_cam_i2c_object_show,
	.store = ext_cam_i2c_object_store,
};

static struct kobj_type ext_cam_i2c_object_type = {
	.sysfs_ops = &ext_cam_i2c_object_sysfs_ops,
	.release   = NULL,
};

static int ext_cam_i2c_init(void)
{
	pr_info("ext_cam_i2c_init Enter\n");
	memset(&ext_cam_i2c_kobject, 0x00, sizeof(ext_cam_i2c_kobject));

	if (kobject_init_and_add(&ext_cam_i2c_kobject, &ext_cam_i2c_object_type, NULL, "ext_cam_i2c")) {
		kobject_put(&ext_cam_i2c_kobject);
		return -ENOMEM;
	}

	kobject_uevent(&ext_cam_i2c_kobject, KOBJ_ADD);

	return 0;
}

static ssize_t  ext_cam_i2c_show(struct kobject *kobj,
						struct kobj_attribute *attr, char *buf)
{
	//cat读取时触发操作
	pr_info("ext_cam_i2c_show Enter\n");
	if (ext_i2c_info.status >= 2)
	{
		if (ext_i2c_info.is_write == 1)
		{
			if (ext_i2c_info.status == 2) {
				strncpy(buf, "WRITE_SUCCESS\n", 15);
			} else {
				strncpy(buf, "WRITE_FAIL\n", 12);
			}
		} else {
			if (ext_i2c_info.status == 2) {
				sprintf(buf, "READ_SUCCESS_0x%x\n", ext_i2c_info.reg_data);
			} else {
				strncpy(buf, "READ_FAIL\n", 11);
			}
		}
		pr_info("ext_cam_i2c_show buff=[%s] size = %lu\n",buf, strlen(buf));
	}
	return strlen(buf);
}

static ssize_t  ext_cam_i2c_store(struct kobject *kobj,
						struct kobj_attribute *attr, const char *buf, size_t size)
{
	//echo 写入是触发操作
	int index = 0;
	int i = 0;
	int count = 0;
	char tmpStr[128] = {0};
	ext_i2c_info.status = 0;
	pr_info("ext_cam_i2c_store buff=[%s] size = %lu\n",buf, strlen(buf));
	for (i = 0; i < strlen(buf); i++)
	{
		if ((buf[i] == ',') || (i == strlen(buf) - 1))
		{
			memset(tmpStr, 0, sizeof(tmpStr));
			if (i > index) {
				strncpy(tmpStr, &buf[index], i-index);
			}
			index = i+1;

			if (strlen(tmpStr) > 0) {
				count++;
				switch(count) {
					case 1:
						sscanf(tmpStr, "%hx", &ext_i2c_info.slave_addr);
						break;
					case 2:
						if (tmpStr[0] == 'r') {
							ext_i2c_info.is_write = 0;
						} else {
							ext_i2c_info.is_write = 1;
						}
						break;
					case 3:
						sscanf(tmpStr, "%x", &ext_i2c_info.reg_addr);
						break;
					case 4:
						sscanf(tmpStr, "%x", &ext_i2c_info.reg_data);
						break;
					case 5:
						sscanf(tmpStr, "%d", &ext_i2c_info.reg_addr_type);
						if (ext_i2c_info.reg_addr_type > 4) {
							ext_i2c_info.reg_addr_type = 4;
						}
						break;
					case 6:
						sscanf(tmpStr, "%d", &ext_i2c_info.reg_data_type);
						if (ext_i2c_info.reg_data_type > 4) {
							ext_i2c_info.reg_data_type = 4;
						}
						break;
					default:
						break;
				}
			}
			if (count == 6)
			{
				ext_i2c_info.status = 1;
				pr_info("nthing test i2c info slaveaddr 0x%x addr 0x%x data 0x%x addr_type %d data_type %d is_write %d\n",
					ext_i2c_info.slave_addr, ext_i2c_info.reg_addr, ext_i2c_info.reg_data,
					ext_i2c_info.reg_addr_type, ext_i2c_info.reg_data_type, ext_i2c_info.is_write);
				break;
			}
		}
	}
	return 1;
}

static struct kobj_attribute ext_cam_i2c_attribute =
__ATTR(EXT_CAM_I2C_NT, 0644, ext_cam_i2c_show, ext_cam_i2c_store);

int cam_nt_do_i2c_info(struct cam_sensor_ctrl_t *sctrl)
{
	int temp_saddr = 0;
	int rc = 0;
	struct cam_sensor_i2c_reg_setting write_setting;
	struct cam_sensor_i2c_reg_array reg_setting;

	if (ext_i2c_info.status == 1) {
		temp_saddr = sctrl->io_master_info.cci_client->sid;
		sctrl->io_master_info.cci_client->sid = ext_i2c_info.slave_addr >> 1;
		if (ext_i2c_info.is_write == 1)
		{
			reg_setting.reg_addr = ext_i2c_info.reg_addr;
			reg_setting.reg_data = ext_i2c_info.reg_data;
			reg_setting.delay = 0;
			reg_setting.data_mask = 0xff;
			write_setting.reg_setting = &reg_setting;
			write_setting.size = 1;
			write_setting.addr_type = ext_i2c_info.reg_addr_type;
			write_setting.data_type = ext_i2c_info.reg_data_type;
			write_setting.delay = 0;
			write_setting.read_buff = NULL;
			write_setting.read_buff_len = 0;
			rc = camera_io_dev_write(
				&(sctrl->io_master_info),
				&(write_setting));
		} else {
			rc = camera_io_dev_read(
				&(sctrl->io_master_info),
				ext_i2c_info.reg_addr,
				&ext_i2c_info.reg_data,
				ext_i2c_info.reg_addr_type,
				ext_i2c_info.reg_data_type,
				true);
		}
		if (rc) {
			ext_i2c_info.status = 3;
		} else {
			ext_i2c_info.status = 2;
		}
		sctrl->io_master_info.cci_client->sid = temp_saddr;
	}
	return 0;
}

static int nt_cam_dev_open(struct inode *inode, struct file *file)
{
	int i = 0;
	for (i = 0; i < NT_CAMERA_MAX; i++)
	{
		cam_vendor_id[i] = 0;
	}
	return 0;
}

static int nt_cam_dev_release(struct inode *inode, struct file *file)
{
	return 0;
}

static int nt_cam_info_save(struct cam_nt_camera_info *info)
{
#ifdef CONFIG_CAMERA_BOOT_INFO
	switch(info->camera_type)
	{
		case NT_CAMERA_MAIN:
			// main_camera
			snprintf(main_camera, sizeof(main_camera), "%s", info->sensor_name);
			memcpy(main_camera_sn, info->camera_sn, sizeof(main_camera_sn));
			CAM_INFO(CAM_SENSOR, "main sensor[%s] sn=%s", main_camera, main_camera_sn);
			cam_vendor_id[NT_CAMERA_MAIN] = info->vendor_id;
			break;
		case NT_CAMERA_FRONT:
			snprintf(sub_front_camera, sizeof(sub_front_camera), "%s", info->sensor_name);
			memcpy(front_camera_sn, info->camera_sn, sizeof(front_camera_sn));
			CAM_INFO(CAM_SENSOR, "front sensor[%s] sn=%s", sub_front_camera, front_camera_sn);
			cam_vendor_id[NT_CAMERA_FRONT] = info->vendor_id;
			break;
		case NT_CAMERA_TELE:
			snprintf(tele_camera, sizeof(tele_camera), "%s", info->sensor_name);
			memcpy(tele_camera_sn, info->camera_sn, sizeof(tele_camera_sn));
			CAM_INFO(CAM_SENSOR, "tele sensor[%s] sn=%s", tele_camera, tele_camera_sn);
			cam_vendor_id[NT_CAMERA_TELE] = info->vendor_id;
			break;
		case NT_CAMERA_DEPTH:
			snprintf(depth_camera, sizeof(depth_camera), "%s", info->sensor_name);
			memcpy(depth_camera_sn, info->camera_sn, sizeof(depth_camera_sn));
			CAM_INFO(CAM_SENSOR, "depth sensor[%s] sn=%s", depth_camera, depth_camera_sn);
			cam_vendor_id[NT_CAMERA_DEPTH] = info->vendor_id;
			break;
		default:
			CAM_ERR(CAM_SENSOR, "camera type [%d] is error ", info->camera_type);
			break;
	}
#endif
	return 0;
}

static int nt_cam_vendor_id_save(void)
{
	int i = 0;
	char vendor_id[64] = {0};
	int length = 64;
	int count = 0;
	for (i = 0; i < NT_CAMERA_MAX; i++)
	{
		snprintf(vendor_id + count, length, "0x%.2x,", cam_vendor_id[i]);
		length -= 5;
		count += 5;
	}

#ifdef CONFIG_CAMERA_BOOT_INFO
	memset(main_module_id, 0, sizeof(main_module_id));
	if (strlen(vendor_id) > 0)
	{
		memcpy(main_module_id, vendor_id, strlen(vendor_id) - 1);
	}
	CAM_INFO(CAM_SENSOR, "main_module_id[%s] vendor_id=%s", main_module_id, vendor_id);
#endif
	return 0;
}

static long nt_cam_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct cam_nt_camera_info nt_cam_info;
	enum cam_nt_cmd_type cmd_type = (enum cam_nt_cmd_type)cmd;
	switch(cmd_type)
	{
		case NT_CAMERA_INFO:
			if (copy_from_user(&nt_cam_info, (void __user *)arg,
				sizeof(nt_cam_info))) {
				CAM_ERR(CAM_SENSOR, "copy data from user space failed");
			} else {
				nt_cam_info_save(&nt_cam_info);
			}
			break;
		case NT_CAMERA_VENDORID:
			nt_cam_vendor_id_save();
			break;
		default:
			CAM_ERR(CAM_SENSOR, "cmd_type [%d] is error ", cmd_type);
			break;
	}
	return 0;
}

static struct file_operations nt_cam_dev_ops = {
	.owner = THIS_MODULE,
	.open = nt_cam_dev_open,
	.release = nt_cam_dev_release,
	.unlocked_ioctl = nt_cam_dev_ioctl,

};

void cam_nt_driver_errcode(int id, enum cam_nt_err_code err_code)
{
	unsigned long error = 0x900000;
	int index = id;
	int i;

	if (err_code == NT_CAM_FLASH_ERR)
	{
		// flash error force set err_code to rear main camera
		index = 0;
	}
	else if (err_code == NT_CAM_ACTUATOR_ERR)
	{
		index = 0; //if actuator error not found sensor,default 0
		for (i = 0; i < MAX_SENSOR_NUM; i++)
		{
			if (NULL != g_sctrl[i])
			{
				if (id == g_sctrl[i]->sensordata->subdev_id[SUB_MODULE_ACTUATOR])
				{
					index = g_sctrl[i]->soc_info.index;
					break;
				}
			}
		}
	}

	error = error | (index << 8) | err_code;

	if (err_code > NT_CAM_PROBE_OK)
	{
		CAM_ERR(CAM_SENSOR, "sensor[%d] error[%d] err_code [%x]", index, err_code, error);
	}

#ifdef CONFIG_CAMERA_ERR_INFO
	switch (index)
	{
		case 0:
			errcode_cam_back = error;
			break;
		case 1:
			errcode_cam_front = error;
			break;
		case 2:
			errcode_cam_uw = error;
			break;
		case 3:
			errcode_cam_tele = error;
			break;
		default:
			break;
	}
#endif
}

void cam_nt_sctrl_save(struct cam_sensor_ctrl_t *sctrl, int index)
{
	g_sctrl[index] = sctrl;
}

void cam_nt_device_init(void)
{
	dev_t nt_cam_dev;
	struct cdev *nt_cam_c_dev;
	struct class *nt_cam_class;
	struct device *nt_cam_device;
	
	nt_cam_c_dev = cdev_alloc();
	if (IS_ERR_OR_NULL(nt_cam_c_dev))
	{
		CAM_ERR(CAM_SENSOR, "Failed to cdev_alloc.");
		return; 
	}

	cdev_init(nt_cam_c_dev, &nt_cam_dev_ops);

	if (alloc_chrdev_region(&nt_cam_dev, 0, 1, NT_CAM_DEV_NAME) < 0) {
		CAM_ERR(CAM_SENSOR, "Failed to allocate char device region.");
		goto cdev_free;
	}

	if (cdev_add(nt_cam_c_dev, nt_cam_dev, 1) < 0) {
		CAM_ERR(CAM_SENSOR, "Failed to add char device.");
		goto unregister_chrdev;
	}

	nt_cam_class = class_create(NT_CAM_DEV_NAME);
	if (IS_ERR_OR_NULL(nt_cam_class))
	{
		CAM_ERR(CAM_SENSOR, "Failed to class_create.");
		goto unregister_chrdev;
	}

	nt_cam_device = device_create(nt_cam_class, NULL, nt_cam_dev, NULL, NT_CAM_DEV_NAME);
	if (IS_ERR_OR_NULL(nt_cam_device))
	{
		CAM_ERR(CAM_SENSOR, "Failed to device_create.");
		goto class_free;
	}
	CAM_INFO(CAM_SENSOR, "nt camera dev node crate success");

	return;

class_free:
	class_destroy(nt_cam_class);
unregister_chrdev:
	unregister_chrdev_region(nt_cam_dev, 1);
cdev_free:
	cdev_del(nt_cam_c_dev);
}

void cam_nt_driver_init(void)
{
	int tmp_rc = 0;
	int i = 0;

	tmp_rc = ext_cam_i2c_init();
	tmp_rc = sysfs_create_file(&ext_cam_i2c_kobject, &ext_cam_i2c_attribute.attr);

	cam_nt_device_init();
	for (i = 0; i < NT_CAMERA_MAX; i++)
	{
		cam_vendor_id[i] = 0;
	}
	for (i = 0; i < MAX_SENSOR_NUM; i++)
	{
		g_sctrl[i] = NULL;
	}
}
