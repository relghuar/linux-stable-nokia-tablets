/*
 * linux/arch/arm/mach-omap2/board-n8x0-video.c
 *
 * Copyright (C) 2010 Nokia
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/spi/spi.h>
#include <linux/mm.h>
#include <linux/mfd/retu.h>
#include <asm/mach-types.h>
#include <video/omapdss.h>
#include <video/omap-panel-data.h>

#include <linux/platform_data/spi-omap2-mcspi.h>

#include "soc.h"

#include "mux.h"

#define N8X0_LCD_RESET_GPIO	30
#define N8X0_POWERDOWN_GPIO	15

#if defined(CONFIG_FB_OMAP2) || defined(CONFIG_FB_OMAP2_MODULE)

spinlock_t tahvo_lock;

#if defined(CONFIG_MFD_RETU)

static int tahvo_read_reg(unsigned reg) {
	return retu_read(tahvo_get_dev(), reg);
}

static void tahvo_write_reg(unsigned reg, u16 val) {
	retu_write(tahvo_get_dev(), reg, val);
}

static void tahvo_set_clear_reg_bits(unsigned reg, u16 set, u16 clear) {
	unsigned long flags;
	u16 w;

	spin_lock_irqsave(&tahvo_lock, flags);
	w = tahvo_read_reg(reg);
	w &= ~clear;
	w |= set;
	tahvo_write_reg(reg, w);
	spin_unlock_irqrestore(&tahvo_lock, flags);
}

#else
static int tahvo_read_reg(unsigned reg) {
	return 0;
}
static void tahvo_write_reg(unsigned reg, u16 val) {
}
static void tahvo_set_clear_reg_bits(unsigned reg, u16 set, u16 clear) {
}
#endif

// Epson Blizzard LCD Controller

static unsigned long blizzard_get_clock_rate(struct device *dev);

static struct {
	struct clk *sys_ck;
} blizzard;

static int blizzard_get_clocks(void)
{
	blizzard.sys_ck = clk_get(0, "osc_ck");
	if (IS_ERR(blizzard.sys_ck)) {
		printk(KERN_ERR "can't get Blizzard clock\n");
		return PTR_ERR(blizzard.sys_ck);
	} else {
		clk_enable(blizzard.sys_ck);
		printk(KERN_INFO " Blizzard clock obtained, rate=%lu\n", blizzard_get_clock_rate(NULL));
	}
	return 0;
}

static unsigned long blizzard_get_clock_rate(struct device *dev)
{
	return clk_get_rate(blizzard.sys_ck);
}

static void blizzard_enable_clocks(int enable)
{
	if (enable)
		clk_enable(blizzard.sys_ck);
	else
		clk_disable(blizzard.sys_ck);
}

static int blizzard_power_up(struct omap_dss_device *dssdev)
{
	dev_info(&dssdev->dev, "%s\n", __func__);

	/* Vcore to 1.475V */
	tahvo_set_clear_reg_bits(0x07, 0, 0xf);
	tahvo_write_reg(0x05, 0x7f);
	msleep(10);

	blizzard_enable_clocks(1);
	return 0;
}

static void blizzard_power_down(struct omap_dss_device *dssdev)
{
	dev_info(&dssdev->dev, "%s\n", __func__);

	blizzard_enable_clocks(0);

	/* Vcore to 1.005V */
	tahvo_set_clear_reg_bits(0x07, 0xf, 0);
	tahvo_write_reg(0x05, 0x00);
}

static struct panel_n8x0_data lcd_data = {
	.panel_reset		= N8X0_LCD_RESET_GPIO,
	.ctrl_pwrdown		= N8X0_POWERDOWN_GPIO,
	.platform_enable	= blizzard_power_up,
	.platform_disable	= blizzard_power_down,
};

static struct omap_dss_device n8x0_lcd_device = {
	.name			= "lcd",
	.driver_name		= "n8x0_panel",
	.type			= OMAP_DISPLAY_TYPE_DBI,
	.phy.rfbi.data_lines	= 8,
	.phy.rfbi.channel	= 0,
	.ctrl.pixel_size	= 16,
	.reset_gpio		= N8X0_LCD_RESET_GPIO,
	.data			= &lcd_data,
	.channel		= OMAP_DSS_CHANNEL_LCD,
};

static struct omap_dss_device *n8x0_dss_devices[] = {
	&n8x0_lcd_device,
};

static struct omap_dss_board_info n8x0_dss_board_info = {
	.num_devices	= ARRAY_SIZE(n8x0_dss_devices),
	.devices	= n8x0_dss_devices,
	.default_device	= &n8x0_lcd_device,
};

static int __init n8x0_video_init(void)
{
	//int r;
	if (!machine_is_nokia_n810())
		return 0;

	spin_lock_init(&tahvo_lock);

	gpio_set_value(N8X0_LCD_RESET_GPIO, 1);
	if (omap_mux_init_signal("gpmc_nbe1.gpio_30", OMAP_PIN_OUTPUT)) {
		pr_err("%s cannot configure MUX for LCD RESET\n", __func__);
	}

	gpio_set_value(N8X0_POWERDOWN_GPIO, 1);
	if (omap_mux_init_signal("vlynq_rx0.gpio_15", OMAP_PIN_OUTPUT)) {
		pr_err("%s cannot configure MUX for LCD POWER\n", __func__);
	}

	blizzard_get_clocks();

	omap_display_init(&n8x0_dss_board_info);
	pr_info("%s display initialized\n", __func__);

	return 0;
}

omap_subsys_initcall(n8x0_video_init);
#endif /* defined(CONFIG_FB_OMAP2) || defined(CONFIG_FB_OMAP2_MODULE) */
