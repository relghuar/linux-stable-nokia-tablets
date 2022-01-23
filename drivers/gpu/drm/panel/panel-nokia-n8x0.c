/*
 * Nokia N8x0 LCD Panel driver
 *   (LS041Y3 panel behind Epson S1D13745 Blizzard framebuffer chip)
 *
 * Copyright (C) 2010 Nokia Corporation
 *
 * Original Driver Author: Imre Deak <imre.deak@nokia.com>
 * Based on panel-generic.c by Tomi Valkeinen <tomi.valkeinen@nokia.com>
 * Adapted to new DSS2 framework: Roger Quadros <roger.quadros@nokia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/clk.h>
#include <video/mipi_display.h>

#include <drm/drm_crtc.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include "../omapdrm/dss/omapdss.h"

#define MIPID_VER_LPH8923		3
#define MIPID_VER_LS041Y3		4


#define BLIZZARD_REV_CODE                      0x00
#define BLIZZARD_CONFIG                        0x02
#define BLIZZARD_PLL_DIV                       0x04
#define BLIZZARD_PLL_LOCK_RANGE                0x06
#define BLIZZARD_PLL_CLOCK_SYNTH_0             0x08
#define BLIZZARD_PLL_CLOCK_SYNTH_1             0x0a
#define BLIZZARD_PLL_MODE                      0x0c
#define BLIZZARD_CLK_SRC                       0x0e
#define BLIZZARD_MEM_BANK0_ACTIVATE            0x10
#define BLIZZARD_MEM_BANK0_STATUS              0x14
#define BLIZZARD_PANEL_CONFIGURATION           0x28
#define BLIZZARD_HDISP                         0x2a
#define BLIZZARD_HNDP                          0x2c
#define BLIZZARD_VDISP0                        0x2e
#define BLIZZARD_VDISP1                        0x30
#define BLIZZARD_VNDP                          0x32
#define BLIZZARD_HSW                           0x34
#define BLIZZARD_VSW                           0x38
#define BLIZZARD_DISPLAY_MODE                  0x68
#define BLIZZARD_INPUT_WIN_X_START_0           0x6c
#define BLIZZARD_DATA_SOURCE_SELECT            0x8e
#define BLIZZARD_DISP_MEM_DATA_PORT            0x90
#define BLIZZARD_DISP_MEM_READ_ADDR0           0x92
#define BLIZZARD_POWER_SAVE                    0xE6
#define BLIZZARD_NDISP_CTRL_STATUS             0xE8

/* Data source select */
/* For S1D13745 */
#define BLIZZARD_SRC_WRITE_LCD_BACKGROUND       0x00
#define BLIZZARD_SRC_WRITE_LCD_DESTRUCTIVE      0x01
#define BLIZZARD_SRC_WRITE_OVERLAY_ENABLE       0x04
#define BLIZZARD_SRC_DISABLE_OVERLAY            0x05
/* For S1D13744 */
#define BLIZZARD_SRC_WRITE_LCD                  0x00

#define BLIZZARD_COLOR_RGB565                   0x01
#define BLIZZARD_COLOR_YUV420                   0x09

#define BLIZZARD_VERSION_S1D13745               0x01    /* Hailstorm */
#define BLIZZARD_VERSION_S1D13744               0x02    /* Blizzard */


struct n8x0_panel {
	struct drm_panel panel;

	struct spi_device *spi;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *powerdown_gpio;

	struct mutex		mutex;

	struct clk *osc_ck;

	struct regulator *vtornado;
	u32 vtornado_on_uV;
	u32 vtornado_off_uV;

	char		*name;
	u8		display_id[3];
	int		model;
	int		revision;

	int		enabled;
	/*
	 * Next value of jiffies when we can issue the next sleep in/out
	 * command.
	 */
	unsigned long	hw_guard_end;
	unsigned long	hw_guard_wait;		/* max guard time in jiffies */

	int blizzard_ver;

	struct device_node *rfbi_node;
	struct omap_dss_device *rfbi;
};

#define to_n8x0_device(p) container_of(p, struct n8x0_panel, panel)

static const struct drm_display_mode n8x0_panel_mode = {
        .clock = 21940,
        .hdisplay = 800,
        .hsync_start = 800 + 28,
        .hsync_end = 800 + 28 + 4,
        .htotal = 800 + 28 + 4 + 24,
        .vdisplay = 480,
        .vsync_start = 480 + 3,
        .vsync_end = 480 + 3 + 3,
        .vtotal = 480 + 3 + 3 + 4,
        .type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
        .flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
        .width_mm = 77,
        .height_mm = 46,
};

static const struct rfbi_timings n8x0_panel_rfbi_timings = {
	.cs_on_time     = 0,

	.we_on_time     = 9000,
	.we_off_time    = 18000,
	.we_cycle_time  = 36000,

	.re_on_time     = 9000,
	.re_off_time    = 27000,
	.re_cycle_time  = 36000,

	.access_time    = 27000,
	.cs_off_time    = 36000,

	.cs_pulse_width = 0,
};

static void n8x0_panel_transfer(struct n8x0_panel *lcd, int cmd,
			      const u8 *wbuf, int wlen, u8 *rbuf, int rlen)
{
	struct spi_message      m;
	struct spi_transfer     *x, xfer[5];
	int                     ret;

	BUG_ON(lcd->spi == NULL);

	spi_message_init(&m);

	memset(xfer, 0, sizeof(xfer));
	x = &xfer[0];

	cmd &=  0xff;
	x->tx_buf = &cmd;
	x->bits_per_word = 9;
	x->len = 2;

	if (rlen > 1 && wlen == 0) {
		/*
		 * Between the command and the response data there is a
		 * dummy clock cycle. Add an extra bit after the command
		 * word to account for this.
		 */
		x->bits_per_word = 10;
		cmd <<= 1;
	}
	spi_message_add_tail(x, &m);

	if (wlen) {
		x++;
		x->tx_buf = wbuf;
		x->len = wlen;
		x->bits_per_word = 9;
		spi_message_add_tail(x, &m);
	}

	if (rlen) {
		x++;
		x->rx_buf       = rbuf;
		x->len          = rlen;
		spi_message_add_tail(x, &m);
	}

	ret = spi_sync(lcd->spi, &m);
	if (ret < 0)
		dev_dbg(&lcd->spi->dev, "spi_sync %d\n", ret);
}

static inline void n8x0_panel_cmd(struct n8x0_panel *lcd, int cmd)
{
//	dev_info(&lcd->spi->dev, "%s(%02x)\n", __func__, cmd);
	n8x0_panel_transfer(lcd, cmd, NULL, 0, NULL, 0);
}

static inline void n8x0_panel_write(struct n8x0_panel *lcd,
			       int reg, const u8 *buf, int len)
{
//	dev_info(&lcd->spi->dev, "%s(%02x, %d, [%02x...])\n", __func__, reg, len, buf[0]);
	n8x0_panel_transfer(lcd, reg, buf, len, NULL, 0);
}

static inline void n8x0_panel_read(struct n8x0_panel *lcd,
			      int reg, u8 *buf, int len)
{
//	int i;
//	dev_info(&lcd->spi->dev, "%s(%02x, %d)\n", __func__, reg, len);
	n8x0_panel_transfer(lcd, reg, NULL, 0, buf, len);
//	for (i=0; i<len; i++)
//		dev_info(&lcd->spi->dev, "%s  [%02x] %02x\n", __func__, i, buf[i]);
}

static void hw_guard_start(struct n8x0_panel *lcd, int guard_msec)
{
	lcd->hw_guard_wait = msecs_to_jiffies(guard_msec);
	lcd->hw_guard_end = jiffies + lcd->hw_guard_wait;
}

static void hw_guard_wait(struct n8x0_panel *lcd)
{
	unsigned long wait = lcd->hw_guard_end - jiffies;

	if ((long)wait > 0 && wait <= lcd->hw_guard_wait) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(wait);
	}
}

static void n8x0_set_sleep_mode(struct n8x0_panel *lcd, int on)
{
	int cmd = on ? MIPI_DCS_ENTER_SLEEP_MODE : MIPI_DCS_EXIT_SLEEP_MODE;

	/*
	 * We have to keep 120msec between sleep in/out commands.
	 * (8.2.15, 8.2.16).
	 */
	hw_guard_wait(lcd);
	n8x0_panel_cmd(lcd, cmd);
	hw_guard_start(lcd, 120);
}

static void n8x0_set_data_lines(struct n8x0_panel *lcd, int data_lines)
{
	u16 par;

	switch (data_lines) {
	case 16:
		par = 0x100 | (MIPI_DCS_PIXEL_FMT_16BIT << 4);
		break;
	case 18:
		par = 0x100 | (MIPI_DCS_PIXEL_FMT_18BIT << 4);
		break;
	case 24:
		par = 0x100 | (MIPI_DCS_PIXEL_FMT_24BIT << 4);
		break;
	}

	n8x0_panel_write(lcd, MIPI_DCS_SET_PIXEL_FORMAT, (u8 *)&par, 2);
}

static void n8x0_send_init_string(struct n8x0_panel *lcd)
{
	u16 initpar[] = { 0x0102, 0x0100, 0x0100 };
	n8x0_panel_write(lcd, 0xc2, (u8 *)initpar, sizeof(initpar));
}


static void n8x0_set_display_state(struct n8x0_panel *lcd, int enabled)
{
	int cmd = enabled ? MIPI_DCS_SET_DISPLAY_ON : MIPI_DCS_SET_DISPLAY_OFF;

	n8x0_panel_cmd(lcd, cmd);
}

static int n8x0_panel_detect(struct n8x0_panel *lcd)
{
	__be32 value;
	u32 status;
	int ret = 0;

	/*
	 * After being taken out of reset the panel needs 5ms before the first
	 * command can be sent.
	 */
	gpiod_set_value(lcd->reset_gpio, 1);
	usleep_range(5000, 10000);

	n8x0_panel_read(lcd, MIPI_DCS_GET_DISPLAY_STATUS, (u8 *)&value, 4);
	status = __be32_to_cpu(value);
	lcd->enabled = (status & (1 << 17)) && (status & (1 << 10));

	dev_info(&lcd->spi->dev,
		"LCD panel %s by bootloader (status 0x%04x)\n",
		lcd->enabled ? "enabled" : "disabled ", status);

	n8x0_panel_read(lcd, MIPI_DCS_GET_DISPLAY_ID, lcd->display_id, 3);
	dev_info(&lcd->spi->dev, "MIPI display ID: %02x%02x%02x\n",
		lcd->display_id[0], lcd->display_id[1], lcd->display_id[2]);

	switch (lcd->display_id[0]) {
	case 0x45:
		lcd->model = MIPID_VER_LPH8923;
		lcd->name = "lph8923";
		break;
	case 0x83:
		lcd->model = MIPID_VER_LS041Y3;
		lcd->name = "ls041y3";
		break;
	default:
		lcd->name = "unknown";
		dev_err(&lcd->spi->dev, "invalid display ID\n");
		ret = -ENODEV;
		goto done;
	}

	lcd->revision = lcd->display_id[1];

	dev_info(&lcd->spi->dev, "omapfb: %s rev %02x LCD detected\n",
			lcd->name, lcd->revision);

done:
	if (!lcd->enabled)
		gpiod_set_value(lcd->reset_gpio, 0);

	return 0;
}

static inline void blizzard_cmd(struct omap_dss_device *dssdev, u8 cmd)
{
	dssdev->rfbi_ops->write_command(dssdev, &cmd, 1);
}

static inline void blizzard_write(struct omap_dss_device *dssdev, u8 cmd, const u8 *buf, int len)
{
	dssdev->rfbi_ops->write_command(dssdev, &cmd, 1);
	dssdev->rfbi_ops->write_data(dssdev, buf, len);
}

static inline void blizzard_read(struct omap_dss_device *dssdev, u8 cmd, u8 *buf, int len)
{
	dssdev->rfbi_ops->write_command(dssdev, &cmd, 1);
	dssdev->rfbi_ops->read_data(dssdev, buf, len);
}

static void blizzard_write_reg(struct omap_dss_device *dssdev, u8 reg, u8 val)
{
	blizzard_write(dssdev, reg, &val, 1);
}

static u8 blizzard_read_reg(struct omap_dss_device *dssdev, u8 cmd)
{
	u8 data;
	blizzard_read(dssdev, cmd, &data, 1);
	return data;
}

static int framebuffer_detect(struct n8x0_panel *lcd)
{
/*
	struct omap_dss_device *dssdev = lcd->in;
	u8 rev, conf;

	if (!(blizzard_read_reg(dssdev,BLIZZARD_PLL_DIV) & 0x80)) {
		dev_err(dssdev->dev, "%s controller not initialized by the bootloader\n", __func__);
	}

	rev = blizzard_read_reg(dssdev, BLIZZARD_REV_CODE);
	conf = blizzard_read_reg(dssdev, BLIZZARD_CONFIG);

	switch (rev & 0xfc) {
	case 0x9c:
		lcd->blizzard_ver = BLIZZARD_VERSION_S1D13744;
		dev_info(dssdev->dev, "s1d13744 LCD controller rev %d "
			"initialized (CNF pins %x)\n", rev & 0x03, conf & 0x07);
		break;
	case 0xa4:
		lcd->blizzard_ver = BLIZZARD_VERSION_S1D13745;
		dev_info(dssdev->dev, "s1d13745 LCD controller rev %d "
			"initialized (CNF pins %x)\n", rev & 0x03, conf & 0x07);
		break;
	default:
		dev_err(dssdev->dev, "invalid s1d1374x revision %02x\n", rev);
		return -ENODEV;
	}
*/
	return 0;
}

static void framebuffer_init(struct n8x0_panel *lcd)
{
/*
	struct omap_dss_device *dssdev = lcd->in;
	u32 l;

	l = blizzard_read_reg(dssdev, BLIZZARD_POWER_SAVE);
	// Standby, Sleep
	l &= ~0x03;
	blizzard_write_reg(dssdev, BLIZZARD_POWER_SAVE, l);
	l = blizzard_read_reg(dssdev, BLIZZARD_PLL_MODE);
	l &= ~0x03;
	// Enable PLL, counter function
	l |= 0x1;
	blizzard_write_reg(dssdev, BLIZZARD_PLL_MODE, l);

	l = 1000;
	while (!(blizzard_read_reg(dssdev, BLIZZARD_PLL_DIV) & (1 << 7))
			&& (l-- > 0))
		msleep(1);
	if (l < 900)
		dev_warn(dssdev->dev, "%s: pll loops left %d\n", __func__, l);

	blizzard_write_reg(dssdev, BLIZZARD_DISPLAY_MODE, 0x01);
*/
}

// TODO: vendor kernel does a lot more to shut the fb chip down, for example
// saving regs and stopping sdram. This would require reverse operations in
// our fb init as well.
// Better solution might be putting it to reset mode (see reset-gpio comment
// in power_down), we'd have to check what the actual consumption is.
static void framebuffer_sleep(struct n8x0_panel *lcd)
{
/*
	struct omap_dss_device *dssdev = lcd->in;
	u32 l;

	dssdev->rfbi_ops->set_data_lines(dssdev, 8);
	dssdev->rfbi_ops->configure(dssdev);

	l = blizzard_read_reg(dssdev, BLIZZARD_POWER_SAVE);
	// Standby, Sleep
	l |= 0x03;
	blizzard_write_reg(dssdev, BLIZZARD_POWER_SAVE, l);

	msleep(100);
*/
}

static void blizzard_ctrl_setup_update(struct n8x0_panel *lcd,
		int x, int y, int w, int h)
{
/*
	u8 tmp[18];
	int x_end, y_end;

	x_end = x + w - 1;
	y_end = y + h - 1;

	tmp[0] = x;
	tmp[1] = x >> 8;
	tmp[2] = y;
	tmp[3] = y >> 8;
	tmp[4] = x_end;
	tmp[5] = x_end >> 8;
	tmp[6] = y_end;
	tmp[7] = y_end >> 8;

	// scaling?
	tmp[8] = x;
	tmp[9] = x >> 8;
	tmp[10] = y;
	tmp[11] = y >> 8;
	tmp[12] = x_end;
	tmp[13] = x_end >> 8;
	tmp[14] = y_end;
	tmp[15] = y_end >> 8;

	tmp[16] = BLIZZARD_COLOR_RGB565;

	if (lcd->blizzard_ver == BLIZZARD_VERSION_S1D13745)
		tmp[17] = BLIZZARD_SRC_WRITE_LCD_BACKGROUND;
	else if (lcd->blizzard_ver == BLIZZARD_VERSION_S1D13744)
		tmp[17] = BLIZZARD_SRC_WRITE_LCD;
	else
		tmp[17] = BLIZZARD_SRC_WRITE_LCD_DESTRUCTIVE;

	dssdev->rfbi_ops->set_data_lines(dssdev, 8);
	dssdev->rfbi_ops->configure(dssdev);

	blizzard_write(dssdev, BLIZZARD_INPUT_WIN_X_START_0, tmp, 18);

	dssdev->rfbi_ops->set_data_lines(dssdev, 16);
	dssdev->rfbi_ops->configure(dssdev);
*/
}

static void n8x0_panel_update_done(void *data)
{
	struct n8x0_panel *lcd = data;
//	struct omap_dss_device *dd = lcd->in;

	dev_info(&lcd->spi->dev, "%s: lcd=%px\n", __func__, lcd);

//	dd->rfbi_ops->bus_unlock(dd);
}

static int n8x0_panel_update(struct n8x0_panel *lcd,
		u16 x, u16 y, u16 w, u16 h)
{
	u16 dw, dh;

	dev_info(&lcd->spi->dev, "%s\n", __func__);

	dw = n8x0_panel_mode.hdisplay;
	dh = n8x0_panel_mode.vdisplay;

	if (x != 0 || y != 0 || w != dw || h != dh) {
		dev_err(&lcd->spi->dev, "invaid update region %d, %d, %d, %d\n",
			x, y, w, h);
		return -EINVAL;
	}

	mutex_lock(&lcd->mutex);
//	dd->rfbi_ops->bus_lock(dd);

	blizzard_ctrl_setup_update(lcd, x, y, w, h);

//	dd->rfbi_ops->update(dd, update_done, lcd);

	mutex_unlock(&lcd->mutex);

	return 0;
}

static int n8x0_panel_power_on(struct n8x0_panel *lcd)
{
	int r;

	dev_dbg(&lcd->spi->dev, "%s en=%d\n", __func__, lcd->enabled);

	regulator_set_voltage(lcd->vtornado, lcd->vtornado_on_uV, lcd->vtornado_on_uV);
	msleep(10);
	clk_enable(lcd->osc_ck);
	msleep(10);
	gpiod_set_value(lcd->powerdown_gpio, 1);
	gpiod_set_value(lcd->reset_gpio, 1);
	msleep(10);
/*
	in->rfbi_ops->set_timings(in, &lcd->videomode);
	in->rfbi_ops->set_rfbi_timings(in, &n8x0_panel_rfbi_timings);
	in->rfbi_ops->set_pixel_size(dssdev, 16);
	in->rfbi_ops->set_data_lines(dssdev, 8);

	r = in->rfbi_ops->enable(in);
	if (r) {
		pr_err("%s rfbi enable failed\n", __func__);
		return r;
	}
	msleep(50);
*/
	if (lcd->enabled) {
		dev_info(&lcd->spi->dev, "panel already enabled - redoing anyway for framebuffer\n");
//		return 0;
	}

	lcd->enabled = 1;

	n8x0_set_sleep_mode(lcd, 0);

	// 5msec between sleep out and the next command. (8.2.16)
	usleep_range(5000, 10000);

	n8x0_send_init_string(lcd);
	n8x0_set_data_lines(lcd, 24);

	n8x0_set_display_state(lcd, 1);

	usleep_range(5000, 10000);

	r = framebuffer_detect(lcd);
	if (r) {
		dev_err(&lcd->spi->dev, "Failed to detect framebuffer!\n");
		goto err_rfbi;
	}

	framebuffer_init(lcd);

	return 0;

err_rfbi:
//	in->rfbi_ops->disable(in);
	return r;
}

static void n8x0_panel_power_off(struct n8x0_panel *lcd)
{
	dev_dbg(&lcd->spi->dev, "%s\n", __func__);

	if (!lcd->enabled)
		return;

	framebuffer_sleep(lcd);

	n8x0_set_display_state(lcd, 0);
	n8x0_set_sleep_mode(lcd, 1);
	lcd->enabled = 0;
	msleep(10);

//	in->rfbi_ops->disable(in);

	// FIXME: we cannot pull down reset apparently without additional
	// initialization in power_on. Even vendor kernel does not do that
	// (actually it does not seem to do anything with this gpio).
//	gpiod_set_value(lcd->reset_gpio, 0);
	gpiod_set_value(lcd->powerdown_gpio, 0);

	clk_disable(lcd->osc_ck);
	regulator_set_voltage(lcd->vtornado, lcd->vtornado_off_uV, lcd->vtornado_off_uV);
}

static int n8x0_panel_prepare(struct drm_panel *panel)
{
	struct n8x0_panel *lcd = to_n8x0_device(panel);

	dev_info(&lcd->spi->dev, "%s\n", __func__);

	return 0;
}

static int n8x0_panel_unprepare(struct drm_panel *panel)
{
	struct n8x0_panel *lcd = to_n8x0_device(panel);

	dev_info(&lcd->spi->dev, "%s\n", __func__);

	return 0;
}

static int n8x0_panel_enable(struct drm_panel *panel)
{
	struct n8x0_panel *lcd = to_n8x0_device(panel);

	dev_info(&lcd->spi->dev, "%s\n", __func__);

	mutex_lock(&lcd->mutex);
	n8x0_panel_power_on(lcd);
	mutex_unlock(&lcd->mutex);

	return 0;
}

static int n8x0_panel_disable(struct drm_panel *panel)
{
	struct n8x0_panel *lcd = to_n8x0_device(panel);

	dev_info(&lcd->spi->dev, "%s\n", __func__);

	mutex_lock(&lcd->mutex);
	n8x0_panel_power_off(lcd);
	mutex_unlock(&lcd->mutex);

	return 0;
}

static int n8x0_panel_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct n8x0_panel *lcd = to_n8x0_device(panel);
        struct drm_display_mode *mode;
	u32 bus_format = MEDIA_BUS_FMT_RGB565_1X16;

	dev_info(&lcd->spi->dev, "%s\n", __func__);

	if (lcd->rfbi_node && !lcd->rfbi) {
		dev_info(&lcd->spi->dev, "%s: rfbi node %pOF\n", __func__, lcd->rfbi_node);
		lcd->rfbi = omapdss_find_device_by_node(lcd->rfbi_node);
		if (!IS_ERR(lcd->rfbi)) {
			dev_info(&lcd->spi->dev, "%s: rfbi=%px\n", __func__, lcd->rfbi);
		}
	}

        mode = drm_mode_duplicate(connector->dev, &n8x0_panel_mode);
        if (!mode)
                return -ENOMEM;

        drm_mode_set_name(mode);
        drm_mode_probed_add(connector, mode);

        connector->display_info.width_mm = n8x0_panel_mode.width_mm;
        connector->display_info.height_mm = n8x0_panel_mode.height_mm;
	drm_display_info_set_bus_formats(&connector->display_info,
					 &bus_format, 1);
        connector->display_info.bus_flags = DRM_BUS_FLAG_DE_HIGH
                                          | DRM_BUS_FLAG_SYNC_SAMPLE_POSEDGE
                                          | DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE;

        return 1;
}

static const struct drm_panel_funcs n8x0_panel_funcs = {
	.prepare = n8x0_panel_prepare,
	.unprepare = n8x0_panel_unprepare,
	.enable = n8x0_panel_enable,
	.disable = n8x0_panel_disable,
	.get_modes = n8x0_panel_get_modes,
};

DEFINE_DRM_GEM_CMA_FOPS(n8x0_dbi_fops);

static struct drm_driver n8x0_dbi_driver = {
        .driver_features        = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
        .fops                   = &n8x0_dbi_fops,
        DRM_GEM_CMA_DRIVER_OPS_VMAP,
        .debugfs_init           = mipi_dbi_debugfs_init,
        .name                   = "n8x0_panel",
        .desc                   = "Nokia N8x0",
        .date                   = "20211231",
        .major                  = 1,
        .minor                  = 0,
};

static int n8x0_panel_probe_of(struct spi_device *spi)
{
	struct n8x0_panel *lcd = dev_get_drvdata(&spi->dev);
	struct device_node *np = spi->dev.of_node;
	int r;

	of_property_read_u32(np, "vtornado-on-microvolt", &lcd->vtornado_on_uV);
	of_property_read_u32(np, "vtornado-off-microvolt", &lcd->vtornado_off_uV);

	r = of_graph_get_endpoint_count(np);
	dev_info(&spi->dev, "%s: %d endpoints found\n", __func__, r);
	if (r == 1) {
		lcd->rfbi_node = of_graph_get_remote_node(np, 0, 0);
		if (!lcd->rfbi_node || IS_ERR(lcd->rfbi_node)) {
			dev_err(&spi->dev, "%s: no remote endpoint found!\n", __func__);
			return -ENODEV;
		}
		dev_info(&spi->dev, "%s: rfbi node %pOF\n", __func__, lcd->rfbi_node);
		lcd->rfbi = omapdss_find_device_by_node(lcd->rfbi_node);
		if (!IS_ERR(lcd->rfbi)) {
			dev_info(&spi->dev, "%s: rfbi=%px\n", __func__, lcd->rfbi);
		}
	} else {
		dev_err(&spi->dev, "%s: exactly one endpoint expected!\n", __func__);
		return -EINVAL;
	}

	lcd->osc_ck = of_clk_get_by_name(np, "osc_ck");
	if (IS_ERR_OR_NULL(lcd->osc_ck)) {
		dev_err(&spi->dev, "failed to find 'osc_ck' clock\n");
		return PTR_ERR(lcd->osc_ck);
	}
	
	return 0;
}

static int n8x0_panel_probe(struct spi_device *spi)
{
	struct n8x0_panel *lcd;
        struct mipi_dbi_dev *dbidev;
        struct mipi_dbi *dbi;
        struct drm_device *drm;
	int r;

	dev_info(&spi->dev, "%s\n", __func__);

	if (!spi->dev.of_node) {
		dev_err(&spi->dev, "OF binding missing!\n");
		return -ENODEV;
	}

	lcd = devm_kzalloc(&spi->dev, sizeof(*lcd), GFP_KERNEL);
	if (lcd == NULL)
		return -ENOMEM;

	spi_set_drvdata(spi, lcd);
	spi->mode = SPI_MODE_0;

	lcd->spi = spi;
	mutex_init(&lcd->mutex);

	r = n8x0_panel_probe_of(spi);
	if (r)
		return r;

	lcd->reset_gpio = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(lcd->reset_gpio)) {
		r = PTR_ERR(lcd->reset_gpio);
		dev_err(&spi->dev, "failed to get reset GPIO (%d)\n", r);
		return r;
	}

	lcd->powerdown_gpio = devm_gpiod_get(&spi->dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(lcd->powerdown_gpio)) {
		r = PTR_ERR(lcd->powerdown_gpio);
		dev_err(&spi->dev, "failed to get powerdown GPIO (%d)\n", r);
		return r;
	}

	lcd->vtornado = devm_regulator_get(&spi->dev, "vtornado");
	if (IS_ERR(lcd->vtornado)) {
		r = PTR_ERR(lcd->vtornado);
		dev_err(&spi->dev, "error acquiring vtornado regulator: %d", r);
		return r;
	}

	/*
	 * After reset we have to wait 5 msec before the first
	 * command can be sent.
	 */
	usleep_range(5000, 10000);

	r = n8x0_panel_detect(lcd);

	if (r) {
		dev_err(&spi->dev, "%s(): panel detect error\n", __func__);
		return r;
	}

	//lcd->videomode = n8x0_panel_timings;

	drm_panel_init(&lcd->panel, &lcd->spi->dev, &n8x0_panel_funcs,
		       DRM_MODE_CONNECTOR_DPI);

	r = drm_panel_of_backlight(&lcd->panel);
	if (r) {
		dev_err(&spi->dev, "%s(): backlight init error\n", __func__);
		return r;
	}

	dbidev = devm_drm_dev_alloc(&spi->dev, &n8x0_dbi_driver,
                                    struct mipi_dbi_dev, drm);
        if (IS_ERR(dbidev))
                return PTR_ERR(dbidev);

        dbi = &dbidev->dbi;
        drm = &dbidev->drm;
        dbi->reset = lcd->reset_gpio;

	drm_mode_config_init(drm);

	drm_panel_add(&lcd->panel);

	dev_info(&spi->dev, "%s: probe successfull\n", __func__);

	return 0;
}

static int n8x0_panel_remove(struct spi_device *spi)
{
	struct n8x0_panel *lcd = dev_get_drvdata(&spi->dev);

	dev_dbg(&lcd->spi->dev, "%s\n", __func__);

	drm_panel_remove(&lcd->panel);

	drm_panel_disable(&lcd->panel);
	drm_panel_unprepare(&lcd->panel);

	return 0;
}

static const struct of_device_id n8x0_panel_of_match[] = {
	{ .compatible = "nokia,n8x0_panel", },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, n8x0_panel_of_match);

static const struct spi_device_id n8x0_panel_ids[] = {
	{ "n8x0_panel", 0 },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(spi, acx565akm_ids);

static struct spi_driver n8x0_panel_driver = {
	.probe		= n8x0_panel_probe,
	.remove		= n8x0_panel_remove,
	.id_table	= n8x0_panel_ids,
	.driver		= {
		.name	= "n8x0_panel",
		.of_match_table = n8x0_panel_of_match,
	},
};

module_spi_driver(n8x0_panel_driver);

MODULE_AUTHOR("Peter Vasil");
MODULE_DESCRIPTION("Nokia N8x0 LCD Driver");
MODULE_LICENSE("GPL");
