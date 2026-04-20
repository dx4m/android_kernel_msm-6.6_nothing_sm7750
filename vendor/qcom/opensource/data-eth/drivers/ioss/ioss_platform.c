/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_mdio.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/phy.h>

#include "ioss_i.h"

static int ioss_plat_add_device(struct platform_device *pdev, struct ioss *ioss)
{
	int rc;
	struct ioss_device *idev;


	idev = ioss_bus_alloc_idev(ioss, &pdev->dev);
	if (!idev)
		return -ENOMEM;

	dev_set_name(&idev->dev, "%s-ioss", dev_name(idev->dev.parent));


	rc = ioss_bus_register_idev(idev);
	if (rc) {
		ioss_bus_free_idev(idev);
		return rc;
	}

	ioss_log_msg(NULL, "%s: dev  %s", __func__, dev_name(&pdev->dev));

	return 0;
}

static int ioss_plat_remove_device(struct platform_device *pdev, struct ioss *ioss)
{
	struct ioss_device *idev = ioss_real_to_idev(&pdev->dev);

	if (!idev)
		return -ENODEV;

	ioss_dev_log(idev, "Removing platform device %s", dev_name(idev->dev.parent));

	ioss_bus_unregister_idev(idev);
	ioss_bus_free_idev(idev);

	return 0;
}

static int plat_bus_notification(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct platform_device *pdev = to_platform_device(data);
	struct ioss *ioss = container_of(nb, typeof(*ioss), plat_bus_nb);
	struct device *dev = &pdev->dev;

	ioss_log_dbg(NULL, "Platform bus notif: %lu device: %s, driver: %s",
			action, dev_name(dev), dev_driver_string(dev));

	switch (action) {
	case BUS_NOTIFY_BOUND_DRIVER:
		ioss_plat_add_device(pdev, ioss);
		break;
	case BUS_NOTIFY_UNBIND_DRIVER:
		ioss_plat_remove_device(pdev, ioss);
		break;
	}

	return NOTIFY_OK;
}

static int __plat_walk_add_device(struct device *dev, void *data)
{
	struct ioss *ioss = data;

	/* Call BUS_NOTIFY_BOUND_DRIVER if the device is already bound to a
	 * driver.
	 */
	if (dev->driver)
		plat_bus_notification(&ioss->plat_bus_nb,
				BUS_NOTIFY_BOUND_DRIVER, dev);

	return 0;
}

int ioss_plat_start(struct ioss *ioss)
{
	int rc;

	ioss->plat_bus_nb.notifier_call = plat_bus_notification;
	rc = bus_register_notifier(&platform_bus_type, &ioss->plat_bus_nb);
	if (rc) {
		ioss_log_err(NULL, "Failed to register Platform bus notifier");
		return rc;
	}

	/* Manually scan all existing PCI devices */
	bus_for_each_dev(&platform_bus_type, NULL, ioss, __plat_walk_add_device);

	return 0;
}

static int __plat_walk_del_device(struct device *dev, void *data)
{
	struct ioss *ioss = data;

	ioss_log_dbg(NULL, "Platform walk del device: %s, driver: %s",
			dev_name(dev), dev_driver_string(dev));

	if (dev->driver)
		plat_bus_notification(&ioss->plat_bus_nb,
				BUS_NOTIFY_UNBIND_DRIVER, dev);

	return 0;
}

void ioss_plat_stop(struct ioss *ioss)
{

	bus_unregister_notifier(&platform_bus_type, &ioss->plat_bus_nb);
	/* Unregister all devices. Drivers will be unregistered when
	 * the glue drivers are unloaded.
	 */
	bus_for_each_dev(&platform_bus_type, NULL, ioss, __plat_walk_del_device);
}

static int ioss_plat_nop_suspend(struct ioss_device *idev)
{

	idev->pm_stats.apps_suspend++;

	ioss_dev_dbg(idev, "Device suspend performing nop");
	return 0;
}

static int ioss_plat_nop_resume(struct ioss_device *idev)
{
	idev->pm_stats.apps_resume++;

	ioss_dev_dbg(idev, "Device resume performing nop");
	return 0;
}

static int ioss_plat_suspend_handler(struct device *dev);
static int ioss_plat_resume_handler(struct device *dev);

static bool __ioss_plat_check_pm_ops(const struct dev_pm_ops *pm_ops)
{
	return pm_ops->suspend == ioss_plat_suspend_handler &&
		pm_ops->resume == ioss_plat_resume_handler;
}

/* There could be instances where a physical device may not have an idev or the
 * idev could not bind to the idrv. Use below API to retrieve real pm ops more
 * reliably. Call this function only through ioss_plat suspend/resume handler.
 */
static const struct dev_pm_ops *__real_pm_ops(struct device *dev)
{
	const struct dev_pm_ops *pm_ops = NULL;

	/* Provided no one has hijacked pm ops any further, below logic should
	 * be able to retrieve real pm ops.
	 */
	if (__ioss_plat_check_pm_ops(dev->driver->pm))
		pm_ops = container_of(dev->driver->pm,
				struct ioss_driver, pm_ops)->pm_ops_real;
	else
		ioss_log_err(dev, "Driver PM ops not pointing to IOSS PM ops");

	if (!pm_ops) {
		struct ioss_device *idev = ioss_real_to_idev(dev);

		/* If there exists a idev and it is bound to an idrv, use real pm ops
		* from it.
		*/
		if (idev) {
			struct ioss_driver *idrv = ioss_dev_to_drv(idev);

			if (idrv)
				pm_ops = idrv->pm_ops_real;
			else
				ioss_dev_err(idev, "idev not bound to driver");
		} else {
			ioss_log_err(dev, "idev not present for device");
		}
	}

	return pm_ops;
}

static int ioss_plat_real_suspend(struct device *dev)
{
	struct ioss_device *idev = ioss_real_to_idev(dev);
	const struct dev_pm_ops *real_pm_ops = __real_pm_ops(dev);

	if (idev)
		idev->pm_stats.system_suspend++;

	ioss_log_msg(dev, "Delegating device suspend to real driver");

	if (real_pm_ops && real_pm_ops->suspend)
		return real_pm_ops->suspend(dev);

	ioss_log_err(dev, "Unable to delegate suspend to real driver");

	return -EINVAL;
}

static int ioss_plat_real_resume(struct device *dev)
{
	struct ioss_device *idev = ioss_real_to_idev(dev);
	const struct dev_pm_ops *real_pm_ops = __real_pm_ops(dev);

	if (idev)
		idev->pm_stats.system_resume++;

	ioss_log_msg(dev, "Delegating device resume to real driver");

	if (real_pm_ops && real_pm_ops->resume)
		return real_pm_ops->resume(dev);

	ioss_log_err(dev, "Unable to delegate resume to real driver");

	return -EINVAL;
}


static int ioss_plat_suspend_handler(struct device *dev)
{
	struct ioss_device *idev = ioss_real_to_idev(dev);
	ioss_log_msg(dev, "%s : Start", __func__);

	return (idev && idev->interface.state == IOSS_IF_ST_ONLINE)?
			ioss_plat_nop_suspend(idev):
			ioss_plat_real_suspend(dev);
}

static int ioss_plat_resume_handler(struct device *dev)
{
	struct ioss_device *idev = ioss_real_to_idev(dev);
	ioss_log_msg(dev, "%s : Start", __func__);

	return (idev && idev->interface.state == IOSS_IF_ST_ONLINE)?
			ioss_plat_nop_resume(idev):
			ioss_plat_real_resume(dev);
}

static const struct dev_pm_ops __ioss_plat_pm_ops = {
	.suspend = ioss_plat_suspend_handler,
	.resume = ioss_plat_resume_handler,
};

int ioss_plat_register_driver(struct ioss_driver *idrv, struct module *owner)
{
	ioss_log_cfg(NULL, "Registering Platform driver %s", idrv->name);

	idrv->drv.owner = owner;
	idrv->pm_ops = __ioss_plat_pm_ops;

	/* Init Platform driver attributes here */

	return ioss_bus_register_driver(idrv);
}
EXPORT_SYMBOL(ioss_plat_register_driver);

void ioss_plat_unregister_driver(struct ioss_driver *idrv)
{
	ioss_log_cfg(NULL, "Unregistering Platform driver %s", idrv->name);

	ioss_bus_unregister_driver(idrv);
}
EXPORT_SYMBOL(ioss_plat_unregister_driver);

