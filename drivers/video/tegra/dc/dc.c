/*
 * drivers/video/tegra/dc/dc.c
 *
 * Copyright (C) 2010 Google, Inc.
 * Author: Erik Gilling <konkers@android.com>
 *
 * Copyright (C) 2010-2012 NVIDIA Corporation
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/backlight.h>
#include <linux/gpio.h>
#include <video/tegrafb.h>
#include <drm/drm_fixed.h>
#ifdef CONFIG_SWITCH
#include <linux/switch.h>
#endif


#include <mach/clk.h>
#include <mach/dc.h>
#include <mach/fb.h>
#include <mach/mc.h>
#include <linux/nvhost.h>
#include <mach/latency_allowance.h>

#include "dc_reg.h"
#include "dc_config.h"
#include "dc_priv.h"
#include "nvsd.h"

#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
#include "../cmc623.h"
#endif
#include "edid.h" /* for CONFIG_MACH_SAMSUNG_HDMI_EDID_FORCE_PASS */
#define TEGRA_CRC_LATCHED_DELAY		34

#define DC_COM_PIN_OUTPUT_POLARITY1_INIT_VAL	0x01000000
#define DC_COM_PIN_OUTPUT_POLARITY3_INIT_VAL	0x0

#ifndef CONFIG_TEGRA_FPGA_PLATFORM
#define ALL_UF_INT (WIN_A_UF_INT | WIN_B_UF_INT | WIN_C_UF_INT)
#else
/* ignore underflows when on simulation and fpga platform */
#define ALL_UF_INT (0)
#endif

#if defined(CONFIG_MACH_SAMSUNG_P5) || defined(CONFIG_MACH_SAMSUNG_P5WIFI)
#define	__SAMSUNG_HDMI_FLAG_WORKAROUND__
#endif

#ifdef	__SAMSUNG_HDMI_FLAG_WORKAROUND__
static int hdmi_enable_flag = -1;
spinlock_t hdmi_enable_lock;

enum {
	HDMI_FLAG_DISABLE = 0,
	HDMI_FLAG_ENABLE = 1,
} EN_HDMI_FLAG;

int tegra_dc_get_hdmi_flag()
{
	int temp;
	unsigned long flags;

	spin_lock_irqsave(&hdmi_enable_lock, flags);
	temp = hdmi_enable_flag;
	spin_unlock_irqrestore(&hdmi_enable_lock, flags);
	return temp;
}
EXPORT_SYMBOL(tegra_dc_get_hdmi_flag);

void tegra_dc_set_hdmi_flag(int flag)
{
	unsigned long flags;

	spin_lock_irqsave(&hdmi_enable_lock, flags);
	hdmi_enable_flag = flag;
	spin_unlock_irqrestore(&hdmi_enable_lock, flags);
}
EXPORT_SYMBOL(tegra_dc_set_hdmi_flag);
#endif

static int no_vsync;
static struct timeval t_suspend;

static struct fb_videomode tegra_dc_hdmi_fallback_mode = {
	.refresh = 60,
	.xres = 640,
	.yres = 480,
	.pixclock = KHZ2PICOS(25200),
	.hsync_len = 96,	/* h_sync_width */
	.vsync_len = 2,		/* v_sync_width */
	.left_margin = 48,	/* h_back_porch */
	.upper_margin = 33,	/* v_back_porch */
	.right_margin = 16,	/* h_front_porch */
	.lower_margin = 10,	/* v_front_porch */
	.vmode = 0,
	.sync = 0,
};

static struct tegra_dc_mode override_disp_mode[3];

static void _tegra_dc_controller_disable(struct tegra_dc *dc);

module_param_named(no_vsync, no_vsync, int, S_IRUGO | S_IWUSR);

struct tegra_dc *tegra_dcs[TEGRA_MAX_DC];

DEFINE_MUTEX(tegra_dc_lock);
DEFINE_MUTEX(shared_lock);

static inline void tegra_dc_clk_enable(struct tegra_dc *dc)
{
	if (!tegra_is_clk_enabled(dc->clk)) {
		clk_enable(dc->clk);
		tegra_dvfs_set_rate(dc->clk, dc->mode.pclk);
	}
}

static inline void tegra_dc_clk_disable(struct tegra_dc *dc)
{
	if (tegra_is_clk_enabled(dc->clk)) {
		clk_disable(dc->clk);
		tegra_dvfs_set_rate(dc->clk, 0);
	}
}

#define DUMP_REG(a) do {			\
	snprintf(buff, sizeof(buff), "%-32s\t%03x\t%08lx\n", \
		 #a, a, tegra_dc_readl(dc, a));		      \
	print(data, buff);				      \
	} while (0)

static void _dump_regs(struct tegra_dc *dc, void *data,
		       void (* print)(void *data, const char *str))
{
	int i;
	char buff[256];

	tegra_dc_io_start(dc);
	tegra_dc_clk_enable(dc);

	DUMP_REG(DC_CMD_DISPLAY_COMMAND_OPTION0);
	DUMP_REG(DC_CMD_DISPLAY_COMMAND);
	DUMP_REG(DC_CMD_SIGNAL_RAISE);
	DUMP_REG(DC_CMD_INT_STATUS);
	DUMP_REG(DC_CMD_INT_MASK);
	DUMP_REG(DC_CMD_INT_ENABLE);
	DUMP_REG(DC_CMD_INT_TYPE);
	DUMP_REG(DC_CMD_INT_POLARITY);
	DUMP_REG(DC_CMD_SIGNAL_RAISE1);
	DUMP_REG(DC_CMD_SIGNAL_RAISE2);
	DUMP_REG(DC_CMD_SIGNAL_RAISE3);
	DUMP_REG(DC_CMD_STATE_ACCESS);
	DUMP_REG(DC_CMD_STATE_CONTROL);
	DUMP_REG(DC_CMD_DISPLAY_WINDOW_HEADER);
	DUMP_REG(DC_CMD_REG_ACT_CONTROL);

	DUMP_REG(DC_DISP_DISP_SIGNAL_OPTIONS0);
	DUMP_REG(DC_DISP_DISP_SIGNAL_OPTIONS1);
	DUMP_REG(DC_DISP_DISP_WIN_OPTIONS);
	DUMP_REG(DC_DISP_MEM_HIGH_PRIORITY);
	DUMP_REG(DC_DISP_MEM_HIGH_PRIORITY_TIMER);
	DUMP_REG(DC_DISP_DISP_TIMING_OPTIONS);
	DUMP_REG(DC_DISP_REF_TO_SYNC);
	DUMP_REG(DC_DISP_SYNC_WIDTH);
	DUMP_REG(DC_DISP_BACK_PORCH);
	DUMP_REG(DC_DISP_DISP_ACTIVE);
	DUMP_REG(DC_DISP_FRONT_PORCH);
	DUMP_REG(DC_DISP_H_PULSE0_CONTROL);
	DUMP_REG(DC_DISP_H_PULSE0_POSITION_A);
	DUMP_REG(DC_DISP_H_PULSE0_POSITION_B);
	DUMP_REG(DC_DISP_H_PULSE0_POSITION_C);
	DUMP_REG(DC_DISP_H_PULSE0_POSITION_D);
	DUMP_REG(DC_DISP_H_PULSE1_CONTROL);
	DUMP_REG(DC_DISP_H_PULSE1_POSITION_A);
	DUMP_REG(DC_DISP_H_PULSE1_POSITION_B);
	DUMP_REG(DC_DISP_H_PULSE1_POSITION_C);
	DUMP_REG(DC_DISP_H_PULSE1_POSITION_D);
	DUMP_REG(DC_DISP_H_PULSE2_CONTROL);
	DUMP_REG(DC_DISP_H_PULSE2_POSITION_A);
	DUMP_REG(DC_DISP_H_PULSE2_POSITION_B);
	DUMP_REG(DC_DISP_H_PULSE2_POSITION_C);
	DUMP_REG(DC_DISP_H_PULSE2_POSITION_D);
	DUMP_REG(DC_DISP_V_PULSE0_CONTROL);
	DUMP_REG(DC_DISP_V_PULSE0_POSITION_A);
	DUMP_REG(DC_DISP_V_PULSE0_POSITION_B);
	DUMP_REG(DC_DISP_V_PULSE0_POSITION_C);
	DUMP_REG(DC_DISP_V_PULSE1_CONTROL);
	DUMP_REG(DC_DISP_V_PULSE1_POSITION_A);
	DUMP_REG(DC_DISP_V_PULSE1_POSITION_B);
	DUMP_REG(DC_DISP_V_PULSE1_POSITION_C);
	DUMP_REG(DC_DISP_V_PULSE2_CONTROL);
	DUMP_REG(DC_DISP_V_PULSE2_POSITION_A);
	DUMP_REG(DC_DISP_V_PULSE3_CONTROL);
	DUMP_REG(DC_DISP_V_PULSE3_POSITION_A);
	DUMP_REG(DC_DISP_M0_CONTROL);
	DUMP_REG(DC_DISP_M1_CONTROL);
	DUMP_REG(DC_DISP_DI_CONTROL);
	DUMP_REG(DC_DISP_PP_CONTROL);
	DUMP_REG(DC_DISP_PP_SELECT_A);
	DUMP_REG(DC_DISP_PP_SELECT_B);
	DUMP_REG(DC_DISP_PP_SELECT_C);
	DUMP_REG(DC_DISP_PP_SELECT_D);
	DUMP_REG(DC_DISP_DISP_CLOCK_CONTROL);
	DUMP_REG(DC_DISP_DISP_INTERFACE_CONTROL);
	DUMP_REG(DC_DISP_DISP_COLOR_CONTROL);
	DUMP_REG(DC_DISP_SHIFT_CLOCK_OPTIONS);
	DUMP_REG(DC_DISP_DATA_ENABLE_OPTIONS);
	DUMP_REG(DC_DISP_SERIAL_INTERFACE_OPTIONS);
	DUMP_REG(DC_DISP_LCD_SPI_OPTIONS);
	DUMP_REG(DC_DISP_BORDER_COLOR);
	DUMP_REG(DC_DISP_COLOR_KEY0_LOWER);
	DUMP_REG(DC_DISP_COLOR_KEY0_UPPER);
	DUMP_REG(DC_DISP_COLOR_KEY1_LOWER);
	DUMP_REG(DC_DISP_COLOR_KEY1_UPPER);
	DUMP_REG(DC_DISP_CURSOR_FOREGROUND);
	DUMP_REG(DC_DISP_CURSOR_BACKGROUND);
	DUMP_REG(DC_DISP_CURSOR_START_ADDR);
	DUMP_REG(DC_DISP_CURSOR_START_ADDR_NS);
	DUMP_REG(DC_DISP_CURSOR_POSITION);
	DUMP_REG(DC_DISP_CURSOR_POSITION_NS);
	DUMP_REG(DC_DISP_INIT_SEQ_CONTROL);
	DUMP_REG(DC_DISP_SPI_INIT_SEQ_DATA_A);
	DUMP_REG(DC_DISP_SPI_INIT_SEQ_DATA_B);
	DUMP_REG(DC_DISP_SPI_INIT_SEQ_DATA_C);
	DUMP_REG(DC_DISP_SPI_INIT_SEQ_DATA_D);
	DUMP_REG(DC_DISP_DC_MCCIF_FIFOCTRL);
	DUMP_REG(DC_DISP_MCCIF_DISPLAY0A_HYST);
	DUMP_REG(DC_DISP_MCCIF_DISPLAY0B_HYST);
	DUMP_REG(DC_DISP_MCCIF_DISPLAY0C_HYST);
	DUMP_REG(DC_DISP_MCCIF_DISPLAY1B_HYST);
	DUMP_REG(DC_DISP_DAC_CRT_CTRL);
	DUMP_REG(DC_DISP_DISP_MISC_CONTROL);


	for (i = 0; i < 3; i++) {
		print(data, "\n");
		snprintf(buff, sizeof(buff), "WINDOW %c:\n", 'A' + i);
		print(data, buff);

		tegra_dc_writel(dc, WINDOW_A_SELECT << i,
				DC_CMD_DISPLAY_WINDOW_HEADER);
		DUMP_REG(DC_CMD_DISPLAY_WINDOW_HEADER);
		DUMP_REG(DC_WIN_WIN_OPTIONS);
		DUMP_REG(DC_WIN_BYTE_SWAP);
		DUMP_REG(DC_WIN_BUFFER_CONTROL);
		DUMP_REG(DC_WIN_COLOR_DEPTH);
		DUMP_REG(DC_WIN_POSITION);
		DUMP_REG(DC_WIN_SIZE);
		DUMP_REG(DC_WIN_PRESCALED_SIZE);
		DUMP_REG(DC_WIN_H_INITIAL_DDA);
		DUMP_REG(DC_WIN_V_INITIAL_DDA);
		DUMP_REG(DC_WIN_DDA_INCREMENT);
		DUMP_REG(DC_WIN_LINE_STRIDE);
		DUMP_REG(DC_WIN_BUF_STRIDE);
		DUMP_REG(DC_WIN_UV_BUF_STRIDE);
		DUMP_REG(DC_WIN_BLEND_NOKEY);
		DUMP_REG(DC_WIN_BLEND_1WIN);
		DUMP_REG(DC_WIN_BLEND_2WIN_X);
		DUMP_REG(DC_WIN_BLEND_2WIN_Y);
		DUMP_REG(DC_WIN_BLEND_3WIN_XY);
		DUMP_REG(DC_WINBUF_START_ADDR);
		DUMP_REG(DC_WINBUF_START_ADDR_U);
		DUMP_REG(DC_WINBUF_START_ADDR_V);
		DUMP_REG(DC_WINBUF_ADDR_H_OFFSET);
		DUMP_REG(DC_WINBUF_ADDR_V_OFFSET);
		DUMP_REG(DC_WINBUF_UFLOW_STATUS);
		DUMP_REG(DC_WIN_CSC_YOF);
		DUMP_REG(DC_WIN_CSC_KYRGB);
		DUMP_REG(DC_WIN_CSC_KUR);
		DUMP_REG(DC_WIN_CSC_KVR);
		DUMP_REG(DC_WIN_CSC_KUG);
		DUMP_REG(DC_WIN_CSC_KVG);
		DUMP_REG(DC_WIN_CSC_KUB);
		DUMP_REG(DC_WIN_CSC_KVB);
	}

	DUMP_REG(DC_CMD_DISPLAY_POWER_CONTROL);
	DUMP_REG(DC_COM_PIN_OUTPUT_ENABLE2);
	DUMP_REG(DC_COM_PIN_OUTPUT_POLARITY2);
	DUMP_REG(DC_COM_PIN_OUTPUT_DATA2);
	DUMP_REG(DC_COM_PIN_INPUT_ENABLE2);
	DUMP_REG(DC_COM_PIN_OUTPUT_SELECT5);
	DUMP_REG(DC_DISP_DISP_SIGNAL_OPTIONS0);
	DUMP_REG(DC_DISP_M1_CONTROL);
	DUMP_REG(DC_COM_PM1_CONTROL);
	DUMP_REG(DC_COM_PM1_DUTY_CYCLE);
	DUMP_REG(DC_DISP_SD_CONTROL);

	tegra_dc_clk_disable(dc->clk);
	tegra_dc_io_end(dc);
}

#undef DUMP_REG

#ifdef DEBUG
static void dump_regs_print(void *data, const char *str)
{
	struct tegra_dc *dc = data;
	dev_dbg(&dc->ndev->dev, "%s", str);
}

static void dump_regs(struct tegra_dc *dc)
{
	_dump_regs(dc, dc, dump_regs_print);
}
#else /* !DEBUG */

static void dump_regs(struct tegra_dc *dc) {}

#endif /* DEBUG */

#ifdef CONFIG_DEBUG_FS

static void dbg_regs_print(void *data, const char *str)
{
	struct seq_file *s = data;

	seq_printf(s, "%s", str);
}

#undef DUMP_REG

static int dbg_dc_show(struct seq_file *s, void *unused)
{
	struct tegra_dc *dc = s->private;

	_dump_regs(dc, s, dbg_regs_print);

	return 0;
}


static int dbg_dc_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_dc_show, inode->i_private);
}

static const struct file_operations regs_fops = {
	.open		= dbg_dc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int dbg_dc_mode_show(struct seq_file *s, void *unused)
{
	struct tegra_dc *dc = s->private;
	struct tegra_dc_mode *m;

	mutex_lock(&dc->lock);
	m = &dc->mode;
	seq_printf(s,
		"pclk: %d\n"
		"h_ref_to_sync: %d\n"
		"v_ref_to_sync: %d\n"
		"h_sync_width: %d\n"
		"v_sync_width: %d\n"
		"h_back_porch: %d\n"
		"v_back_porch: %d\n"
		"h_active: %d\n"
		"v_active: %d\n"
		"h_front_porch: %d\n"
		"v_front_porch: %d\n"
		"stereo_mode: %d\n",
		m->pclk, m->h_ref_to_sync, m->v_ref_to_sync,
		m->h_sync_width, m->v_sync_width,
		m->h_back_porch, m->v_back_porch,
		m->h_active, m->v_active,
		m->h_front_porch, m->v_front_porch,
		m->stereo_mode);
	mutex_unlock(&dc->lock);
	return 0;
}

static int dbg_dc_mode_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_dc_mode_show, inode->i_private);
}

static const struct file_operations mode_fops = {
	.open		= dbg_dc_mode_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int dbg_dc_stats_show(struct seq_file *s, void *unused)
{
	struct tegra_dc *dc = s->private;

	mutex_lock(&dc->lock);
	seq_printf(s,
		"underflows: %llu\n"
		"underflows_a: %llu\n"
		"underflows_b: %llu\n"
		"underflows_c: %llu\n",
		dc->stats.underflows,
		dc->stats.underflows_a,
		dc->stats.underflows_b,
		dc->stats.underflows_c);
	mutex_unlock(&dc->lock);

	return 0;
}

static int dbg_dc_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_dc_stats_show, inode->i_private);
}

static const struct file_operations stats_fops = {
	.open		= dbg_dc_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void __devexit tegra_dc_remove_debugfs(struct tegra_dc *dc)
{
	if (dc->debugdir)
		debugfs_remove_recursive(dc->debugdir);
	dc->debugdir = NULL;
}

static void tegra_dc_create_debugfs(struct tegra_dc *dc)
{
	struct dentry *retval;

	dc->debugdir = debugfs_create_dir(dev_name(&dc->ndev->dev), NULL);
	if (!dc->debugdir)
		goto remove_out;

	retval = debugfs_create_file("regs", S_IRUGO, dc->debugdir, dc,
		&regs_fops);
	if (!retval)
		goto remove_out;

	retval = debugfs_create_file("mode", S_IRUGO, dc->debugdir, dc,
		&mode_fops);
	if (!retval)
		goto remove_out;

	retval = debugfs_create_file("stats", S_IRUGO, dc->debugdir, dc,
		&stats_fops);
	if (!retval)
		goto remove_out;

	return;
remove_out:
	dev_err(&dc->ndev->dev, "could not create debugfs\n");
	tegra_dc_remove_debugfs(dc);
}

#else /* !CONFIG_DEBUGFS */
static inline void tegra_dc_create_debugfs(struct tegra_dc *dc) { };
static inline void __devexit tegra_dc_remove_debugfs(struct tegra_dc *dc) { };
#endif /* CONFIG_DEBUGFS */

static int tegra_dc_set(struct tegra_dc *dc, int index)
{
	int ret = 0;

	mutex_lock(&tegra_dc_lock);
	if (index >= TEGRA_MAX_DC) {
		ret = -EINVAL;
		goto out;
	}

	if (dc != NULL && tegra_dcs[index] != NULL) {
		ret = -EBUSY;
		goto out;
	}

	tegra_dcs[index] = dc;

out:
	mutex_unlock(&tegra_dc_lock);

	return ret;
}

unsigned int tegra_dc_has_multiple_dc(void)
{
	unsigned int idx;
	unsigned int cnt = 0;
	struct tegra_dc *dc;

	mutex_lock(&tegra_dc_lock);
	for (idx = 0; idx < TEGRA_MAX_DC; idx++)
		cnt += ((dc = tegra_dcs[idx]) != NULL && dc->enabled) ? 1 : 0;
	mutex_unlock(&tegra_dc_lock);

	return (cnt > 1);
}

/* get the stride size of a window.
 * return: stride size in bytes for window win. or 0 if unavailble. */
int tegra_dc_get_stride(struct tegra_dc *dc, unsigned win)
{
	u32 tmp;
	u32 stride;

	if (!dc->enabled)
		return 0;
	BUG_ON(win > DC_N_WINDOWS);
	tegra_dc_writel(dc, WINDOW_A_SELECT << win,
		DC_CMD_DISPLAY_WINDOW_HEADER);
	tmp = tegra_dc_readl(dc, DC_WIN_LINE_STRIDE);
	return GET_LINE_STRIDE(tmp);
}
EXPORT_SYMBOL(tegra_dc_get_stride);

struct tegra_dc *tegra_dc_get_dc(unsigned idx)
{
	if (idx < TEGRA_MAX_DC)
		return tegra_dcs[idx];
	else
		return NULL;
}
EXPORT_SYMBOL(tegra_dc_get_dc);

struct tegra_dc_win *tegra_dc_get_window(struct tegra_dc *dc, unsigned win)
{
	if (win >= dc->n_windows)
		return NULL;

	return &dc->windows[win];
}
EXPORT_SYMBOL(tegra_dc_get_window);

static int get_topmost_window(u32 *depths, unsigned long *wins)
{
	int idx, best = -1;

	for_each_set_bit(idx, wins, DC_N_WINDOWS) {
		if (best == -1 || depths[idx] < depths[best])
			best = idx;
	}
	clear_bit(best, wins);
	return best;
}

bool tegra_dc_get_connected(struct tegra_dc *dc)
{
	return dc->connected;
}
EXPORT_SYMBOL(tegra_dc_get_connected);

bool tegra_dc_hpd(struct tegra_dc *dc)
{
	int sense;
	int level;

#ifdef	__SAMSUNG_HDMI_FLAG_WORKAROUND__
	int flag;
	flag = tegra_dc_get_hdmi_flag();

	if (flag == HDMI_FLAG_DISABLE) {
		printk(KERN_INFO "[HDMI] %s() HDMI_FLAG_DISABLE\n", __func__);
		level = 0;
	} else if (flag == HDMI_FLAG_ENABLE) {
		printk(KERN_INFO "[HDMI] %s() HDMI_FLAG_ENABLE\n", __func__);
		level = gpio_get_value(dc->out->hotplug_gpio);
	} else {
		printk(KERN_INFO "[HDMI] %s() flag = %d()\n", __func__, flag);
		level = 0;
	}
#else
	level = gpio_get_value(dc->out->hotplug_gpio);
#endif

	sense = dc->out->flags & TEGRA_DC_OUT_HOTPLUG_MASK;

	return (sense == TEGRA_DC_OUT_HOTPLUG_HIGH && level) ||
		(sense == TEGRA_DC_OUT_HOTPLUG_LOW && !level);
}
EXPORT_SYMBOL(tegra_dc_hpd);

static u32 blend_topwin(u32 flags)
{
	if (flags & TEGRA_WIN_FLAG_BLEND_COVERAGE)
		return BLEND(NOKEY, ALPHA, 0xff, 0xff);
	else if (flags & TEGRA_WIN_FLAG_BLEND_PREMULT)
		return BLEND(NOKEY, PREMULT, 0xff, 0xff);
	else
		return BLEND(NOKEY, FIX, 0xff, 0xff);
}

static u32 blend_2win(int idx, unsigned long behind_mask, u32* flags, int xy)
{
	int other;

	for (other = 0; other < DC_N_WINDOWS; other++) {
		if (other != idx && (xy-- == 0))
			break;
	}
	if (BIT(other) & behind_mask)
		return blend_topwin(flags[idx]);
	else if (flags[other])
		return BLEND(NOKEY, DEPENDANT, 0x00, 0x00);
	else
		return BLEND(NOKEY, FIX, 0x00, 0x00);
}

static u32 blend_3win(int idx, unsigned long behind_mask, u32* flags)
{
	unsigned long infront_mask;
	int first;

	infront_mask = ~(behind_mask | BIT(idx));
	infront_mask &= (BIT(DC_N_WINDOWS) - 1);
	first = ffs(infront_mask) - 1;

	if (!infront_mask)
		return blend_topwin(flags[idx]);
	else if (behind_mask && first != -1 && flags[first])
		return BLEND(NOKEY, DEPENDANT, 0x00, 0x00);
	else
		return BLEND(NOKEY, FIX, 0x0, 0x0);
}

static void tegra_dc_set_blending(struct tegra_dc *dc, struct tegra_dc_blend *blend)
{
	unsigned long mask = BIT(DC_N_WINDOWS) - 1;

	while (mask) {
		int idx = get_topmost_window(blend->z, &mask);

		tegra_dc_writel(dc, WINDOW_A_SELECT << idx,
				DC_CMD_DISPLAY_WINDOW_HEADER);
		tegra_dc_writel(dc, BLEND(NOKEY, FIX, 0xff, 0xff),
				DC_WIN_BLEND_NOKEY);
		tegra_dc_writel(dc, BLEND(NOKEY, FIX, 0xff, 0xff),
				DC_WIN_BLEND_1WIN);
		tegra_dc_writel(dc, blend_2win(idx, mask, blend->flags, 0),
				DC_WIN_BLEND_2WIN_X);
		tegra_dc_writel(dc, blend_2win(idx, mask, blend->flags, 1),
				DC_WIN_BLEND_2WIN_Y);
		tegra_dc_writel(dc, blend_3win(idx, mask, blend->flags),
				DC_WIN_BLEND_3WIN_XY);
	}
}

static void tegra_dc_init_csc_defaults(struct tegra_dc_csc *csc)
{
	csc->yof   = 0x00f0;
	csc->kyrgb = 0x012a;
	csc->kur   = 0x0000;
	csc->kvr   = 0x0198;
	csc->kug   = 0x039b;
	csc->kvg   = 0x032f;
	csc->kub   = 0x0204;
	csc->kvb   = 0x0000;
}

static void tegra_dc_set_csc(struct tegra_dc *dc, struct tegra_dc_csc *csc)
{
	tegra_dc_writel(dc, csc->yof,	DC_WIN_CSC_YOF);
	tegra_dc_writel(dc, csc->kyrgb,	DC_WIN_CSC_KYRGB);
	tegra_dc_writel(dc, csc->kur,	DC_WIN_CSC_KUR);
	tegra_dc_writel(dc, csc->kvr,	DC_WIN_CSC_KVR);
	tegra_dc_writel(dc, csc->kug,	DC_WIN_CSC_KUG);
	tegra_dc_writel(dc, csc->kvg,	DC_WIN_CSC_KVG);
	tegra_dc_writel(dc, csc->kub,	DC_WIN_CSC_KUB);
	tegra_dc_writel(dc, csc->kvb,	DC_WIN_CSC_KVB);
}

int tegra_dc_update_csc(struct tegra_dc *dc, int win_idx)
{
	mutex_lock(&dc->lock);

	if (!dc->enabled) {
		mutex_unlock(&dc->lock);
		return -EFAULT;
	}

	tegra_dc_writel(dc, WINDOW_A_SELECT << win_idx,
			DC_CMD_DISPLAY_WINDOW_HEADER);

	tegra_dc_set_csc(dc, &dc->windows[win_idx].csc);

	mutex_unlock(&dc->lock);

	return 0;
}
EXPORT_SYMBOL(tegra_dc_update_csc);

static void tegra_dc_init_lut_defaults(struct tegra_dc_lut *lut)
{
	int i;
	for (i = 0; i < 256; i++)
		lut->r[i] = lut->g[i] = lut->b[i] = (u8)i;
}

static int tegra_dc_loop_lut(struct tegra_dc *dc,
			     struct tegra_dc_win *win,
			     int(*lambda)(struct tegra_dc *dc, int i, u32 rgb))
{
	struct tegra_dc_lut *lut = &win->lut;
	struct tegra_dc_lut *global_lut = &dc->fb_lut;
	int i;
	for (i = 0; i < 256; i++) {

		u32 r = (u32)lut->r[i];
		u32 g = (u32)lut->g[i];
		u32 b = (u32)lut->b[i];

		if (!(win->ppflags & TEGRA_WIN_PPFLAG_CP_FBOVERRIDE)) {
			r = (u32)global_lut->r[r];
			g = (u32)global_lut->g[g];
			b = (u32)global_lut->b[b];
		}

		if (!lambda(dc, i, r | (g<<8) | (b<<16)))
			return 0;
	}
	return 1;
}

static int tegra_dc_lut_isdefaults_lambda(struct tegra_dc *dc, int i, u32 rgb)
{
	if (rgb != (i | (i<<8) | (i<<16)))
		return 0;
	return 1;
}

static int tegra_dc_set_lut_setreg_lambda(struct tegra_dc *dc, int i, u32 rgb)
{
	tegra_dc_writel(dc, rgb, DC_WIN_COLOR_PALETTE(i));
	return 1;
}

static void tegra_dc_set_lut(struct tegra_dc *dc, struct tegra_dc_win* win)
{
	unsigned long val = tegra_dc_readl(dc, DC_WIN_WIN_OPTIONS);

	tegra_dc_loop_lut(dc, win, tegra_dc_set_lut_setreg_lambda);

	if (win->ppflags & TEGRA_WIN_PPFLAG_CP_ENABLE)
		val |= CP_ENABLE;
	else
		val &= ~CP_ENABLE;

	tegra_dc_writel(dc, val, DC_WIN_WIN_OPTIONS);
}

static int tegra_dc_update_winlut(struct tegra_dc *dc, int win_idx, int fbovr)
{
	struct tegra_dc_win *win = &dc->windows[win_idx];

	mutex_lock(&dc->lock);

	if (!dc->enabled) {
		mutex_unlock(&dc->lock);
		return -EFAULT;
	}

	if (fbovr > 0)
		win->ppflags |= TEGRA_WIN_PPFLAG_CP_FBOVERRIDE;
	else if (fbovr == 0)
		win->ppflags &= ~TEGRA_WIN_PPFLAG_CP_FBOVERRIDE;

	if (!tegra_dc_loop_lut(dc, win, tegra_dc_lut_isdefaults_lambda))
		win->ppflags |= TEGRA_WIN_PPFLAG_CP_ENABLE;
	else
		win->ppflags &= ~TEGRA_WIN_PPFLAG_CP_ENABLE;

	tegra_dc_writel(dc, WINDOW_A_SELECT << win_idx,
			DC_CMD_DISPLAY_WINDOW_HEADER);

	tegra_dc_set_lut(dc, win);

	mutex_unlock(&dc->lock);

	tegra_dc_update_windows(&win, 1);

	return 0;
}

int tegra_dc_update_lut(struct tegra_dc *dc, int win_idx, int fboveride)
{
	if (win_idx > -1)
		return tegra_dc_update_winlut(dc, win_idx, fboveride);

	for (win_idx = 0; win_idx < DC_N_WINDOWS; win_idx++) {
		int err = tegra_dc_update_winlut(dc, win_idx, fboveride);
		if (err)
			return err;
	}

	return 0;
}
EXPORT_SYMBOL(tegra_dc_update_lut);

static void tegra_dc_set_scaling_filter(struct tegra_dc *dc)
{
	unsigned i;
	unsigned v0 = 128;
	unsigned v1 = 0;
	/* linear horizontal and vertical filters */
	for (i = 0; i < 16; i++) {
		tegra_dc_writel(dc, (v1 << 16) | (v0 << 8),
				DC_WIN_H_FILTER_P(i));

		tegra_dc_writel(dc, v0,
				DC_WIN_V_FILTER_P(i));
		v0 -= 8;
		v1 += 8;
	}
}

void tegra_dc_host_suspend(struct tegra_dc *dc)
{
	tegra_dsi_host_suspend(dc);
	tegra_dc_clk_disable(dc);
}

void tegra_dc_host_resume(struct tegra_dc *dc) {
	tegra_dc_clk_enable(dc);
	tegra_dsi_host_resume(dc);
}

static inline u32 compute_dda_inc(fixed20_12 in, unsigned out_int,
				  bool v, unsigned Bpp)
{
	/*
	 * min(round((prescaled_size_in_pixels - 1) * 0x1000 /
	 *	     (post_scaled_size_in_pixels - 1)), MAX)
	 * Where the value of MAX is as follows:
	 * For V_DDA_INCREMENT: 15.0 (0xF000)
	 * For H_DDA_INCREMENT:  4.0 (0x4000) for 4 Bytes/pix formats.
	 *			 8.0 (0x8000) for 2 Bytes/pix formats.
	 */

	fixed20_12 out = dfixed_init(out_int);
	u32 dda_inc;
	int max;

	if (v) {
		max = 15;
	} else {
		switch (Bpp) {
		default:
			WARN_ON_ONCE(1);
			/* fallthrough */
		case 4:
			max = 4;
			break;
		case 2:
			max = 8;
			break;
		}
	}

	out.full = max_t(u32, out.full - dfixed_const(1), dfixed_const(1));
	in.full -= dfixed_const(1);

	dda_inc = dfixed_div(in, out);

	dda_inc = min_t(u32, dda_inc, dfixed_const(max));

	return dda_inc;
}

static inline u32 compute_initial_dda(fixed20_12 in)
{
	return dfixed_frac(in);
}

/* does not support updating windows on multiple dcs in one call */
int tegra_dc_update_windows(struct tegra_dc_win *windows[], int n)
{
	struct tegra_dc *dc;
	unsigned long update_mask = GENERAL_ACT_REQ;
	unsigned long val;
	bool update_blend = false;
	int i;

	dc = windows[0]->dc;

	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_MODE) {
		/* Acquire one_shot_lock to avoid race condition between
		 * cancellation of old delayed work and schedule of new
		 * delayed work. */
		mutex_lock(&dc->one_shot_lock);
		cancel_delayed_work_sync(&dc->one_shot_work);
	}
	mutex_lock(&dc->lock);

	if (!dc->enabled) {
		mutex_unlock(&dc->lock);
		if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_MODE)
			mutex_unlock(&dc->one_shot_lock);
		return -EFAULT;
	}

	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_LP_MODE)
		tegra_dc_host_resume(dc);

	val = tegra_dc_readl(dc, DC_CMD_INT_MASK);
	val &= ~(FRAME_END_INT | V_BLANK_INT | ALL_UF_INT);
	tegra_dc_writel(dc, val, DC_CMD_INT_MASK);

	if (no_vsync)
		tegra_dc_writel(dc, WRITE_MUX_ACTIVE | READ_MUX_ACTIVE, DC_CMD_STATE_ACCESS);
	else
		tegra_dc_writel(dc, WRITE_MUX_ASSEMBLY | READ_MUX_ASSEMBLY, DC_CMD_STATE_ACCESS);

	for (i = 0; i < DC_N_WINDOWS; i++) {
		tegra_dc_writel(dc, WINDOW_A_SELECT << i,
					DC_CMD_DISPLAY_WINDOW_HEADER);
		tegra_dc_writel(dc, 0, DC_WIN_WIN_OPTIONS);
		if (!no_vsync)
			update_mask |= WIN_A_ACT_REQ << i;
	}

	for (i = 0; i < n; i++) {
		struct tegra_dc_win *win = windows[i];
		unsigned h_dda;
		unsigned v_dda;
		fixed20_12 h_offset, v_offset;
		bool invert_h = (win->flags & TEGRA_WIN_FLAG_INVERT_H) != 0;
		bool invert_v = (win->flags & TEGRA_WIN_FLAG_INVERT_V) != 0;
		bool yuv = tegra_dc_is_yuv(win->fmt);
		bool yuvp = tegra_dc_is_yuv_planar(win->fmt);
		unsigned Bpp = tegra_dc_fmt_bpp(win->fmt) / 8;
		/* Bytes per pixel of bandwidth, used for dda_inc calculation */
		unsigned Bpp_bw = Bpp * (yuvp ? 2 : 1);
		const bool filter_h = win_use_h_filter(dc, win);
		const bool filter_v = win_use_v_filter(dc, win);

		if (win->z != dc->blend.z[win->idx]) {
			dc->blend.z[win->idx] = win->z;
			update_blend = true;
		}
		if ((win->flags & TEGRA_WIN_BLEND_FLAGS_MASK) !=
			dc->blend.flags[win->idx]) {
			dc->blend.flags[win->idx] =
				win->flags & TEGRA_WIN_BLEND_FLAGS_MASK;
			update_blend = true;
		}

		tegra_dc_writel(dc, WINDOW_A_SELECT << win->idx,
				DC_CMD_DISPLAY_WINDOW_HEADER);

		if (!no_vsync)
			update_mask |= WIN_A_ACT_REQ << win->idx;

		if (!WIN_IS_ENABLED(win)) {
			tegra_dc_writel(dc, 0, DC_WIN_WIN_OPTIONS);
			continue;
		}

		tegra_dc_writel(dc, win->fmt & 0x1f, DC_WIN_COLOR_DEPTH);
		tegra_dc_writel(dc, win->fmt >> 6, DC_WIN_BYTE_SWAP);

		tegra_dc_writel(dc,
				V_POSITION(win->out_y) | H_POSITION(win->out_x),
				DC_WIN_POSITION);
		tegra_dc_writel(dc,
				V_SIZE(win->out_h) | H_SIZE(win->out_w),
				DC_WIN_SIZE);
		if (tegra_dc_feature_has_scaling(dc, win->idx)) {
			tegra_dc_writel(dc,
					V_PRESCALED_SIZE(dfixed_trunc(win->h)) |
					H_PRESCALED_SIZE(dfixed_trunc(win->w) * Bpp),
					DC_WIN_PRESCALED_SIZE);

			h_dda = compute_dda_inc(win->w, win->out_w, false, Bpp_bw);
			v_dda = compute_dda_inc(win->h, win->out_h, true, Bpp_bw);
			tegra_dc_writel(dc, V_DDA_INC(v_dda) | H_DDA_INC(h_dda),
					DC_WIN_DDA_INCREMENT);
			h_dda = compute_initial_dda(win->x);
			v_dda = compute_initial_dda(win->y);
			tegra_dc_writel(dc, h_dda, DC_WIN_H_INITIAL_DDA);
			tegra_dc_writel(dc, v_dda, DC_WIN_V_INITIAL_DDA);
		}

		tegra_dc_writel(dc, 0, DC_WIN_BUF_STRIDE);
		tegra_dc_writel(dc, 0, DC_WIN_UV_BUF_STRIDE);
		tegra_dc_writel(dc,
				(unsigned long)win->phys_addr,
				DC_WINBUF_START_ADDR);

		if (!yuvp) {
			tegra_dc_writel(dc, win->stride, DC_WIN_LINE_STRIDE);
		} else {
			tegra_dc_writel(dc,
					(unsigned long)win->phys_addr_u,
					DC_WINBUF_START_ADDR_U);
			tegra_dc_writel(dc,
					(unsigned long)win->phys_addr_v,
					DC_WINBUF_START_ADDR_V);
			tegra_dc_writel(dc,
					LINE_STRIDE(win->stride) |
					UV_LINE_STRIDE(win->stride_uv),
					DC_WIN_LINE_STRIDE);
		}

		h_offset = win->x;
		if (invert_h) {
			h_offset.full += win->w.full - dfixed_const(1);
		}

		v_offset = win->y;
		if (invert_v) {
			v_offset.full += win->h.full - dfixed_const(1);
		}

		tegra_dc_writel(dc, dfixed_trunc(h_offset) * Bpp,
				DC_WINBUF_ADDR_H_OFFSET);
		tegra_dc_writel(dc, dfixed_trunc(v_offset),
				DC_WINBUF_ADDR_V_OFFSET);

		if (tegra_dc_feature_has_tiling(dc, win->idx)) {
			if (WIN_IS_TILED(win))
				tegra_dc_writel(dc,
						DC_WIN_BUFFER_ADDR_MODE_TILE |
						DC_WIN_BUFFER_ADDR_MODE_TILE_UV,
						DC_WIN_BUFFER_ADDR_MODE);
			else
				tegra_dc_writel(dc,
						DC_WIN_BUFFER_ADDR_MODE_LINEAR |
						DC_WIN_BUFFER_ADDR_MODE_LINEAR_UV,
						DC_WIN_BUFFER_ADDR_MODE);
		}

		val = WIN_ENABLE;
		if (yuv)
			val |= CSC_ENABLE;
		else if (tegra_dc_fmt_bpp(win->fmt) < 24)
			val |= COLOR_EXPAND;

		if (win->ppflags & TEGRA_WIN_PPFLAG_CP_ENABLE)
			val |= CP_ENABLE;

		if (filter_h)
			val |= H_FILTER_ENABLE;
		if (filter_v)
			val |= V_FILTER_ENABLE;

		if (invert_h)
			val |= H_DIRECTION_DECREMENT;
		if (invert_v)
			val |= V_DIRECTION_DECREMENT;

		tegra_dc_writel(dc, val, DC_WIN_WIN_OPTIONS);

		win->dirty = no_vsync ? 0 : 1;

		dev_dbg(&dc->ndev->dev, "%s():idx=%d z=%d x=%d y=%d w=%d h=%d "
			"out_x=%u out_y=%u out_w=%u out_h=%u "
			"fmt=%d yuvp=%d Bpp=%u filter_h=%d filter_v=%d",
			__func__, win->idx, win->z,
			dfixed_trunc(win->x), dfixed_trunc(win->y),
			dfixed_trunc(win->w), dfixed_trunc(win->h),
			win->out_x, win->out_y, win->out_w, win->out_h,
			win->fmt, yuvp, Bpp, filter_h, filter_v);
	}

	if (update_blend) {
		tegra_dc_set_blending(dc, &dc->blend);
		for (i = 0; i < DC_N_WINDOWS; i++) {
			if (!no_vsync)
				dc->windows[i].dirty = 1;
			update_mask |= WIN_A_ACT_REQ << i;
		}
	}

	tegra_dc_set_dynamic_emc(windows, n);

	tegra_dc_writel(dc, update_mask << 8, DC_CMD_STATE_CONTROL);

	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_MODE)
		schedule_delayed_work(&dc->one_shot_work,
				msecs_to_jiffies(dc->one_shot_delay_ms));

	/* update EMC clock if calculated bandwidth has changed */
	tegra_dc_program_bandwidth(dc, false);

	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_MODE)
		update_mask |= NC_HOST_TRIG;

	tegra_dc_writel(dc, update_mask, DC_CMD_STATE_CONTROL);

	/* clean & enable DC interrupts */
	tegra_dc_writel(dc, FRAME_END_INT | V_BLANK_INT, DC_CMD_INT_STATUS);
	if (!no_vsync) {
		val = tegra_dc_readl(dc, DC_CMD_INT_MASK);
		val |= (FRAME_END_INT | V_BLANK_INT | ALL_UF_INT);
		tegra_dc_writel(dc, val, DC_CMD_INT_MASK);
	} else {
		val = tegra_dc_readl(dc, DC_CMD_INT_MASK);
		val &= ~(FRAME_END_INT | V_BLANK_INT | ALL_UF_INT);
		tegra_dc_writel(dc, val, DC_CMD_INT_MASK);
	}

	mutex_unlock(&dc->lock);
	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_MODE)
		mutex_unlock(&dc->one_shot_lock);

	return 0;
}
EXPORT_SYMBOL(tegra_dc_update_windows);

u32 tegra_dc_get_syncpt_id(const struct tegra_dc *dc, int i)
{
	return dc->syncpt[i].id;
}
EXPORT_SYMBOL(tegra_dc_get_syncpt_id);

u32 tegra_dc_incr_syncpt_max(struct tegra_dc *dc, int i)
{
	u32 max;

	mutex_lock(&dc->lock);
	max = nvhost_syncpt_incr_max(&nvhost_get_host(dc->ndev)->syncpt,
		dc->syncpt[i].id, ((dc->enabled) ? 1 : 0));
	dc->syncpt[i].max = max;
	mutex_unlock(&dc->lock);

	return max;
}

void tegra_dc_incr_syncpt_min(struct tegra_dc *dc, int i, u32 val)
{
	mutex_lock(&dc->lock);
	if (dc->enabled)
		while (dc->syncpt[i].min < val) {
			dc->syncpt[i].min++;
			nvhost_syncpt_cpu_incr(
					&nvhost_get_host(dc->ndev)->syncpt,
					dc->syncpt[i].id);
		}
	mutex_unlock(&dc->lock);
}

static bool tegra_dc_windows_are_clean(struct tegra_dc_win *windows[],
					     int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (windows[i]->dirty)
			return false;
	}

	return true;
}

/* does not support syncing windows on multiple dcs in one call */
int tegra_dc_sync_windows(struct tegra_dc_win *windows[], int n)
{
	if (n < 1 || n > DC_N_WINDOWS)
		return -EINVAL;

	if (!windows[0]->dc->enabled)
		return -EFAULT;

#ifdef CONFIG_TEGRA_SIMULATION_PLATFORM
	/* Don't want to timeout on simulator */
	return wait_event_interruptible(windows[0]->dc->wq,
		tegra_dc_windows_are_clean(windows, n));
#else
	return wait_event_interruptible_timeout(windows[0]->dc->wq,
					 tegra_dc_windows_are_clean(windows, n),
					 HZ);
#endif
}
EXPORT_SYMBOL(tegra_dc_sync_windows);

static unsigned long tegra_dc_clk_get_rate(struct tegra_dc *dc)
{
#ifdef CONFIG_TEGRA_SILICON_PLATFORM
	return clk_get_rate(dc->clk);
#else
	return 27000000;
#endif
}

static unsigned long tegra_dc_pclk_round_rate(struct tegra_dc *dc, int pclk)
{
	unsigned long rate;
	unsigned long div;

	rate = tegra_dc_clk_get_rate(dc);

	div = DIV_ROUND_CLOSEST(rate * 2, pclk);

	if (div < 2)
		return 0;

	return rate * 2 / div;
}

static unsigned long tegra_dc_pclk_predict_rate(struct clk *parent, int pclk)
{
	unsigned long rate;
	unsigned long div;

	rate = clk_get_rate(parent);

	div = DIV_ROUND_CLOSEST(rate * 2, pclk);

	if (div < 2)
		return 0;

	return rate * 2 / div;
}

void tegra_dc_setup_clk(struct tegra_dc *dc, struct clk *clk)
{
	int pclk;

	if (dc->out->type == TEGRA_DC_OUT_RGB) {
		unsigned long rate;

		struct clk *pll_c_clk = clk_get_sys(NULL, "pll_c");

		struct clk *pll_p_clk = clk_get_sys(NULL, "pll_p");

		switch (dc->mode.pclk) {
		case 68941176:
			rate = 586000000;
			if (clk_get_parent(clk) != pll_c_clk)
				clk_set_parent(clk, pll_c_clk);

			if (rate != clk_get_rate(pll_c_clk))
				clk_set_rate(pll_c_clk, rate);
		break;

		case 70000000:
		case 74666667:
			rate = 560000000;
			if (clk_get_parent(clk) != pll_c_clk)
				clk_set_parent(clk, pll_c_clk);

			if (rate != clk_get_rate(pll_c_clk))
				clk_set_rate(pll_c_clk, rate);
		break;
		case 72000000:
			if (clk_get_parent(clk) != pll_p_clk)
				clk_set_parent(clk, pll_p_clk);

		break;

		case 76000000:
			rate = 570000000;
			if (clk_get_parent(clk) != pll_c_clk)
				clk_set_parent(clk, pll_c_clk);

			if (rate != clk_get_rate(pll_c_clk))
				clk_set_rate(pll_c_clk, rate);
		break;

		case 75500000:
			rate = 453000000;
			if (clk_get_parent(clk) != pll_c_clk)
				clk_set_parent(clk, pll_c_clk);

			if (rate != clk_get_rate(pll_c_clk))
				clk_set_rate(pll_c_clk, rate);
		break;
		}

	}

	if (dc->out->type == TEGRA_DC_OUT_HDMI) {
		unsigned long rate;
		struct clk *parent_clk =
			clk_get_sys(NULL, dc->out->parent_clk ? : "pll_d_out0");
		struct clk *base_clk = clk_get_parent(parent_clk);

		/*
		 * Providing dynamic frequency rate setting for T20/T30 HDMI.
		 * The required rate needs to be setup at 4x multiplier,
		 * as out0 is 1/2 of the actual PLL output.
		 */

		rate = dc->mode.pclk * 4;
		if (rate != clk_get_rate(base_clk))
			clk_set_rate(base_clk, rate);

		if (clk_get_parent(clk) != parent_clk)
			clk_set_parent(clk, parent_clk);
		}

	if (dc->out->type == TEGRA_DC_OUT_DSI) {
		unsigned long rate;
		struct clk *parent_clk;
		struct clk *base_clk;

		if (clk == dc->clk) {
			parent_clk = clk_get_sys(NULL,
					dc->out->parent_clk ? : "pll_d_out0");
			base_clk = clk_get_parent(parent_clk);
			tegra_clk_cfg_ex(base_clk,
					TEGRA_CLK_PLLD_DSI_OUT_ENB, 1);
		} else {
			if (dc->pdata->default_out->dsi->dsi_instance) {
				parent_clk = clk_get_sys(NULL,
					dc->out->parent_clk ? : "pll_d2_out0");
				base_clk = clk_get_parent(parent_clk);
				tegra_clk_cfg_ex(base_clk,
						TEGRA_CLK_PLLD_CSI_OUT_ENB, 1);
			} else {
				parent_clk = clk_get_sys(NULL,
					dc->out->parent_clk ? : "pll_d_out0");
				base_clk = clk_get_parent(parent_clk);
				tegra_clk_cfg_ex(base_clk,
						TEGRA_CLK_PLLD_DSI_OUT_ENB, 1);
			}
		}

		/* divide by 1000 to avoid overflow */
		dc->mode.pclk /= 1000;
		rate = (dc->mode.pclk * dc->shift_clk_div.mul * 2)
					/ dc->shift_clk_div.div;
		rate *= 1000;
		dc->mode.pclk *= 1000;

		if (rate != clk_get_rate(base_clk))
			clk_set_rate(base_clk, rate);

		if (clk_get_parent(clk) != parent_clk)
			clk_set_parent(clk, parent_clk);
	}

	pclk = tegra_dc_pclk_round_rate(dc, dc->mode.pclk);
	printk(KERN_INFO "tegra_dc_setup_clk :: set pclk to %d\n", pclk);
	tegra_dvfs_set_rate(clk, pclk);
}

/* return non-zero if constraint is violated */
static int calc_h_ref_to_sync(const struct tegra_dc_mode *mode, int *href)
{
	long a, b;

	/* Constraint 5: H_REF_TO_SYNC >= 0 */
	a = 0;

	/* Constraint 6: H_FRONT_PORT >= (H_REF_TO_SYNC + 1) */
	b = mode->h_front_porch - 1;

	/* Constraint 1: H_REF_TO_SYNC + H_SYNC_WIDTH + H_BACK_PORCH > 11 */
	if (a + mode->h_sync_width + mode->h_back_porch <= 11)
		a = 1 + 11 - mode->h_sync_width - mode->h_back_porch;
	/* check Constraint 1 and 6 */
	if (a > b)
		return 1;

	/* Constraint 4: H_SYNC_WIDTH >= 1 */
	if (mode->h_sync_width < 1)
		return 4;

	/* Constraint 7: H_DISP_ACTIVE >= 16 */
	if (mode->h_active < 16)
		return 7;

	if (href) {
		if (b > a && a % 2)
			*href = a + 1; /* use smallest even value */
		else
			*href = a; /* even or only possible value */
	}

	return 0;
}

static int calc_v_ref_to_sync(const struct tegra_dc_mode *mode, int *vref)
{
	long a;
	a = 1; /* Constraint 5: V_REF_TO_SYNC >= 1 */

	/* Constraint 2: V_REF_TO_SYNC + V_SYNC_WIDTH + V_BACK_PORCH > 1 */
	if (a + mode->v_sync_width + mode->v_back_porch <= 1)
		a = 1 + 1 - mode->v_sync_width - mode->v_back_porch;

	/* Constraint 6 */
	if (mode->v_front_porch < a + 1)
		a = mode->v_front_porch - 1;

	/* Constraint 4: V_SYNC_WIDTH >= 1 */
	if (mode->v_sync_width < 1)
		return 4;

	/* Constraint 7: V_DISP_ACTIVE >= 16 */
	if (mode->v_active < 16)
		return 7;

	if (vref)
		*vref = a;
	return 0;
}

static int calc_ref_to_sync(struct tegra_dc_mode *mode)
{
	int ret;
	ret = calc_h_ref_to_sync(mode, &mode->h_ref_to_sync);
	if (ret)
		return ret;
	ret = calc_v_ref_to_sync(mode, &mode->v_ref_to_sync);
	if (ret)
		return ret;

	return 0;
}

static bool check_ref_to_sync(struct tegra_dc_mode *mode)
{
	/* Constraint 1: H_REF_TO_SYNC + H_SYNC_WIDTH + H_BACK_PORCH > 11. */
	if (mode->h_ref_to_sync + mode->h_sync_width + mode->h_back_porch <= 11)
		return false;

	/* Constraint 2: V_REF_TO_SYNC + V_SYNC_WIDTH + V_BACK_PORCH > 1. */
	if (mode->v_ref_to_sync + mode->v_sync_width + mode->v_back_porch <= 1)
		return false;

	/* Constraint 3: V_FRONT_PORCH + V_SYNC_WIDTH + V_BACK_PORCH > 1
	 * (vertical blank). */
	if (mode->v_front_porch + mode->v_sync_width + mode->v_back_porch <= 1)
		return false;

	/* Constraint 4: V_SYNC_WIDTH >= 1; H_SYNC_WIDTH >= 1. */
	if (mode->v_sync_width < 1 || mode->h_sync_width < 1)
		return false;

	/* Constraint 5: V_REF_TO_SYNC >= 1; H_REF_TO_SYNC >= 0. */
	if (mode->v_ref_to_sync < 1 || mode->h_ref_to_sync < 0)
		return false;

	/* Constraint 6: V_FRONT_PORT >= (V_REF_TO_SYNC + 1);
	 * H_FRONT_PORT >= (H_REF_TO_SYNC + 1). */
	if (mode->v_front_porch < mode->v_ref_to_sync + 1 ||
		mode->h_front_porch < mode->h_ref_to_sync + 1)
		return false;

	/* Constraint 7: H_DISP_ACTIVE >= 16; V_DISP_ACTIVE >= 16. */
	if (mode->h_active < 16 || mode->v_active < 16)
		return false;

	return true;
}

#ifdef DEBUG
/* return in 1000ths of a Hertz */
static int calc_refresh(const struct tegra_dc_mode *m)
{
	long h_total, v_total, refresh;
	h_total = m->h_active + m->h_front_porch + m->h_back_porch +
		m->h_sync_width;
	v_total = m->v_active + m->v_front_porch + m->v_back_porch +
		m->v_sync_width;
	refresh = m->pclk / h_total;
	refresh *= 1000;
	refresh /= v_total;
	return refresh;
}

static void print_mode(struct tegra_dc *dc,
			const struct tegra_dc_mode *mode, const char *note)
{
	if (mode) {
		int refresh = calc_refresh(dc, mode);
		dev_info(&dc->ndev->dev, "%s():MODE:%dx%d@%d.%03uHz pclk=%d\n",
			note ? note : "",
			mode->h_active, mode->v_active,
			refresh / 1000, refresh % 1000,
			mode->pclk);
	}
}
#else /* !DEBUG */
static inline void print_mode(struct tegra_dc *dc,
			const struct tegra_dc_mode *mode, const char *note) { }
#endif /* DEBUG */

static inline void enable_dc_irq(unsigned int irq)
{
#ifndef CONFIG_TEGRA_FPGA_PLATFORM
	enable_irq(irq);
#else
	/* Always disable DC interrupts on FPGA. */
	disable_irq(irq);
#endif
}

static inline void disable_dc_irq(unsigned int irq)
{
	disable_irq(irq);
}

static int tegra_dc_program_mode(struct tegra_dc *dc, struct tegra_dc_mode *mode)
{
	unsigned long val;
	unsigned long rate;
	unsigned long div;
	unsigned long pclk;

	print_mode(dc, mode, __func__);

	/* use default EMC rate when switching modes */
	dc->new_emc_clk_rate = tegra_dc_get_default_emc_clk_rate(dc);
	tegra_dc_program_bandwidth(dc, true);

	tegra_dc_writel(dc, 0x0, DC_DISP_DISP_TIMING_OPTIONS);
	tegra_dc_writel(dc, mode->h_ref_to_sync | (mode->v_ref_to_sync << 16),
			DC_DISP_REF_TO_SYNC);
	tegra_dc_writel(dc, mode->h_sync_width | (mode->v_sync_width << 16),
			DC_DISP_SYNC_WIDTH);
	tegra_dc_writel(dc, mode->h_back_porch | (mode->v_back_porch << 16),
			DC_DISP_BACK_PORCH);
	tegra_dc_writel(dc, mode->h_active | (mode->v_active << 16),
			DC_DISP_DISP_ACTIVE);
	tegra_dc_writel(dc, mode->h_front_porch | (mode->v_front_porch << 16),
			DC_DISP_FRONT_PORCH);

	tegra_dc_writel(dc, DE_SELECT_ACTIVE | DE_CONTROL_NORMAL,
			DC_DISP_DATA_ENABLE_OPTIONS);

	/* TODO: MIPI/CRT/HDMI clock cals */

	val = DISP_DATA_FORMAT_DF1P1C;

	if (dc->out->align == TEGRA_DC_ALIGN_MSB)
		val |= DISP_DATA_ALIGNMENT_MSB;
	else
		val |= DISP_DATA_ALIGNMENT_LSB;

	if (dc->out->order == TEGRA_DC_ORDER_RED_BLUE)
		val |= DISP_DATA_ORDER_RED_BLUE;
	else
		val |= DISP_DATA_ORDER_BLUE_RED;

	tegra_dc_writel(dc, val, DC_DISP_DISP_INTERFACE_CONTROL);

	rate = tegra_dc_clk_get_rate(dc);

	pclk = tegra_dc_pclk_round_rate(dc, mode->pclk);
	if (pclk < (mode->pclk / 100 * 99) ||
	    pclk > (mode->pclk / 100 * 109)) {
		dev_err(&dc->ndev->dev,
			"can't divide %ld clock to %d -1/+9%% %ld %d %d\n",
			rate, mode->pclk,
			pclk, (mode->pclk / 100 * 99),
			(mode->pclk / 100 * 109));
		return -EINVAL;
	}

	div = (rate * 2 / pclk) - 2;

	tegra_dc_writel(dc, 0x00010001,
			DC_DISP_SHIFT_CLOCK_OPTIONS);
	tegra_dc_writel(dc, PIXEL_CLK_DIVIDER_PCD1 | SHIFT_CLK_DIVIDER(div),
			DC_DISP_DISP_CLOCK_CONTROL);

#ifdef CONFIG_SWITCH
	switch_set_state(&dc->modeset_switch,
			 (mode->h_active << 16) | mode->v_active);
#endif

	tegra_dc_writel(dc, GENERAL_UPDATE, DC_CMD_STATE_CONTROL);
	tegra_dc_writel(dc, GENERAL_ACT_REQ, DC_CMD_STATE_CONTROL);

	return 0;
}


int tegra_dc_set_mode(struct tegra_dc *dc, const struct tegra_dc_mode *mode)
{
	memcpy(&dc->mode, mode, sizeof(dc->mode));

	print_mode(dc, mode, __func__);

	return 0;
}
EXPORT_SYMBOL(tegra_dc_set_mode);

int tegra_dc_set_fb_mode(struct tegra_dc *dc,
		const struct fb_videomode *fbmode, bool stereo_mode)
{
	struct tegra_dc_mode mode;

	if (!fbmode->pixclock)
		return -EINVAL;

	mode.pclk = PICOS2KHZ(fbmode->pixclock) * 1000;
	mode.h_sync_width = fbmode->hsync_len;
	mode.v_sync_width = fbmode->vsync_len;
	mode.h_back_porch = fbmode->left_margin;
	mode.v_back_porch = fbmode->upper_margin;
	mode.h_active = fbmode->xres;
	mode.v_active = fbmode->yres;
	mode.h_front_porch = fbmode->right_margin;
	mode.v_front_porch = fbmode->lower_margin;
	mode.stereo_mode = stereo_mode;
	if (dc->out->type == TEGRA_DC_OUT_HDMI) {
		/* HDMI controller requires h_ref=1, v_ref=1 */
		mode.h_ref_to_sync = 1;
		mode.v_ref_to_sync = 1;
	} else {
		calc_ref_to_sync(&mode);
	}
	if (!check_ref_to_sync(&mode)) {
		dev_err(&dc->ndev->dev,
				"Display timing doesn't meet restrictions.\n");
		return -EINVAL;
	}
	dev_info(&dc->ndev->dev, "Using mode %dx%d pclk=%d href=%d vref=%d\n",
		mode.h_active, mode.v_active, mode.pclk,
		mode.h_ref_to_sync, mode.v_ref_to_sync
	);

#ifndef CONFIG_TEGRA_HDMI_74MHZ_LIMIT
	/* Double the pixel clock and update v_active only for frame packed mode */
	if (mode.stereo_mode) {
		mode.pclk *= 2;
		/* total v_active = yres*2 + activespace */
		mode.v_active = fbmode->yres*2 +
				fbmode->vsync_len +
				fbmode->upper_margin +
				fbmode->lower_margin;
	}
#endif

	mode.flags = 0;

	if (!(fbmode->sync & FB_SYNC_HOR_HIGH_ACT))
		mode.flags |= TEGRA_DC_MODE_FLAG_NEG_H_SYNC;

	if (!(fbmode->sync & FB_SYNC_VERT_HIGH_ACT))
		mode.flags |= TEGRA_DC_MODE_FLAG_NEG_V_SYNC;

	return tegra_dc_set_mode(dc, &mode);
}
EXPORT_SYMBOL(tegra_dc_set_fb_mode);

void
tegra_dc_config_pwm(struct tegra_dc *dc, struct tegra_dc_pwm_params *cfg)
{
	unsigned int ctrl;
	unsigned long out_sel;
	unsigned long cmd_state;

	mutex_lock(&dc->lock);
	if (!dc->enabled) {
		mutex_unlock(&dc->lock);
		return;
	}

	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_LP_MODE)
		tegra_dc_host_resume(dc);

	ctrl = ((cfg->period << PM_PERIOD_SHIFT) |
		(cfg->clk_div << PM_CLK_DIVIDER_SHIFT) |
		cfg->clk_select);

	/* The new value should be effected immediately */
	cmd_state = tegra_dc_readl(dc, DC_CMD_STATE_ACCESS);
	tegra_dc_writel(dc, (cmd_state | (1 << 2)), DC_CMD_STATE_ACCESS);

	if (cfg->switch_to_sfio && cfg->gpio_conf_to_sfio)
		cfg->switch_to_sfio(cfg->gpio_conf_to_sfio);
	else
		dev_err(&dc->ndev->dev, "Error: Need gpio_conf_to_sfio\n");

	switch (cfg->which_pwm) {
	case TEGRA_PWM_PM0:
		/* Select the LM0 on PM0 */
		out_sel = tegra_dc_readl(dc, DC_COM_PIN_OUTPUT_SELECT5);
		out_sel &= ~(7 << 0);
		out_sel |= (3 << 0);
		tegra_dc_writel(dc, out_sel, DC_COM_PIN_OUTPUT_SELECT5);
		tegra_dc_writel(dc, ctrl, DC_COM_PM0_CONTROL);
		tegra_dc_writel(dc, cfg->duty_cycle, DC_COM_PM0_DUTY_CYCLE);
		break;
	case TEGRA_PWM_PM1:
		/* Select the LM1 on PM1 */
		out_sel = tegra_dc_readl(dc, DC_COM_PIN_OUTPUT_SELECT5);
		out_sel &= ~(7 << 4);
		out_sel |= (3 << 4);
		tegra_dc_writel(dc, out_sel, DC_COM_PIN_OUTPUT_SELECT5);
		tegra_dc_writel(dc, ctrl, DC_COM_PM1_CONTROL);
		tegra_dc_writel(dc, cfg->duty_cycle, DC_COM_PM1_DUTY_CYCLE);
		break;
	default:
		dev_err(&dc->ndev->dev, "Error: Need which_pwm\n");
		break;
	}
	tegra_dc_writel(dc, cmd_state, DC_CMD_STATE_ACCESS);
	mutex_unlock(&dc->lock);
}
EXPORT_SYMBOL(tegra_dc_config_pwm);

void tegra_dc_set_out_pin_polars(struct tegra_dc *dc,
				const struct tegra_dc_out_pin *pins,
				const unsigned int n_pins)
{
	unsigned int i;

	int name;
	int pol;

	u32 pol1, pol3;

	u32 set1, unset1;
	u32 set3, unset3;

	set1 = set3 = unset1 = unset3 = 0;

	for (i = 0; i < n_pins; i++) {
		name = (pins + i)->name;
		pol  = (pins + i)->pol;

		/* set polarity by name */
		switch (name) {
		case TEGRA_DC_OUT_PIN_DATA_ENABLE:
			if (pol == TEGRA_DC_OUT_PIN_POL_LOW)
				set3 |= LSPI_OUTPUT_POLARITY_LOW;
			else
				unset3 |= LSPI_OUTPUT_POLARITY_LOW;
			break;
		case TEGRA_DC_OUT_PIN_H_SYNC:
			if (pol == TEGRA_DC_OUT_PIN_POL_LOW)
				set1 |= LHS_OUTPUT_POLARITY_LOW;
			else
				unset1 |= LHS_OUTPUT_POLARITY_LOW;
			break;
		case TEGRA_DC_OUT_PIN_V_SYNC:
			if (pol == TEGRA_DC_OUT_PIN_POL_LOW)
				set1 |= LVS_OUTPUT_POLARITY_LOW;
			else
				unset1 |= LVS_OUTPUT_POLARITY_LOW;
			break;
		case TEGRA_DC_OUT_PIN_PIXEL_CLOCK:
			if (pol == TEGRA_DC_OUT_PIN_POL_LOW)
				set1 |= LSC0_OUTPUT_POLARITY_LOW;
			else
				unset1 |= LSC0_OUTPUT_POLARITY_LOW;
			break;
		default:
			printk(KERN_INFO "Invalid argument in function %s\n",
			       __func__);
			break;
		}
	}

	pol1 = DC_COM_PIN_OUTPUT_POLARITY1_INIT_VAL;
	pol3 = DC_COM_PIN_OUTPUT_POLARITY3_INIT_VAL;

	pol1 |= set1;
	pol1 &= ~unset1;

	pol3 |= set3;
	pol3 &= ~unset3;

	tegra_dc_writel(dc, pol1, DC_COM_PIN_OUTPUT_POLARITY1);
	tegra_dc_writel(dc, pol3, DC_COM_PIN_OUTPUT_POLARITY3);
}

static struct tegra_dc_mode *tegra_dc_get_override_mode(struct tegra_dc *dc)
{
	if (dc->out->type == TEGRA_DC_OUT_RGB ||
		dc->out->type == TEGRA_DC_OUT_HDMI ||
		dc->out->type == TEGRA_DC_OUT_DSI)
		return override_disp_mode[dc->out->type].pclk ?
			&override_disp_mode[dc->out->type] : NULL;
	else
		return NULL;
}

static void tegra_dc_set_out(struct tegra_dc *dc, struct tegra_dc_out *out)
{
	struct tegra_dc_mode *mode;

	dc->out = out;
	mode = tegra_dc_get_override_mode(dc);

	if (mode)
		tegra_dc_set_mode(dc, mode);
	else if (out->n_modes > 0)
		tegra_dc_set_mode(dc, &dc->out->modes[0]);

	switch (out->type) {
	case TEGRA_DC_OUT_RGB:
		dc->out_ops = &tegra_dc_rgb_ops;
		break;

	case TEGRA_DC_OUT_HDMI:
		dc->out_ops = &tegra_dc_hdmi_ops;
		break;

	case TEGRA_DC_OUT_DSI:
		dc->out_ops = &tegra_dc_dsi_ops;
		break;

	default:
		dc->out_ops = NULL;
		break;
	}

	if (dc->out_ops && dc->out_ops->init)
		dc->out_ops->init(dc);

}

unsigned tegra_dc_get_out_height(const struct tegra_dc *dc)
{
	if (dc->out)
		return dc->out->height;
	else
		return 0;
}
EXPORT_SYMBOL(tegra_dc_get_out_height);

unsigned tegra_dc_get_out_width(const struct tegra_dc *dc)
{
	if (dc->out)
		return dc->out->width;
	else
		return 0;
}
EXPORT_SYMBOL(tegra_dc_get_out_width);

unsigned tegra_dc_get_out_max_pixclock(const struct tegra_dc *dc)
{
	if (dc->out && dc->out->max_pixclock)
		return dc->out->max_pixclock;
	else
		return 0;
}
EXPORT_SYMBOL(tegra_dc_get_out_max_pixclock);

void tegra_dc_enable_crc(struct tegra_dc *dc)
{
	u32 val;
	tegra_dc_io_start(dc);

	val = CRC_ALWAYS_ENABLE | CRC_INPUT_DATA_ACTIVE_DATA |
		CRC_ENABLE_ENABLE;
	tegra_dc_writel(dc, val, DC_COM_CRC_CONTROL);
	tegra_dc_writel(dc, GENERAL_UPDATE, DC_CMD_STATE_CONTROL);
	tegra_dc_writel(dc, GENERAL_ACT_REQ, DC_CMD_STATE_CONTROL);
}

void tegra_dc_disable_crc(struct tegra_dc *dc)
{
	tegra_dc_writel(dc, 0x0, DC_COM_CRC_CONTROL);
	tegra_dc_writel(dc, GENERAL_UPDATE, DC_CMD_STATE_CONTROL);
	tegra_dc_writel(dc, GENERAL_ACT_REQ, DC_CMD_STATE_CONTROL);

	tegra_dc_io_end(dc);
}

u32 tegra_dc_read_checksum_latched(struct tegra_dc *dc)
{
	int crc = 0;

	if (!dc) {
		dev_err(&dc->ndev->dev, "Failed to get dc.\n");
		goto crc_error;
	}

	/* TODO: Replace mdelay with code to sync VBlANK, since
	 * DC_COM_CRC_CHECKSUM_LATCHED is available after VBLANK */
	mdelay(TEGRA_CRC_LATCHED_DELAY);

	crc = tegra_dc_readl(dc, DC_COM_CRC_CHECKSUM_LATCHED);
crc_error:
	return crc;
}

static void tegra_dc_vblank(struct work_struct *work)
{
	struct tegra_dc *dc = container_of(work, struct tegra_dc, vblank_work);
	bool nvsd_updated = false;

	mutex_lock(&dc->lock);
	/* use the new frame's bandwidth setting instead of max(current, new),
	 * skip this if we're using tegra_dc_one_shot_worker() */
	if (!(dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_MODE))
		tegra_dc_program_bandwidth(dc, true);

	/* Update the SD brightness */
	if (dc->enabled && dc->out->sd_settings)
		nvsd_updated = nvsd_update_brightness(dc);

	mutex_unlock(&dc->lock);

	/* Do the actual brightness update outside of the mutex */
	if (nvsd_updated && dc->out->sd_settings &&
	    dc->out->sd_settings->bl_device) {

		struct platform_device *pdev = dc->out->sd_settings->bl_device;
		struct backlight_device *bl = platform_get_drvdata(pdev);
		if (bl)
			backlight_update_status(bl);
	}
}

static void tegra_dc_one_shot_worker(struct work_struct *work)
{
	struct tegra_dc *dc = container_of(
		to_delayed_work(work), struct tegra_dc, one_shot_work);
	mutex_lock(&dc->lock);
	/* memory client has gone idle */
	tegra_dc_clear_bandwidth(dc);

	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_LP_MODE)
		tegra_dc_host_suspend(dc);

	mutex_unlock(&dc->lock);
}

/* return an arbitrarily large number if count overflow occurs.
 * make it a nice base-10 number to show up in stats output */
static u64 tegra_dc_underflow_count(struct tegra_dc *dc, unsigned reg)
{
	unsigned count = tegra_dc_readl(dc, reg);
	tegra_dc_writel(dc, 0, reg);
	return ((count & 0x80000000) == 0) ? count : 10000000000ll;
}

static void tegra_dc_underflow_handler(struct tegra_dc *dc)
{
	u32 val;
	int i;

	dc->stats.underflows++;
	if (dc->underflow_mask & WIN_A_UF_INT)
		dc->stats.underflows_a += tegra_dc_underflow_count(dc,
			DC_WINBUF_AD_UFLOW_STATUS);
	if (dc->underflow_mask & WIN_B_UF_INT)
		dc->stats.underflows_b += tegra_dc_underflow_count(dc,
			DC_WINBUF_BD_UFLOW_STATUS);
	if (dc->underflow_mask & WIN_C_UF_INT)
		dc->stats.underflows_c += tegra_dc_underflow_count(dc,
			DC_WINBUF_CD_UFLOW_STATUS);

	/* Check for any underflow reset conditions */
	for (i = 0; i < DC_N_WINDOWS; i++) {
		u32 masks[] = {
			WIN_A_UF_INT,
			WIN_B_UF_INT,
			WIN_C_UF_INT,
		};

		if (WARN_ONCE(i >= ARRAY_SIZE(masks),
			"underflow stats unsupported"))
			break; /* bail if the table above is missing entries */
		if (!masks[i])
			continue; /* skip empty entries */

		if (dc->underflow_mask & masks[i]) {
			dc->windows[i].underflows++;

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
			if (i < 3 && dc->windows[i].underflows > 4) {
				schedule_work(&dc->reset_work);
				/* reset counter */
				dc->windows[i].underflows = 0;
			}
#endif
		} else {
			dc->windows[i].underflows = 0;
		}
	}

	/* Clear the underflow mask now that we've checked it. */
	tegra_dc_writel(dc, dc->underflow_mask, DC_CMD_INT_STATUS);
	dc->underflow_mask = 0;
	val = tegra_dc_readl(dc, DC_CMD_INT_MASK);
	tegra_dc_writel(dc, val | ALL_UF_INT, DC_CMD_INT_MASK);
}

#ifndef CONFIG_TEGRA_FPGA_PLATFORM
static bool tegra_dc_windows_are_dirty(struct tegra_dc *dc)
{
#ifndef CONFIG_TEGRA_SIMULATION_PLATFORM
	u32 val;

	val = tegra_dc_readl(dc, DC_CMD_STATE_CONTROL);
	if (val & (WIN_A_UPDATE | WIN_B_UPDATE | WIN_C_UPDATE))
	    return true;
#endif
	return false;
}

static void tegra_dc_trigger_windows(struct tegra_dc *dc)
{
	u32 val, i;
	u32 completed = 0;
	u32 dirty = 0;

	val = tegra_dc_readl(dc, DC_CMD_STATE_CONTROL);
	for (i = 0; i < DC_N_WINDOWS; i++) {
#ifdef CONFIG_TEGRA_SIMULATION_PLATFORM
		/* FIXME: this is not needed when the simulator
		   clears WIN_x_UPDATE bits as in HW */
		dc->windows[i].dirty = 0;
		completed = 1;
#else
		if (!(val & (WIN_A_UPDATE << i))) {
			dc->windows[i].dirty = 0;
			completed = 1;
		} else {
			dirty = 1;
		}
#endif
	}

	if (!dirty) {
		val = tegra_dc_readl(dc, DC_CMD_INT_MASK);
		if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_MODE)
			val &= ~V_BLANK_INT;
		else
			val &= ~FRAME_END_INT;
		tegra_dc_writel(dc, val, DC_CMD_INT_MASK);
	}

	if (completed) {
		if (!dirty) {
			/* With the last completed window, go ahead
			   and enable the vblank interrupt for nvsd. */
			val = tegra_dc_readl(dc, DC_CMD_INT_MASK);
			val |= V_BLANK_INT;
			tegra_dc_writel(dc, val, DC_CMD_INT_MASK);
		}

		wake_up(&dc->wq);
	}
}

static void tegra_dc_one_shot_irq(struct tegra_dc *dc, unsigned long status)
{
	if (status & V_BLANK_INT) {
		/* Sync up windows. */
		tegra_dc_trigger_windows(dc);

		/* Schedule any additional bottom-half vblank actvities. */
		queue_work(system_freezable_wq, &dc->vblank_work);
	}

	if (status & FRAME_END_INT) {
		/* Mark the frame_end as complete. */
		if (!completion_done(&dc->frame_end_complete))
			complete(&dc->frame_end_complete);
	}
}

static void tegra_dc_continuous_irq(struct tegra_dc *dc, unsigned long status)
{
	if (status & V_BLANK_INT) {
		/* Schedule any additional bottom-half vblank actvities. */
		queue_work(system_freezable_wq, &dc->vblank_work);

		/* All windows updated. Mask subsequent V_BLANK interrupts */
		if (!tegra_dc_windows_are_dirty(dc)) {
			u32 val;

			val = tegra_dc_readl(dc, DC_CMD_INT_MASK);
			val &= ~V_BLANK_INT;
			tegra_dc_writel(dc, val, DC_CMD_INT_MASK);
		}
	}

	if (status & FRAME_END_INT) {
		/* Mark the frame_end as complete. */
		if (!completion_done(&dc->frame_end_complete))
			complete(&dc->frame_end_complete);

		tegra_dc_trigger_windows(dc);
	}
}
#endif

static irqreturn_t tegra_dc_irq(int irq, void *ptr)
{
#ifndef CONFIG_TEGRA_FPGA_PLATFORM
	struct tegra_dc *dc = ptr;
	unsigned long status;
	unsigned long underflow_mask;
	u32 val;

	if (!nvhost_module_powered(nvhost_get_host(dc->ndev)->dev)) {
		WARN(1, "IRQ when DC not powered!\n");
		tegra_dc_io_start(dc);
		status = tegra_dc_readl(dc, DC_CMD_INT_STATUS);
		tegra_dc_writel(dc, status, DC_CMD_INT_STATUS);
		tegra_dc_io_end(dc);
		return IRQ_HANDLED;
	}

	/* clear all status flags except underflow, save those for the worker */
	status = tegra_dc_readl(dc, DC_CMD_INT_STATUS);
	tegra_dc_writel(dc, status & ~ALL_UF_INT, DC_CMD_INT_STATUS);
	val = tegra_dc_readl(dc, DC_CMD_INT_MASK);
	tegra_dc_writel(dc, val & ~ALL_UF_INT, DC_CMD_INT_MASK);

	/*
	 * Overlays can get thier internal state corrupted during and underflow
	 * condition.  The only way to fix this state is to reset the DC.
	 * if we get 6 consecutive frames with underflows, assume we're
	 * hosed and reset.
	 */
	underflow_mask = status & ALL_UF_INT;

	/* Check underflow */
	if (underflow_mask) {
		dc->underflow_mask |= underflow_mask;
		schedule_delayed_work(&dc->underflow_work,
			msecs_to_jiffies(1));
	}

	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_MODE)
		tegra_dc_one_shot_irq(dc, status);
	else
		tegra_dc_continuous_irq(dc, status);

	return IRQ_HANDLED;
#else /* CONFIG_TEGRA_FPGA_PLATFORM */
	return IRQ_NONE;
#endif /* !CONFIG_TEGRA_FPGA_PLATFORM */
}

static void tegra_dc_set_color_control(struct tegra_dc *dc)
{
	u32 color_control;

	switch (dc->out->depth) {
	case 3:
		color_control = BASE_COLOR_SIZE111;
		break;

	case 6:
		color_control = BASE_COLOR_SIZE222;
		break;

	case 8:
		color_control = BASE_COLOR_SIZE332;
		break;

	case 9:
		color_control = BASE_COLOR_SIZE333;
		break;

	case 12:
		color_control = BASE_COLOR_SIZE444;
		break;

	case 15:
		color_control = BASE_COLOR_SIZE555;
		break;

	case 16:
		color_control = BASE_COLOR_SIZE565;
		break;

	case 18:
		color_control = BASE_COLOR_SIZE666;
		break;

	default:
		color_control = BASE_COLOR_SIZE888;
		break;
	}

	switch (dc->out->dither) {
	case TEGRA_DC_DISABLE_DITHER:
		color_control |= DITHER_CONTROL_DISABLE;
		break;
	case TEGRA_DC_ORDERED_DITHER:
		color_control |= DITHER_CONTROL_ORDERED;
		break;
	case TEGRA_DC_ERRDIFF_DITHER:
		/* The line buffer for error-diffusion dither is limited
		 * to 1280 pixels per line. This limits the maximum
		 * horizontal active area size to 1280 pixels when error
		 * diffusion is enabled.
		 */
		BUG_ON(dc->mode.h_active > 1280);
		color_control |= DITHER_CONTROL_ERRDIFF;
		break;
	}

	tegra_dc_writel(dc, color_control, DC_DISP_DISP_COLOR_CONTROL);
}

static u32 get_syncpt(struct tegra_dc *dc, int idx)
{
	u32 syncpt_id;

	switch (dc->ndev->id) {
	case 0:
		switch (idx) {
		case 0:
			syncpt_id = NVSYNCPT_DISP0_A;
			break;
		case 1:
			syncpt_id = NVSYNCPT_DISP0_B;
			break;
		case 2:
			syncpt_id = NVSYNCPT_DISP0_C;
			break;
		default:
			BUG();
			break;
		}
		break;
	case 1:
		switch (idx) {
		case 0:
			syncpt_id = NVSYNCPT_DISP1_A;
			break;
		case 1:
			syncpt_id = NVSYNCPT_DISP1_B;
			break;
		case 2:
			syncpt_id = NVSYNCPT_DISP1_C;
			break;
		default:
			BUG();
			break;
		}
		break;
	default:
		BUG();
		break;
	}

	return syncpt_id;
}

static int tegra_dc_init(struct tegra_dc *dc)
{
	int i;

	tegra_dc_writel(dc, 0x00000100, DC_CMD_GENERAL_INCR_SYNCPT_CNTRL);
	if (dc->ndev->id == 0) {
		tegra_mc_set_priority(TEGRA_MC_CLIENT_DISPLAY0A,
				      TEGRA_MC_PRIO_MED);
		tegra_mc_set_priority(TEGRA_MC_CLIENT_DISPLAY0B,
				      TEGRA_MC_PRIO_MED);
		tegra_mc_set_priority(TEGRA_MC_CLIENT_DISPLAY0C,
				      TEGRA_MC_PRIO_MED);
		tegra_mc_set_priority(TEGRA_MC_CLIENT_DISPLAY1B,
				      TEGRA_MC_PRIO_MED);
		tegra_mc_set_priority(TEGRA_MC_CLIENT_DISPLAYHC,
				      TEGRA_MC_PRIO_HIGH);
	} else if (dc->ndev->id == 1) {
		tegra_mc_set_priority(TEGRA_MC_CLIENT_DISPLAY0AB,
				      TEGRA_MC_PRIO_MED);
		tegra_mc_set_priority(TEGRA_MC_CLIENT_DISPLAY0BB,
				      TEGRA_MC_PRIO_MED);
		tegra_mc_set_priority(TEGRA_MC_CLIENT_DISPLAY0CB,
				      TEGRA_MC_PRIO_MED);
		tegra_mc_set_priority(TEGRA_MC_CLIENT_DISPLAY1BB,
				      TEGRA_MC_PRIO_MED);
		tegra_mc_set_priority(TEGRA_MC_CLIENT_DISPLAYHCB,
				      TEGRA_MC_PRIO_HIGH);
	}
	tegra_dc_writel(dc, 0x00000100 | dc->vblank_syncpt,
			DC_CMD_CONT_SYNCPT_VSYNC);
	tegra_dc_writel(dc, 0x00004700, DC_CMD_INT_TYPE);
	tegra_dc_writel(dc, 0x0001c700, DC_CMD_INT_POLARITY);
	tegra_dc_writel(dc, 0x00202020, DC_DISP_MEM_HIGH_PRIORITY);
	tegra_dc_writel(dc, 0x00010101, DC_DISP_MEM_HIGH_PRIORITY_TIMER);

	/* enable interrupts for vblank, frame_end and underflows */
	tegra_dc_writel(dc, (FRAME_END_INT | V_BLANK_INT | ALL_UF_INT),
		DC_CMD_INT_ENABLE);
	tegra_dc_writel(dc, ALL_UF_INT, DC_CMD_INT_MASK);

	tegra_dc_writel(dc, 0x00000000, DC_DISP_BORDER_COLOR);

	tegra_dc_set_color_control(dc);
	for (i = 0; i < DC_N_WINDOWS; i++) {
		struct tegra_dc_win *win = &dc->windows[i];
		tegra_dc_writel(dc, WINDOW_A_SELECT << i,
				DC_CMD_DISPLAY_WINDOW_HEADER);
		tegra_dc_set_csc(dc, &win->csc);
		tegra_dc_set_lut(dc, win);
		tegra_dc_set_scaling_filter(dc);
	}


	for (i = 0; i < dc->n_windows; i++) {
		u32 syncpt = get_syncpt(dc, i);

		dc->syncpt[i].id = syncpt;

		dc->syncpt[i].min = dc->syncpt[i].max =
			nvhost_syncpt_read(&nvhost_get_host(dc->ndev)->syncpt,
					syncpt);
	}

	print_mode(dc, &dc->mode, __func__);

	if (dc->mode.pclk)
		if (tegra_dc_program_mode(dc, &dc->mode))
			return -EINVAL;

	/* Initialize SD AFTER the modeset.
	   nvsd_init handles the sd_settings = NULL case. */
	nvsd_init(dc, dc->out->sd_settings);

	return 0;
}

#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
static bool _tegra_dc_controller_enable(struct tegra_dc *dc, bool no_reset)
#else
static bool _tegra_dc_controller_enable(struct tegra_dc *dc)
#endif
{
	int failed_init = 0;

	if (dc->out->enable)
		dc->out->enable();

	tegra_dc_setup_clk(dc, dc->clk);
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
	if (!no_reset)
		tegra_periph_reset_assert(dc->clk);
#else
	tegra_periph_reset_assert(dc->clk);
#endif
	tegra_dc_clk_enable(dc);
	tegra_periph_reset_deassert(dc->clk);
	msleep(10);

	/* do not accept interrupts during initialization */
	tegra_dc_writel(dc, 0, DC_CMD_INT_ENABLE);
	tegra_dc_writel(dc, 0, DC_CMD_INT_MASK);

	enable_dc_irq(dc->irq);

	failed_init = tegra_dc_init(dc);
	if (failed_init) {
		_tegra_dc_controller_disable(dc);
		return false;
	}

	if (dc->out_ops && dc->out_ops->enable)
		dc->out_ops->enable(dc);

	if (dc->out->postpoweron)
		dc->out->postpoweron();

	/* force a full blending update */
	dc->blend.z[0] = -1;

	tegra_dc_ext_enable(dc->ext);

	return true;
}

#ifdef CONFIG_ARCH_TEGRA_2x_SOC

#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
/* In samsung device, the bootloader initialize LCD and draw device logo.
 * If dc is reset, the device logo might be disappeared on screen.
 * In addition, CMC623 chip is not tolerant of some change on input RGB interface signals.
 */
static bool _tegra_dc_enable_noreset(struct tegra_dc *dc)
{
	if (dc->ndev->id == 0) {
			struct timeval t_resume;
			int diff_msec = 0;
			do_gettimeofday(&t_resume);
			diff_msec = ((t_resume.tv_sec - t_suspend.tv_sec) * 1000000 +(t_resume.tv_usec - t_suspend.tv_usec)) / 1000;
			printk("Disp: diff_msec= %d\n", diff_msec);
			if((diff_msec < 150) && (diff_msec >= 0))
					msleep(150 - diff_msec);
	}


	if (dc->mode.pclk == 0)
		return false;

	if (!dc->out)
		return false;

	tegra_dc_io_start(dc);

	return _tegra_dc_controller_enable(dc, true);
}
#endif

static bool _tegra_dc_controller_reset_enable(struct tegra_dc *dc)
{
	bool ret = true;

	if (dc->out->enable)
		dc->out->enable();

	tegra_dc_setup_clk(dc, dc->clk);
	tegra_dc_clk_enable(dc);

	if (dc->ndev->id == 0 && tegra_dcs[1] != NULL) {
		mutex_lock(&tegra_dcs[1]->lock);
		disable_irq(tegra_dcs[1]->irq);
	} else if (dc->ndev->id == 1 && tegra_dcs[0] != NULL) {
		mutex_lock(&tegra_dcs[0]->lock);
		disable_irq(tegra_dcs[0]->irq);
	}

	msleep(5);
	tegra_periph_reset_assert(dc->clk);
	msleep(2);
#ifdef CONFIG_TEGRA_SILICON_PLATFORM
	tegra_periph_reset_deassert(dc->clk);
	msleep(1);
#endif

	if (dc->ndev->id == 0 && tegra_dcs[1] != NULL) {
		enable_dc_irq(tegra_dcs[1]->irq);
		mutex_unlock(&tegra_dcs[1]->lock);
	} else if (dc->ndev->id == 1 && tegra_dcs[0] != NULL) {
		enable_dc_irq(tegra_dcs[0]->irq);
		mutex_unlock(&tegra_dcs[0]->lock);
	}

	enable_dc_irq(dc->irq);

	if (tegra_dc_init(dc)) {
		dev_err(&dc->ndev->dev, "cannot initialize\n");
		ret = false;
	}

	if (dc->out_ops && dc->out_ops->enable)
		dc->out_ops->enable(dc);

	if (dc->out->postpoweron)
		dc->out->postpoweron();

	/* force a full blending update */
	dc->blend.z[0] = -1;

	tegra_dc_ext_enable(dc->ext);

	if (!ret) {
		dev_err(&dc->ndev->dev, "initialization failed,disabling");
		_tegra_dc_controller_disable(dc);
	}

	return ret;
}
#endif

static int _tegra_dc_set_default_videomode(struct tegra_dc *dc)
{
	return tegra_dc_set_fb_mode(dc, &tegra_dc_hdmi_fallback_mode, 0);
}

static bool _tegra_dc_enable(struct tegra_dc *dc)
{
	if (dc->mode.pclk == 0) {
		switch (dc->out->type) {
		case TEGRA_DC_OUT_HDMI:
		/* DC enable called but no videomode is loaded.
		     Check if HDMI is connected, then set fallback mdoe */
		if (tegra_dc_hpd(dc)) {
			if (_tegra_dc_set_default_videomode(dc))
				return false;
		} else
			return false;

		break;

		/* Do nothing for other outputs for now */
		case TEGRA_DC_OUT_RGB:

		case TEGRA_DC_OUT_DSI:

		default:
			return false;
		}
	}

	if (!dc->out)
		return false;

	tegra_dc_io_start(dc);

#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
	return _tegra_dc_controller_enable(dc, false);
#else
	return _tegra_dc_controller_enable(dc);
#endif	
}

void tegra_dc_enable(struct tegra_dc *dc)
{
	mutex_lock(&dc->lock);

	if (!dc->enabled)
		dc->enabled = _tegra_dc_enable(dc);

	mutex_unlock(&dc->lock);
}

static void _tegra_dc_controller_disable(struct tegra_dc *dc)
{
	unsigned i;

	if (dc->out_ops && dc->out_ops->disable)
		dc->out_ops->disable(dc);

	tegra_dc_writel(dc, 0, DC_CMD_INT_MASK);
	tegra_dc_writel(dc, 0, DC_CMD_INT_ENABLE);
	disable_irq(dc->irq);

	tegra_dc_clear_bandwidth(dc);
	tegra_dc_clk_disable(dc);

	if (dc->out && dc->out->disable)
		dc->out->disable();

	for (i = 0; i < dc->n_windows; i++) {
		struct tegra_dc_win *w = &dc->windows[i];

		/* reset window bandwidth */
		w->bandwidth = 0;
		w->new_bandwidth = 0;

		/* disable windows */
		w->flags &= ~TEGRA_WIN_FLAG_ENABLED;

		/* flush any pending syncpt waits */
		while (dc->syncpt[i].min < dc->syncpt[i].max) {
			dc->syncpt[i].min++;
			nvhost_syncpt_cpu_incr(
				&nvhost_get_host(dc->ndev)->syncpt,
				dc->syncpt[i].id);
		}
	}
}

void tegra_dc_stats_enable(struct tegra_dc *dc, bool enable)
{
#if 0 /* underflow interrupt is already enabled by dc reset worker */
	u32 val;
	if (dc->enabled)  {
		val = tegra_dc_readl(dc, DC_CMD_INT_ENABLE);
		if (enable)
			val |= (WIN_A_UF_INT | WIN_B_UF_INT | WIN_C_UF_INT);
		else
			val &= ~(WIN_A_UF_INT | WIN_B_UF_INT | WIN_C_UF_INT);
		tegra_dc_writel(dc, val, DC_CMD_INT_ENABLE);
	}
#endif
}

bool tegra_dc_stats_get(struct tegra_dc *dc)
{
#if 0 /* right now it is always enabled */
	u32 val;
	bool res;

	if (dc->enabled)  {
		val = tegra_dc_readl(dc, DC_CMD_INT_ENABLE);
		res = !!(val & (WIN_A_UF_INT | WIN_B_UF_INT | WIN_C_UF_INT));
	} else {
		res = false;
	}

	return res;
#endif
	return true;
}

/* make the screen blank by disabling all windows */
void tegra_dc_blank(struct tegra_dc *dc)
{
	struct tegra_dc_win *dcwins[DC_N_WINDOWS];
	unsigned i;

	for (i = 0; i < DC_N_WINDOWS; i++) {
		dcwins[i] = tegra_dc_get_window(dc, i);
		dcwins[i]->flags &= ~TEGRA_WIN_FLAG_ENABLED;
	}

	tegra_dc_update_windows(dcwins, DC_N_WINDOWS);
	tegra_dc_sync_windows(dcwins, DC_N_WINDOWS);
}

static void _tegra_dc_disable(struct tegra_dc *dc)
{
	if (dc->ndev->id == 0) {
		do_gettimeofday(&t_suspend);
	}

	_tegra_dc_controller_disable(dc);
	tegra_dc_io_end(dc);
}

void tegra_dc_disable(struct tegra_dc *dc)
{
	tegra_dc_ext_disable(dc->ext);

	/* it's important that new underflow work isn't scheduled before the
	 * lock is acquired. */
	cancel_delayed_work_sync(&dc->underflow_work);
	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_MODE) {
		mutex_lock(&dc->one_shot_lock);
		cancel_delayed_work_sync(&dc->one_shot_work);
	}

	mutex_lock(&dc->lock);

	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_LP_MODE)
		tegra_dc_host_resume(dc);

	if (dc->enabled) {
		dc->enabled = false;

		if (!dc->suspended)
			_tegra_dc_disable(dc);
	}

#ifdef CONFIG_SWITCH
	switch_set_state(&dc->modeset_switch, 0);
#endif

	mutex_unlock(&dc->lock);
	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_MODE)
		mutex_unlock(&dc->one_shot_lock);
}

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
extern int lcdonoff;
static void tegra_dc_reset_worker(struct work_struct *work)
{
	unsigned int suspend_flag = 0;
	struct tegra_dc *dc =
		container_of(work, struct tegra_dc, reset_work);

	unsigned long val = 0;

	dev_warn(&dc->ndev->dev, "overlay stuck in underflow state.  resetting.\n");

	tegra_dc_ext_disable(dc->ext);

	mutex_lock(&shared_lock);
	mutex_lock(&dc->lock);

	if (dc->enabled == false)
	{
		dev_warn(&dc->ndev->dev, "overlay stuck in underflowstate.  tegra_dc_reset_worker: skipping reset.\n");
		goto unlock;
	}

#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
#ifdef CONFIG_HAS_EARLYSUSPEND
	if (lcdonoff == 1) {
		cmc623_suspend(NULL);
		suspend_flag = 1;
	}
#endif
#endif

	dc->enabled = false;

	/*
	 * off host read bus
	 */
	val = tegra_dc_readl(dc, DC_CMD_CONT_SYNCPT_VSYNC);
	val &= ~(0x00000100);
	tegra_dc_writel(dc, val, DC_CMD_CONT_SYNCPT_VSYNC);

	/*
	 * set DC to STOP mode
	 */
	tegra_dc_writel(dc, DISP_CTRL_MODE_STOP, DC_CMD_DISPLAY_COMMAND);

	msleep(10);

	_tegra_dc_controller_disable(dc);

	/* _tegra_dc_controller_reset_enable deasserts reset */
	_tegra_dc_controller_reset_enable(dc);

	dc->enabled = true;
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA	
#ifdef CONFIG_HAS_EARLYSUSPEND
	if (suspend_flag == 1)
		cmc623_resume(NULL);
#endif
#endif
	/* reopen host read bus */
	val = tegra_dc_readl(dc, DC_CMD_CONT_SYNCPT_VSYNC);
	val &= ~(0x00000100);
	val |= 0x100;
	tegra_dc_writel(dc, val, DC_CMD_CONT_SYNCPT_VSYNC);



unlock:
	mutex_unlock(&dc->lock);
	mutex_unlock(&shared_lock);
}
#endif

static void tegra_dc_underflow_worker(struct work_struct *work)
{
	struct tegra_dc *dc = container_of(
		to_delayed_work(work), struct tegra_dc, underflow_work);

	mutex_lock(&dc->lock);
	if (dc->out->flags & TEGRA_DC_OUT_ONE_SHOT_LP_MODE)
		tegra_dc_host_resume(dc);

	if (dc->enabled) {
		tegra_dc_underflow_handler(dc);
	}
	mutex_unlock(&dc->lock);
}

#ifdef CONFIG_SWITCH
static ssize_t switch_modeset_print_mode(struct switch_dev *sdev, char *buf)
{
	struct tegra_dc *dc =
		container_of(sdev, struct tegra_dc, modeset_switch);

	if (!sdev->state)
		return sprintf(buf, "offline\n");

	return sprintf(buf, "%dx%d\n", dc->mode.h_active, dc->mode.v_active);
}
#endif

static int tegra_dc_probe(struct nvhost_device *ndev)
{
	struct tegra_dc *dc;
	struct tegra_dc_mode *mode;
	struct clk *clk;
	struct clk *emc_clk;
	struct resource	*res;
	struct resource *base_res;
	struct resource *fb_mem = NULL;
	int ret = 0;
	void __iomem *base;
	int irq;
	int i;

#ifdef	__SAMSUNG_HDMI_FLAG_WORKAROUND__
	spin_lock_init(&hdmi_enable_lock);
#endif

	if (!ndev->dev.platform_data) {
		dev_err(&ndev->dev, "no platform data\n");
		return -ENOENT;
	}

	dc = kzalloc(sizeof(struct tegra_dc), GFP_KERNEL);
	if (!dc) {
		dev_err(&ndev->dev, "can't allocate memory for tegra_dc\n");
		return -ENOMEM;
	}

	irq = nvhost_get_irq_byname(ndev, "irq");
	if (irq <= 0) {
		dev_err(&ndev->dev, "no irq\n");
		ret = -ENOENT;
		goto err_free;
	}

	res = nvhost_get_resource_byname(ndev, IORESOURCE_MEM, "regs");
	if (!res) {
		dev_err(&ndev->dev, "no mem resource\n");
		ret = -ENOENT;
		goto err_free;
	}

	base_res = request_mem_region(res->start, resource_size(res), ndev->name);
	if (!base_res) {
		dev_err(&ndev->dev, "request_mem_region failed\n");
		ret = -EBUSY;
		goto err_free;
	}

	base = ioremap(res->start, resource_size(res));
	if (!base) {
		dev_err(&ndev->dev, "registers can't be mapped\n");
		ret = -EBUSY;
		goto err_release_resource_reg;
	}

	fb_mem = nvhost_get_resource_byname(ndev, IORESOURCE_MEM, "fbmem");

	clk = clk_get(&ndev->dev, NULL);
	if (IS_ERR_OR_NULL(clk)) {
		dev_err(&ndev->dev, "can't get clock\n");
		ret = -ENOENT;
		goto err_iounmap_reg;
	}

	emc_clk = clk_get(&ndev->dev, "emc");
	if (IS_ERR_OR_NULL(emc_clk)) {
		dev_err(&ndev->dev, "can't get emc clock\n");
		ret = -ENOENT;
		goto err_put_clk;
	}

	dc->clk = clk;
	dc->emc_clk = emc_clk;
	dc->shift_clk_div.mul = dc->shift_clk_div.div = 1;
	/* Initialize one shot work delay, it will be assigned by dsi
	 * according to refresh rate later. */
	dc->one_shot_delay_ms = 40;

	dc->base_res = base_res;
	dc->base = base;
	dc->irq = irq;
	dc->ndev = ndev;
	dc->pdata = ndev->dev.platform_data;

	/*
	 * The emc is a shared clock, it will be set based on
	 * the requirements for each user on the bus.
	 */
	dc->emc_clk_rate = 0;

	mutex_init(&dc->lock);
	mutex_init(&dc->one_shot_lock);
	init_completion(&dc->frame_end_complete);
	init_waitqueue_head(&dc->wq);
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	INIT_WORK(&dc->reset_work, tegra_dc_reset_worker);
#endif
	INIT_WORK(&dc->vblank_work, tegra_dc_vblank);
	INIT_DELAYED_WORK(&dc->underflow_work, tegra_dc_underflow_worker);
	INIT_DELAYED_WORK(&dc->one_shot_work, tegra_dc_one_shot_worker);

	tegra_dc_init_lut_defaults(&dc->fb_lut);

	dc->n_windows = DC_N_WINDOWS;
	for (i = 0; i < dc->n_windows; i++) {
		struct tegra_dc_win *win = &dc->windows[i];
		win->idx = i;
		win->dc = dc;
		tegra_dc_init_csc_defaults(&win->csc);
		tegra_dc_init_lut_defaults(&win->lut);
	}

	ret = tegra_dc_set(dc, ndev->id);
	if (ret < 0) {
		dev_err(&ndev->dev, "can't add dc\n");
		goto err_free_irq;
	}

	nvhost_set_drvdata(ndev, dc);

	tegra_dc_feature_register(dc);

#ifdef CONFIG_SWITCH
	dc->modeset_switch.name = dev_name(&ndev->dev);
	dc->modeset_switch.state = 0;
	dc->modeset_switch.print_state = switch_modeset_print_mode;
	switch_dev_register(&dc->modeset_switch);
#endif

	if (dc->pdata->default_out)
		tegra_dc_set_out(dc, dc->pdata->default_out);
	else
		dev_err(&ndev->dev, "No default output specified.  Leaving output disabled.\n");

	dc->vblank_syncpt = (dc->ndev->id == 0) ?
		NVSYNCPT_VBLANK0 : NVSYNCPT_VBLANK1;

	dc->ext = tegra_dc_ext_register(ndev, dc);
	if (IS_ERR_OR_NULL(dc->ext)) {
		dev_warn(&ndev->dev, "Failed to enable Tegra DC extensions.\n");
		dc->ext = NULL;
	}

	mutex_lock(&dc->lock);
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA	
	if (dc->pdata->flags & TEGRA_DC_FLAG_ENABLED)
		dc->enabled = _tegra_dc_enable_noreset(dc);
#else
	if (dc->pdata->flags & TEGRA_DC_FLAG_ENABLED)
		dc->enabled = _tegra_dc_enable(dc);
#endif		
	mutex_unlock(&dc->lock);

	/* interrupt handler must be registered before tegra_fb_register() */
	if (request_irq(irq, tegra_dc_irq, IRQF_DISABLED,
			dev_name(&ndev->dev), dc)) {
		dev_err(&ndev->dev, "request_irq %d failed\n", irq);
		ret = -EBUSY;
		goto err_put_emc_clk;
	}

	tegra_dc_create_debugfs(dc);

	dev_info(&ndev->dev, "probed\n");

	if (dc->pdata->fb) {
		if (dc->pdata->fb->bits_per_pixel == -1) {
			unsigned long fmt;
			tegra_dc_writel(dc,
					WINDOW_A_SELECT << dc->pdata->fb->win,
					DC_CMD_DISPLAY_WINDOW_HEADER);

			fmt = tegra_dc_readl(dc, DC_WIN_COLOR_DEPTH);
			dc->pdata->fb->bits_per_pixel =
				tegra_dc_fmt_bpp(fmt);
		}

		mode = tegra_dc_get_override_mode(dc);
		if (mode) {
			dc->pdata->fb->xres = mode->h_active;
			dc->pdata->fb->yres = mode->v_active;
		}

		dc->fb = tegra_fb_register(ndev, dc, dc->pdata->fb, fb_mem);
		if (IS_ERR_OR_NULL(dc->fb))
			dc->fb = NULL;
	}

	if (dc->out && dc->out->hotplug_init)
		dc->out->hotplug_init();

	if (dc->out_ops && dc->out_ops->detect)
		dc->out_ops->detect(dc);
	else
		dc->connected = true;

	tegra_dc_create_sysfs(&dc->ndev->dev);

	return 0;

err_free_irq:
	free_irq(irq, dc);
err_put_emc_clk:
	clk_put(emc_clk);
err_put_clk:
	clk_put(clk);
err_iounmap_reg:
	iounmap(base);
	if (fb_mem)
		release_resource(fb_mem);
err_release_resource_reg:
	release_resource(base_res);
err_free:
	kfree(dc);

	return ret;
}

static int tegra_dc_remove(struct nvhost_device *ndev)
{
	struct tegra_dc *dc = nvhost_get_drvdata(ndev);

	tegra_dc_remove_sysfs(&dc->ndev->dev);
	tegra_dc_remove_debugfs(dc);

	if (dc->fb) {
		tegra_fb_unregister(dc->fb);
		if (dc->fb_mem)
			release_resource(dc->fb_mem);
	}

	tegra_dc_ext_disable(dc->ext);

	if (dc->ext)
		tegra_dc_ext_unregister(dc->ext);

	if (dc->enabled)
		_tegra_dc_disable(dc);

#ifdef CONFIG_SWITCH
	switch_dev_unregister(&dc->modeset_switch);
#endif
	free_irq(dc->irq, dc);
	clk_put(dc->emc_clk);
	clk_put(dc->clk);
	iounmap(dc->base);
	if (dc->fb_mem)
		release_resource(dc->base_res);
	kfree(dc);
	tegra_dc_set(NULL, ndev->id);
	return 0;
}

#ifdef CONFIG_PM
static int tegra_dc_suspend(struct nvhost_device *ndev, pm_message_t state)
{
#ifndef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
	struct tegra_dc *dc = nvhost_get_drvdata(ndev);

	dev_info(&ndev->dev, "suspend\n");

	tegra_dc_ext_disable(dc->ext);

	mutex_lock(&dc->lock);

	if (dc->out_ops && dc->out_ops->suspend)
		dc->out_ops->suspend(dc);

	if (dc->enabled) {
		_tegra_dc_disable(dc);

		dc->suspended = true;
	}

	if (dc->out && dc->out->postsuspend) {
		dc->out->postsuspend();
		if (dc->out->type && dc->out->type == TEGRA_DC_OUT_HDMI)
			/*
			 * avoid resume event due to voltage falling
			 */
			msleep(100);
	}

	mutex_unlock(&dc->lock);
#endif
	return 0;
}

static int tegra_dc_resume(struct nvhost_device *ndev)
{
	struct tegra_dc *dc = nvhost_get_drvdata(ndev);

	dev_info(&ndev->dev, "resume\n");

	mutex_lock(&dc->lock);
	dc->suspended = false;

	if (dc->enabled)
		_tegra_dc_enable(dc);

	if (dc->out && dc->out->hotplug_init)
		dc->out->hotplug_init();

	if (dc->out_ops && dc->out_ops->resume)
		dc->out_ops->resume(dc);
	mutex_unlock(&dc->lock);

	return 0;
}

#endif /* CONFIG_PM */

static void tegra_dc_shutdown(struct nvhost_device *ndev)
{
	struct tegra_dc *dc = nvhost_get_drvdata(ndev);

	if (!dc || !dc->enabled)
		return;

	tegra_dc_blank(dc);
	tegra_dc_disable(dc);
}

extern int suspend_set(const char *val, struct kernel_param *kp)
{
	if (!strcmp(val, "dump"))
		dump_regs(tegra_dcs[0]);
#ifdef CONFIG_PM
	else if (!strcmp(val, "suspend"))
		tegra_dc_suspend(tegra_dcs[0]->ndev, PMSG_SUSPEND);
	else if (!strcmp(val, "resume"))
		tegra_dc_resume(tegra_dcs[0]->ndev);
#endif

	return 0;
}

extern int suspend_get(char *buffer, struct kernel_param *kp)
{
	return 0;
}

int suspend;

module_param_call(suspend, suspend_set, suspend_get, &suspend, 0644);

#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
static int tegra_dc_prepare(struct device *dev)
{
	struct nvhost_device *ndev = to_nvhost_device(dev);
	struct tegra_dc *dc = nvhost_get_drvdata(ndev);

	dev_info(&ndev->dev, "prepare\n");
/*
	if (dc->overlay)
		tegra_overlay_disable(dc->overlay);
*/
	mutex_lock(&dc->lock);
	if (dc->out_ops && dc->out_ops->suspend)
		dc->out_ops->suspend(dc);

	if (dc->enabled) {
		/*tegra_fb_suspend(dc->fb);*/
		_tegra_dc_disable(dc);

		dc->suspended = true;
	}
	mutex_unlock(&dc->lock);

	return 0;
}

static void tegra_dc_complete(struct device *dev)
{
	struct nvhost_device *ndev = to_nvhost_device(dev);
	struct tegra_dc *dc = nvhost_get_drvdata(ndev);

	dev_info(&ndev->dev, "complete\n");

	mutex_lock(&dc->lock);
	dc->suspended = false;

	if (dc->enabled)
		_tegra_dc_enable(dc);

	if (dc->out_ops && dc->out_ops->resume)
		dc->out_ops->resume(dc);
	mutex_unlock(&dc->lock);
}

const struct dev_pm_ops tegra_dc_pm_ops = {
	.prepare = tegra_dc_prepare,
	.complete = tegra_dc_complete,
};
#endif

struct nvhost_driver tegra_dc_driver = {
	.driver = {
		.name = "tegradc",
		.owner = THIS_MODULE,
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
		.pm = &tegra_dc_pm_ops,
#endif
	},
	.probe = tegra_dc_probe,
	.remove = tegra_dc_remove,
#ifdef CONFIG_PM
	.suspend = tegra_dc_suspend,
	.resume = tegra_dc_resume,
#endif
	.shutdown = tegra_dc_shutdown,
};

#ifndef MODULE
static int __init parse_disp_params(char *options, struct tegra_dc_mode *mode)
{
	int i, params[11];
	char *p;

	for (i = 0; i < ARRAY_SIZE(params); i++) {
		if ((p = strsep(&options, ",")) != NULL) {
			if (*p)
				params[i] = simple_strtoul(p, &p, 10);
		} else
			return -EINVAL;
	}

	if ((mode->pclk = params[0]) == 0)
		return -EINVAL;

	mode->h_active      = params[1];
	mode->v_active      = params[2];
	mode->h_ref_to_sync = params[3];
	mode->v_ref_to_sync = params[4];
	mode->h_sync_width  = params[5];
	mode->v_sync_width  = params[6];
	mode->h_back_porch  = params[7];
	mode->v_back_porch  = params[8];
	mode->h_front_porch = params[9];
	mode->v_front_porch = params[10];

	return 0;
}

static int __init tegra_dc_mode_override(char *str)
{
	char *p = str, *options;

	if (!p || !*p)
		return -EINVAL;

	p = strstr(str, "hdmi:");
	if (p) {
		p += 5;
		options = strsep(&p, ";");
		if (parse_disp_params(options, &override_disp_mode[TEGRA_DC_OUT_HDMI]))
			return -EINVAL;
	}

	p = strstr(str, "rgb:");
	if (p) {
		p += 4;
		options = strsep(&p, ";");
		if (parse_disp_params(options, &override_disp_mode[TEGRA_DC_OUT_RGB]))
			return -EINVAL;
	}

	p = strstr(str, "dsi:");
	if (p) {
		p += 4;
		options = strsep(&p, ";");
		if (parse_disp_params(options, &override_disp_mode[TEGRA_DC_OUT_DSI]))
			return -EINVAL;
	}

	return 0;
}

__setup("disp_params=", tegra_dc_mode_override);
#endif

static int __init tegra_dc_module_init(void)
{
	int ret = tegra_dc_ext_module_init();
	if (ret)
		return ret;
	return nvhost_driver_register(&tegra_dc_driver);
}

static void __exit tegra_dc_module_exit(void)
{
	nvhost_driver_unregister(&tegra_dc_driver);
	tegra_dc_ext_module_exit();
}

module_exit(tegra_dc_module_exit);
module_init(tegra_dc_module_init);
