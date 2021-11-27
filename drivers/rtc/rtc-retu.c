// SPDX-License-Identifier: GPL-2.0+
// Copyright 2015 IBM Corp.

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/io.h>
#include <linux/mfd/retu.h>
#include <linux/timekeeping.h>

struct retu_rtc {
	struct rtc_device *rtc_dev;
	struct retu_dev *rdev;
	struct mutex mutex;
	u16 alarm_expired;
	int irq_rtcs;
	int irq_rtca;
};

#define RTC_TIME	0x00
#define RTC_YEAR	0x04
#define RTC_CTRL	0x10

#define RTC_UNLOCK	BIT(1)
#define RTC_ENABLE	BIT(0)

static void retu_rtc_do_reset(struct retu_rtc *rtc)
{
	u16 ccr1;

	mutex_lock(&rtc->mutex);

	/* If the calibration register is zero, we've probably lost power */
	/* If not, there should be no reason to reset */
	ccr1 = retu_read(rtc->rdev, RETU_REG_RTCCALR);
	dev_info(&rtc->rtc_dev->dev, "%s: rtccal=%04x\n", __func__, ccr1);
	if (ccr1 & 0x00ff) {
		mutex_unlock(&rtc->mutex);
		return;
	}

	dev_info(&rtc->rtc_dev->dev, "%s: resetting rtc\n", __func__);

	ccr1 = retu_read(rtc->rdev, RETU_REG_CC1);
	/* RTC in reset */
	retu_write(rtc->rdev, RETU_REG_CC1, ccr1 | 0x0001);
	/* RTC in normal operating mode */
	retu_write(rtc->rdev, RETU_REG_CC1, ccr1 & ~0x0001);

	/* Disable alarm and RTC WD */
	retu_write(rtc->rdev, RETU_REG_RTCHMAR, 0x7f3f);
	/* Set Calibration register to default value */
	retu_write(rtc->rdev, RETU_REG_RTCCALR, 0x00c0);

	rtc->alarm_expired = 0;

	mutex_unlock(&rtc->mutex);
}

static int retu_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct retu_rtc *rtc = dev_get_drvdata(dev);
	u16 dsr;
	u16 hmr;
	u16 dsr2;

	dev_info(dev, "%s\n", __func__);

	mutex_lock(&rtc->mutex);

	/*
	 * This read code has been taken over from original 2.6.21 vendor kernel.
	 * It looks awful, but the comments suggest RTC in the chip is awful itself.
	 */

	do {
		u16 dummy;

		/*
		 * Not being in_interrupt() for a retu rtc IRQ, we need to
		 * read twice for consistency..
		 */
		dummy   = retu_read(rtc->rdev, RETU_REG_RTCDSR);
		dsr     = retu_read(rtc->rdev, RETU_REG_RTCDSR);

		dummy   = retu_read(rtc->rdev, RETU_REG_RTCHMR);
		hmr     = retu_read(rtc->rdev, RETU_REG_RTCHMR);

		dummy   = retu_read(rtc->rdev, RETU_REG_RTCDSR);
		dsr2    = retu_read(rtc->rdev, RETU_REG_RTCDSR);
	} while ((dsr != dsr2));

	/*
	 * DSR holds days and seconds
	 * HMR hols hours and minutes
	 *
	 * both are 16 bit registers with 8-bit for each field.
	 * Conversion code was also taken from vendor kernel, including bitmasks.
	 */

	rtc_time64_to_tm(ktime_get_real_seconds(), tm);
	tm->tm_yday     = 0;
	tm->tm_wday     = 0;
	tm->tm_sec      = dsr & 0x3f;
	tm->tm_min      = hmr & 0x3f;
	tm->tm_hour     = (hmr >> 8) & 0x1f;
	tm->tm_mday     = (dsr >> 8) & 0xff;

	dev_info(dev, "%s: dsr=%04x hmr=%04x %d-%02d-%02d %02d:%02d:%02d\n",
			__func__, dsr, hmr, tm->tm_year, tm->tm_mon,
			tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);

	mutex_unlock(&rtc->mutex);

	return 0;
}

static int retu_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct retu_rtc *rtc = dev_get_drvdata(dev);
	u16 dsr, dsrr;
	u16 hmr;

	dsr = ((tm->tm_mday & 0xff) << 8) | (tm->tm_sec & 0xff);
	hmr = ((tm->tm_hour & 0xff) << 8) | (tm->tm_min & 0xff);

	mutex_lock(&rtc->mutex);

	/*
	 * Comment from original vendor kernel:
	 * Writing anything to the day counter forces it to 0
	 * The seconds counter would be cleared by resetting the minutes counter.
	 * Reset day counter, but keep Temperature Shutdown state
	 */
	dsrr = retu_read(rtc->rdev, RETU_REG_RTCDSR);
	dsrr &= 1 << 6;

	dev_info(dev, "%s: dsr=%04x dsrr=%04x hmr=%04x %d.%02d:%02d:%02d\n", __func__,
			dsr, dsrr, hmr, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);

	retu_write(rtc->rdev, RETU_REG_RTCDSR, dsrr);
	retu_write(rtc->rdev, RETU_REG_RTCHMR, hmr);

	mutex_unlock(&rtc->mutex);

	return 0;
}

static int retu_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alm)
{
	struct retu_rtc         *rtc = dev_get_drvdata(dev);
	u16                     chmar;

	mutex_lock(&rtc->mutex);

	chmar = ((alm->time.tm_hour & 0x1f) << 8) | (alm->time.tm_min & 0x3f);
	retu_write(rtc->rdev, RETU_REG_RTCHMAR, chmar);

	mutex_unlock(&rtc->mutex);

	return 0;
}

static int retu_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alm)
{
	struct retu_rtc         *rtc = dev_get_drvdata(dev);
	u16                     chmar;

	mutex_lock(&rtc->mutex);

	chmar = retu_read(rtc->rdev, RETU_REG_RTCHMAR);

	alm->time.tm_hour       = (chmar >> 8) & 0x1f;
	alm->time.tm_min        = chmar & 0x3f;
	alm->enabled            = !!rtc->alarm_expired;

	mutex_unlock(&rtc->mutex);

	return 0;
}

static const struct rtc_class_ops retu_rtc_ops = {
	.read_time = retu_rtc_read_time,
	.set_time = retu_rtc_set_time,
	.read_alarm = retu_rtc_read_alarm,
	.set_alarm = retu_rtc_set_alarm,
};

static int retu_rtc_probe(struct platform_device *pdev)
{
	struct retu_dev *rdev = dev_get_drvdata(pdev->dev.parent);
	struct retu_rtc *rtc;

	dev_info(&pdev->dev, "%s\n", __func__);

	rtc = devm_kzalloc(&pdev->dev, sizeof(*rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	rtc->rdev = rdev;

	rtc->rtc_dev = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc->rtc_dev))
		return PTR_ERR(rtc->rtc_dev);

	platform_set_drvdata(pdev, rtc);
	mutex_init(&rtc->mutex);

	rtc->alarm_expired = retu_read(rtc->rdev, RETU_REG_IDR) &
			(0x1 << RETU_INT_RTCA);

	retu_rtc_do_reset(rtc);

	rtc->rtc_dev->ops = &retu_rtc_ops;
	rtc->rtc_dev->range_min = RTC_TIMESTAMP_BEGIN_1900;
	rtc->rtc_dev->range_max = 38814989399LL; /* 3199-12-31 23:59:59 */

	dev_info(&pdev->dev, "%s: registering rtc device, aexp=%d\n", __func__, rtc->alarm_expired);

	return devm_rtc_register_device(rtc->rtc_dev);
}

static const struct of_device_id retu_rtc_match[] = {
	{ .compatible = "nokia,retu,rtc", },
	{}
};

static struct platform_driver retu_rtc_driver = {
	.probe = retu_rtc_probe,
	.driver = {
		.name = "retu-rtc",
		.of_match_table = of_match_ptr(retu_rtc_match),
	},
};

module_platform_driver(retu_rtc_driver);

MODULE_DESCRIPTION("Retu RTC driver");
MODULE_AUTHOR("Peter Vasil <petervasil@gmail.com>");
MODULE_LICENSE("GPL");
