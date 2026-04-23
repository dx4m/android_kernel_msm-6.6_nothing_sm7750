#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define CLASS_NAME "slot_check"

struct slot_info {
	struct platform_device *pdev;
	struct class *slot_class;
	struct gpio_desc *usim0_desc;
	struct gpio_desc *usim1_desc;
};

static struct slot_info *slot = NULL;

static ssize_t usim0_show(const struct class *class, const struct class_attribute *attr,
							char *buf)
{
	int value;
	value = gpiod_get_value(slot->usim0_desc);
	return sprintf(buf, "%d\n", value);
}
static CLASS_ATTR_RO(usim0);

static ssize_t usim1_show(const struct class *class, const struct class_attribute *attr,
							char *buf)
{
	int value;
	value = gpiod_get_value(slot->usim1_desc);
	return sprintf(buf, "%d\n", value);
}
static CLASS_ATTR_RO(usim1);

static struct attribute *slot_class_attrs[] = {
		&class_attr_usim0.attr,
		&class_attr_usim1.attr,
		NULL,
};
ATTRIBUTE_GROUPS(slot_class);

static struct class slot_class = {
	.name = CLASS_NAME,
	.class_groups = slot_class_groups,
};

static int slot_parse_dt(void)
{
	int ret = 0;

	slot->usim0_desc = devm_gpiod_get(&slot->pdev->dev, "usim0_present", GPIOD_IN);
	if (unlikely(IS_ERR(slot->usim0_desc))) {
		ret = PTR_ERR(slot->usim0_desc);
		dev_err(&slot->pdev->dev,
				"get usim0 present gpio desc error, return %d\n",
				ret);
		return ret;
	}

	slot->usim1_desc = devm_gpiod_get(&slot->pdev->dev, "usim1_present", GPIOD_IN);
	if (unlikely(IS_ERR(slot->usim1_desc))) {
		ret = PTR_ERR(slot->usim1_desc);
		dev_err(&slot->pdev->dev,
				"get usim1 present gpio desc error, return %d\n",
				ret);
		return ret;
	}

	return 0;
}

static int slot_class_probe(struct platform_device *pdev)
{
	int ret;

	slot = kzalloc(sizeof(struct slot_info), GFP_KERNEL);
	slot->pdev = pdev;
	slot->slot_class = &slot_class;

	ret = slot_parse_dt();
	if (ret) {
		dev_err(&slot->pdev->dev,"slot_parse_dt failed\n");
		goto err;
	}

	ret = class_register(&slot_class);
	if (ret) {
		dev_err(&slot->pdev->dev,"class_register fail\n");
		goto err;
	}

	return ret;

err:
	kfree(slot);
	return ret;
}

static const struct of_device_id slot_of_match[] = {
	{ .compatible = "noth,slot_check", },
	{ },
};

MODULE_DEVICE_TABLE(of, slot_of_match);

static struct platform_driver slot_class_driver = {
	.probe		= slot_class_probe,
	.driver		= {
		.name	= "slot_class",
		.of_match_table = slot_of_match,
	}
};

static int __init slot_check_init(void)
{
	return platform_driver_register(&slot_class_driver);
}

static void __exit slot_check_exit(void)
{
	class_unregister(&slot_class);
	platform_driver_unregister(&slot_class_driver);
	kfree(slot);
}


module_init(slot_check_init);
module_exit(slot_check_exit);
MODULE_DESCRIPTION("Nothing USIM slot check driver");
MODULE_LICENSE("GPL v2");