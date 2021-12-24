// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nokia N800 (RX-34), N810 (RX-44) and N810 WiMax (RX-48) battery driver
 *
 * Copyright (C) 2021  Peter Vasil <petervasil@gmail.com>
 */

#include <linux/module.h>
#include <linux/param.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/iio/consumer.h>
#include <linux/of.h>

struct n8x0_device_info {
	struct device *dev;
	struct power_supply *bat;
	struct power_supply_desc bat_desc;
	struct iio_channel *channel_temp;
	struct iio_channel *channel_bsi;
	struct iio_channel *channel_vbat;
};

/*
 * Read raw Retu MADC channel value
 */
static int n8x0_battery_read_adc(struct iio_channel *channel)
{
	int val, err;
	err = iio_read_channel_raw(channel, &val);
	if (err < 0)
		return err;
	return val;
}

/*
 * Read Retu MADC channel 8 (BATVOLT) and convert to microVolts
 * conversion equation taken from openwrt/omap24xx/linux-3.3/n810bm patch
 */
static int n8x0_battery_read_voltage(struct n8x0_device_info *di)
{
	int voltage = n8x0_battery_read_adc(di->channel_vbat);

	if (voltage < 0) {
		dev_err(di->dev, "Could not read ADC: %d\n", voltage);
		return voltage;
	}

	if (voltage <= 0x37)
		return 2800000;
	else
		return 2800000 + ( (voltage - 0x37) * (4200 - 2800) * 1000 / (0x236 - 0x37) );
}

/*
 * Read Retu MADC channel 2 (BATTEMP) and convert value to tenths of Celsius
 * FIXME: this conversion is not even extrapolated, just plainly made up :-)
 * ~measured linear mapping: 300->20dC ; 250->30dC ; 200->40dC
 * -> 500-> -20dC ... 0->80dC - probably wrong, but acceptable for now
 */
static int n8x0_battery_read_temperature(struct n8x0_device_info *di)
{
	int raw = n8x0_battery_read_adc(di->channel_temp);

	if (raw < 0)
		dev_err(di->dev, "Could not read ADC: %d\n", raw);

	/* Zero and negative values are undefined */
	if (raw <= 0)
		return INT_MAX;

	/* ADC channels are 10 bit, higher value are undefined */
	if (raw >= (1 << 10))
		return INT_MIN;

	return 800 - (raw * 200)/100;
}

/*
 * Read Retu MADC channel 1 (BSI) and convert RAW value to micro Ah
 * This conversion formula was "inferred from revealed self-evident wisdom
 * and extrapolated from associated sources" :-)
 */
static int n8x0_battery_read_capacity(struct n8x0_device_info *di)
{
	int capacity = n8x0_battery_read_adc(di->channel_bsi);

	if (capacity < 0) {
		dev_err(di->dev, "Could not read ADC: %d\n", capacity);
		return capacity;
	}

	if (capacity <= 416) return 0;
	return (capacity - 416) * 229 * 12;
}

/*
 * Return power_supply property
 */
static int n8x0_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct n8x0_device_info *di = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = 4200000;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		// FIXME: this should be obtained from Tahvo Status register
		// but the device does not work at all without battery, so...
		val->intval = n8x0_battery_read_voltage(di) ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = n8x0_battery_read_voltage(di);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = n8x0_battery_read_temperature(di);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = n8x0_battery_read_capacity(di);
		break;
	default:
		return -EINVAL;
	}

	if (val->intval == INT_MAX || val->intval == INT_MIN)
		return -EINVAL;

	return 0;
}

static enum power_supply_property n8x0_battery_props[] = {
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
};

static int n8x0_battery_probe(struct platform_device *pdev)
{
	struct power_supply_config psy_cfg = {};
	struct n8x0_device_info *di;
	int ret;

	di = devm_kzalloc(&pdev->dev, sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	platform_set_drvdata(pdev, di);

	di->dev = &pdev->dev;
	di->bat_desc.name = "n8x0-battery";
	di->bat_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	di->bat_desc.properties = n8x0_battery_props;
	di->bat_desc.num_properties = ARRAY_SIZE(n8x0_battery_props);
	di->bat_desc.get_property = n8x0_battery_get_property;

	psy_cfg.drv_data = di;

	di->channel_temp = devm_iio_channel_get(di->dev, "temp");
	if (IS_ERR(di->channel_temp)) {
		if (PTR_ERR(di->channel_temp) == -ENODEV)
			return -EPROBE_DEFER;
		return PTR_ERR(di->channel_temp);
	}

	di->channel_bsi  = devm_iio_channel_get(di->dev, "bsi");
	if (IS_ERR(di->channel_bsi)) {
		if (PTR_ERR(di->channel_bsi) == -ENODEV)
			return -EPROBE_DEFER;
		return PTR_ERR(di->channel_bsi);
	}

	di->channel_vbat = devm_iio_channel_get(di->dev, "vbat");
	if (IS_ERR(di->channel_vbat)) {
		if (PTR_ERR(di->channel_vbat) == -ENODEV)
			return -EPROBE_DEFER;
		return PTR_ERR(di->channel_vbat);
	}

	di->bat = power_supply_register(di->dev, &di->bat_desc, &psy_cfg);
	if (IS_ERR(di->bat)) {
		return PTR_ERR(di->bat);
	}

	return 0;
}

static int n8x0_battery_remove(struct platform_device *pdev)
{
	struct n8x0_device_info *di = platform_get_drvdata(pdev);

	power_supply_unregister(di->bat);

	iio_channel_release(di->channel_vbat);
	iio_channel_release(di->channel_bsi);
	iio_channel_release(di->channel_temp);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id n8x0_battery_of_match[] = {
	{.compatible = "nokia,n8x0-battery", },
	{ },
};
MODULE_DEVICE_TABLE(of, n8x0_battery_of_match);
#endif

static struct platform_driver n8x0_battery_driver = {
	.probe = n8x0_battery_probe,
	.remove = n8x0_battery_remove,
	.driver = {
		.name = "n8x0-battery",
		.of_match_table = of_match_ptr(n8x0_battery_of_match),
	},
};
module_platform_driver(n8x0_battery_driver);

MODULE_ALIAS("platform:n8x0-battery");
MODULE_AUTHOR("Peter Vasil <petervasil@gmail.com");
MODULE_DESCRIPTION("Nokia N8x0 battery driver");
MODULE_LICENSE("GPL");
