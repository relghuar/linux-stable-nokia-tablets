/*
 * linux/drivers/video/omap2/dss/rfbi.c
 *
 * Copyright (C) 2009 Nokia Corporation
 * Author: Tomi Valkeinen <tomi.valkeinen@nokia.com>
 *
 * Some code and ideas taken from drivers/video/omap/ driver
 * by Imre Deak.
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

#define DSS_SUBSYS_NAME "RFBI"

#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/vmalloc.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/seq_file.h>
#include <linux/semaphore.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/component.h>

#include <drm/drm_bridge.h>

#include "omapdss.h"
#include "dss.h"

#define RFBI_REG(idx)		( idx )

#define RFBI_REVISION		RFBI_REG(0x0000)
#define RFBI_SYSCONFIG		RFBI_REG(0x0010)
#define RFBI_SYSSTATUS		RFBI_REG(0x0014)
#define RFBI_CONTROL		RFBI_REG(0x0040)
#define RFBI_PIXEL_CNT		RFBI_REG(0x0044)
#define RFBI_LINE_NUMBER	RFBI_REG(0x0048)
#define RFBI_CMD		RFBI_REG(0x004c)
#define RFBI_PARAM		RFBI_REG(0x0050)
#define RFBI_DATA		RFBI_REG(0x0054)
#define RFBI_READ		RFBI_REG(0x0058)
#define RFBI_STATUS		RFBI_REG(0x005c)

#define RFBI_CONFIG(n)		RFBI_REG(0x0060 + (n)*0x18)
#define RFBI_ONOFF_TIME(n)	RFBI_REG(0x0064 + (n)*0x18)
#define RFBI_CYCLE_TIME(n)	RFBI_REG(0x0068 + (n)*0x18)
#define RFBI_DATA_CYCLE1(n)	RFBI_REG(0x006c + (n)*0x18)
#define RFBI_DATA_CYCLE2(n)	RFBI_REG(0x0070 + (n)*0x18)
#define RFBI_DATA_CYCLE3(n)	RFBI_REG(0x0074 + (n)*0x18)

#define RFBI_VSYNC_WIDTH	RFBI_REG(0x0090)
#define RFBI_HSYNC_WIDTH	RFBI_REG(0x0094)

#define REG_FLD_MOD(rfbi, idx, val, start, end) \
	rfbi_write_reg(rfbi, idx, FLD_MOD(rfbi_read_reg(rfbi, idx), val, start, end))

enum omap_rfbi_cycleformat {
	OMAP_DSS_RFBI_CYCLEFORMAT_1_1 = 0,
	OMAP_DSS_RFBI_CYCLEFORMAT_2_1 = 1,
	OMAP_DSS_RFBI_CYCLEFORMAT_3_1 = 2,
	OMAP_DSS_RFBI_CYCLEFORMAT_3_2 = 3,
};

enum omap_rfbi_datatype {
	OMAP_DSS_RFBI_DATATYPE_12 = 0,
	OMAP_DSS_RFBI_DATATYPE_16 = 1,
	OMAP_DSS_RFBI_DATATYPE_18 = 2,
	OMAP_DSS_RFBI_DATATYPE_24 = 3,
};

enum omap_rfbi_parallelmode {
	OMAP_DSS_RFBI_PARALLELMODE_8 = 0,
	OMAP_DSS_RFBI_PARALLELMODE_9 = 1,
	OMAP_DSS_RFBI_PARALLELMODE_12 = 2,
	OMAP_DSS_RFBI_PARALLELMODE_16 = 3,
};

struct rfbi_data {
	struct device *dev;
	void __iomem	*base;

	bool is_enabled;
	struct clk *dss_clk;
	struct dss_device *dss;

	int debug_read;
	int debug_write;
	struct {
		struct dss_debugfs_entry *irqs;
		struct dss_debugfs_entry *regs;
		struct dss_debugfs_entry *clks;
	} debugfs;

	unsigned long	l4_khz;

	enum omap_rfbi_datatype datatype;
	enum omap_rfbi_parallelmode parallelmode;

	int te_enabled;

	void (*framedone_callback)(void *data);
	void *framedone_callback_data;

	struct mutex lock;
	struct semaphore bus_lock;

	struct dss_lcd_mgr_config mgr_config;
	struct videomode vm;

	int pixel_size;
	int data_lines;
	struct rfbi_timings intf_timings;

	struct omap_dss_device output;
	struct drm_bridge bridge;
};

#define drm_bridge_to_rfbi(bridge) \
	container_of(bridge, struct rfbi_data, bridge)

static inline struct rfbi_data *to_rfbi_data(struct omap_dss_device *dssdev)
{
	return dev_get_drvdata(dssdev->dev);
}

static int rfbi_convert_timings(struct rfbi_data *rfbi, struct rfbi_timings *t);
static void rfbi_get_clk_info(struct rfbi_data *rfbi, u32 *clk_period, u32 *max_clk_div);

static inline void rfbi_write_reg(struct rfbi_data *rfbi, const u16 idx, u32 val)
{
	__raw_writel(val, rfbi->base + idx);
}

static inline u32 rfbi_read_reg(struct rfbi_data *rfbi, const u16 idx)
{
	return __raw_readl(rfbi->base + idx);
}

static int rfbi_runtime_get(struct rfbi_data *rfbi)
{
	int r;

	DSSDBG("%s\n", __func__);

	r = pm_runtime_get_sync(rfbi->dev);
	DSSDBG("%s: pm_runtime_get_sync() = %d\n", __func__, r);
	if (WARN_ON(r < 0)) {
		pm_runtime_put_noidle(rfbi->dev);
		return r;
	}
	return 0;
}

static void rfbi_runtime_put(struct rfbi_data *rfbi)
{
	int r;

	DSSDBG("%s\n", __func__);

	r = pm_runtime_put_sync(rfbi->dev);
	WARN_ON(r < 0 && r != -ENOSYS);
}

static void rfbi_bus_lock(struct rfbi_data *rfbi)
{
	DSSDBG("%s\n", __func__);
	down(&rfbi->bus_lock);
}

static void omapdss_rfbi_bus_lock(struct omap_dss_device *dssdev)
{
	rfbi_bus_lock(to_rfbi_data(dssdev));
}

static void rfbi_bus_unlock(struct rfbi_data *rfbi)
{
	DSSDBG("%s\n", __func__);
	up(&rfbi->bus_lock);
}

static void omapdss_rfbi_bus_unlock(struct omap_dss_device *dssdev)
{
	rfbi_bus_unlock(to_rfbi_data(dssdev));
}

static void rfbi_write_command(struct omap_dss_device *dssdev, const void *buf, u32 len)
{
	struct rfbi_data *rfbi = to_rfbi_data(dssdev);

	DSSDBG("%s\n", __func__);

	switch (rfbi->parallelmode) {
	case OMAP_DSS_RFBI_PARALLELMODE_8:
	{
		const u8 *b = buf;
		for (; len; len--)
			rfbi_write_reg(rfbi, RFBI_CMD, *b++);
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_16:
	{
		const u16 *w = buf;
		BUG_ON(len & 1);
		for (; len; len -= 2)
			rfbi_write_reg(rfbi, RFBI_CMD, *w++);
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_9:
	case OMAP_DSS_RFBI_PARALLELMODE_12:
	default:
		BUG();
	}
}

static void rfbi_read_data(struct omap_dss_device *dssdev, void *buf, u32 len)
{
	struct rfbi_data *rfbi = to_rfbi_data(dssdev);

	DSSDBG("%s\n", __func__);

	switch (rfbi->parallelmode) {
	case OMAP_DSS_RFBI_PARALLELMODE_8:
	{
		u8 *b = buf;
		for (; len; len--) {
			rfbi_write_reg(rfbi, RFBI_READ, 0);
			*b++ = rfbi_read_reg(rfbi, RFBI_READ);
		}
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_16:
	{
		u16 *w = buf;
		BUG_ON(len & ~1);
		for (; len; len -= 2) {
			rfbi_write_reg(rfbi, RFBI_READ, 0);
			*w++ = rfbi_read_reg(rfbi, RFBI_READ);
		}
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_9:
	case OMAP_DSS_RFBI_PARALLELMODE_12:
	default:
		BUG();
	}
}

static void rfbi_write_data(struct omap_dss_device *dssdev, const void *buf, u32 len)
{
	struct rfbi_data *rfbi = to_rfbi_data(dssdev);

	DSSDBG("%s\n", __func__);

	switch (rfbi->parallelmode) {
	case OMAP_DSS_RFBI_PARALLELMODE_8:
	{
		const u8 *b = buf;
		for (; len; len--)
			rfbi_write_reg(rfbi, RFBI_PARAM, *b++);
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_16:
	{
		const u16 *w = buf;
		BUG_ON(len & 1);
		for (; len; len -= 2)
			rfbi_write_reg(rfbi, RFBI_PARAM, *w++);
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_9:
	case OMAP_DSS_RFBI_PARALLELMODE_12:
	default:
		BUG();

	}
}

static int rfbi_transfer_area(struct omap_dss_device *dssdev,
		void (*callback)(void *data), void *data)
{
	u32 l;
	struct rfbi_data *rfbi = to_rfbi_data(dssdev);
	u16 width = rfbi->vm.hactive;
	u16 height = rfbi->vm.vactive;

	/*BUG_ON(callback == 0);*/
	BUG_ON(rfbi->framedone_callback != NULL);

	DSSDBG("%s: %dx%d\n", __func__, width, height);

	dss_mgr_start_update(&rfbi->output);

	rfbi->framedone_callback = callback;
	rfbi->framedone_callback_data = data;

	rfbi_write_reg(rfbi, RFBI_PIXEL_CNT, width * height);

	l = rfbi_read_reg(rfbi, RFBI_CONTROL);
	l = FLD_MOD(l, 1, 0, 0); /* enable */
	if (!rfbi->te_enabled)
		l = FLD_MOD(l, 1, 4, 4); /* ITE */

	rfbi_write_reg(rfbi, RFBI_CONTROL, l);

	return 0;
}

static void rfbi_framedone_callback(void *data)
{
	struct rfbi_data *rfbi = (struct rfbi_data *) data;
	void (*callback)(void *data);

	DSSDBG("%s\n", __func__);

	REG_FLD_MOD(rfbi, RFBI_CONTROL, 0, 0, 0);

	callback = rfbi->framedone_callback;
	rfbi->framedone_callback = NULL;

	if (callback != NULL)
		callback(rfbi->framedone_callback_data);
}

#if 1 /* VERBOSE */
static void rfbi_print_timings(struct rfbi_data *rfbi)
{
	u32 l;
	u32 time;

	l = rfbi_read_reg(rfbi, RFBI_CONFIG(0));
	time = 1000000000 / rfbi->l4_khz;
	if (l & (1 << 4))
		time *= 2;

	DSSDBG("Tick time %u ps\n", time);
	l = rfbi_read_reg(rfbi, RFBI_ONOFF_TIME(0));
	DSSDBG("CSONTIME %d, CSOFFTIME %d, WEONTIME %d, WEOFFTIME %d, "
		"REONTIME %d, REOFFTIME %d\n",
		l & 0x0f, (l >> 4) & 0x3f, (l >> 10) & 0x0f, (l >> 14) & 0x3f,
		(l >> 20) & 0x0f, (l >> 24) & 0x3f);

	l = rfbi_read_reg(rfbi, RFBI_CYCLE_TIME(0));
	DSSDBG("WECYCLETIME %d, RECYCLETIME %d, CSPULSEWIDTH %d, "
		"ACCESSTIME %d\n",
		(l & 0x3f), (l >> 6) & 0x3f, (l >> 12) & 0x3f,
		(l >> 22) & 0x3f);
}
#else
static void rfbi_print_timings(struct rfbi_data *rfbi) {}
#endif




static u32 extif_clk_period;

static inline unsigned long round_to_extif_ticks(unsigned long ps, int div)
{
	int bus_tick = extif_clk_period * div;

	DSSDBG("%s\n", __func__);

	return (ps + bus_tick - 1) / bus_tick * bus_tick;
}

static int calc_reg_timing(struct rfbi_data *rfbi,
		struct rfbi_timings *t, int div)
{
	DSSDBG("%s\n", __func__);

	t->clk_div = div;

	t->cs_on_time = round_to_extif_ticks(t->cs_on_time, div);

	t->we_on_time = round_to_extif_ticks(t->we_on_time, div);
	t->we_off_time = round_to_extif_ticks(t->we_off_time, div);
	t->we_cycle_time = round_to_extif_ticks(t->we_cycle_time, div);

	t->re_on_time = round_to_extif_ticks(t->re_on_time, div);
	t->re_off_time = round_to_extif_ticks(t->re_off_time, div);
	t->re_cycle_time = round_to_extif_ticks(t->re_cycle_time, div);

	t->access_time = round_to_extif_ticks(t->access_time, div);
	t->cs_off_time = round_to_extif_ticks(t->cs_off_time, div);
	t->cs_pulse_width = round_to_extif_ticks(t->cs_pulse_width, div);

	DSSDBG("[reg]cson %d csoff %d reon %d reoff %d\n",
	       t->cs_on_time, t->cs_off_time, t->re_on_time, t->re_off_time);
	DSSDBG("[reg]weon %d weoff %d recyc %d wecyc %d\n",
	       t->we_on_time, t->we_off_time, t->re_cycle_time,
	       t->we_cycle_time);
	DSSDBG("[reg]rdaccess %d cspulse %d\n",
	       t->access_time, t->cs_pulse_width);

	return rfbi_convert_timings(rfbi, t);
}

static int calc_extif_timings(struct rfbi_data *rfbi,
		struct rfbi_timings *t)
{
	u32 max_clk_div;
	int div;

	DSSDBG("%s\n", __func__);

	rfbi_get_clk_info(rfbi, &extif_clk_period, &max_clk_div);
	for (div = 1; div <= max_clk_div; div++) {
		if (calc_reg_timing(rfbi, t, div) == 0)
			break;
	}

	if (div <= max_clk_div)
		return 0;

	DSSERR("can't setup timings\n");
	return -1;
}


static void _rfbi_set_rfbi_timings(struct rfbi_data *rfbi,
		int rfbi_module, struct rfbi_timings *t)
{
	int r;

	DSSDBG("%s\n", __func__);

	if (!t->converted) {
		r = calc_extif_timings(rfbi, t);
		if (r < 0)
			DSSERR("Failed to calc timings\n");
	}

	BUG_ON(!t->converted);

	rfbi_write_reg(rfbi, RFBI_ONOFF_TIME(rfbi_module), t->tim[0]);
	rfbi_write_reg(rfbi, RFBI_CYCLE_TIME(rfbi_module), t->tim[1]);

	/* TIMEGRANULARITY */
	REG_FLD_MOD(rfbi, RFBI_CONFIG(rfbi_module),
		    (t->tim[2] ? 1 : 0), 4, 4);

	rfbi_print_timings(rfbi);
}

static int ps_to_rfbi_ticks(struct rfbi_data *rfbi, int time, int div)
{
	unsigned long tick_ps;
	int ret;

	DSSDBG("%s\n", __func__);

	/* Calculate in picosecs to yield more exact results */
	tick_ps = 1000000000 / (rfbi->l4_khz) * div;

	ret = (time + tick_ps - 1) / tick_ps;

	return ret;
}

static void rfbi_get_clk_info(struct rfbi_data *rfbi, u32 *clk_period, u32 *max_clk_div)
{
	DSSDBG("%s\n", __func__);

	*clk_period = 1000000000 / rfbi->l4_khz;
	*max_clk_div = 2;
}

static int rfbi_convert_timings(struct rfbi_data *rfbi, struct rfbi_timings *t)
{
	u32 l;
	int reon, reoff, weon, weoff, cson, csoff, cs_pulse;
	int actim, recyc, wecyc;
	int div = t->clk_div;

	DSSDBG("%s\n", __func__);

	if (div <= 0 || div > 2)
		return -1;

	/* Make sure that after conversion it still holds that:
	 * weoff > weon, reoff > reon, recyc >= reoff, wecyc >= weoff,
	 * csoff > cson, csoff >= max(weoff, reoff), actim > reon
	 */
	weon = ps_to_rfbi_ticks(rfbi, t->we_on_time, div);
	weoff = ps_to_rfbi_ticks(rfbi, t->we_off_time, div);
	if (weoff <= weon)
		weoff = weon + 1;
	if (weon > 0x0f)
		return -1;
	if (weoff > 0x3f)
		return -1;

	reon = ps_to_rfbi_ticks(rfbi, t->re_on_time, div);
	reoff = ps_to_rfbi_ticks(rfbi, t->re_off_time, div);
	if (reoff <= reon)
		reoff = reon + 1;
	if (reon > 0x0f)
		return -1;
	if (reoff > 0x3f)
		return -1;

	cson = ps_to_rfbi_ticks(rfbi, t->cs_on_time, div);
	csoff = ps_to_rfbi_ticks(rfbi, t->cs_off_time, div);
	if (csoff <= cson)
		csoff = cson + 1;
	if (csoff < max(weoff, reoff))
		csoff = max(weoff, reoff);
	if (cson > 0x0f)
		return -1;
	if (csoff > 0x3f)
		return -1;

	l =  cson;
	l |= csoff << 4;
	l |= weon  << 10;
	l |= weoff << 14;
	l |= reon  << 20;
	l |= reoff << 24;

	t->tim[0] = l;

	actim = ps_to_rfbi_ticks(rfbi, t->access_time, div);
	if (actim <= reon)
		actim = reon + 1;
	if (actim > 0x3f)
		return -1;

	wecyc = ps_to_rfbi_ticks(rfbi, t->we_cycle_time, div);
	if (wecyc < weoff)
		wecyc = weoff;
	if (wecyc > 0x3f)
		return -1;

	recyc = ps_to_rfbi_ticks(rfbi, t->re_cycle_time, div);
	if (recyc < reoff)
		recyc = reoff;
	if (recyc > 0x3f)
		return -1;

	cs_pulse = ps_to_rfbi_ticks(rfbi, t->cs_pulse_width, div);
	if (cs_pulse > 0x3f)
		return -1;

	l =  wecyc;
	l |= recyc    << 6;
	l |= cs_pulse << 12;
	l |= actim    << 22;

	t->tim[1] = l;

	t->tim[2] = div - 1;

	t->converted = 1;

	return 0;
}

static int rfbi_configure_bus(struct rfbi_data *rfbi, int rfbi_module,
		int bpp, int lines)
{
	u32 l;
	int cycle1 = 0, cycle2 = 0, cycle3 = 0;
	enum omap_rfbi_cycleformat cycleformat;
	enum omap_rfbi_datatype datatype;
	enum omap_rfbi_parallelmode parallelmode;

	DSSDBG("%s\n", __func__);

	switch (bpp) {
	case 12:
		datatype = OMAP_DSS_RFBI_DATATYPE_12;
		break;
	case 16:
		datatype = OMAP_DSS_RFBI_DATATYPE_16;
		break;
	case 18:
		datatype = OMAP_DSS_RFBI_DATATYPE_18;
		break;
	case 24:
		datatype = OMAP_DSS_RFBI_DATATYPE_24;
		break;
	default:
		DSSERR("%s: invalid bpp=%d\n", __func__, bpp);
		BUG();
		return 1;
	}
	rfbi->datatype = datatype;

	switch (lines) {
	case 8:
		parallelmode = OMAP_DSS_RFBI_PARALLELMODE_8;
		break;
	case 9:
		parallelmode = OMAP_DSS_RFBI_PARALLELMODE_9;
		break;
	case 12:
		parallelmode = OMAP_DSS_RFBI_PARALLELMODE_12;
		break;
	case 16:
		parallelmode = OMAP_DSS_RFBI_PARALLELMODE_16;
		break;
	default:
		DSSERR("%s: invalid lines=%d\n", __func__, lines);
		BUG();
		return 1;
	}
	rfbi->parallelmode = parallelmode;

	if ((bpp % lines) == 0) {
		switch (bpp / lines) {
		case 1:
			cycleformat = OMAP_DSS_RFBI_CYCLEFORMAT_1_1;
			break;
		case 2:
			cycleformat = OMAP_DSS_RFBI_CYCLEFORMAT_2_1;
			break;
		case 3:
			cycleformat = OMAP_DSS_RFBI_CYCLEFORMAT_3_1;
			break;
		default:
			DSSERR("%s: invalid ratio %d\n", __func__, bpp/lines);
			BUG();
			return 1;
		}
	} else if ((2 * bpp % lines) == 0) {
		if ((2 * bpp / lines) == 3)
			cycleformat = OMAP_DSS_RFBI_CYCLEFORMAT_3_2;
		else {
			BUG();
			return 1;
		}
	} else {
		BUG();
		return 1;
	}

	switch (cycleformat) {
	case OMAP_DSS_RFBI_CYCLEFORMAT_1_1:
		cycle1 = lines;
		break;

	case OMAP_DSS_RFBI_CYCLEFORMAT_2_1:
		cycle1 = lines;
		cycle2 = lines;
		break;

	case OMAP_DSS_RFBI_CYCLEFORMAT_3_1:
		cycle1 = lines;
		cycle2 = lines;
		cycle3 = lines;
		break;

	case OMAP_DSS_RFBI_CYCLEFORMAT_3_2:
		cycle1 = lines;
		cycle2 = (lines / 2) | ((lines / 2) << 16);
		cycle3 = (lines << 16);
		break;
	}

	REG_FLD_MOD(rfbi, RFBI_CONTROL, 0, 3, 2); /* clear CS */

	l = 0;
	l |= FLD_VAL(parallelmode, 1, 0);
	l |= FLD_VAL(0, 3, 2);		/* TRIGGERMODE: ITE */
	l |= FLD_VAL(0, 4, 4);		/* TIMEGRANULARITY */
	l |= FLD_VAL(datatype, 6, 5);
	/* l |= FLD_VAL(2, 8, 7); */	/* L4FORMAT, 2pix/L4 */
	l |= FLD_VAL(0, 8, 7);	/* L4FORMAT, 1pix/L4 */
	l |= FLD_VAL(cycleformat, 10, 9);
	l |= FLD_VAL(0, 12, 11);	/* UNUSEDBITS */
	l |= FLD_VAL(0, 16, 16);	/* A0POLARITY */
	l |= FLD_VAL(0, 17, 17);	/* REPOLARITY */
	l |= FLD_VAL(0, 18, 18);	/* WEPOLARITY */
	l |= FLD_VAL(0, 19, 19);	/* CSPOLARITY */
	l |= FLD_VAL(1, 20, 20);	/* TE_VSYNC_POLARITY */
	l |= FLD_VAL(1, 21, 21);	/* HSYNCPOLARITY */
	rfbi_write_reg(rfbi, RFBI_CONFIG(rfbi_module), l);

	rfbi_write_reg(rfbi, RFBI_DATA_CYCLE1(rfbi_module), cycle1);
	rfbi_write_reg(rfbi, RFBI_DATA_CYCLE2(rfbi_module), cycle2);
	rfbi_write_reg(rfbi, RFBI_DATA_CYCLE3(rfbi_module), cycle3);


	l = rfbi_read_reg(rfbi, RFBI_CONTROL);
	l = FLD_MOD(l, rfbi_module+1, 3, 2); /* Select CSx */
	l = FLD_MOD(l, 0, 1, 1); /* clear bypass */
	rfbi_write_reg(rfbi, RFBI_CONTROL, l);

	//DSSDBG("RFBI config: bpp %d, lines %d, cycles: 0x%x 0x%x 0x%x\n",
	//       bpp, lines, cycle1, cycle2, cycle3);

	return 0;
}

static int rfbi_configure(struct omap_dss_device *dssdev)
{
	struct rfbi_data *rfbi = to_rfbi_data(dssdev);

	DSSDBG("%s\n", __func__);

	return rfbi_configure_bus(rfbi, rfbi->output.of_port,
			rfbi->pixel_size, rfbi->data_lines);
}

static int rfbi_update(struct omap_dss_device *dssdev, void (*callback)(void *),
		void *data)
{
	DSSDBG("%s\n", __func__);

	return rfbi_transfer_area(dssdev, callback, data);
}

static void rfbi_set_pixel_size(struct omap_dss_device *dssdev, int pixel_size)
{
	struct rfbi_data *rfbi = to_rfbi_data(dssdev);

	DSSDBG("%s\n", __func__);

	rfbi->pixel_size = pixel_size;
}

static void rfbi_set_data_lines(struct omap_dss_device *dssdev, int data_lines)
{
	struct rfbi_data *rfbi = to_rfbi_data(dssdev);

	DSSDBG("%s\n", __func__);

	rfbi->data_lines = data_lines;
}

static void rfbi_set_interface_timings(struct omap_dss_device *dssdev,
		const struct rfbi_timings *timings)
{
	struct rfbi_data *rfbi = to_rfbi_data(dssdev);

	DSSDBG("%s\n", __func__);

	rfbi->intf_timings = *timings;
}

static int rfbi_dump_regs(struct seq_file *s, void *p)
{
	struct rfbi_data *rfbi = s->private;
#define DUMPREG(r) seq_printf(s, "%-35s %08x\n", #r, rfbi_read_reg(rfbi,r))

	DSSDBG("%s\n", __func__);

	if (rfbi_runtime_get(rfbi))
		return 0;

	DUMPREG(RFBI_REVISION);
	DUMPREG(RFBI_SYSCONFIG);
	DUMPREG(RFBI_SYSSTATUS);
	DUMPREG(RFBI_CONTROL);
	DUMPREG(RFBI_PIXEL_CNT);
	DUMPREG(RFBI_LINE_NUMBER);
	DUMPREG(RFBI_CMD);
	DUMPREG(RFBI_PARAM);
	DUMPREG(RFBI_DATA);
	DUMPREG(RFBI_READ);
	DUMPREG(RFBI_STATUS);

	DUMPREG(RFBI_CONFIG(0));
	DUMPREG(RFBI_ONOFF_TIME(0));
	DUMPREG(RFBI_CYCLE_TIME(0));
	DUMPREG(RFBI_DATA_CYCLE1(0));
	DUMPREG(RFBI_DATA_CYCLE2(0));
	DUMPREG(RFBI_DATA_CYCLE3(0));

	DUMPREG(RFBI_CONFIG(1));
	DUMPREG(RFBI_ONOFF_TIME(1));
	DUMPREG(RFBI_CYCLE_TIME(1));
	DUMPREG(RFBI_DATA_CYCLE1(1));
	DUMPREG(RFBI_DATA_CYCLE2(1));
	DUMPREG(RFBI_DATA_CYCLE3(1));

	DUMPREG(RFBI_VSYNC_WIDTH);
	DUMPREG(RFBI_HSYNC_WIDTH);

	rfbi_runtime_put(rfbi);
#undef DUMPREG
	return 0;
}

static int rfbi_dump_irqs(struct seq_file *s, void *p)
{
	struct rfbi_data *rfbi = s->private;

	DSSDBG("%s\n", __func__);

	if (rfbi_runtime_get(rfbi))
		return 0;

	rfbi_runtime_put(rfbi);
	return 0;
}

static int rfbi_dump_clks(struct seq_file *s, void *p)
{
	struct rfbi_data *rfbi = s->private;

	DSSDBG("%s\n", __func__);

	if (rfbi_runtime_get(rfbi))
		return 0;

	rfbi_runtime_put(rfbi);
	return 0;
}

static int rfbi_prepare_clock_info(struct rfbi_data *rfbi, struct dispc_clock_info *cinfo) {
	unsigned long fck_rate = rfbi->l4_khz*1000;

	DSSDBG("%s\n", __func__);

	cinfo->lck_div = 1;
	cinfo->pck_div = fck_rate / rfbi->vm.pixelclock;
	return dispc_calc_clock_rates(rfbi->dss->dispc, fck_rate, cinfo);
}

static void rfbi_config_lcd_manager(struct rfbi_data *rfbi, const struct drm_display_mode *adjusted_mode)
{
	struct omap_dss_device *out = &rfbi->output;
	struct dss_lcd_mgr_config *mgr_config = &rfbi->mgr_config;

	DSSDBG("%s: ??\n", __func__);

	dss_mgr_set_timings(out, &rfbi->vm);

	mgr_config->io_pad_mode = DSS_IO_PAD_MODE_RFBI;

	mgr_config->stallmode = true;
	/* Do we need fifohandcheck for RFBI? */
	mgr_config->fifohandcheck = false;

	rfbi_prepare_clock_info(rfbi, &mgr_config->clock_info);

	mgr_config->video_port_width = rfbi->pixel_size;
	mgr_config->lcden_sig_polarity = 0;

	DSSDBG("%s: calling set_lcd_config\n", __func__);
	dss_mgr_set_lcd_config(out, mgr_config);
	DSSDBG("%s: set_lcd_config done\n", __func__);

	DSSDBG("%s: calling set_clock_div\n", __func__);
	dispc_mgr_set_clock_div(rfbi->dss->dispc, out->dispc_channel, &mgr_config->clock_info);
	DSSDBG("%s: set_clock_div done\n", __func__);
}

static int rfbi_display_enable(struct rfbi_data *rfbi)
{
	struct omap_dss_device *out = &rfbi->output;
	int r;

	DSSDBG("%s\n", __func__);

	mutex_lock(&rfbi->lock);

	r = rfbi_runtime_get(rfbi);
	if (r)
		return r;

	r = dss_mgr_register_framedone_handler(out, rfbi_framedone_callback, rfbi);
	if (r) {
		DSSERR("can't get FRAMEDONE irq\n");
		goto err1;
	}

	DSSDBG("%s: rfbi=%px ps=%d dl=%d\n", __func__, rfbi, rfbi->pixel_size,
			rfbi->data_lines);
	rfbi_configure_bus(rfbi, rfbi->output.of_port, rfbi->pixel_size,
			rfbi->data_lines);

	_rfbi_set_rfbi_timings(rfbi, rfbi->output.of_port, &rfbi->intf_timings);

	dss_mgr_set_timings(out, &rfbi->vm);

	r = dss_mgr_enable(out);
	if (r)
		goto err1;

	mutex_unlock(&rfbi->lock);

	return 0;
err1:
	rfbi_runtime_put(rfbi);
	return r;
}

static int omapdss_rfbi_display_enable(struct omap_dss_device *dssdev)
{
	return rfbi_display_enable(to_rfbi_data(dssdev));
}

static void rfbi_display_disable(struct rfbi_data *rfbi)
{
	struct omap_dss_device *out = &rfbi->output;

	DSSDBG("%s\n", __func__);

	mutex_lock(&rfbi->lock);

	dss_mgr_disable(out);

	dss_mgr_unregister_framedone_handler(out, rfbi_framedone_callback, rfbi);

	rfbi_runtime_put(rfbi);

	mutex_unlock(&rfbi->lock);
}

static void omapdss_rfbi_display_disable(struct omap_dss_device *dssdev)
{
	rfbi_display_disable(to_rfbi_data(dssdev));
}

static const struct omapdss_rfbi_ops rfbi_ops = {

	.enable = omapdss_rfbi_display_enable,
	.disable = omapdss_rfbi_display_disable,

	.update = rfbi_update,

	.set_pixel_size = rfbi_set_pixel_size,
	.set_data_lines = rfbi_set_data_lines,
	.set_rfbi_timings = rfbi_set_interface_timings,

	.configure = rfbi_configure,

	.bus_lock = omapdss_rfbi_bus_lock,
	.bus_unlock = omapdss_rfbi_bus_unlock,

	.write_command = rfbi_write_command,
	.read_data = rfbi_read_data,
	.write_data = rfbi_write_data,
};

/* -----------------------------------------------------------------------------
 * DRM Bridge Operations
 */

static int rfbi_bridge_attach(struct drm_bridge *bridge,
			      enum drm_bridge_attach_flags flags)
{
	struct rfbi_data *rfbi = drm_bridge_to_rfbi(bridge);

	DSSDBG("%s\n", __func__);

	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR))
		return -EINVAL;

	return drm_bridge_attach(bridge->encoder, rfbi->output.next_bridge,
				 bridge, flags);
}

static enum drm_mode_status
rfbi_bridge_mode_valid(struct drm_bridge *bridge,
		       const struct drm_display_info *info,
		       const struct drm_display_mode *mode)
{
	struct rfbi_data *rfbi = drm_bridge_to_rfbi(bridge);
	//struct dsi_clk_calc_ctx ctx;
	int r;

	DSSDBG("%s\n", __func__);

	mutex_lock(&rfbi->lock);
	//r = __dsi_calc_config(dsi, mode, &ctx);
	r = 0;
	mutex_unlock(&rfbi->lock);

	return r ? MODE_CLOCK_RANGE : MODE_OK;
}

static void rfbi_bridge_mode_set(struct drm_bridge *bridge,
				 const struct drm_display_mode *mode,
				 const struct drm_display_mode *adjusted_mode)
{
	struct rfbi_data *rfbi = drm_bridge_to_rfbi(bridge);

	DSSDBG("%s\n", __func__);

	rfbi_config_lcd_manager(rfbi, adjusted_mode);
}

static void rfbi_bridge_enable(struct drm_bridge *bridge)
{
	struct rfbi_data *rfbi = drm_bridge_to_rfbi(bridge);

	DSSDBG("%s\n", __func__);

	rfbi_bus_lock(rfbi);

	rfbi_display_enable(rfbi);

	rfbi_bus_unlock(rfbi);
}

static void rfbi_bridge_disable(struct drm_bridge *bridge)
{
	struct rfbi_data *rfbi = drm_bridge_to_rfbi(bridge);

	DSSDBG("%s\n", __func__);

	rfbi_bus_lock(rfbi);

	rfbi_display_disable(rfbi);

	rfbi_bus_unlock(rfbi);
}

static const struct drm_bridge_funcs rfbi_bridge_funcs = {
	.attach = rfbi_bridge_attach,
	.mode_valid = rfbi_bridge_mode_valid,
	.mode_set = rfbi_bridge_mode_set,
	.enable = rfbi_bridge_enable,
	.disable = rfbi_bridge_disable,
};

static void rfbi_bridge_init(struct rfbi_data *rfbi)
{
	DSSDBG("%s\n", __func__);

	rfbi->bridge.funcs = &rfbi_bridge_funcs;
	rfbi->bridge.of_node = rfbi->dev->of_node;
	rfbi->bridge.type = DRM_MODE_CONNECTOR_DPI;

	drm_bridge_add(&rfbi->bridge);
}

static void rfbi_bridge_cleanup(struct rfbi_data *rfbi)
{
	DSSDBG("%s\n", __func__);

	drm_bridge_remove(&rfbi->bridge);
}


static int rfbi_init_output(struct rfbi_data *rfbi)
{
	int r;
	struct omap_dss_device *out = &rfbi->output;

	DSSDBG("%s: rfbi=%px out=%px\n", __func__, rfbi, out);

	rfbi_bridge_init(rfbi);

	out->dev = rfbi->dev;
	out->id = OMAP_DSS_OUTPUT_DBI;
	out->type = OMAP_DISPLAY_TYPE_DBI;
	out->of_port = 0;
	out->name = "rfbi.0";
	out->dispc_channel = OMAP_DSS_CHANNEL_LCD;

	r = omapdss_device_init_output(out, &rfbi->bridge);
	if (r < 0) {
		DSSERR("%s: error init output (%d)\n", __func__, r);
		rfbi_bridge_cleanup(rfbi);
		return r;
	}

	omapdss_device_register(out);
	DSSINFO("%s: device registered '%s' %pOF\n", __func__, out->name,
			out->dev->of_node);

	return 0;
}

static void rfbi_uninit_output(struct rfbi_data *rfbi)
{
	struct omap_dss_device *out = &rfbi->output;

	DSSDBG("%s: rfbi=%px out=%px\n", __func__, rfbi, out);

	omapdss_device_unregister(out);
	omapdss_device_cleanup_output(out);

	rfbi_bridge_cleanup(rfbi);
}

/* RFBI HW IP initialisation */
static int rfbi_bind(struct device *dev, struct device *master, void *data)
{
	struct dss_device *dss = dss_get_device(master);
	struct rfbi_data *rfbi = dev_get_drvdata(dev);
	u32 rev;
	int r;

	DSSDBG("%s\n", __func__);

	rfbi->dss = dss;

	r = rfbi_runtime_get(rfbi);
	if (r) {
		dev_err(dev, "%s: cannot get runtime (%d)\n", __func__, r);
		goto err_runtime_get;
	}

	msleep(10);

	rev = rfbi_read_reg(rfbi, RFBI_REVISION);
	dev_dbg(dev, "OMAP RFBI rev %d.%d\n",
	       FLD_GET(rev, 7, 4), FLD_GET(rev, 3, 0));

	rfbi_runtime_put(rfbi);

	rfbi->debugfs.regs = dss_debugfs_create_file(dss, "rfbi_regs", rfbi_dump_regs, rfbi);
	rfbi->debugfs.irqs = dss_debugfs_create_file(dss, "rfbi_irqs", rfbi_dump_irqs, rfbi);
	rfbi->debugfs.clks = dss_debugfs_create_file(dss, "rfbi_clks", rfbi_dump_clks, rfbi);

	rfbi_init_output(rfbi);

	return 0;

err_runtime_get:
	return r;
}

static void rfbi_unbind(struct device *dev, struct device *master, void *data)
{
	struct rfbi_data *rfbi = dev_get_drvdata(dev);

	DSSDBG("%s\n", __func__);

	dss_debugfs_remove_file(rfbi->debugfs.clks);
	dss_debugfs_remove_file(rfbi->debugfs.irqs);
	dss_debugfs_remove_file(rfbi->debugfs.regs);

	rfbi_uninit_output(rfbi);

	pm_runtime_disable(dev);
}

static const struct component_ops rfbi_component_ops = {
	.bind	= rfbi_bind,
	.unbind	= rfbi_unbind,
};

static int rfbi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rfbi_data *rfbi;
	struct resource *rfbi_mem;
	struct clk *clk;

	DSSDBG("%s\n", __func__);

	rfbi = devm_kzalloc(dev, sizeof(*rfbi), GFP_KERNEL);
	if (!rfbi)
		return -ENOMEM;

	rfbi->dev = dev;
	dev_set_drvdata(dev, rfbi);

	mutex_init(&rfbi->lock);
	sema_init(&rfbi->bus_lock, 1);

	rfbi_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!rfbi_mem) {
		DSSERR("can't get IORESOURCE_MEM RFBI\n");
		return -EINVAL;
	}

	rfbi->base = devm_ioremap(dev, rfbi_mem->start,
				 resource_size(rfbi_mem));
	if (!rfbi->base) {
		DSSERR("can't ioremap RFBI\n");
		return -ENOMEM;
	}

	rfbi->dss_clk = devm_clk_get(dev, "fck");
	if (IS_ERR(rfbi->dss_clk)) {
		DSSERR("can't get fck\n");
		return PTR_ERR(rfbi->dss_clk);
	}

	clk = clk_get(dev, "ick");
	if (IS_ERR(clk)) {
		DSSERR("can't get ick\n");
		return PTR_ERR(clk);
	}

	rfbi->l4_khz = clk_get_rate(clk) / 1000;

	clk_put(clk);

	pm_runtime_enable(dev);

	DSSDBG("%s: adding component\n", __func__);

	return component_add(dev, &rfbi_component_ops);
}

static int rfbi_remove(struct platform_device *pdev)
{
	DSSDBG("%s\n", __func__);

	component_del(&pdev->dev, &rfbi_component_ops);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static int rfbi_runtime_suspend(struct device *dev)
{
	struct rfbi_data *rfbi = dev_get_drvdata(dev);

	DSSDBG("%s\n", __func__);

	rfbi->is_enabled = false;

	return 0;
}

static int rfbi_runtime_resume(struct device *dev)
{
	struct rfbi_data *rfbi = dev_get_drvdata(dev);

	DSSDBG("%s\n", __func__);

	rfbi->is_enabled = true;

	return 0;
}

static const struct dev_pm_ops rfbi_pm_ops = {
	.runtime_suspend = rfbi_runtime_suspend,
	.runtime_resume = rfbi_runtime_resume,
	SET_LATE_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static const struct of_device_id rfbi_of_match[] = {
	{ .compatible = "ti,omap2-rfbi", },
	{ .compatible = "ti,omap3-rfbi", },
	{ .compatible = "ti,omap4-rfbi", },
	{},
};

struct platform_driver omap_rfbihw_driver = {
	.probe		= rfbi_probe,
	.remove         = rfbi_remove,
	.driver         = {
		.name   = "omapdss_rfbi",
		.pm	= &rfbi_pm_ops,
		.of_match_table = rfbi_of_match,
		.suppress_bind_attrs = true,
	},
};

