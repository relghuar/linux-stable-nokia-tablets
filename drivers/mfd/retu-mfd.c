/*
 * Retu/Tahvo MFD driver
 *
 * Copyright (C) 2004, 2005 Nokia Corporation
 *
 * Based on code written by Juha Yrjölä, David Weinehall and Mikko Ylinen.
 * Rewritten by Aaro Koskinen.
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License. See the file "COPYING" in the main directory of this
 * archive for more details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/mfd/core.h>
#include <linux/mfd/retu.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>

struct retu_dev {
	struct regmap			*regmap;
	struct device			*dev;
	struct mutex			mutex;
	struct regmap_irq_chip_data	*irq_data;
};

static const struct resource retu_pwrbutton_res[] = {
	{
		.name	= "retu-pwrbutton",
		.start	= RETU_INT_PWR,
		.end	= RETU_INT_PWR,
		.flags	= IORESOURCE_IRQ,
	},
};

static const struct mfd_cell retu_devs[] = {
	{
		.name		= "retu-wdt"
	},
	{
		.name		= "retu-pwrbutton",
		.resources	= retu_pwrbutton_res,
		.num_resources	= ARRAY_SIZE(retu_pwrbutton_res),
	}
};

static struct regmap_irq retu_irqs[] = {
	[RETU_INT_PWR] = {
		.mask = 1 << RETU_INT_PWR,
	}
};

static struct regmap_irq_chip retu_irq_chip = {
	.name		= "RETU",
	.irqs		= retu_irqs,
	.num_irqs	= ARRAY_SIZE(retu_irqs),
	.num_regs	= 1,
	.status_base	= RETU_REG_IDR,
	.mask_base	= RETU_REG_IMR,
	.ack_base	= RETU_REG_IDR,
};

/* Retu device registered for the power off. */
static struct retu_dev *retu_pm_power_off;

static const struct resource tahvo_usb_res[] = {
	{
		.name	= "tahvo-usb",
		.start	= TAHVO_INT_VBUS,
		.end	= TAHVO_INT_VBUS,
		.flags	= IORESOURCE_IRQ,
	},
};

static const struct mfd_cell tahvo_devs[] = {
	{
		.name		= "tahvo-usb",
		.resources	= tahvo_usb_res,
		.num_resources	= ARRAY_SIZE(tahvo_usb_res),
	},
};

static struct regmap_irq tahvo_irqs[] = {
	[TAHVO_INT_VBUS] = {
		.mask = 1 << TAHVO_INT_VBUS,
	}
};

static struct regmap_irq_chip tahvo_irq_chip = {
	.name		= "TAHVO",
	.irqs		= tahvo_irqs,
	.num_irqs	= ARRAY_SIZE(tahvo_irqs),
	.num_regs	= 1,
	.status_base	= RETU_REG_IDR,
	.mask_base	= TAHVO_REG_IMR,
	.ack_base	= RETU_REG_IDR,
};

static const struct retu_data {
	char			*chip_name;
	char			*companion_name;
	struct regmap_irq_chip	*irq_chip;
	const struct mfd_cell	*children;
	int			nchildren;
} retu_data[] = {
	[0] = {
		.chip_name	= "Retu",
		.companion_name	= "Vilma",
		.irq_chip	= &retu_irq_chip,
		.children	= retu_devs,
		.nchildren	= ARRAY_SIZE(retu_devs),
	},
	[1] = {
		.chip_name	= "Tahvo",
		.companion_name	= "Betty",
		.irq_chip	= &tahvo_irq_chip,
		.children	= tahvo_devs,
		.nchildren	= ARRAY_SIZE(tahvo_devs),
	}
};

static struct retu_dev *retu_dev_retu;
static struct retu_dev *retu_dev_tahvo;

struct retu_dev *retu_get_dev_retu() {
	WARN_ON(retu_dev_retu == NULL);
	return retu_dev_retu;
}

struct retu_dev *retu_get_dev_tahvo() {
	WARN_ON(retu_dev_tahvo == NULL);
	return retu_dev_tahvo;
}

int retu_read(struct retu_dev *rdev, u8 reg)
{
	int ret;
	int value;

	mutex_lock(&rdev->mutex);
	ret = regmap_read(rdev->regmap, reg, &value);
	mutex_unlock(&rdev->mutex);

	return ret ? ret : value;
}
EXPORT_SYMBOL_GPL(retu_read);

int retu_write(struct retu_dev *rdev, u8 reg, u16 data)
{
	int ret;

	mutex_lock(&rdev->mutex);
	ret = regmap_write(rdev->regmap, reg, data);
	mutex_unlock(&rdev->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(retu_write);

static void retu_power_off(void)
{
	struct retu_dev *rdev = retu_pm_power_off;
	int reg;

	mutex_lock(&retu_pm_power_off->mutex);

	/* Ignore power button state */
	regmap_read(rdev->regmap, RETU_REG_CC1, &reg);
	regmap_write(rdev->regmap, RETU_REG_CC1, reg | 2);

	/* Expire watchdog immediately */
	regmap_write(rdev->regmap, RETU_REG_WATCHDOG, 0);

	/* Wait for poweroff */
	for (;;)
		cpu_relax();

	mutex_unlock(&retu_pm_power_off->mutex);
}

static int retu_regmap_read(void *context, const void *reg, size_t reg_size,
			    void *val, size_t val_size)
{
	int ret;
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);

	BUG_ON(reg_size != 1 || val_size != 2);

	ret = i2c_smbus_read_word_data(i2c, *(u8 const *)reg);
	if (ret < 0)
		return ret;

	*(u16 *)val = ret;
	return 0;
}

static int retu_regmap_write(void *context, const void *data, size_t count)
{
	u8 reg;
	u16 val;
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);

	BUG_ON(count != sizeof(reg) + sizeof(val));
	memcpy(&reg, data, sizeof(reg));
	memcpy(&val, data + sizeof(reg), sizeof(val));
	return i2c_smbus_write_word_data(i2c, reg, val);
}

static struct regmap_bus retu_bus = {
	.read = retu_regmap_read,
	.write = retu_regmap_write,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

static const struct regmap_config retu_config = {
	.reg_bits = 8,
	.val_bits = 16,
};

static int retu_probe(struct i2c_client *i2c, const struct i2c_device_id *id)
{
	struct retu_data const *rdat;
	struct retu_dev *rdev;
	int ret;

	if (i2c->addr > ARRAY_SIZE(retu_data))
		return -ENODEV;
	rdat = &retu_data[i2c->addr - 1];

	rdev = devm_kzalloc(&i2c->dev, sizeof(*rdev), GFP_KERNEL);
	if (rdev == NULL)
		return -ENOMEM;

	i2c_set_clientdata(i2c, rdev);
	rdev->dev = &i2c->dev;
	mutex_init(&rdev->mutex);
	rdev->regmap = devm_regmap_init(&i2c->dev, &retu_bus, &i2c->dev,
					&retu_config);
	if (IS_ERR(rdev->regmap))
		return PTR_ERR(rdev->regmap);

	ret = retu_read(rdev, RETU_REG_ASICR);
	if (ret < 0) {
		dev_err(rdev->dev, "could not read %s revision: %d\n",
			rdat->chip_name, ret);
		return ret;
	}

	dev_info(rdev->dev, "%s%s%s v%d.%d found\n", rdat->chip_name,
		 (ret & RETU_REG_ASICR_VILMA) ? " & " : "",
		 (ret & RETU_REG_ASICR_VILMA) ? rdat->companion_name : "",
		 (ret >> 4) & 0x7, ret & 0xf);

	/* Mask all interrupts. */
	ret = retu_write(rdev, rdat->irq_chip->mask_base, 0xffff);
	if (ret < 0)
		return ret;

	ret = regmap_add_irq_chip(rdev->regmap, i2c->irq, IRQF_ONESHOT, -1,
				  rdat->irq_chip, &rdev->irq_data);
	if (ret < 0)
		return ret;

	ret = mfd_add_devices(rdev->dev, -1, rdat->children, rdat->nchildren,
			      NULL, regmap_irq_chip_get_base(rdev->irq_data),
			      NULL);
	if (ret < 0) {
		regmap_del_irq_chip(i2c->irq, rdev->irq_data);
		return ret;
	}

	if (rdev->dev->of_node) {
		int ncells;
		struct device_node *np = dev_of_node(rdev->dev);
		struct device_node *child;
		struct mfd_cell *cells;

		ncells = of_get_available_child_count(np);
		cells = devm_kzalloc(rdev->dev, ncells*sizeof(struct mfd_cell), GFP_KERNEL);

		ncells = 0;
		for_each_available_child_of_node(np, child) {
			ret = of_property_read_string(child, "compatible", &cells[ncells].of_compatible);
			if (!ret) {
				cells[ncells].name = child->name;
				dev_dbg(rdev->dev, "%s:   found cell '%s' compat='%s'\n",
						__func__, child->name, cells[ncells].of_compatible);
				ncells++;
			}
		}

		dev_dbg(rdev->dev, "%s: adding %d cells\n", __func__, ncells);
		if (ncells > 0) {
			ret = mfd_add_devices(rdev->dev, -1, cells, ncells,
					     NULL, regmap_irq_chip_get_base(rdev->irq_data),
					     NULL);
		}
	}

	if (i2c->addr == 1 && !pm_power_off) {
		retu_pm_power_off = rdev;
		pm_power_off	  = retu_power_off;
	}

	if (strcmp(rdat->chip_name, "Retu") == 0)
		retu_dev_retu = rdev;
	else if (strcmp(rdat->chip_name, "Tahvo") == 0)
		retu_dev_tahvo = rdev;

	return 0;
}

static int retu_remove(struct i2c_client *i2c)
{
	struct retu_dev *rdev = i2c_get_clientdata(i2c);

	if (retu_pm_power_off == rdev) {
		pm_power_off	  = NULL;
		retu_pm_power_off = NULL;
	}
	mfd_remove_devices(rdev->dev);
	regmap_del_irq_chip(i2c->irq, rdev->irq_data);

	return 0;
}

static const struct i2c_device_id retu_id[] = {
	{ "retu", 0 },
	{ "tahvo", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, retu_id);

static const struct of_device_id retu_of_match[] = {
	{ .compatible = "nokia,retu" },
	{ .compatible = "nokia,tahvo" },
	{ }
};
MODULE_DEVICE_TABLE(of, retu_of_match);

static struct i2c_driver retu_driver = {
	.driver		= {
		.name = "retu-mfd",
		.of_match_table = retu_of_match,
	},
	.probe		= retu_probe,
	.remove		= retu_remove,
	.id_table	= retu_id,
};
module_i2c_driver(retu_driver);

MODULE_DESCRIPTION("Retu MFD driver");
MODULE_AUTHOR("Juha Yrjölä");
MODULE_AUTHOR("David Weinehall");
MODULE_AUTHOR("Mikko Ylinen");
MODULE_AUTHOR("Aaro Koskinen <aaro.koskinen@iki.fi>");
MODULE_LICENSE("GPL");
