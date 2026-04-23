/*
 * aw37004-regulator.c - Regulator device driver for AW37004
 * Copyright (C) 2021  ETEK Semiconductor Ltd.
 *
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
 #include <linux/init.h>
 #include <linux/module.h>
 #include <linux/of.h>
 #include <linux/of_gpio.h>
 #include <linux/gpio.h>
 #include <linux/regmap.h>
 #include <linux/regulator/driver.h>
 #include <linux/regulator/machine.h>
 #include <linux/regulator/of_regulator.h>
 #include <linux/kernel.h>

 #define AW37004_CHIPID			0x00
 #define AW37004_CHIPID2		0x19
 #define AW37004_ILIMIT			0x01
 #define AW37004_RDIS			0x02

 #define AW37004_LDO1_DVO1		0x03
 #define AW37004_LDO2_DVO2		0x04
 #define AW37004_LDO3_AVO1		0x05
 #define AW37004_LDO4_AVO2		0x06

 #define AW37004_LDO1_LDO2_SEQ1	0x0a
 #define AW37004_LDO3_LDO4_SEQ2	0x0b

 #define AW37004_LDO_EN			0x0e
 #define AW37004_SEQ_C			0x0f
 #define AW37004_ILMIT_COAR		0x1A

 #define AW37004_MAX_REG_NO		0x1A
 #define AW37004_VSEL_MASK		0xff

 #define DEVICE_ID				0x04


 enum aw37004_regulators {
 	AW37004_REGULATOR_LDO1 = 0,
 	AW37004_REGULATOR_LDO2,
 	AW37004_REGULATOR_LDO3,
 	AW37004_REGULATOR_LDO4,
 	AW37004_MAX_REGULATORS,
 };

 /* ldo1~4  0-min current,1- max current */
 /*
  *	LDO1	1550000, 2000000
  *	LDO2	1250000, 1900000
  *	LDO3	400000, 1000000
  *	LDO4	400000, 1000000
  */
 static const unsigned int aw37004_crtable1[] = {1550000, 3000000};
 static const unsigned int aw37004_crtable2[] = {1250000, 1900000};
 static const unsigned int aw37004_crtable3[] = {400000, 1000000};
 static const unsigned int aw37004_crtable4[] = {400000, 1000000};

 struct aw37004 {
 	struct 	device *dev;
 	struct 	regmap *regmap;
 	struct 	regulator_dev *rdev;
 	struct 	gpio_desc *en_gpio;
 	int 	min_dropout_uv;
 	int 	ldo_vout[4];
 	int 	ldo_en;
 };

 static const struct regulator_ops aw37004_reg_ops = {
 	.list_voltage		= regulator_list_voltage_linear,
 	.map_voltage		= regulator_map_voltage_linear,
 	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
 	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
 	.set_current_limit  = regulator_set_current_limit_regmap,
 	.enable				= regulator_enable_regmap,
 	.disable			= regulator_disable_regmap,
 	.is_enabled			= regulator_is_enabled_regmap,
 };

 #define AW37004_DESC(_id, _match, _supply, _min, _max, _step,        \
                     _vreg, _vmask, _ereg, _emask,                     \
                     _enval, _disval, _curr_table, _creg, _cmask, _minsel)  \
 	{																	\
 		.id					= (_id),									\
 		.name				= (_match),									\
 		.of_match			= of_match_ptr(_match),						\
 		.supply_name		= (_supply),								\
 		.min_uV				= (_min) * 1000,							\
 		.uV_step			= (_step) * 1000,							\
 		.n_voltages			= (((_max) - (_min)) / (_step) + _minsel),	\
 		.n_current_limits 	= ARRAY_SIZE(_curr_table),					\
 		.regulators_node 	= of_match_ptr("regulators"),				\
 		.type				= REGULATOR_VOLTAGE,						\
 		.vsel_reg			= (_vreg),									\
 		.vsel_mask			= (_vmask),									\
  		.csel_reg			= (_creg),									\
  		.csel_mask			= (_cmask),									\
  		.enable_reg			= (_ereg),									\
  		.enable_mask		= (_emask),									\
  		.enable_val     	= (_enval),									\
  		.disable_val    	= (_disval),								\
  		.ops				= &aw37004_reg_ops,							\
  		.curr_table 		= _curr_table,								\
  		.linear_min_sel 	= _minsel,									\
  		.owner				= THIS_MODULE,								\
  	}

  static const struct regulator_desc aw37004_reg[] = {
  	AW37004_DESC(AW37004_REGULATOR_LDO1, "AW_LDO1", "vin1", 600, 2130, 6,
  		     	AW37004_LDO1_DVO1, AW37004_VSEL_MASK, AW37004_LDO_EN, BIT(0),
  			 	BIT(0), 0,	aw37004_crtable1, AW37004_ILIMIT, BIT(0), 0),

  	AW37004_DESC(AW37004_REGULATOR_LDO2, "AW_LDO2", "vin1", 600, 2130, 6,
  		     	AW37004_LDO2_DVO2, AW37004_VSEL_MASK, AW37004_LDO_EN, BIT(1),
  			 	BIT(1), 0, aw37004_crtable2, AW37004_ILIMIT, BIT(1), 0),

  	AW37004_DESC(AW37004_REGULATOR_LDO3, "AW_LDO3", "vin2", 1200, 4387.5, 12.5,
  		     	AW37004_LDO3_AVO1, AW37004_VSEL_MASK, AW37004_LDO_EN, BIT(2),
  			 	BIT(2), 0, aw37004_crtable3, AW37004_ILIMIT, BIT(2), 0),

  	AW37004_DESC(AW37004_REGULATOR_LDO4, "AW_LDO4", "vin2", 1200, 4387.5, 12.5,
  		     	AW37004_LDO4_AVO2, AW37004_VSEL_MASK, AW37004_LDO_EN, BIT(3),
  			 	BIT(3), 0, aw37004_crtable4, AW37004_ILIMIT, BIT(3), 0),
  };


  static const struct regmap_range aw37004_writeable_ranges[] = {
  	regmap_reg_range(AW37004_ILIMIT, AW37004_ILMIT_COAR),
  };

  static const struct regmap_range aw37004_readable_ranges[] = {
  	regmap_reg_range(AW37004_CHIPID, AW37004_ILMIT_COAR),
  };

  static const struct regmap_range aw37004_volatile_ranges[] = {
  	regmap_reg_range(AW37004_ILIMIT, AW37004_ILMIT_COAR),
  };

  static const struct regmap_access_table aw37004_writeable_table = {
  	.yes_ranges   = aw37004_writeable_ranges,
  	.n_yes_ranges = ARRAY_SIZE(aw37004_writeable_ranges),
  };

  static const struct regmap_access_table aw37004_readable_table = {
  	.yes_ranges   = aw37004_readable_ranges,
  	.n_yes_ranges = ARRAY_SIZE(aw37004_readable_ranges),
  };

  static const struct regmap_access_table aw37004_volatile_table = {
  	.yes_ranges   = aw37004_volatile_ranges,
  	.n_yes_ranges = ARRAY_SIZE(aw37004_volatile_ranges),
  };

  static const struct regmap_config aw37004_regmap_config = {
  	.reg_bits = 8,
  	.val_bits = 8,
  	.max_register = AW37004_MAX_REG_NO,
  	.wr_table = &aw37004_writeable_table,
  	.rd_table = &aw37004_readable_table,
  	.cache_type = REGCACHE_RBTREE,
  	.volatile_table = &aw37004_volatile_table,
  };

  static void aw37004_reset(struct aw37004 *aw37004)
  {
  	gpiod_set_value_cansleep(aw37004->en_gpio, 0);	//set L
  }

  static int aw37004_check_device_id(struct regmap *regmap, unsigned int reg, unsigned int expected_id)
  {
      unsigned int device_id;
      int ret;

      // 读取设备ID
      ret = regmap_read(regmap, reg, &device_id);
      if (ret < 0) {
          pr_info("Failed to read device ID: %d\n", ret);
          return ret;
      }

      // 检查设备ID
      if (device_id != expected_id) {
          pr_info("Invalid device ID: 0x%02X, expected 0x%02X\n", device_id, expected_id);
          return -ENODEV;
      }

      // 设备匹配成功
      pr_info("Device (ID: 0x%02X) detected\n", device_id);

      return 0;
  }

  static int aw37004_i2c_probe(struct i2c_client *client)
  {
  	struct device *dev = &client->dev;
  	struct regulator_config config = {};
  	struct regulator_dev *rdev;
  	const struct regulator_desc *regulators;
  	struct aw37004 *aw37004;
  	int ret, i;

  	aw37004 = devm_kzalloc(dev, sizeof(struct aw37004), GFP_KERNEL);
  	if (!aw37004)
  		return -ENOMEM;

  	aw37004->en_gpio = devm_gpiod_get(dev, "en", GPIOD_OUT_LOW);
  	if (IS_ERR(aw37004->en_gpio)) {
  		ret = PTR_ERR(aw37004->en_gpio);
  		dev_err(dev, "failed to request reset GPIO: %d\n", ret);
  		return ret;
  	}

  	aw37004_reset(aw37004);

  	regulators = aw37004_reg;

  	i2c_set_clientdata(client, aw37004);
  	aw37004->dev = dev;

  	aw37004->regmap = devm_regmap_init_i2c(client, &aw37004_regmap_config);
  	if (IS_ERR(aw37004->regmap)) {
  		ret = PTR_ERR(aw37004->regmap);
  		dev_err(dev, "Failed to allocate register map: %d\n", ret);
  		return ret;
  	}

  	ret = aw37004_check_device_id(aw37004->regmap,AW37004_CHIPID2,DEVICE_ID);
  	if(ret < 0){
  		return ret;
  	}

  	config.dev = &client->dev;
  	config.regmap = aw37004->regmap;

  	/* Instantiate the regulators */
  	for (i = 0; i < AW37004_MAX_REGULATORS; i++) {
  		rdev = devm_regulator_register(&client->dev,
  					       &regulators[i], &config);
  		if (IS_ERR(rdev)) {
  			dev_err(&client->dev,
  				"failed to register %d regulator\n", i);
  			return PTR_ERR(rdev);
  		}
  	}

  	/*Inital related mask for interrupt here*/
  	for (i = 0; i < ARRAY_SIZE(aw37004->ldo_vout); i++)
  	{
  		regmap_read(aw37004->regmap, AW37004_LDO1_DVO1 + i,
  			&aw37004->ldo_vout[i]);
  		pr_info("aw37004 ET_LDO%d value is 0x%x\n", i, aw37004->ldo_vout[i]);
  	}

  	return 0;
  }

  static void aw37004_regulator_shutdown(struct i2c_client *client)
  {
  	struct aw37004 *aw37004 = i2c_get_clientdata(client);
  	if (system_state == SYSTEM_POWER_OFF)
  		regmap_write(aw37004->regmap, AW37004_LDO_EN, 0x80);
  }

  static int __maybe_unused aw37004_suspend(struct device *dev)
  {
  	struct i2c_client *client = to_i2c_client(dev);
  	struct aw37004 *aw37004 = i2c_get_clientdata(client);
  	int i;
  	regmap_read(aw37004->regmap, AW37004_LDO_EN, &aw37004->ldo_en);
  	for (i = 0; i < ARRAY_SIZE(aw37004->ldo_vout); i++)
  		regmap_read(aw37004->regmap, AW37004_LDO1_DVO1 + i,
  			    &aw37004->ldo_vout[i]);

  	return 0;
  }

  static int __maybe_unused aw37004_resume(struct device *dev)
  {
  	struct i2c_client *client = to_i2c_client(dev);
  	struct aw37004 *aw37004 = i2c_get_clientdata(client);
  	int i;

  	for (i = 0; i < ARRAY_SIZE(aw37004->ldo_vout); i++)
  		regmap_write(aw37004->regmap, AW37004_LDO1_DVO1 + i,
  			     aw37004->ldo_vout[i]);
  	regmap_write(aw37004->regmap, AW37004_LDO_EN, aw37004->ldo_en);

  	return 0;
  }

  static SIMPLE_DEV_PM_OPS(aw37004_pm_ops, aw37004_suspend, aw37004_resume);

  static const struct i2c_device_id aw37004_i2c_id[] = {
  	{ "aw37004", 0 },
  	{ },
  };

  MODULE_DEVICE_TABLE(i2c, aw37004_i2c_id);

  static const struct of_device_id aw37004_of_match[] = {
  	{ .compatible = "awinic,aw37004" },
  	{}
  };
  MODULE_DEVICE_TABLE(of, aw37004_of_match);

  static struct i2c_driver aw37004_i2c_driver = {
  	.driver = {
  		.name = "aw37004",
  		.of_match_table = of_match_ptr(aw37004_of_match),
  		.pm = &aw37004_pm_ops,
  	},
  	.id_table 	= aw37004_i2c_id,
  	.probe		= aw37004_i2c_probe,
  	.shutdown 	= aw37004_regulator_shutdown,
  };

  module_i2c_driver(aw37004_i2c_driver);

  MODULE_DESCRIPTION("AW37004 regulator driver");
  MODULE_LICENSE("GPL");