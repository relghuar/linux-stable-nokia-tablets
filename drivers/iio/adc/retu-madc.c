// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Retu/Vilma MADC module driver-This driver monitors the real time
 * conversion of analog signals like battery temperature,
 * battery type, battery level etc.
 *
 * Peter Vasil <petervasil@gmail.com>
 *
 * Based on original vendor kernel for Nokia N810
 * Copyright (C) 2008 Nokia Corporation
 * Mikko Ylinen <mikko.k.ylinen@nokia.com>
 *
 */

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mfd/retu.h>
#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/mutex.h>
#include <linux/bitops.h>
#include <linux/jiffies.h>
#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/iio.h>

#define RETU_MADC_MAX_CHANNELS 14

/* ADC channels */
#define RETU_MADC_GND           0x00 /* Ground */
#define RETU_MADC_BSI           0x01 /* Battery Size Indicator */
#define RETU_MADC_BATTEMP       0x02 /* Battery temperature */
#define RETU_MADC_CHGVOLT       0x03 /* Charger voltage */
#define RETU_MADC_HEADSET       0x04 /* Headset detection */
#define RETU_MADC_HOOKDET       0x05 /* Hook detection */
#define RETU_MADC_RFGP          0x06 /* RF GP */
#define RETU_MADC_WBTX          0x07 /* Wideband Tx detection */
#define RETU_MADC_BATVOLT       0x08 /* Battery voltage measurement */
#define RETU_MADC_GND2          0x09 /* Ground */
#define RETU_MADC_LIGHTSENS     0x0A /* Light sensor */
#define RETU_MADC_LIGHTTEMP     0x0B /* Light sensor temperature */
#define RETU_MADC_BKUPVOLT      0x0C /* Backup battery voltage */
#define RETU_MADC_TEMP          0x0D /* RETU temperature */


struct retu_madc_data {
	struct retu_dev *retu;
	bool is_vilma;
};

/**
 * retu_madc_read - Reads AD conversion result
 * @retu: device pointer to the underlying retu device
 * @channel: the ADC channel to read from
 */
int retu_madc_read(struct retu_madc_data *ddata, int channel)
{
        int res;

        if (!ddata || !ddata->retu)
                return -ENODEV;

        if (channel < 0 || channel >= RETU_MADC_MAX_CHANNELS)
                return -EINVAL;

        if ((channel == 8) && ddata->is_vilma) {
                int scr = retu_read(ddata->retu, RETU_REG_ADCSCR);
                int ch = (retu_read(ddata->retu, RETU_REG_ADCR) >> 10) & 0xf;
                if (((scr & 0xff) != 0) && (ch != 8))
                        retu_write(ddata->retu, RETU_REG_ADCSCR, (scr & ~0xff));
        }

        /* Select the channel and read result */
        retu_write(ddata->retu, RETU_REG_ADCR, channel << 10);
        res = retu_read(ddata->retu, RETU_REG_ADCR) & 0x3ff;

        if (ddata->is_vilma)
                retu_write(ddata->retu, RETU_REG_ADCR, (1 << 13));

        return res;
}

static int retu_madc_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct retu_madc_data *ddata = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&indio_dev->mlock);
		ret = retu_madc_read(ddata, chan->channel);
		mutex_unlock(&indio_dev->mlock);
		if (ret < 0)
			return ret;

		*val = ret;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_PROCESSED:
		mutex_lock(&indio_dev->mlock);
		ret = retu_madc_read(ddata, chan->channel);
		mutex_unlock(&indio_dev->mlock);
		if (ret < 0)
			return ret;

		// We only know conversion methods for very few channels
		switch (chan->channel) {
		case RETU_MADC_BSI:
			/*
			 * conversion table extended from openwrt/omap24xx/linux-3.3/n810bm
			 * Resistance  |  ADC value
			 * ========================
			 * 120k        |  0x3AC
			 * 110k        |  0x37C
			 * 100k        |  0x351
			 *  90k        |  0x329
			 *  ...
			 *   0k        |  0x1A0
			 */
			if (ret <= 0x1A0)
				ret = 0;
			else
				ret = (ret - 0x1A0) * 229;
			break;
		case RETU_MADC_CHGVOLT:
			/*
			 * conversion table measured by hand:
			 * Voltage | ADC
			 * ================
			 * 5.50V   | 322
			 * 5.00V   | 293
			 * 4.50V   | 264
			 * 4.00V   | 235
			 * 3.50V   | 206
			 * 3.00V   | 177
			 * 2.50V   | 147
			 * 2.40V   | 141 (CHG==0)
			 * 2.20V   | 130 (CHG==0)
			 * 2.00V   | 118 (CHG==0)
			 * ==> Vchg = (Achg*100*100/586)mV
			 */
			if (ret <= 0)
				ret = 0;
			else
				ret = (ret*10000)/586;
			break;
		case RETU_MADC_BATVOLT:
			// conversion equation taken from openwrt/omap24xx/linux-3.3/n810bm
			if (ret <= 0x37)
				ret = 2800;
			else
				ret = 2800 + ( (ret - 0x37) * ((4200 - 2800) * 1000) / (0x236 - 0x37) / 1000 );
			break;
		default:
			return -EINVAL;
		}

		*val = ret;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}


static const struct iio_info retu_madc_iio_info = {
	.read_raw = &retu_madc_read_raw,
};

#define RETU_ADC_CHAN_RAW(_chan, _ds_name) { \
        .type = IIO_VOLTAGE, \
        .indexed = 1, \
        .channel = (_chan), \
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
        .datasheet_name = (_ds_name), \
}

#define RETU_ADC_CHAN_PROC(_chan, _ds_name) { \
        .type = IIO_VOLTAGE, \
        .indexed = 1, \
        .channel = (_chan), \
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW) \
		| BIT(IIO_CHAN_INFO_PROCESSED), \
        .datasheet_name = (_ds_name), \
}

static const struct iio_chan_spec retu_madc_iio_channels[] = {
	RETU_ADC_CHAN_RAW(RETU_MADC_GND,	"GND"),
	RETU_ADC_CHAN_PROC(RETU_MADC_BSI,	"BSI"),
	RETU_ADC_CHAN_RAW(RETU_MADC_BATTEMP,	"BATTEMP"),
	RETU_ADC_CHAN_PROC(RETU_MADC_CHGVOLT,	"CHGVOLT"),
	RETU_ADC_CHAN_RAW(RETU_MADC_HEADSET,	"HEADSET"),
	RETU_ADC_CHAN_RAW(RETU_MADC_HOOKDET,	"HOOKDET"),
	RETU_ADC_CHAN_RAW(RETU_MADC_RFGP,	"RFGP"),
	RETU_ADC_CHAN_RAW(RETU_MADC_WBTX,	"WBTX"),
	RETU_ADC_CHAN_PROC(RETU_MADC_BATVOLT,	"BATVOLT"),
	RETU_ADC_CHAN_RAW(RETU_MADC_GND2,	"GND2"),
	RETU_ADC_CHAN_RAW(RETU_MADC_LIGHTSENS,	"LIGHTSENS"),
	RETU_ADC_CHAN_RAW(RETU_MADC_LIGHTTEMP,	"LIGHTTEMP"),
	RETU_ADC_CHAN_RAW(RETU_MADC_BKUPVOLT,	"BKUPVOLT"),
	RETU_ADC_CHAN_RAW(RETU_MADC_TEMP,	"TEMP"),
};

/*
 * Initialize MADC and request for threaded irq
 */
static int retu_madc_probe(struct platform_device *pdev)
{
	struct retu_dev *retu = dev_get_drvdata(pdev->dev.parent);
	struct retu_madc_data *madc;
	struct device_node *np = pdev->dev.of_node;
	int ret;
	int regval;
	struct iio_dev *iio_dev = NULL;

	dev_info(&pdev->dev, "%s\n", __func__);

	if (!np) {
		dev_err(&pdev->dev, "no Device Tree node available\n");
		return -EINVAL;
	}

	iio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*madc));
	if (!iio_dev) {
		dev_err(&pdev->dev, "failed allocating iio device\n");
		return -ENOMEM;
	}

	madc = iio_priv(iio_dev);
	madc->retu = retu;

	regval = retu_read(retu, RETU_REG_ASICR);
        if (regval < 0) {
                dev_err(&pdev->dev, "could not read retu revision: %d\n",
                        regval);
                return regval;
        }
	madc->is_vilma = (ret & RETU_REG_ASICR_VILMA) > 0;

	iio_dev->name = dev_name(&pdev->dev);
	iio_dev->info = &retu_madc_iio_info;
	iio_dev->modes = INDIO_DIRECT_MODE;
	iio_dev->channels = retu_madc_iio_channels;
	iio_dev->num_channels = ARRAY_SIZE(retu_madc_iio_channels);

	platform_set_drvdata(pdev, iio_dev);

	ret = iio_device_register(iio_dev);
	if (ret) {
		dev_err(&pdev->dev, "could not register iio device\n");
		goto err_devreg;
	}

	return 0;

err_devreg:
	return ret;
}

static int retu_madc_remove(struct platform_device *pdev)
{
	struct iio_dev *iio_dev = platform_get_drvdata(pdev);

	iio_device_unregister(iio_dev);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id retu_madc_of_match[] = {
	{ .compatible = "nokia,retu-madc", },
	{ },
};
MODULE_DEVICE_TABLE(of, retu_madc_of_match);
#endif

static struct platform_driver retu_madc_driver = {
	.probe = retu_madc_probe,
	.remove = retu_madc_remove,
	.driver = {
		   .name = "retu_madc",
		   .of_match_table = of_match_ptr(retu_madc_of_match),
	},
};

module_platform_driver(retu_madc_driver);

MODULE_DESCRIPTION("Retu/Vilma MADC driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Peter Vasil <petervasil@gmail.com>");
MODULE_ALIAS("platform:retu_madc");
