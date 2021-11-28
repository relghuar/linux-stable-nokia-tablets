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
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/clk.h>

#include <video/omapfb_dss.h>
#include <video/omap-panel-data.h>

#define MIPID_CMD_READ_DISP_ID		0x04
#define MIPID_CMD_READ_DISP_STATUS	0x09
#define MIPID_CMD_SLEEP_IN		0x10
#define MIPID_CMD_SLEEP_OUT		0x11
#define MIPID_CMD_DISP_OFF		0x28
#define MIPID_CMD_DISP_ON		0x29

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
#define BLIZZARD_SRC_BLT_LCD                    0x06

#define BLIZZARD_COLOR_RGB565                   0x01
#define BLIZZARD_COLOR_YUV420                   0x09

#define BLIZZARD_VERSION_S1D13745               0x01    /* Hailstorm */
#define BLIZZARD_VERSION_S1D13744               0x02    /* Blizzard */


struct panel_drv_data {
	struct omap_dss_device	dssdev;
	struct omap_dss_device *in;

	struct clk *osc_ck;

	int reset_gpio;
	int powerdown_gpio;

	struct regulator *vtornado;
	u32 vtornado_on_uV;
	u32 vtornado_off_uV;

	struct omap_video_timings videomode;

	char		*name;
	int		enabled;
	int		model;
	int		revision;
	u8		display_id[3];
	unsigned long	hw_guard_end;		/* next value of jiffies
						   when we can issue the
						   next sleep in/out command */
	unsigned long	hw_guard_wait;		/* max guard time in jiffies */

	struct spi_device	*spi;
	struct mutex		mutex;

	struct backlight_device *bl_dev;

	int blizzard_ver;
};

static const struct omap_video_timings n8x0_panel_timings = {
	.x_res		= 800,
	.y_res		= 480,
	.pixelclock	= 21940000,
	.hfp		= 28,
	.hsw		= 4,
	.hbp		= 24,
	.vfp		= 3,
	.vsw		= 3,
	.vbp		= 4,

	.interlace	= false,
	.vsync_level	= OMAPDSS_SIG_ACTIVE_HIGH,
	.hsync_level	= OMAPDSS_SIG_ACTIVE_HIGH,

	.data_pclk_edge	= OMAPDSS_DRIVE_SIG_RISING_EDGE,
	.de_level	= OMAPDSS_SIG_ACTIVE_HIGH,
	.sync_pclk_edge	= OMAPDSS_DRIVE_SIG_FALLING_EDGE,
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


#define to_panel_data(p) container_of(p, struct panel_drv_data, dssdev)

#define SPI_VER 4

#if (SPI_VER == 3)

// exact copy from a working 3.10 kernel - NOT working either in qemu nor n810!
static void n8x0_panel_transfer(struct panel_drv_data *ddata, int cmd, const u8 *wbuf,
                int wlen, u8 *rbuf, int rlen)
{
        struct spi_message      m;
        struct spi_transfer     *x, xfer[4];
        u16                     w;
        int                     r;

        spi_message_init(&m);

        memset(xfer, 0, sizeof(xfer));
        x = &xfer[0];

        cmd &=  0xff;
        x->tx_buf               = &cmd;
        x->bits_per_word        = 9;
        x->len                  = 2;
        spi_message_add_tail(x, &m);

        if (wlen) {
                x++;
                x->tx_buf               = wbuf;
                x->len                  = wlen;
                x->bits_per_word        = 9;
                spi_message_add_tail(x, &m);
        }

        if (rlen) {
                x++;
                x->rx_buf       = &w;
                x->len          = 1;
                spi_message_add_tail(x, &m);

                if (rlen > 1) {
                        /* Arrange for the extra clock before the first
                         * data bit.
                         */
                        x->bits_per_word = 9;
                        x->len           = 2;

                        x++;
                        x->rx_buf        = &rbuf[1];
                        x->len           = rlen - 1;
                        spi_message_add_tail(x, &m);
                }
        }

        r = spi_sync(ddata->spi, &m);
        if (r < 0)
                dev_dbg(&ddata->spi->dev, "spi_sync %d\n", r);

        if (rlen)
                rbuf[0] = w & 0xff;
}

#elif (SPI_VER == 4) 

// function from 4.19 kernel, panel acx565akm - works on n810 but not in qemu because of 10bit
static void n8x0_panel_transfer(struct panel_drv_data *ddata, int cmd,
                              const u8 *wbuf, int wlen, u8 *rbuf, int rlen)
{
        struct spi_message      m;
        struct spi_transfer     *x, xfer[5];
        int                     r;

        BUG_ON(ddata->spi == NULL);

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

        r = spi_sync(ddata->spi, &m);
        if (r < 0)
                dev_dbg(&ddata->spi->dev, "spi_sync %d\n", r);
}

#elif (SPI_VER == 99)

// custom version built from 3.10.108 to work in qemu - does NOT work on real n810!
static void n8x0_panel_transfer(struct panel_drv_data *ddata, int cmd,
			      const u8 *wbuf, int wlen, u8 *rbuf, int rlen)
{
	struct spi_message	m;
	struct spi_transfer	*x, xfer[5];
	u16			w;
	int			r;

	BUG_ON(ddata->spi == NULL);
	dev_info(&ddata->spi->dev, "%s(%02x, %d, %d)\n", __func__, cmd, wlen, rlen);

	spi_message_init(&m);

	memset(xfer, 0, sizeof(xfer));
	x = &xfer[0];

	cmd &=  0xff;
	x->tx_buf		= &cmd;
	x->bits_per_word	= 9;
	x->len			= 2;
	spi_message_add_tail(x, &m);

	if (wlen) {
		x++;
		x->tx_buf		= wbuf;
		x->len			= wlen;
		x->bits_per_word	= 9;
		spi_message_add_tail(x, &m);
	}

	if (rlen) {
		// Arrange for the extra clock before the first data bit.
		x++;
		x->rx_buf		= &w;
		x->len			= 2;
		x->bits_per_word	= 9;
		spi_message_add_tail(x, &m);
		// Now read real data
		x++;
		x->rx_buf        = rbuf;
		x->len           = rlen;
		spi_message_add_tail(x, &m);
	}

	r = spi_sync(ddata->spi, &m);
	if (r < 0)
		dev_info(&ddata->spi->dev, "spi_sync %d\n", r);
}

#endif

static inline void n8x0_panel_cmd(struct panel_drv_data *ddata, int cmd)
{
//	dev_info(&ddata->spi->dev, "%s(%02x)\n", __func__, cmd);
	n8x0_panel_transfer(ddata, cmd, NULL, 0, NULL, 0);
}

static inline void n8x0_panel_write(struct panel_drv_data *ddata,
			       int reg, const u8 *buf, int len)
{
//	dev_info(&ddata->spi->dev, "%s(%02x, %d, [%02x...])\n", __func__, reg, len, buf[0]);
	n8x0_panel_transfer(ddata, reg, buf, len, NULL, 0);
}

static inline void n8x0_panel_read(struct panel_drv_data *ddata,
			      int reg, u8 *buf, int len)
{
	int i;
//	dev_info(&ddata->spi->dev, "%s(%02x, %d)\n", __func__, reg, len);
	n8x0_panel_transfer(ddata, reg, NULL, 0, buf, len);
	for (i=0; i<len; i++)
		dev_info(&ddata->spi->dev, "%s  [%02x] %02x\n", __func__, i, buf[i]);
}

static void hw_guard_start(struct panel_drv_data *ddata, int guard_msec)
{
	ddata->hw_guard_wait = msecs_to_jiffies(guard_msec);
	ddata->hw_guard_end = jiffies + ddata->hw_guard_wait;
}

static void hw_guard_wait(struct panel_drv_data *ddata)
{
	unsigned long wait = ddata->hw_guard_end - jiffies;

	if ((long)wait > 0 && wait <= ddata->hw_guard_wait) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(wait);
	}
}

static void set_sleep_mode(struct panel_drv_data *ddata, int on)
{
	int cmd;

	if (on)
		cmd = MIPID_CMD_SLEEP_IN;
	else
		cmd = MIPID_CMD_SLEEP_OUT;
	/*
	 * We have to keep 120msec between sleep in/out commands.
	 * (8.2.15, 8.2.16).
	 */
	hw_guard_wait(ddata);
	n8x0_panel_cmd(ddata, cmd);
	hw_guard_start(ddata, 120);
}

static void set_data_lines(struct panel_drv_data *ddata, int data_lines)
{
        u16 par;

        switch (data_lines) {
        case 16:
                par = 0x150;
                break;
        case 18:
                par = 0x160;
                break;
        case 24:
                par = 0x170;
                break;
        }

        n8x0_panel_write(ddata, 0x3a, (u8 *)&par, 2);
}

static void send_init_string(struct panel_drv_data *ddata)
{
        u16 initpar[] = { 0x0102, 0x0100, 0x0100 };
        n8x0_panel_write(ddata, 0xc2, (u8 *)initpar, sizeof(initpar));
}


static void set_display_state(struct panel_drv_data *ddata, int enabled)
{
	int cmd = enabled ? MIPID_CMD_DISP_ON : MIPID_CMD_DISP_OFF;

	n8x0_panel_cmd(ddata, cmd);
}

static int panel_enabled(struct panel_drv_data *ddata)
{
	u32 disp_status;
	int enabled;

	n8x0_panel_read(ddata, MIPID_CMD_READ_DISP_STATUS,
			(u8 *)&disp_status, 4);
	disp_status = __be32_to_cpu(disp_status);
	enabled = (disp_status & (1 << 17)) && (disp_status & (1 << 10));
	dev_info(&ddata->spi->dev,
		"LCD panel %senabled by bootloader (status 0x%04x)\n",
		enabled ? "" : "not ", disp_status);
	return enabled;
}

static int panel_detect(struct panel_drv_data *ddata)
{
	n8x0_panel_read(ddata, MIPID_CMD_READ_DISP_ID, ddata->display_id, 3);
	dev_info(&ddata->spi->dev, "MIPI display ID: %02x%02x%02x\n",
		ddata->display_id[0],
		ddata->display_id[1],
		ddata->display_id[2]);

	switch (ddata->display_id[0]) {
	case 0x45:
		ddata->model = MIPID_VER_LPH8923;
		ddata->name = "lph8923";
		break;
	case 0x83:
		ddata->model = MIPID_VER_LS041Y3;
		ddata->name = "ls041y3";
		break;
	default:
		ddata->name = "unknown";
		dev_err(&ddata->spi->dev, "invalid display ID\n");
		return -ENODEV;
	}

	ddata->revision = ddata->display_id[1];

	dev_info(&ddata->spi->dev, "omapfb: %s rev %02x LCD detected\n",
			ddata->name, ddata->revision);

	return 0;
}

static inline void blizzard_cmd(struct omap_dss_device *dssdev, u8 cmd)
{
        dssdev->ops.rfbi->write_command(dssdev, &cmd, 1);
}

static inline void blizzard_write(struct omap_dss_device *dssdev, u8 cmd, const u8 *buf, int len)
{
        dssdev->ops.rfbi->write_command(dssdev, &cmd, 1);
        dssdev->ops.rfbi->write_data(dssdev, buf, len);
}

static inline void blizzard_read(struct omap_dss_device *dssdev, u8 cmd, u8 *buf, int len)
{
        dssdev->ops.rfbi->write_command(dssdev, &cmd, 1);
        dssdev->ops.rfbi->read_data(dssdev, buf, len);
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

static int framebuffer_detect(struct panel_drv_data *ddata)
{
	struct omap_dss_device *dssdev = ddata->in;
	u8 rev, conf;

        if (!(blizzard_read_reg(dssdev,BLIZZARD_PLL_DIV) & 0x80)) {
                dev_err(dssdev->dev, "%s controller not initialized by the bootloader\n", __func__);
        }

        rev = blizzard_read_reg(dssdev, BLIZZARD_REV_CODE);
        conf = blizzard_read_reg(dssdev, BLIZZARD_CONFIG);

        switch (rev & 0xfc) {
        case 0x9c:
                ddata->blizzard_ver = BLIZZARD_VERSION_S1D13744;
                dev_info(dssdev->dev, "s1d13744 LCD controller rev %d "
                        "initialized (CNF pins %x)\n", rev & 0x03, conf & 0x07);
                break;
        case 0xa4:
                ddata->blizzard_ver = BLIZZARD_VERSION_S1D13745;
                dev_info(dssdev->dev, "s1d13745 LCD controller rev %d "
                        "initialized (CNF pins %x)\n", rev & 0x03, conf & 0x07);
                break;
        default:
                dev_err(dssdev->dev, "invalid s1d1374x revision %02x\n", rev);
                return -ENODEV;
        }

	return 0;
}

static void framebuffer_init(struct panel_drv_data *ddata)
{
	struct omap_dss_device *dssdev = ddata->in;
	u32 l;

        l = blizzard_read_reg(dssdev, BLIZZARD_POWER_SAVE);
        /* Standby, Sleep */
        l &= ~0x03;
        blizzard_write_reg(dssdev, BLIZZARD_POWER_SAVE, l);
        l = blizzard_read_reg(dssdev, BLIZZARD_PLL_MODE);
        l &= ~0x03;
        /* Enable PLL, counter function */
        l |= 0x1;
        blizzard_write_reg(dssdev, BLIZZARD_PLL_MODE, l);

	l = 1000;
        while (!(blizzard_read_reg(dssdev, BLIZZARD_PLL_DIV) & (1 << 7))
			&& (l-- > 0))
                msleep(1);
	if (l < 900)
		dev_warn(dssdev->dev, "%s: pll loops left %d\n", __func__, l);

	blizzard_write_reg(dssdev, BLIZZARD_DISPLAY_MODE, 0x01);
}

// TODO: vendor kernel does a lot more to shut the fb chip down, for example
// saving regs and stopping sdram. This would require reverse operations in
// our fb init as well.
// Better solution might be putting it to reset mode (see reset-gpio comment
// in power_down), we'd have to check what the actual consumption is.
static void framebuffer_sleep(struct panel_drv_data *ddata)
{
	struct omap_dss_device *dssdev = ddata->in;
	u32 l;

        dssdev->ops.rfbi->set_data_lines(dssdev, 8);
        dssdev->ops.rfbi->configure(dssdev);

        l = blizzard_read_reg(dssdev, BLIZZARD_POWER_SAVE);
        /* Standby, Sleep */
        l |= 0x03;
        blizzard_write_reg(dssdev, BLIZZARD_POWER_SAVE, l);

	msleep(100);
}

static void blizzard_ctrl_setup_update(struct omap_dss_device *dssdev,
                int x, int y, int w, int h)
{
        struct panel_drv_data *ddata = to_panel_data(dssdev);
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

        /* scaling? */
        tmp[8] = x;
        tmp[9] = x >> 8;
        tmp[10] = y;
        tmp[11] = y >> 8;
        tmp[12] = x_end;
        tmp[13] = x_end >> 8;
        tmp[14] = y_end;
        tmp[15] = y_end >> 8;

        tmp[16] = BLIZZARD_COLOR_RGB565;

        if (ddata->blizzard_ver == BLIZZARD_VERSION_S1D13745)
                tmp[17] = BLIZZARD_SRC_WRITE_LCD_BACKGROUND;
        else
                tmp[17] = ddata->blizzard_ver == BLIZZARD_VERSION_S1D13744 ?
                        BLIZZARD_SRC_WRITE_LCD :
                        BLIZZARD_SRC_WRITE_LCD_DESTRUCTIVE;

        dssdev->ops.rfbi->set_data_lines(dssdev, 8);
        dssdev->ops.rfbi->configure(dssdev);

        blizzard_write(dssdev, BLIZZARD_INPUT_WIN_X_START_0, tmp, 18);

        dssdev->ops.rfbi->set_data_lines(dssdev, 16);
        dssdev->ops.rfbi->configure(dssdev);
}


static int n8x0_panel_connect(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	dev_dbg(dssdev->dev, "%s\n", __func__);

	if (omapdss_device_is_connected(dssdev))
		return 0;

	r = in->ops.rfbi->connect(in, dssdev);
	if (r)
		return r;

	return 0;
}

static void n8x0_panel_disconnect(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	dev_dbg(dssdev->dev, "%s\n", __func__);

	if (!omapdss_device_is_connected(dssdev))
		return;

	in->ops.rfbi->disconnect(in, dssdev);
}

static int n8x0_panel_power_on(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;

	dev_dbg(&ddata->spi->dev, "%s ps=%d dl=%d en=%d\n", __func__,
			dssdev->ctrl.pixel_size, dssdev->phy.rfbi.data_lines,
			ddata->enabled);

	regulator_set_voltage(ddata->vtornado, ddata->vtornado_on_uV, ddata->vtornado_on_uV);
	msleep(10);
	clk_enable(ddata->osc_ck);
	msleep(10);
	if (gpio_is_valid(ddata->powerdown_gpio))
		gpio_set_value(ddata->powerdown_gpio, 1);
	if (gpio_is_valid(ddata->reset_gpio))
		gpio_set_value(ddata->reset_gpio, 1);
	msleep(10);

	in->ops.rfbi->set_timings(in, &ddata->videomode);
	in->ops.rfbi->set_rfbi_timings(in, &n8x0_panel_rfbi_timings);
	in->ops.rfbi->set_pixel_size(dssdev, 16);
	in->ops.rfbi->set_data_lines(dssdev, 8);

	r = in->ops.rfbi->enable(in);
	if (r) {
		pr_err("%s rfbi enable failed\n", __func__);
		return r;
	}
	msleep(50);

	if (ddata->enabled) {
		dev_info(&ddata->spi->dev, "panel already enabled - redoing anyway for framebuffer\n");
//		return 0;
	}

	ddata->enabled = 1;

	set_sleep_mode(ddata, 0);

	// 5msec between sleep out and the next command. (8.2.16)
	usleep_range(5000, 10000);

	send_init_string(ddata);
	set_data_lines(ddata, 24);

	set_display_state(ddata, 1);

	usleep_range(5000, 10000);

	r = framebuffer_detect(ddata);
	if (r) {
		dev_err(&ddata->spi->dev, "Failed to detect framebuffer!\n");
		goto err_rfbi;
	}

	framebuffer_init(ddata);

	r = backlight_enable(ddata->bl_dev);
	if (r) {
		dev_err(&ddata->spi->dev, "Failed to enable backlight!\n");
		goto err_rfbi;
	}

	return 0;

err_rfbi:
	in->ops.rfbi->disable(in);
	return r;
}

static void n8x0_panel_power_off(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	dev_dbg(dssdev->dev, "%s\n", __func__);

	if (!ddata->enabled)
		return;

	backlight_disable(ddata->bl_dev);

	framebuffer_sleep(ddata);

	set_display_state(ddata, 0);
	set_sleep_mode(ddata, 1);
	ddata->enabled = 0;
	msleep(10);

	in->ops.rfbi->disable(in);

	// FIXME: we cannot pull down reset apparently without additional
	// initialization in power_on. Even vendor kernel does not do that
	// (actually it does not seem to do anything with this gpio).
//	if (gpio_is_valid(ddata->reset_gpio))
//		gpio_set_value(ddata->reset_gpio, 0);
	if (gpio_is_valid(ddata->powerdown_gpio))
		gpio_set_value(ddata->powerdown_gpio, 0);

	clk_disable(ddata->osc_ck);
	regulator_set_voltage(ddata->vtornado, ddata->vtornado_off_uV, ddata->vtornado_off_uV);
}

static int n8x0_panel_enable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	int r;

	if (!omapdss_device_is_connected(dssdev))
		return -ENODEV;

	if (omapdss_device_is_enabled(dssdev))
		return 0;

	mutex_lock(&ddata->mutex);
	ddata->in->ops.rfbi->bus_lock(ddata->in);

	r = n8x0_panel_power_on(dssdev);
	if (r)
		goto out;

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

out:
	ddata->in->ops.rfbi->bus_unlock(ddata->in);
	mutex_unlock(&ddata->mutex);
	return r;
}

static void n8x0_panel_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	if (!omapdss_device_is_enabled(dssdev))
		return;

	mutex_lock(&ddata->mutex);
	ddata->in->ops.rfbi->bus_lock(ddata->in);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
	n8x0_panel_power_off(dssdev);

	ddata->in->ops.rfbi->bus_unlock(ddata->in);
	mutex_unlock(&ddata->mutex);

}

static void n8x0_panel_set_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	ddata->videomode = *timings;
	dssdev->panel.timings = *timings;

	in->ops.rfbi->set_timings(in, timings);
}

static void n8x0_panel_get_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	*timings = ddata->videomode;
}

static int n8x0_panel_check_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	return in->ops.rfbi->check_timings(in, timings);
}

static void update_done(void *data)
{
	struct panel_drv_data *ddata = data;
	struct omap_dss_device *dd = ddata->in;

//	dev_info(dd->dev, "%s: ddata=%px dd=%px\n", __func__, ddata, dd);

	dd->ops.rfbi->bus_unlock(dd);
}

static int n8x0_panel_update(struct omap_dss_device *dssdev,
                u16 x, u16 y, u16 w, u16 h)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *dd = ddata->in;
        u16 dw, dh;

        dw = dssdev->panel.timings.x_res;
        dh = dssdev->panel.timings.y_res;

        if (x != 0 || y != 0 || w != dw || h != dh) {
                dev_err(dssdev->dev, "invaid update region %d, %d, %d, %d\n",
                        x, y, w, h);
                return -EINVAL;
        }

        mutex_lock(&ddata->mutex);
	dd->ops.rfbi->bus_lock(dd);

        blizzard_ctrl_setup_update(dd, x, y, w, h);

	dd->ops.rfbi->update(dd, update_done, ddata);

        mutex_unlock(&ddata->mutex);

        return 0;
}

static int n8x0_panel_sync(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *dd = ddata->in;

        dev_dbg(dd->dev, "%s: dssdev=%px dd=%px\n", __func__, dssdev, dd);

        mutex_lock(&ddata->mutex);
	dd->ops.rfbi->bus_lock(dd);
	dd->ops.rfbi->bus_unlock(dd);
        mutex_unlock(&ddata->mutex);

        return 0;
}

static struct omap_dss_driver n8x0_panel_ops = {
	.connect	= n8x0_panel_connect,
	.disconnect	= n8x0_panel_disconnect,

	.enable		= n8x0_panel_enable,
	.disable	= n8x0_panel_disable,

	.set_timings	= n8x0_panel_set_timings,
	.get_timings	= n8x0_panel_get_timings,
	.check_timings	= n8x0_panel_check_timings,

	.get_resolution	= omapdss_default_get_resolution,

        .update         = n8x0_panel_update,
        .sync           = n8x0_panel_sync,
};

static int n8x0_panel_probe_of(struct spi_device *spi)
{
	struct panel_drv_data *ddata = dev_get_drvdata(&spi->dev);
	struct device_node *np = spi->dev.of_node;

	ddata->reset_gpio = of_get_named_gpio(np, "reset-gpios", 0);
	ddata->powerdown_gpio = of_get_named_gpio(np, "powerdown-gpio", 0);

	ddata->vtornado = devm_regulator_get(&spi->dev, "vtornado");
	if (IS_ERR(ddata->vtornado)) {
		int error = PTR_ERR(ddata->vtornado);
		dev_err(&spi->dev, "error acquiring vtornado regulator: %d", error);
		return error;
	}

	of_property_read_u32(np, "vtornado-on-microvolt", &ddata->vtornado_on_uV);
	of_property_read_u32(np, "vtornado-off-microvolt", &ddata->vtornado_off_uV);

	ddata->in = omapdss_of_find_source_for_first_ep(np);
	if (IS_ERR(ddata->in)) {
		dev_err(&spi->dev, "failed to find video source\n");
		return PTR_ERR(ddata->in);
	}
	dev_dbg(&spi->dev, "%s: found dss source %px\n", __func__, ddata->in);

	ddata->osc_ck = of_clk_get_by_name(np, "osc_ck");
        if (IS_ERR_OR_NULL(ddata->osc_ck)) {
		dev_err(&spi->dev, "failed to find 'osc_ck' clock\n");
                return PTR_ERR(ddata->osc_ck);
	}
	
	return 0;
}

static int n8x0_panel_probe(struct spi_device *spi)
{
	struct panel_drv_data *ddata;
	struct omap_dss_device *dssdev;
	int r;

	dev_info(&spi->dev, "%s\n", __func__);

	spi->mode = SPI_MODE_0;

	ddata = devm_kzalloc(&spi->dev, sizeof(*ddata), GFP_KERNEL);
	if (ddata == NULL)
		return -ENOMEM;

	dev_set_drvdata(&spi->dev, ddata);

	ddata->spi = spi;

	mutex_init(&ddata->mutex);

	if (spi->dev.of_node) {
		r = n8x0_panel_probe_of(spi);
		if (r)
			return r;
	} else {
		dev_err(&spi->dev, "OF binding missing!\n");
		return -ENODEV;
	}

	if (gpio_is_valid(ddata->reset_gpio)) {
		r = devm_gpio_request_one(&spi->dev, ddata->reset_gpio,
				GPIOF_OUT_INIT_HIGH, "lcd reset");
		if (r)
			goto err_gpio;
	}

	if (gpio_is_valid(ddata->powerdown_gpio)) {
		r = devm_gpio_request_one(&spi->dev, ddata->powerdown_gpio,
				GPIOF_OUT_INIT_HIGH, "lcd powerdown");
		if (r)
			goto err_gpio;
	}

	/*
	 * After reset we have to wait 5 msec before the first
	 * command can be sent.
	 */
	usleep_range(5000, 10000);

	ddata->enabled = panel_enabled(ddata);

	r = panel_detect(ddata);

	if (!ddata->enabled && gpio_is_valid(ddata->reset_gpio))
		gpio_set_value(ddata->reset_gpio, 0);

	if (r) {
		dev_err(&spi->dev, "%s panel detect error\n", __func__);
		goto err_detect;
	}

	ddata->bl_dev = devm_of_find_backlight(&ddata->spi->dev);
	if (IS_ERR(ddata->bl_dev))
		return PTR_ERR(ddata->bl_dev);
	dev_info(&spi->dev, "%s: found backlight %px\n", __func__, ddata->bl_dev);

	ddata->videomode = n8x0_panel_timings;

	dssdev = &ddata->dssdev;
	dssdev->dev = &spi->dev;
	dssdev->driver = &n8x0_panel_ops;
	dssdev->type = OMAP_DISPLAY_TYPE_DBI;
	dssdev->caps = OMAP_DSS_DISPLAY_CAP_MANUAL_UPDATE;
	dssdev->owner = THIS_MODULE;
	dssdev->panel.timings = ddata->videomode;

	r = omapdss_register_display(dssdev);
	if (r) {
		dev_err(&spi->dev, "Failed to register panel\n");
		goto err_reg;
	}

	return 0;

err_reg:
err_detect:
err_gpio:
	omap_dss_put_device(ddata->in);
	return r;
}

static int n8x0_panel_remove(struct spi_device *spi)
{
	struct panel_drv_data *ddata = dev_get_drvdata(&spi->dev);
	struct omap_dss_device *dssdev = &ddata->dssdev;
	struct omap_dss_device *in = ddata->in;

	dev_dbg(&ddata->spi->dev, "%s\n", __func__);

	omapdss_unregister_display(dssdev);

	n8x0_panel_disable(dssdev);
	n8x0_panel_disconnect(dssdev);

	omap_dss_put_device(in);

	return 0;
}

static const struct of_device_id n8x0_panel_of_match[] = {
	{ .compatible = "omapdss,nokia,n8x0_panel", },
	{},
};
MODULE_DEVICE_TABLE(of, n8x0_panel_of_match);

static struct spi_driver n8x0_panel_driver = {
	.driver = {
		.name	= "n8x0_panel",
		.of_match_table = n8x0_panel_of_match,
	},
	.probe	= n8x0_panel_probe,
	.remove	= n8x0_panel_remove,
};

module_spi_driver(n8x0_panel_driver);

MODULE_AUTHOR("Peter Vasil");
MODULE_DESCRIPTION("Nokia N8x0 LCD Driver");
MODULE_LICENSE("GPL");
