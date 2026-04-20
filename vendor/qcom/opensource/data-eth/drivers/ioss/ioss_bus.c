/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include "ioss_i.h"
#include "ioss_qos.h"
#include <linux/cdev.h>

/* Wake lock duration to allow the device to settle after a resume */
#define IOSS_RESUME_SETTLE_MS 5000

#define MAX_IOSS_DEVICES 10
#define MAX_IOSS_DRIVERS 10

struct ioss_device *ioss_devices[MAX_IOSS_DEVICES];
struct ioss_driver *ioss_drivers[MAX_IOSS_DRIVERS];


static struct class *emac_ipa_class;
static dev_t emac_ipa_dev_num;
static struct cdev *emac_ipa_cdev;
static struct device *emac_ipa_dev;


static int ioss_panic_notifier(struct notifier_block *nb,
		unsigned long event, void *ptr)
{
	int rc;
	struct ioss_device *idev =
			container_of(nb, struct ioss_device, panic_nb);

	ioss_dev_log(idev, "Panic notifier called for device");

	rc = ioss_dev_op(idev, save_regs, idev, NULL, NULL);
	if (rc)
		ioss_dev_err(idev, "save_regs() failed with error %d", rc);
	else
		ioss_dev_cfg(idev, "Panic notifier completed for device");

	return NOTIFY_DONE;
}

static int ioss_register_panic_notifier(struct ioss_device *idev)
{
	idev->panic_nb.notifier_call = ioss_panic_notifier;

	return atomic_notifier_chain_register(
			&panic_notifier_list, &idev->panic_nb);
}

static void ioss_unregister_panic_notifier(struct ioss_device *idev)
{
	atomic_notifier_chain_unregister(&panic_notifier_list, &idev->panic_nb);
	idev->panic_nb.notifier_call = NULL;
}

static int ioss_bus_match(struct device *dev, struct device_driver *drv)
{
	struct device *real_dev = dev->parent;
	struct ioss_driver *idrv = to_ioss_driver(drv);
	struct ioss_device *idev = to_ioss_device(dev);
	int rc;

	if (dev->type != &ioss_idev_type)
		return false;

	ioss_dev_dbg(idev, "Matching against %s", idrv->name);

	/* If a match function is provided by the IOSS driver, use that for
	 * matching the device driver with the IOSS driver. Otherwise use a
	 * simple name matching against the IOSS driver name.
	 */
	rc = idrv->match ?
			idrv->match(real_dev) :
			!strcmp(idrv->name, real_dev->driver->name);

	ioss_dev_dbg(idev, "Matching against %s, rc = %d, real_dev->driver->name =%s", idrv->name, rc, real_dev->driver->name);

	return rc;
}

static ssize_t show_suspend_ipa_offload(struct device *dev,
		struct device_attribute *attr, char *user_buf)
{
	struct net_device *net_dev = NULL;
	struct ioss_interface *iface = NULL;
	struct ioss_device *idev = NULL;

	if (!dev)
		return -EINVAL;

	net_dev = to_net_dev(dev);
	if (!net_dev)
		return -EINVAL;

	iface = ioss_netdev_to_iface(net_dev);
	if (!iface)
		return -EINVAL;

	idev = ioss_iface_dev(iface);
	if(!idev)
		return -EINVAL;

	return snprintf(user_buf, PAGE_SIZE, "%d\n", idev->dev.offline);
}

static ssize_t store_suspend_ipa_offload(struct device *dev,
		struct device_attribute *attr, const char *user_buf, size_t size)
{
	int ret = 0;
	struct net_device *net_dev = NULL;
	struct ioss_interface *iface = NULL;
	struct ioss_device *idev = NULL;
	bool input;

	if (!dev)
		return -EINVAL;

	net_dev = to_net_dev(dev);
	if (!net_dev)
		return -EINVAL;

	iface = ioss_netdev_to_iface(net_dev);
	if (!iface)
		return -EINVAL;

	idev = ioss_iface_dev(iface);
	if (!idev)
		return -EINVAL;

	if (kstrtobool(user_buf, &input) < 0)
		return -EINVAL;

	idev->dev.offline = input;

	if (!input && idev->qos_enabled) {
		ret = ioss_qos_reconfigure(idev);
		if (ret)
			ioss_dev_err(idev, "ioss_qos_reconfigure failed on resume");
	}

	ioss_iface_queue_refresh(iface, true);

	if (!input && idev->qos_enabled) {
		ret = ioss_qos_enable(idev);
		if (ret)
			ioss_dev_err(idev, "enable_qos failed on resume");
	}

	ioss_dev_log(idev, "Device Offline set to %d", idev->dev.offline);

	return size;
}

/* By default assign port #0 to have LLCC enabled. Only one port can get LLCC. */
static int enable_tcm_eth = 1;

module_param(enable_tcm_eth, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(enable_tcm_eth, "Enable use of LLCC TCM memory for Ethernet port: Value 1 = eth0, 2 = eth1, 0 = disabled");

static DEVICE_ATTR(suspend_ipa_offload, S_IWUSR | S_IRUGO,
		show_suspend_ipa_offload, store_suspend_ipa_offload);

static int ioss_bus_probe(struct device *dev)
{
	int rc;
	struct ioss_device *idev = to_ioss_device(dev);
	struct ioss_interface *iface = &idev->interface;

	ioss_dev_log(idev, "Initializing device for offload");

	if ((enable_tcm_eth == 1 && strnstr(dev_name(dev), "emac0", strlen(dev_name(dev)))) ||
	    (enable_tcm_eth == 2 && strnstr(dev_name(dev), "emac1", strlen(dev_name(dev)))))
		idev->llcc_enabled = true;

	device_init_wakeup(dev, true);

	rc = ioss_net_link_device(idev);
	if (rc) {
		ioss_dev_err(idev, "Failed to link to net device");
		return rc;
	}

	ioss_pci_hijack_pm_ops(idev);

	rc = ioss_register_panic_notifier(idev);
	if (rc) {
		ioss_dev_err(idev, "Failed to register panic notifier");
		goto err_notifier;
	}

	rc = ioss_dev_op(idev, open_device, idev);
	if (rc) {
		ioss_dev_err(idev, "Failed to open device");
		goto err_open;
	}

	rc = ioss_net_watch_device(idev);
	if (rc) {
		ioss_dev_err(idev, "Failed to watch net device");
		goto err_watch;
	}

	rc = ioss_sysfs_add_idev(idev);
	if (rc) {
		ioss_dev_err(idev, "Unable to add idev to sysfs");
		goto err_add_sysfs;
	}

	rc = sysfs_create_file(&idev->net_dev->dev.kobj,
				&dev_attr_suspend_ipa_offload.attr);
	if (rc) {
		ioss_dev_err(idev, "unable to create suspend_ipa_offload node");
		goto err_create_sysfs;
	}

	rc = ioss_qos_add_idev(idev);
	if (rc) {
		ioss_dev_err(idev, "QoS add failed");
		goto err_qos_add;
	}

	if (iface->auto_resume_disabled) {
		ioss_dev_cfg(idev, "creating dev char device for auto platform\n");

		rc = alloc_chrdev_region(&emac_ipa_dev_num, 0, 1, "emac_ipa");
		if (rc) {
			ioss_dev_err(idev, "alloc_chrdev_region error for node %s\n",
				  "emac_ipa");
			goto err_chrdev;
		}

		emac_ipa_cdev = cdev_alloc();
		if (!emac_ipa_cdev) {
			rc = -ENOMEM;
			ioss_dev_err(idev, "failed to alloc emac_ipa cdev\n");
			goto fail_alloc_emac_ipa_cdev;
		}

		cdev_init(emac_ipa_cdev,NULL);

		rc = cdev_add(emac_ipa_cdev,emac_ipa_dev_num,1);
		if (rc < 0) {
			ioss_dev_err(idev, "emac_ipa cdev_add err=%d\n", -rc);
			goto emac_ipa_cdev_add_fail;
		}

		emac_ipa_class = class_create("emac_ipa");
		if (!emac_ipa_class) {
			rc= -ENODEV;
			ioss_dev_err(idev, "failed to create emac_ipa class\n");
			goto fail_create_emac_ipa_class;
		}

		emac_ipa_dev = device_create(emac_ipa_class, NULL,
				emac_ipa_dev_num, NULL, "emac_ipa");
		if (!emac_ipa_dev) {
			rc = -EINVAL;
			ioss_dev_err(idev, "failed to create emac_ipa device\n");
			goto fail_create_emac_ipa_device;
		}
	}

	return 0;

fail_create_emac_ipa_device:
	class_destroy(emac_ipa_class);
fail_create_emac_ipa_class:
	cdev_del(emac_ipa_cdev);
emac_ipa_cdev_add_fail:
fail_alloc_emac_ipa_cdev:
	unregister_chrdev_region(emac_ipa_dev_num, 1);
err_chrdev:
	ioss_qos_remove_idev(idev);
err_qos_add:
	sysfs_remove_file(&idev->net_dev->dev.kobj,
			&dev_attr_suspend_ipa_offload.attr);
err_create_sysfs:
	ioss_sysfs_remove_idev(idev);;
err_add_sysfs:
	ioss_net_unwatch_device(idev);
err_watch:
	ioss_dev_op(idev, close_device, idev);
err_open:
	ioss_unregister_panic_notifier(idev);
err_notifier:
	ioss_pci_restore_pm_ops(idev);

	return rc;
}

static void ioss_bus_remove(struct device *dev)
{
	int rc;
	struct ioss_device *idev = to_ioss_device(dev);
	struct ioss_interface *iface = &idev->interface;

	ioss_dev_log(idev, "De-initializing device");

	ioss_qos_remove_idev(idev);

	sysfs_remove_file(&idev->net_dev->dev.kobj,
			&dev_attr_suspend_ipa_offload.attr);
	ioss_sysfs_remove_idev(idev);

	if(emac_ipa_cdev && iface->auto_resume_disabled)
	{
		device_destroy(emac_ipa_class, emac_ipa_dev_num);
		class_destroy(emac_ipa_class);
		cdev_del(emac_ipa_cdev);
		unregister_chrdev_region(emac_ipa_dev_num, 1);
	}

	rc = ioss_net_unwatch_device(idev);
	if (rc) {
		ioss_dev_err(idev, "Failed to unwatch device");
	}

	rc = ioss_dev_op(idev, close_device, idev);
	if (rc) {
		ioss_dev_err(idev, "Failed to close device");
	}

	ioss_unregister_panic_notifier(idev);
	ioss_pci_restore_pm_ops(idev);
	device_init_wakeup(dev, false);
}

static int __ioss_bus_suspend_idev(struct device *dev, pm_message_t state)
{
	struct ioss_device *idev = to_ioss_device(dev);

	ioss_dev_log(idev, "Suspending device");

	return 0;
}

static int __ioss_bus_resume_idev(struct device *dev)
{
	bool need_refresh = false;
	bool was_suspended = true;
	struct ioss_device *idev = to_ioss_device(dev);
	struct ioss_interface *iface = &idev->interface;

	ioss_dev_log(idev, "Resuming device");

	if (iface->auto_resume_disabled) {
		ioss_dev_log(idev, "Auto resume of device disabled let eth PM call IOSS enable");
		return 0;
	}

	if (dev->offline) {
		dev->offline = 0;
		need_refresh = true;
	}

	if (iface->state == IOSS_IF_ST_ONLINE)
		was_suspended = false;

	if (need_refresh)
		ioss_iface_queue_refresh(iface, false);

	if (was_suspended)
		pm_wakeup_dev_event(dev, IOSS_RESUME_SETTLE_MS, false);

	return 0;
}

static int __ioss_bus_online_idev(struct device *dev)
{
	struct ioss_device *idev = to_ioss_device(dev);
	struct ioss_interface *iface = &idev->interface;

	dev->offline = 0;

	ioss_iface_queue_refresh(iface, true);

	ioss_dev_log(idev, "Online device");

	return 0;
}

static int __ioss_bus_offline_idev(struct device *dev)
{
	struct ioss_device *idev = to_ioss_device(dev);
	struct ioss_interface *iface = &idev->interface;

	dev->offline = 1;

	ioss_iface_queue_refresh(iface, true);

	ioss_dev_log(idev, "Offline device");

	return 0;
}

static int ioss_bus_suspend(struct device *dev, pm_message_t state)
{
	if (dev->type == &ioss_idev_type)
		return __ioss_bus_suspend_idev(dev, state);

	return 0;
}

static int ioss_bus_resume(struct device *dev)
{
	if (dev->type == &ioss_idev_type)
		return __ioss_bus_resume_idev(dev);

	return 0;
}

static int ioss_bus_online(struct device *dev)
{
	if (dev->type == &ioss_idev_type)
		return __ioss_bus_online_idev(dev);

	return 0;
}

static int ioss_bus_offline(struct device *dev)
{
	if (dev->type == &ioss_idev_type)
		return __ioss_bus_offline_idev(dev);

	return 0;
}

struct device_type ioss_idev_type = {
	.name = "ioss_device",
};

struct device_type ioss_iface_type = {
	.name = "ioss_interface",
};

struct bus_type ioss_bus = {
	.name = "ioss",
	.match = ioss_bus_match,
	.probe = ioss_bus_probe,
	.remove = ioss_bus_remove,
	.suspend = ioss_bus_suspend,
	.resume = ioss_bus_resume,
	.online = ioss_bus_online,
	.offline = ioss_bus_offline,
};

int ioss_bus_register_driver(struct ioss_driver *idrv)
{
	int i, rc;
	struct ioss_driver **idrv_list_entry = NULL;

	for (i = 0; i < ARRAY_SIZE(ioss_drivers); i++) {
		if (!ioss_drivers[i]) {
			idrv_list_entry = &ioss_drivers[i];
			break;
		}
	}

	if (!idrv_list_entry) {
		ioss_log_cfg(NULL,
			"No space to add %s to ioss_drivers[]", idrv->name);
		return -ENOSPC;
	}

	/* Add to driver list before registering the driver with kernel so that
	 * this is accessible from crashdumps.
	 */
	*idrv_list_entry = idrv;

	idrv->drv.name = idrv->name;
	idrv->drv.bus = &ioss_bus;

	mutex_init(&idrv->pm_lock);
	refcount_set(&idrv->pm_refcnt, 0);

	/* Init drv */
	/* Register driver */

	rc = driver_register(&idrv->drv);
	if (rc) {
		ioss_log_err(NULL, "Failed to register driver %s", idrv->name);
		*idrv_list_entry = NULL;
		return rc;
	}

	return 0;
}

void ioss_bus_unregister_driver(struct ioss_driver *idrv)
{
	int i;

	driver_unregister(&idrv->drv);

	mutex_destroy(&idrv->pm_lock);

	for (i = 0; i < ARRAY_SIZE(ioss_drivers); i++) {
		if (ioss_drivers[i] == idrv) {
			ioss_drivers[i] = NULL;
			break;
		}
	}
}

struct ioss_device *ioss_bus_alloc_idev(struct ioss *ioss, struct device *dev)
{
	int i;
	struct ioss_device *idev;
	struct ioss_device **idev_list_entry = NULL;

	for (i = 0; i < ARRAY_SIZE(ioss_devices); i++) {
		if (!ioss_devices[i]) {
			idev_list_entry = &ioss_devices[i];
			break;
		}
	}

	if (!idev_list_entry) {
		ioss_log_err(NULL,
			"No space to add %s to ioss_devices[]", dev_name(dev));
		return NULL;
	}

	if (!dev->driver || !dev->driver->pm) {
		return NULL;
	}

	idev = kzalloc(sizeof(*idev), GFP_KERNEL);
	if (!idev) {
		ioss_log_err(NULL, "Failed to alloc ioss device");
		return NULL;
	}

	*idev_list_entry = idev;

	idev->root = ioss;

	INIT_LIST_HEAD(&idev->interface.valid_channels);
	INIT_LIST_HEAD(&idev->interface.invalid_channels);

	idev->dev.parent = dev;
	idev->dev.bus = &ioss_bus;
	idev->dev.type = &ioss_idev_type;

	ioss_qos_init(idev);

	return idev;
}

void ioss_bus_free_idev(struct ioss_device *idev)
{
	int i;
	struct ioss_channel *ch, *tmp_ch;
	struct ioss_interface *iface = &idev->interface;

	/* Free channels */
	list_for_each_entry_safe(ch, tmp_ch, &iface->valid_channels, node) {
		list_del(&ch->node);
		kfree(ch->ipa_configs);
		kfree_sensitive(ch->ioss_priv);
		kfree_sensitive(ch);
	}

	list_for_each_entry_safe(ch, tmp_ch, &iface->invalid_channels, node) {
		list_del(&ch->node);
		kfree(ch->ipa_configs);
		kfree_sensitive(ch->ioss_priv);
		kfree_sensitive(ch);
	}

	kfree_sensitive(iface->ioss_priv);

	for (i = 0; i < ARRAY_SIZE(ioss_devices); i++) {
		if (ioss_devices[i] == idev) {
			ioss_devices[i] = NULL;
			break;
		}
	}

	kfree_sensitive(idev);
}

int ioss_bus_register_idev(struct ioss_device *idev)
{
	if (ioss_of_parse(idev)) {
		return -EINVAL;
	}

	return device_register(&idev->dev);
}

void ioss_bus_unregister_idev(struct ioss_device *idev)
{
	device_unregister(&idev->dev);
}

int ioss_bus_register_iface(struct ioss_interface *iface,
		struct net_device *net_dev)
{
	struct ioss_device *idev = ioss_iface_dev(iface);

	dev_hold(net_dev);

	iface->dev.parent = &net_dev->dev;
	iface->dev.bus = &ioss_bus;
	iface->dev.type = &ioss_iface_type;

	dev_set_name(&iface->dev, "%s-%s",
			dev_name(&idev->dev), net_dev->name);

	return device_register(&iface->dev);
}

void ioss_bus_unregister_iface(struct ioss_interface *iface)
{
	struct net_device *net_dev = ioss_iface_to_netdev(iface);

	device_unregister(&iface->dev);
	iface->dev.parent = NULL;
	dev_put(net_dev);
}
