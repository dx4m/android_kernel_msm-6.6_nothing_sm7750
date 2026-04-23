#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define CLASS_NAME "cable_detect"

struct cable_info {
	struct platform_device *pdev;
	struct class *cable_class;
	struct gpio_desc *ant0_desc;
	struct gpio_desc *ant2_desc;
	struct gpio_desc *ant8_desc;
};

static struct cable_info *cable = NULL;
/* ative low */
static const char *cable_status[] = {"absent", "exist"};

static ssize_t ant0_show(const struct class *class, const struct class_attribute *attr,
							char *buf)
{
	int value;
	value = gpiod_get_value(cable->ant0_desc);
	return sprintf(buf, "%s\n", cable_status[!!value]);
}
static CLASS_ATTR_RO(ant0);

static ssize_t ant2_show(const struct class *class, const struct class_attribute *attr,
							char *buf)
{
	int value;
	value = gpiod_get_value(cable->ant2_desc);
	return sprintf(buf, "%s\n", cable_status[!!value]);
}
static CLASS_ATTR_RO(ant2);

static ssize_t ant8_show(const struct class *class, const struct class_attribute *attr,
							char *buf)
{
	int value;
	value = gpiod_get_value(cable->ant8_desc);
	return sprintf(buf, "%s\n", cable_status[!!value]);
}
static CLASS_ATTR_RO(ant8);

static struct attribute *cable_class_attrs[] = {
		&class_attr_ant0.attr,
		&class_attr_ant2.attr,
		&class_attr_ant8.attr,
		NULL,
};
ATTRIBUTE_GROUPS(cable_class);

static struct class cable_class = {
	.name = CLASS_NAME,
	.class_groups = cable_class_groups,
};

static int cable_parse_dt(void)
{
	int ret = 0;

	cable->ant0_desc = devm_gpiod_get(&cable->pdev->dev, "ant0_present", GPIOD_IN);
	if (unlikely(IS_ERR(cable->ant0_desc))) {
		ret = PTR_ERR(cable->ant0_desc);
		dev_err(&cable->pdev->dev,
				"get ant0 present gpio desc error, return %d\n",
				ret);
		return ret;
	}

	cable->ant2_desc = devm_gpiod_get(&cable->pdev->dev, "ant2_present", GPIOD_IN);
	if (unlikely(IS_ERR(cable->ant2_desc))) {
		ret = PTR_ERR(cable->ant2_desc);
		dev_err(&cable->pdev->dev,
				"get ant2 present gpio desc error, return %d\n",
				ret);
		return ret;
	}

	cable->ant8_desc = devm_gpiod_get(&cable->pdev->dev, "ant8_present", GPIOD_IN);
	if (unlikely(IS_ERR(cable->ant8_desc))) {
		ret = PTR_ERR(cable->ant8_desc);
		dev_err(&cable->pdev->dev,
				"get ant8 present gpio desc error, return %d\n",
				ret);
		return ret;
	}

	return 0;
}

static int cable_class_probe(struct platform_device *pdev)
{
	int ret;

	cable = kzalloc(sizeof(struct cable_info), GFP_KERNEL);
	cable->pdev = pdev;
	cable->cable_class = &cable_class;

	ret = cable_parse_dt();
	if (ret) {
		dev_err(&cable->pdev->dev,"cable_parse_dt failed\n");
		goto err;
	}

	ret = class_register(&cable_class);
	if (ret) {
		dev_err(&cable->pdev->dev,"class_register fail\n");
		goto err;
	}

	return ret;

err:
	kfree(cable);
	return ret;
}

static const struct of_device_id cable_of_match[] = {
	{ .compatible = "noth,cable_detect", },
	{ },
};

MODULE_DEVICE_TABLE(of, cable_of_match);

static struct platform_driver cable_class_driver = {
	.probe		= cable_class_probe,
	.driver		= {
		.name	= "cable_class",
		.of_match_table = cable_of_match,
	}
};

static int __init cable_detect_init(void)
{
	return platform_driver_register(&cable_class_driver);
}

static void __exit cable_detect_exit(void)
{
	class_unregister(&cable_class);
	platform_driver_unregister(&cable_class_driver);
	kfree(cable);
}


module_init(cable_detect_init);
module_exit(cable_detect_exit);
MODULE_DESCRIPTION("Nothing antenna cable detect driver");
MODULE_LICENSE("GPL v2");