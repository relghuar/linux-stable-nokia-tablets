// SPDX-License-Identifier: GPL-2.0+
/*
 * Tahvo LED PWM driver
 *
 * Copyright (C) 2004, 2005 Nokia Corporation
 *
 * Based on original 2.6 kernel driver for Nokia N8x0 LCD panel.
 * Rewritten by Peter Vasil.
 */

#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/mfd/retu.h>

/* Maximum power/brightness value */
#define TAHVO_LEDPWM_MAX 127

struct tahvo_led {
	struct led_classdev cdev;
	struct retu_dev *rdev;
};

static int tahvo_led_brightness_set(struct led_classdev *cdev,
				    enum led_brightness brightness)
{
	struct tahvo_led *led = container_of(cdev, struct tahvo_led, cdev);

	return retu_write(led->rdev, TAHVO_REG_LEDPWM, brightness);
}

static int tahvo_led_probe(struct platform_device *pdev)
{
	struct retu_dev *rdev = dev_get_drvdata(pdev->dev.parent);
	struct tahvo_led *led;
	struct led_init_data init_data;
	int ret;

	led = devm_kzalloc(&pdev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	if (pdev->dev.of_node && pdev->dev.of_node->name) {
		led->cdev.name = pdev->dev.of_node->name;
	} else {
		dev_warn(&pdev->dev, "No OF node found, using default name!\n");
		led->cdev.name = "tahvo:led";
	}
	led->rdev = rdev;
	led->cdev.max_brightness = TAHVO_LEDPWM_MAX;
	led->cdev.brightness_set_blocking = tahvo_led_brightness_set;

	init_data.fwnode = of_fwnode_handle(pdev->dev.of_node);

	ret = devm_led_classdev_register_ext(&pdev->dev, &led->cdev, &init_data);
	if (ret) {
		dev_err(&pdev->dev, "failed to register PWM LED (%d)\n", ret);
		return ret;
	}

	return 0;
}

static const struct of_device_id of_tahvo_leds_match[] = {
	{ .compatible = "nokia,tahvo,ledpwm", },
	{},
};

static struct platform_driver tahvo_led_driver = {
	.probe		= tahvo_led_probe,
	.driver		= {
		.name	= "tahvo-ledpwm",
		.of_match_table = of_match_ptr(of_tahvo_leds_match),
	},
};
module_platform_driver(tahvo_led_driver);

MODULE_ALIAS("platform:tahvo-ledpwm");
MODULE_DESCRIPTION("Tahvo LED PWM");
MODULE_AUTHOR("Peter Vasil <petervasil@gmail.com>");
MODULE_LICENSE("GPL");
