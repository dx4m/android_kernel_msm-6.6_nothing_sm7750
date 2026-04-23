// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-18 Linaro Limited

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/regmap.h>

#define PON_SOFT_RB_SPARE		0x8f

#define GEN1_REASON_SHIFT		2
#define GEN2_REASON_SHIFT		1

#define NO_REASON_SHIFT			0

#if IS_ENABLED(CONFIG_NOTHING_IS_METROID) || IS_ENABLED(CONFIG_NOTHING_IS_FROGGERPRO)
#define QPNP_PON_BUFFER_SIZE		9
#endif /* CONFIG_NOTHING_IS_METROID */

struct pm8916_pon {
	struct device *dev;
	struct regmap *regmap;
	u32 baseaddr;
	struct reboot_mode_driver reboot_mode;
	long reason_shift;
#if IS_ENABLED(CONFIG_NOTHING_IS_METROID) || IS_ENABLED(CONFIG_NOTHING_IS_FROGGERPRO)
	u32 force_key_warm_reset;
#if IS_ENABLED(CONFIG_PINCTRL_MSM_S2IDLE_DUMP)
    /* Catch dump during S2idle. System wakeup when pwrkey press
     * Use Resin_N instead. Also disable Resin_N hw interrupt */
    u32 force_resin_warm_in_s2idle;
#endif /* CONFIG_PINCTRL_MSM_S2IDLE_DUMP */
#endif /* CONFIG_NOTHING_IS_METROID */
};

#if IS_ENABLED(CONFIG_NOTHING_IS_METROID) || IS_ENABLED(CONFIG_NOTHING_IS_FROGGERPRO)
// Support force key warm_reset feature @{
static ssize_t force_key_warm_reset_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct pm8916_pon *pon = dev_get_drvdata(dev);
	return scnprintf(buf, QPNP_PON_BUFFER_SIZE, "%d\n", pon->force_key_warm_reset);
}

static ssize_t force_key_warm_reset_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t size)
{
	struct pm8916_pon *pon = dev_get_drvdata(dev);
	u32 value;
	int rc;

	if (size > QPNP_PON_BUFFER_SIZE)
		return -EINVAL;

	rc = kstrtou32(buf, 10, &value);
	if (rc)
		return rc;

	if (value == 1) {
		/* Disable RESIN_N_S1 and RESIN_N_DEB interrupt */
		rc = regmap_write(pon->regmap, 0x1316, 0x42);
		/* Set RESIN_AND_KPDPWR_S2 and PS_HOLD interrupt type to edge trigger */
		rc |= regmap_write(pon->regmap, 0x811, 0x14);
		/* Set RESIN_AND_KPDPWR_S2 and PS_HOLD interrupt trigger on falling edge */
		rc |= regmap_write(pon->regmap, 0x813, 0x14);
		/* Enable RESIN_AND_KPDPWR_S2 interrupt */
		rc |= regmap_write(pon->regmap, 0x815, 0x4);
		/* Set S1 timer to 3072ms */
		rc |= regmap_write(pon->regmap, 0x844, 0xc);
		/* Set S2 timer to 2s */
		rc |= regmap_write(pon->regmap, 0x845, 0x7);
		/* Set S2 reset type to WARM_RESET */
		rc |= regmap_write(pon->regmap, 0x846, 0x1);
		/* Enable S2 reset*/
		rc |= regmap_write(pon->regmap, 0x847, 0x80);

		if (rc)
			dev_err(pon->dev, "%s enable registers error\n", __func__);
	} else if (value == 0) {
		/* Disable S2 reset */
		rc = regmap_write(pon->regmap, 0x847, 0x00);
		if (rc)
			dev_err(pon->dev, "%s disable register error\n", __func__);
	}
	pon->force_key_warm_reset = value;

	return size;
}
static DEVICE_ATTR(force_key_warm_reset, 0664, force_key_warm_reset_show, force_key_warm_reset_store);

#if IS_ENABLED(CONFIG_PINCTRL_MSM_S2IDLE_DUMP)
static ssize_t force_resin_warm_s2idle_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct pm8916_pon *pon = dev_get_drvdata(dev);
	return scnprintf(buf, QPNP_PON_BUFFER_SIZE, "%d\n", pon->force_resin_warm_in_s2idle);
}

static ssize_t force_resin_warm_s2idle_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t size)
{
	struct pm8916_pon *pon = dev_get_drvdata(dev);
	u32 value;
	int rc;

	if (size > QPNP_PON_BUFFER_SIZE)
		return -EINVAL;

	rc = kstrtou32(buf, 10, &value);
	if (rc)
		return rc;

	if (value == 1) {
		/* Disable RESIN_N_S1 and RESIN_N_DEB interrupt */
		rc = regmap_write(pon->regmap, 0x1316, 0x42);
		/* Set RESIN_N_S2 and PS_HOLD interrupt type to edge trigger */
		rc |= regmap_write(pon->regmap, 0x811, 0x12);
		/* Set RESIN_N_S2 and PS_HOLD interrupt trigger on falling edge */
		rc |= regmap_write(pon->regmap, 0x813, 0x12);
		/* Enable RESIN_N_S2 interrupt */
		rc |= regmap_write(pon->regmap, 0x815, 0x2);
		/* Set S1 timer to 2048ms */
		rc |= regmap_write(pon->regmap, 0x844, 0xb);
		/* Set S2 timer to 2s */
		rc |= regmap_write(pon->regmap, 0x845, 0x7);
		/* Set S2 reset type to WARM_RESET */
		rc |= regmap_write(pon->regmap, 0x846, 0x1);
		/* Enable S2 reset */
		rc |= regmap_write(pon->regmap, 0x847, 0x80);
		if (rc)
			dev_err(pon->dev, "%s enable registers error\n", __func__);
	} else if (value == 0) {
		/* Disable S2 reset */
		rc = regmap_write(pon->regmap, 0x847, 0x00);
		if (rc)
			dev_err(pon->dev, "%s disable register error\n", __func__);
	}
	pon->force_resin_warm_in_s2idle = value;

	return size;
}
static DEVICE_ATTR(force_resin_warm_s2idle, 0664, force_resin_warm_s2idle_show, force_resin_warm_s2idle_store);
#endif /* CONFIG_PINCTRL_MSM_S2IDLE_DUMP */
// @}
#endif /* CONFIG_NOTHING_IS_METROID */


static int pm8916_reboot_mode_write(struct reboot_mode_driver *reboot,
				    unsigned int magic)
{
	struct pm8916_pon *pon = container_of
			(reboot, struct pm8916_pon, reboot_mode);
	int ret;

	ret = regmap_update_bits(pon->regmap,
				 pon->baseaddr + PON_SOFT_RB_SPARE,
				 GENMASK(7, pon->reason_shift),
				 magic << pon->reason_shift);
	if (ret < 0)
		dev_err(pon->dev, "update reboot mode bits failed\n");

	return ret;
}

static int pm8916_pon_probe(struct platform_device *pdev)
{
	struct pm8916_pon *pon;
	long reason_shift;
	int error;

	pon = devm_kzalloc(&pdev->dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;

	pon->dev = &pdev->dev;

	pon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!pon->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	error = of_property_read_u32(pdev->dev.of_node, "reg",
				     &pon->baseaddr);
	if (error)
		return error;

	reason_shift = (long)of_device_get_match_data(&pdev->dev);

	if (reason_shift != NO_REASON_SHIFT) {
		pon->reboot_mode.dev = &pdev->dev;
		pon->reason_shift = reason_shift;
		pon->reboot_mode.write = pm8916_reboot_mode_write;
		error = devm_reboot_mode_register(&pdev->dev, &pon->reboot_mode);
		if (error) {
			dev_err(&pdev->dev, "can't register reboot mode\n");
			return error;
		}
	}

#if IS_ENABLED(CONFIG_NOTHING_IS_METROID) || IS_ENABLED(CONFIG_NOTHING_IS_FROGGERPRO)
// Support force key warm_reset feature @{
	error = device_create_file(&pdev->dev, &dev_attr_force_key_warm_reset);
	if (error) {
		dev_err(&pdev->dev, "sysfs force key warm reset file creation failed, error = %d\n",
			error);
		return error;
	}

#if IS_ENABLED(CONFIG_PINCTRL_MSM_S2IDLE_DUMP)
	error = device_create_file(&pdev->dev, &dev_attr_force_resin_warm_s2idle);
	if (error) {
		dev_err(&pdev->dev, "sysfs force resin warm reset file creation failed, error = %d\n",
			error);
		return error;
	}
#endif /* CONFIG_PINCTRL_MSM_S2IDLE_DUMP */
// @}
#endif /* CONFIG_NOTHING_IS_METROID */

	platform_set_drvdata(pdev, pon);

	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id pm8916_pon_id_table[] = {
	{ .compatible = "qcom,pm8916-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8941-pon", .data = (void *)NO_REASON_SHIFT },
	{ .compatible = "qcom,pms405-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8998-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ .compatible = "qcom,pmk8350-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ }
};
MODULE_DEVICE_TABLE(of, pm8916_pon_id_table);

static struct platform_driver pm8916_pon_driver = {
	.probe = pm8916_pon_probe,
	.driver = {
		.name = "pm8916-pon",
		.of_match_table = pm8916_pon_id_table,
	},
};
module_platform_driver(pm8916_pon_driver);

MODULE_DESCRIPTION("pm8916 Power On driver");
MODULE_LICENSE("GPL v2");
