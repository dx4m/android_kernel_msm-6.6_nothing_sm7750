#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#define OISLOGD(format, arg...) \
		 printk(KERN_DEBUG "[%s:%d] "format"\n", __func__, __LINE__, ##arg)

#define OISLOGI(format, arg...) \
		 printk(KERN_INFO "[%s:%d] "format"\n", __func__, __LINE__, ##arg)

#define OISLOGE(format, arg...) \
		 printk(KERN_ERR "[%s:%d] "format"\n", __func__, __LINE__, ##arg)

struct ois_data {
	dev_t devt;
	struct kobject *kobj;
	struct platform_device *plat_dev;
	int value;
	unsigned int main_enable_gpio;
	unsigned int sub_enable_gpio;
	unsigned int cam_main_vdd_max_uv;
	unsigned int cam_main_vdd_min_uv;
	unsigned int cam_sub_vdd_max_uv;
	unsigned int cam_sub_vdd_min_uv;
	struct regulator *cam_main_custom_vdd;
	struct regulator *cam_sub_custom_vdd;
};

static struct ois_data ois_data;

static ssize_t ois_value_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct ois_data *data = &ois_data;

	return sprintf(buf, "%d\n", data->value);
}

static ssize_t ois_value_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	struct ois_data *data = &ois_data;

	if (kstrtoint(buf, 10, &data->value) < 0) {
		OISLOGE("Invalid input: not a valid integer\n");
		return -EINVAL;
	}
	if (data->value == 1) {
		gpio_set_value(data->main_enable_gpio, 1);
		gpio_set_value(data->sub_enable_gpio, 1);
		int ret;
		ret = regulator_enable(data->cam_main_custom_vdd);
		if (ret) {
			OISLOGE("Failed to enable cam_main_custom_vdd: %d\n", ret);
			return ret;
		}
		ret = regulator_enable(data->cam_sub_custom_vdd);
		if (ret) {
			OISLOGE("Failed to enable cam_sub_custom_vdd: %d\n", ret);
		}
	} else {
		gpio_set_value(data->main_enable_gpio, 0);
		gpio_set_value(data->sub_enable_gpio, 0);
		ret = regulator_disable(data->cam_main_custom_vdd);
		if(ret){
			OISLOGE("Failed to disable cam_main_custom_vdd: %d\n", ret);
			return ret;
		}
		ret = regulator_disable(data->cam_sub_custom_vdd);
		if(ret){
			OISLOGE("Failed to disable cam_sub_custom_vdd: %d\n", ret);
		}
	}
	return count;
}

static struct kobj_attribute ois_value_attribute = __ATTR(value, 0664, ois_value_show, ois_value_store);

int ois_parse_dts(struct ois_data *data)
{
	int rc = 0;
	struct device *dev = &data->plat_dev->dev;
	struct device_node *np = dev->of_node;

	data->main_enable_gpio = of_get_named_gpio(np, "cam_main_enable_gpio", 0);
	if (data->main_enable_gpio < 0) {
		OISLOGE("falied to get reset gpio!\n");
		return data->main_enable_gpio;
	}

	data->sub_enable_gpio = of_get_named_gpio(np, "cam_sub_enable_gpio", 0);
	if (data->sub_enable_gpio < 0) {
		OISLOGE("falied to get reset gpio!\n");
		return data->sub_enable_gpio;
	}
	printk("[ois] %d %d\n", data->main_enable_gpio, data->sub_enable_gpio);

	data->cam_main_custom_vdd =  regulator_get(dev, "cam_main_custom_vdd");
	if (IS_ERR(data->cam_main_custom_vdd)) {
		rc = PTR_ERR(data->cam_main_custom_vdd);
		OISLOGE("Regulator get failed vdd rc = %d\n", rc);
		goto err_cam_main_vdd;
	}

	data->cam_sub_custom_vdd =  regulator_get(dev, "cam_sub_custom_vdd");
	if (IS_ERR(data->cam_sub_custom_vdd)) {
		rc = PTR_ERR(data->cam_sub_custom_vdd);
		OISLOGE("Regulator get failed vdd rc = %d\n", rc);
		goto err_cam_sub_vdd;
	}

	rc = of_property_read_u32(np, "cam_main_vdd-max-uv", &data->cam_main_vdd_max_uv);
	if(rc){
		OISLOGE("fail to get vdd_max_uv\n");
		goto err_reg;
	}

	rc = of_property_read_u32(np, "cam_main_vdd-min-uv", &data->cam_main_vdd_min_uv);
	if(rc){
		OISLOGE("fail to get vdd_min_uv\n");
		goto err_reg;
	}
	rc = of_property_read_u32(np, "cam_sub_vdd-max-uv", &data->cam_sub_vdd_max_uv);
	if(rc){
		OISLOGE("fail to get vdd_max_uv\n");
		goto err_reg;
	}

	rc = of_property_read_u32(np, "cam_sub_vdd-min-uv", &data->cam_sub_vdd_min_uv);
	if(rc){
		OISLOGE("fail to get vdd_min_uv\n");
		goto err_reg;
	}

	OISLOGI("main_vdd_max_uv=%d, main_vdd_min_uv=%d, sub_vdd_max_uv=%d, sub_vdd_min_uv=%d\n",
		data->cam_main_vdd_max_uv, data->cam_main_vdd_min_uv, data->cam_sub_vdd_max_uv, data->cam_sub_vdd_min_uv);

	rc = regulator_set_voltage(data->cam_main_custom_vdd, data->cam_main_vdd_min_uv, data->cam_main_vdd_max_uv);
	if (rc) {
		OISLOGE("Regulator set voltage failed rc=%d\n", rc);
		goto err_reg;
	}

	 rc = regulator_set_voltage(data->cam_sub_custom_vdd,data->cam_sub_vdd_min_uv, data->cam_sub_vdd_max_uv);
	if (rc) {
		OISLOGE("Regulator set voltage failed rc=%d\n", rc);
		goto err_reg;
	}
	printk("ois_parse_dts success\n");
	return rc;
err_reg:
	regulator_put(data->cam_sub_custom_vdd);
err_cam_sub_vdd:
	regulator_put(data->cam_main_custom_vdd);
err_cam_main_vdd:
	return rc;
}

static int ois_probe(struct platform_device *pdev)
{
	struct ois_data *data = &ois_data;
	int retval;

	data->plat_dev = pdev;

	retval = ois_parse_dts(data);
	if (retval) {
		OISLOGE("ois_parse_dts failed\n");
		return retval;
	}
	data->kobj = kobject_create_and_add("ois", kernel_kobj);
	if (!data->kobj)
		return -ENOMEM;

	retval = sysfs_create_file(data->kobj, &ois_value_attribute.attr);
	if (retval) {
		kobject_put(data->kobj);
		return retval;
	}
	platform_set_drvdata(pdev, data);
	return 0;
}

static int ois_remove(struct platform_device *pdev)
{
	struct ois_data *data = platform_get_drvdata(pdev);
	kobject_put(data->kobj);
	return 0;
}

static const struct of_device_id ois_of_match[] = {
	{ .compatible = "vendor,ois_regulator", },
	{ }
};
MODULE_DEVICE_TABLE(of, ois_of_match);

static struct platform_driver ois_driver = {
	.probe = ois_probe,
	.remove = ois_remove,
	.driver = {
		.name = "ois",
		.of_match_table = ois_of_match,
	},
};

module_platform_driver(ois_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nothing driver team");
MODULE_DESCRIPTION("A simple platform driver for OIS regulator");