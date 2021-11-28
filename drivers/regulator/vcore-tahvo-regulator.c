// SPDX-License-Identifier: GPL-2.0+
//
// Copyright (C) 2004, 2005 Nokia Corporation
//
// Based on original 2.6 kernel driver for Nokia N8x0 LCD panel.
// Rewritten in 2021 by Peter Vasil <petervasil@gmail.com>.
//
// Driver for Nokia Betty/Tahvo Vcore regulator
// The only known voltages are currently 1.005V==0x0f and 1.475V==0x00 with mask 0x0f
// Whether the sequence is actually linear is only a guess.

#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/mfd/retu.h>

static const unsigned int tahvo_vcore_voltages[] = {
	1475000, 1443667, 1412333, 1381000, 1349667, 1318333, 1287000, 1255667,
	1224333, 1193000, 1161667, 1130333, 1099000, 1067667, 1036333, 1005000,
};

static const struct regulator_ops tahvo_vcore_regulator_voltage_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};

static const struct regulator_desc vcore_regulator = {
	.name		= "vcore",
	.ops		= &tahvo_vcore_regulator_voltage_ops,
	.type		= REGULATOR_VOLTAGE,
	.owner		= THIS_MODULE,
	.volt_table	= tahvo_vcore_voltages,
	.n_voltages	= ARRAY_SIZE(tahvo_vcore_voltages),
	.vsel_reg	= TAHVO_REG_VCORE,
	.vsel_mask	= 0x0f,
};

static const struct regmap_config tahvo_vcore_regmap_config = {
	.reg_bits	= 8,
	.reg_stride	= 1,
	.val_bits	= 16,
};

static int tahvo_vcore_regulator_probe(struct platform_device *pdev)
{
	struct retu_dev *retu = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct regulator_init_data *init_data;
	struct regulator_config cfg = {};
	struct regulator_dev *rdev;

	dev_dbg(dev, "%s\n", __func__);

	init_data = of_get_regulator_init_data(dev, dev->of_node,
					       &vcore_regulator);
	if (!init_data) {
		dev_err(dev, "Failed to init regulator data!\n");
		return -EINVAL;
	}

	cfg.dev = dev;
	cfg.init_data = init_data;
	cfg.of_node = dev->of_node;
	cfg.regmap = retu_get_regmap(retu);

	rdev = devm_regulator_register(dev, &vcore_regulator, &cfg);
	if (IS_ERR(rdev)) {
		dev_err(dev, "Failed to register regulator: %ld\n",
			PTR_ERR(rdev));
		return PTR_ERR(rdev);
	}
	platform_set_drvdata(pdev, rdev);

	return 0;
}

static const struct of_device_id regulator_tahvo_vcore_of_match[] = {
	{ .compatible = "nokia,tahvo,vcore-regulator", },
	{},
};

static struct platform_driver tahvo_vcore_regulator_driver = {
	.probe = tahvo_vcore_regulator_probe,
	.driver = {
		.name = "vcore-tahvo-regulator",
		.of_match_table = of_match_ptr(regulator_tahvo_vcore_of_match),
	},
};
module_platform_driver(tahvo_vcore_regulator_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Peter Vasil <petervasil@gmail.com>");
MODULE_DESCRIPTION("Tahvo/Betty Vcore voltage regulator");
