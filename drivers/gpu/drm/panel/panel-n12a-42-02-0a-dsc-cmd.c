// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 Xiaomi Inc.
 * Copyright (c) 2022 MediaTek Inc.
 */

#include <linux/backlight.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_modes.h>
#include <linux/delay.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_graph.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"
#include "../mediatek/mediatek_v2/mtk_log.h"
#endif
#include "../mediatek/mediatek_v2/mi_disp/mi_panel_ext.h"
#include "../mediatek/mediatek_v2/mi_disp/mi_dsi_panel.h"
#include "../mediatek/mediatek_v2/mtk_dsi.h"

#include "include/panel-n12a-42-02-0a-dsc-cmd.h"

/*
 * Early panels need a fix-up pushed ahead of the initialisation table. The
 * stock driver keys that off the panel build id it reads out of the command
 * line: three ids want the whole ffd sequence, one wants only the enable, and
 * anything later needs neither.
 */
#define BUILD_ID_FFD_FULL_A		0x8040
#define BUILD_ID_FFD_FULL_B		0x8140
#define BUILD_ID_FFD_FULL_C		0x9040
#define BUILD_ID_FFD_ON_ONLY		0xc040

static struct regulator *disp_vci;
static struct regulator *disp_vddi;
static bool vibr_start_up = true;
static bool vio18_start_up = true;

static struct lcm *panel_ctx;
static unsigned int panel_build_id;
static int current_fps = 60;

static char build_id_cmdline[8];
module_param_string(build_id, build_id_cmdline, sizeof(build_id_cmdline), 0600);
MODULE_PARM_DESC(build_id, "build_id=<buildid_info>");

static char oled_wp_cmdline[16];
module_param_string(oled_wp, oled_wp_cmdline, sizeof(oled_wp_cmdline), 0600);
MODULE_PARM_DESC(oled_wp, "oled_wp=<white point info>");

static inline struct lcm *panel_to_lcm(struct drm_panel *panel)
{
	return container_of(panel, struct lcm, panel);
}

static void lcm_dcs_write(struct lcm *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;
	char *addr;

	if (ctx->error < 0)
		return;

	addr = (char *)data;
	if ((int)*addr < 0xB0)
		ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	else
		ret = mipi_dsi_generic_write(dsi, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}

#define lcm_dcs_write_seq_static(ctx, seq...) \
({\
	static const u8 d[] = { seq };\
	lcm_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

static void push_table(struct lcm *ctx, struct LCM_setting_table *table,
		       unsigned int count)
{
	unsigned int i, j;
	unsigned char temp[255] = {0};

	for (i = 0; i < count; i++) {
		unsigned int cmd = table[i].cmd;

		memset(temp, 0, sizeof(temp));
		switch (cmd) {
		case REGFLAG_DELAY:
			msleep(table[i].count);
			break;
		case REGFLAG_END_OF_TABLE:
			break;
		default:
			temp[0] = cmd;
			for (j = 0; j < table[i].count; j++)
				temp[j + 1] = table[i].para_list[j];
			lcm_dcs_write(ctx, temp, table[i].count + 1);
		}
	}
}

static bool panel_needs_ffd_setting(void)
{
	return panel_build_id == BUILD_ID_FFD_FULL_A ||
	       panel_build_id == BUILD_ID_FFD_FULL_B ||
	       panel_build_id == BUILD_ID_FFD_FULL_C;
}

static bool panel_needs_ffd_on(void)
{
	return panel_needs_ffd_setting() || panel_build_id == BUILD_ID_FFD_ON_ONLY;
}

static int lcm_panel_vci_regulator_init(struct device *dev)
{
	static int vibr_regulator_inited;
	int ret = 0;

	if (vibr_regulator_inited)
		return ret;

	/* please only get regulator once in a driver */
	disp_vci = regulator_get(dev, "vibr30");
	if (IS_ERR(disp_vci)) {
		ret = PTR_ERR(disp_vci);
		disp_vci = NULL;
		pr_err("get disp_vci fail, error: %d\n", ret);
		return ret;
	}

	vibr_regulator_inited = 1;
	return ret;
}

static int lcm_panel_vci_enable(struct device *dev)
{
	int ret;
	int retval = 0;

	if (!disp_vci)
		return 0;

	ret = regulator_set_voltage(disp_vci, 3000000, 3000000);
	if (ret < 0)
		pr_err("set voltage disp_vci fail, ret = %d\n", ret);
	retval |= ret;

	if (!regulator_is_enabled(disp_vci) || vibr_start_up) {
		ret = regulator_enable(disp_vci);
		if (ret < 0)
			pr_err("enable regulator disp_vci fail, ret = %d\n", ret);
		vibr_start_up = false;
		retval |= ret;
	}

	return retval;
}

static int lcm_panel_vci_disable(struct device *dev)
{
	int ret = 0;

	if (!disp_vci)
		return 0;

	if (regulator_is_enabled(disp_vci)) {
		ret = regulator_disable(disp_vci);
		if (ret < 0)
			pr_err("disable regulator disp_vci fail, ret = %d\n", ret);
	}

	return ret;
}

static int lcm_panel_vddi_regulator_init(struct device *dev)
{
	static int vio18_regulator_inited;
	int ret = 0;

	if (vio18_regulator_inited)
		return ret;

	disp_vddi = regulator_get(dev, "vio18");
	if (IS_ERR(disp_vddi)) {
		ret = PTR_ERR(disp_vddi);
		disp_vddi = NULL;
		pr_err("get disp_vddi fail, error: %d\n", ret);
		return ret;
	}

	vio18_regulator_inited = 1;
	return ret;
}

static int lcm_panel_vddi_enable(struct device *dev)
{
	int ret;
	int retval = 0;

	if (!disp_vddi)
		return 0;

	ret = regulator_set_voltage(disp_vddi, 1800000, 1800000);
	if (ret < 0)
		pr_err("set voltage disp_vddi fail, ret = %d\n", ret);
	retval |= ret;

	if (!regulator_is_enabled(disp_vddi) || vio18_start_up) {
		ret = regulator_enable(disp_vddi);
		if (ret < 0)
			pr_err("enable regulator disp_vddi fail, ret = %d\n", ret);
		vio18_start_up = false;
		retval |= ret;
	}

	return retval;
}

static int lcm_panel_vddi_disable(struct device *dev)
{
	int ret = 0;

	if (!disp_vddi)
		return 0;

	if (regulator_is_enabled(disp_vddi)) {
		ret = regulator_disable(disp_vddi);
		if (ret < 0)
			pr_err("disable regulator disp_vddi fail, ret = %d\n", ret);
	}

	return ret;
}

static void lcm_panel_init(struct lcm *ctx)
{
	ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return;
	}

	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10 * 1000, (10 * 1000) + 20);
	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(1 * 1000, (1 * 1000) + 20);
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(11 * 1000, (11 * 1000) + 20);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	pr_info("%s: build_id 0x%x\n", __func__, panel_build_id);

	if (panel_needs_ffd_setting())
		push_table(ctx, ffd_setting, ARRAY_SIZE(ffd_setting));
	if (panel_needs_ffd_on())
		push_table(ctx, ffd_on, ARRAY_SIZE(ffd_on));

	push_table(ctx, init_setting, ARRAY_SIZE(init_setting));
}

static int lcm_unprepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (!ctx->prepared)
		return 0;

	pr_info("%s\n", __func__);

	mutex_lock(&ctx->panel_lock);
	lcm_dcs_write_seq_static(ctx, MIPI_DCS_SET_DISPLAY_OFF);
	msleep(20);
	lcm_dcs_write_seq_static(ctx, MIPI_DCS_ENTER_SLEEP_MODE);
	msleep(125);
	mutex_unlock(&ctx->panel_lock);

	ctx->error = 0;
	ctx->prepared = false;

	return 0;
}

static int lcm_prepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret;

	pr_info("%s\n", __func__);

	if (ctx->prepared)
		return 0;

	mutex_lock(&ctx->panel_lock);
	lcm_panel_init(ctx);
	mutex_unlock(&ctx->panel_lock);

	ret = ctx->error;
	if (ret < 0)
		lcm_unprepare(panel);

	ctx->prepared = true;
	ctx->gir_status = 1;

	return ret;
}

static int lcm_panel_poweron(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (ctx->prepared)
		return 0;

	pr_info("%s\n", __func__);

	/* VDDI 1.8V */
	lcm_panel_vddi_enable(ctx->dev);
	udelay(1000);

	/* VDD 1.2V */
	ctx->dvdd_gpio = devm_gpiod_get_index(ctx->dev, "dvdd", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->dvdd_gpio)) {
		dev_err(ctx->dev, "%s: cannot get dvdd gpio %ld\n",
			__func__, PTR_ERR(ctx->dvdd_gpio));
		return PTR_ERR(ctx->dvdd_gpio);
	}
	gpiod_set_value(ctx->dvdd_gpio, 1);
	devm_gpiod_put(ctx->dev, ctx->dvdd_gpio);
	udelay(1000);

	/* VCI 3.0V */
	lcm_panel_vci_enable(ctx->dev);
	udelay(12 * 1000);

	return 0;
}

static int lcm_panel_poweroff(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (ctx->prepared)
		return 0;

	ctx->reset_gpio = devm_gpiod_get_index(ctx->dev, "reset", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	gpiod_set_value(ctx->reset_gpio, 0);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);
	udelay(1000);

	lcm_panel_vci_disable(ctx->dev);
	udelay(1000);

	ctx->dvdd_gpio = devm_gpiod_get_index(ctx->dev, "dvdd", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->dvdd_gpio)) {
		dev_err(ctx->dev, "%s: cannot get dvdd gpio %ld\n",
			__func__, PTR_ERR(ctx->dvdd_gpio));
		return PTR_ERR(ctx->dvdd_gpio);
	}
	gpiod_set_value(ctx->dvdd_gpio, 0);
	devm_gpiod_put(ctx->dev, ctx->dvdd_gpio);
	udelay(1000);

	lcm_panel_vddi_disable(ctx->dev);
	udelay(1000);

	return 0;
}

static int lcm_disable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	return 0;
}

static int lcm_enable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;

	return 0;
}

/*
 * Every refresh rate shares one set of parameters: the stock driver carries
 * four copies of this struct and they are byte for byte identical.
 */
static struct mtk_panel_params ext_params = {
	.pll_clk = PLL_CLOCK,
	.data_rate = DATA_RATE,
	.cust_esd_check = 0,
	.esd_check_enable = 0,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x1c,
	},
	.is_cphy = 0,
	.lcm_color_mode = MTK_DRM_COLOR_MODE_DISPLAY_P3,
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
	.dsc_params = {
		.enable                =  DSC_ENABLE,
		.ver                   =  DSC_VER,
		.slice_mode            =  DSC_SLICE_MODE,
		.rgb_swap              =  DSC_RGB_SWAP,
		.dsc_cfg               =  DSC_DSC_CFG,
		.rct_on                =  DSC_RCT_ON,
		.bit_per_channel       =  DSC_BIT_PER_CHANNEL,
		.dsc_line_buf_depth    =  DSC_DSC_LINE_BUF_DEPTH,
		.bp_enable             =  DSC_BP_ENABLE,
		.bit_per_pixel         =  DSC_BIT_PER_PIXEL,
		.pic_height            =  DSC_PIC_HEIGHT,
		.pic_width             =  DSC_PIC_WIDTH,
		.slice_height          =  DSC_SLICE_HEIGHT,
		.slice_width           =  DSC_SLICE_WIDTH,
		.chunk_size            =  DSC_CHUNK_SIZE,
		.xmit_delay            =  DSC_XMIT_DELAY,
		.dec_delay             =  DSC_DEC_DELAY,
		.scale_value           =  DSC_SCALE_VALUE,
		.increment_interval    =  DSC_INCREMENT_INTERVAL,
		.decrement_interval    =  DSC_DECREMENT_INTERVAL,
		.line_bpg_offset       =  DSC_LINE_BPG_OFFSET,
		.nfl_bpg_offset        =  DSC_NFL_BPG_OFFSET,
		.slice_bpg_offset      =  DSC_SLICE_BPG_OFFSET,
		.initial_offset        =  DSC_INITIAL_OFFSET,
		.final_offset          =  DSC_FINAL_OFFSET,
		.flatness_minqp        =  DSC_FLATNESS_MINQP,
		.flatness_maxqp        =  DSC_FLATNESS_MAXQP,
		.rc_model_size         =  DSC_RC_MODEL_SIZE,
		.rc_edge_factor        =  DSC_RC_EDGE_FACTOR,
		.rc_quant_incr_limit0  =  DSC_RC_QUANT_INCR_LIMIT0,
		.rc_quant_incr_limit1  =  DSC_RC_QUANT_INCR_LIMIT1,
		.rc_tgt_offset_hi      =  DSC_RC_TGT_OFFSET_HI,
		.rc_tgt_offset_lo      =  DSC_RC_TGT_OFFSET_LO,
		.ext_pps_cfg = {
			.enable = 1,
			.rc_buf_thresh = rc_buf_thresh,
			.range_min_qp = range_min_qp,
			.range_max_qp = range_max_qp,
			.range_bpg_ofs = range_bpg_ofs,
		},
	},
};

struct drm_display_mode *get_mode_by_id(struct drm_connector *connector,
					unsigned int mode)
{
	struct drm_display_mode *m;
	unsigned int i = 0;

	list_for_each_entry(m, &connector->modes, head) {
		if (i == mode)
			return m;
		i++;
	}
	return NULL;
}

static int mtk_panel_ext_param_get(struct drm_panel *panel,
				   struct drm_connector *connector,
				   struct mtk_panel_params **ext_param,
				   unsigned int mode)
{
	struct drm_display_mode *m_dst = get_mode_by_id(connector, mode);
	int dst_fps = m_dst ? drm_mode_vrefresh(m_dst) : -EINVAL;

	switch (dst_fps) {
	case 60:
	case 90:
	case 120:
	case 144:
		*ext_param = &ext_params;
		current_fps = dst_fps;
		return 0;
	default:
		pr_err("%s, dst_fps %d\n", __func__, dst_fps);
		return -EINVAL;
	}
}

static int mtk_panel_ext_param_set(struct drm_panel *panel,
				   struct drm_connector *connector,
				   unsigned int mode)
{
	struct mtk_panel_ext *ext = find_panel_ext(panel);
	struct lcm *ctx = panel_to_lcm(panel);
	struct drm_display_mode *m_dst = get_mode_by_id(connector, mode);
	int dst_fps = m_dst ? drm_mode_vrefresh(m_dst) : -EINVAL;

	switch (dst_fps) {
	case 60:
	case 90:
	case 120:
	case 144:
		ext->params = &ext_params;
		ctx->dynamic_fps = dst_fps;
		current_fps = dst_fps;
		return 0;
	default:
		pr_err("%s, dst_fps %d\n", __func__, dst_fps);
		return -EINVAL;
	}
}

static void mode_switch_to(struct drm_panel *panel, int fps)
{
	struct lcm *ctx = panel_to_lcm(panel);

	switch (fps) {
	case 60:
		push_table(ctx, mode_60hz_setting, ARRAY_SIZE(mode_60hz_setting));
		break;
	case 90:
		push_table(ctx, mode_90hz_setting, ARRAY_SIZE(mode_90hz_setting));
		break;
	case 120:
		push_table(ctx, mode_120hz_setting, ARRAY_SIZE(mode_120hz_setting));
		break;
	default:
		push_table(ctx, mode_144hz_setting, ARRAY_SIZE(mode_144hz_setting));
		break;
	}

	ctx->dynamic_fps = fps;
}

static int mode_switch(struct drm_panel *panel,
		       struct drm_connector *connector, unsigned int cur_mode,
		       unsigned int dst_mode, enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	struct lcm *ctx = panel_to_lcm(panel);
	struct drm_display_mode *m_dst = get_mode_by_id(connector, dst_mode);
	int dst_fps = m_dst ? drm_mode_vrefresh(m_dst) : -EINVAL;

	if (stage != BEFORE_DSI_POWERDOWN)
		return 0;

	switch (dst_fps) {
	case 60:
	case 90:
	case 120:
	case 144:
		mutex_lock(&ctx->panel_lock);
		mode_switch_to(panel, dst_fps);
		mutex_unlock(&ctx->panel_lock);
		return 0;
	default:
		pr_err("%s, dst_fps %d\n", __func__, dst_fps);
		return -EINVAL;
	}
}

static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct lcm *ctx = panel_to_lcm(panel);

	ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	gpiod_set_value(ctx->reset_gpio, on);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	return 0;
}

/*
 * Brightness path.
 *
 * The panel keeps its 0x51 level in DDIC registers that do not survive the
 * reset an ESD recovery performs, so remember the last non-zero level and put
 * it back afterwards - restoring 0 would leave the screen dark until userspace
 * happens to push a new level.
 *
 * While AOD is on the doze path owns the brightness (it drives a much smaller
 * range), so swallow the normal updates instead of fighting it.
 */
static unsigned int last_non_zero_bl_level = 511;
static atomic_t doze_enable = ATOMIC_INIT(0);

static int lcm_setbacklight_cmdq(void *dsi, dcs_write_gce cb, void *handle,
				 unsigned int level)
{
	char bl_tb0[] = {0x51, 0x00, 0x00};
	struct mtk_dsi *mtk_dsi = dsi;
	struct lcm *ctx = panel_ctx;

	if (!dsi) {
		pr_err("dsi is null\n");
		return -EINVAL;
	}
	if (!cb || !ctx)
		return -1;

	if (level) {
		bl_tb0[1] = (level >> 8) & 0xff;
		bl_tb0[2] = level & 0xff;
		mtk_dsi->mi_cfg.last_no_zero_bl_level = level;
	}

	if (atomic_read(&doze_enable)) {
		pr_info("%s: Return it when aod on, %d %d %d\n", __func__,
			level, bl_tb0[1], bl_tb0[2]);
		return 0;
	}

	pr_info("%s %d %d %d\n", __func__, level, bl_tb0[1], bl_tb0[2]);

	mutex_lock(&ctx->panel_lock);
	cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));
	mutex_unlock(&ctx->panel_lock);

	if (level)
		last_non_zero_bl_level = level;

	mtk_dsi->mi_cfg.last_bl_level = level;

	return 0;
}

static void lcm_esd_restore_backlight(struct drm_panel *panel)
{
	char bl_tb0[] = {0x51, 0x00, 0x00};
	struct lcm *ctx = panel_to_lcm(panel);

	bl_tb0[1] = (last_non_zero_bl_level >> 8) & 0xff;
	bl_tb0[2] = last_non_zero_bl_level & 0xff;

	pr_info("%s: restore to level = %d\n", __func__, last_non_zero_bl_level);

	mutex_lock(&ctx->panel_lock);
	lcm_dcs_write(ctx, bl_tb0, ARRAY_SIZE(bl_tb0));
	mutex_unlock(&ctx->panel_lock);
}

static bool get_panel_initialized(struct drm_panel *panel)
{
	struct lcm *ctx;

	if (!panel) {
		pr_err("%s panel is NULL\n", __func__);
		return false;
	}

	ctx = panel_to_lcm(panel);

	return ctx->prepared;
}

static int panel_get_panel_info(struct drm_panel *panel, char *buf)
{
	struct lcm *ctx;

	if (!panel || !buf) {
		pr_err("invalid params\n");
		return -EAGAIN;
	}

	ctx = panel_to_lcm(panel);

	return snprintf(buf, PAGE_SIZE, "%s\n", ctx->panel_info);
}

static int panel_get_max_brightness_clone(struct drm_panel *panel,
					  u32 *max_brightness_clone)
{
	struct lcm *ctx;

	if (!panel) {
		pr_err("invalid params\n");
		return -EAGAIN;
	}

	ctx = panel_to_lcm(panel);
	*max_brightness_clone = ctx->max_brightness_clone;

	return 0;
}

static int panel_get_factory_max_brightness(struct drm_panel *panel,
					    u32 *max_brightness_clone)
{
	struct lcm *ctx;

	if (!panel) {
		pr_err("invalid params\n");
		return -EAGAIN;
	}

	ctx = panel_to_lcm(panel);
	*max_brightness_clone = ctx->factory_max_brightness;

	return 0;
}

/*
 * AOD.
 *
 * The panel has its own low-power idle mode: doze_enable only records that we
 * are in it (the display driver has already sent the mode change), while
 * doze_disable takes the panel back out with DCS exit_idle_mode. Brightness in
 * that mode is not the usual 0x51 range - it is one of two fixed levels, which
 * is why the stock driver ships them as tables rather than a value.
 */
static int panel_doze_enable(struct drm_panel *panel, void *dsi,
			     dcs_write_gce cb, void *handle)
{
	atomic_set(&doze_enable, 1);
	pr_info("%s !-\n", __func__);

	return 0;
}

static int panel_doze_disable(struct drm_panel *panel, void *dsi,
			      dcs_write_gce cb, void *handle)
{
	char exit_idle_mode[] = {0x38, 0x00};

	if (!dsi) {
		pr_err("%s dsi is null\n", __func__);
		return -1;
	}
	if (!panel) {
		pr_err("%s invalid panel\n", __func__);
		return -1;
	}

	pr_info("%s +\n", __func__);

	cb(dsi, handle, exit_idle_mode, ARRAY_SIZE(exit_idle_mode));
	atomic_set(&doze_enable, 0);

	pr_info("%s -\n", __func__);

	return 0;
}

static int panel_set_doze_brightness(struct drm_panel *panel,
				     int doze_brightness)
{
	/*
	 * This panel takes its two AOD levels as an extended 0x51 write rather
	 * than as a table of its own, unlike the 36-02-0b part.
	 */
	struct LCM_setting_table aod_low[] = {
		{0x51, 6, {0x00, 0x3D, 0x00, 0x3D, 0x05, 0x55} },
	};
	struct LCM_setting_table aod_high[] = {
		{0x51, 6, {0x04, 0x00, 0x04, 0x00, 0x3F, 0xFF} },
	};
	struct LCM_setting_table *table;
	struct lcm *ctx;
	int ret = 0;

	if (!panel) {
		pr_err("invalid params\n");
		return -1;
	}

	ctx = panel_to_lcm(panel);

	if (ctx->doze_brightness_state == doze_brightness) {
		pr_info("%s skip same doze_brightness set:%d\n", __func__,
			doze_brightness);
		return 0;
	}

	/*
	 * Userspace can ask for a doze level while the panel is running
	 * normally; the levels only mean anything in idle mode, so record the
	 * request and let the next doze_enable pick it up.
	 */
	if (!atomic_read(&doze_enable)) {
		pr_info("%s normal mode cannot set doze brightness\n", __func__);
		goto out;
	}

	switch (doze_brightness) {
	case DOZE_BRIGHTNESS_LBM:
		table = aod_low;
		break;
	case DOZE_BRIGHTNESS_HBM:
		table = aod_high;
		break;
	default:
		if (doze_brightness == DOZE_TO_NORMAL)
			atomic_set(&doze_enable, 0);
		goto out;
	}

	ret = mi_disp_panel_ddic_send_cmd(table, 1, false);
	if (ret) {
		mtk_dprec_logger_pr(0, "%s: failed to send ddic cmd\n", __func__);
		DDPPR_ERR("%s: failed to send ddic cmd\n", __func__);
	}

out:
	ctx->doze_brightness_state = doze_brightness;
	pr_info("%s end -\n", __func__);

	return ret;
}

static int panel_get_doze_brightness(struct drm_panel *panel,
				     u32 *doze_brightness)
{
	struct lcm *ctx;

	if (!panel) {
		pr_err("invalid params\n");
		return -EAGAIN;
	}

	ctx = panel_to_lcm(panel);
	*doze_brightness = ctx->doze_brightness_state;

	return 0;
}

/*
 * GIR ("gamma index remap") is Xiaomi's flat/vivid tone switch. 0x5F 0x00 turns
 * the remap on, 0x5F 0x01 takes it off again; the panel keeps the state itself,
 * so all we have to do is remember which way we last set it for the readback.
 */
static int panel_set_gir_on(struct drm_panel *panel)
{
	struct LCM_setting_table gir_on_set[] = {
		{0x5F, 1, {0x00} },
	};
	struct lcm *ctx;
	int ret = 0;

	pr_info("%s: +\n", __func__);

	if (!panel) {
		pr_err("%s: panel is NULL\n", __func__);
		return -1;
	}

	ctx = panel_to_lcm(panel);
	ctx->gir_status = 1;

	if (!ctx->enabled)
		pr_err("%s: panel isn't enabled\n", __func__);
	else
		ret = mi_disp_panel_ddic_send_cmd(gir_on_set,
						  ARRAY_SIZE(gir_on_set), false);

	return ret;
}

static int panel_set_gir_off(struct drm_panel *panel)
{
	struct lcm *ctx;
	int ret = 0;

	pr_info("%s: +\n", __func__);

	if (!panel) {
		pr_err("%s: panel is NULL\n", __func__);
		return -1;
	}

	ctx = panel_to_lcm(panel);
	ctx->gir_status = 0;

	if (!ctx->enabled)
		pr_err("%s: panel isn't enabled\n", __func__);
	else
		ret = mi_disp_panel_ddic_send_cmd(gir_off_settings,
						  ARRAY_SIZE(gir_off_settings),
						  false);

	return ret;
}

static int panel_get_gir_status(struct drm_panel *panel)
{
	struct lcm *ctx;

	if (!panel) {
		pr_err("%s; panel is NULL\n", __func__);
		return -1;
	}

	ctx = panel_to_lcm(panel);

	return ctx->gir_status;
}

/*
 * White point. The panel is measured on the line and the result is burned into
 * the bootloader, which hands it over on the command line - reading it back out
 * of the DDIC would mean a DSI read during suspend, so Xiaomi does not bother.
 */
static int panel_get_wp_info(struct drm_panel *panel, char *buf, size_t size)
{
	static u16 lux, wx, wy;

	pr_info("%s: +\n", __func__);

	if (lux || wx || wy) {
		pr_info("%s: got wp info from cache\n", __func__);
	} else if (sscanf(oled_wp_cmdline, "%04hx%04hx%04hx", &lux, &wx, &wy) == 3) {
		pr_info("%s: got wp info from cmdline\n", __func__);
	} else {
		pr_err("No panel is Connected !\n");
		pr_info("%s: get error\n", __func__);
		return 0;
	}

	pr_info("%s: Lux=0x%04hx, Wx=0x%04hx, Wy=0x%04hx\n", __func__, lux, wx, wy);

	return snprintf(buf, size, "%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx\n",
			lux >> 8, lux & 0xff, wx >> 8, wx & 0xff,
			wy >> 8, wy & 0xff);
}

/*
 * LHBM - the bright ring the panel draws under the optical fingerprint sensor.
 * The display driver needs to know how many frames to wait after asking for it
 * before the reader may fire, and that count differs between normal and AOD.
 */
static int panel_fod_lhbm_init(struct mtk_dsi *dsi)
{
	if (!dsi) {
		pr_info("invalid dsi point\n");
		return -1;
	}

	pr_info("panel_fod_lhbm_init enter\n");

	dsi->display_type = "primary";
	dsi->mi_cfg.lhbm_ui_ready_delay_frame = 5;
	dsi->mi_cfg.lhbm_ui_ready_delay_frame_aod = 7;
	dsi->mi_cfg.local_hbm_enabled = 1;

	return 0;
}

/*
 * Backlight and ELVSS in one grouped write.
 *
 * ELVSS is the OLED cathode voltage: it has to track the brightness or the
 * panel either clips the highlights or burns more power than it needs to. The
 * display driver works out the pairing and asks for both here, and they have to
 * land in the same frame - hence the grouped write rather than two DCS calls.
 *
 * The tables are static because the callback hands the pointer straight to the
 * CMDQ packet, which is consumed after we return.
 */
static int lcm_set_bl_elvss_cmdq(void *dsi, dcs_grp_write_gce cb, void *handle,
				 struct mtk_bl_ext_config *bl_config)
{
	static struct mtk_panel_para_table bl_tb = {3, {0x51, 0x0f, 0xff} };
	static struct mtk_panel_para_table elvss_tb = {2, {0x83, 0xff} };
	static struct mtk_panel_para_table bl_elvss_tb[2] = {
		{3, {0x51, 0x0f, 0xff} },
		{2, {0x83, 0xff} },
	};
	unsigned int cfg_flag, elvss_pn, level;

	if (!cb)
		return -1;

	cfg_flag = bl_config->cfg_flag;
	elvss_pn = bl_config->elvss_pn;

	if (cfg_flag & BIT(0)) {
		level = bl_config->backlight_level;

		/* AOD owns the brightness, see panel_set_doze_brightness() */
		if (atomic_read(&doze_enable)) {
			pr_info("%s: Return it when aod on, %d %d %d\n", __func__,
				level, (level >> 8) & 0x0f, level & 0xff);
			if (!(cfg_flag & BIT(1)))
				return 0;
			goto elvss;
		}

		if (cfg_flag & BIT(1)) {
			pr_info("%s backlight = -%d\n", __func__, level);
			bl_elvss_tb[0].para_list[1] = (level >> 8) & 0x0f;
			bl_elvss_tb[0].para_list[2] = level & 0xff;

			pr_info("%s elvss = -%d\n", __func__, elvss_pn);
			bl_elvss_tb[1].para_list[1] = elvss_pn | 0x80;

			cb(dsi, handle, bl_elvss_tb, ARRAY_SIZE(bl_elvss_tb));
			return 0;
		}

		pr_info("%s backlight = -%d\n", __func__, level);
		bl_tb.para_list[1] = (level >> 8) & 0x0f;
		bl_tb.para_list[2] = level & 0xff;

		cb(dsi, handle, &bl_tb, 1);
		return 0;
	}

	if (cfg_flag & BIT(1)) {
elvss:
		pr_info("%s elvss = -%d\n", __func__, elvss_pn);
		elvss_tb.para_list[1] = elvss_pn | 0x80;

		cb(dsi, handle, &elvss_tb, 1);
	}

	return 0;
}

/*
 * Entering AOD from a running display.
 *
 * The sequence matters: pick the brightness for whichever AOD level is
 * selected, push it, and only then put the panel into idle mode with DCS
 * enter_idle_mode. Doing it the other way round makes the panel flash at the
 * old brightness for a frame.
 */
static int panel_doze_suspend(struct drm_panel *panel, void *dsi,
			      dcs_write_gce cb, void *handle)
{
	char aod_start[] = {0x2F, 0x00};
	char aod_bl[] = {0x51, 0x00, 0x3D, 0x00, 0x3D, 0x05, 0x55};
	char enter_idle_mode[] = {0x39, 0x00};
	struct lcm *ctx;

	if (!dsi) {
		pr_err("%s dsi is null\n", __func__);
		return -1;
	}
	if (!panel) {
		pr_err("%s invalid panel\n", __func__);
		return -1;
	}

	ctx = panel_to_lcm(panel);
	if (!ctx) {
		pr_err("ctx is null\n");
		return -1;
	}

	if (ctx->doze_suspend) {
		pr_info("%s already suspend, skip\n", __func__);
		goto out;
	}

	if (ctx->doze_brightness_state == DOZE_BRIGHTNESS_HBM) {
		char hbm[] = {0x04, 0x00, 0x04, 0x00, 0x3F, 0xFF};

		memcpy(&aod_bl[1], hbm, sizeof(hbm));
	}

	cb(dsi, handle, aod_start, ARRAY_SIZE(aod_start));
	cb(dsi, handle, aod_bl, ARRAY_SIZE(aod_bl));
	cb(dsi, handle, enter_idle_mode, ARRAY_SIZE(enter_idle_mode));

	ctx->doze_suspend = true;
	pr_info("lhbm enter aod in doze_suspend\n");

out:
	pr_info("%s !-\n", __func__);

	return 0;
}

/*
 * LHBM - the bright spot drawn under the optical fingerprint reader.
 *
 * This part ships one command table per backlight range, with a separate copy
 * for the low-power (HLPM) case that carries the extra command taking the panel
 * out of idle first. Unlike the 36-02-0b part, the alpha is baked into those
 * tables rather than looked up per level.
 */
#define LHBM_TYPE_WHITE_1300		0
#define LHBM_TYPE_WHITE_250		1
#define LHBM_TYPE_GREEN_500		2
#define LHBM_TYPE_OFF			3
#define LHBM_TYPE_HLPM_WHITE_1300	4
#define LHBM_TYPE_HLPM_WHITE_250	5

#define LHBM_BL_MIN			15
#define LHBM_BL_MAX			15603
#define LHBM_BL_INTERVAL1_MAX		1307
#define LHBM_BL_INTERVAL2_MAX		11467

static int mi_disp_panel_send_lhbm(struct mtk_dsi *dsi, int type, int bl_level)
{
	struct LCM_setting_table *table;
	unsigned int count;
	bool hlpm;

	if (!dsi || !dsi->panel || !panel_to_lcm(dsi->panel)) {
		pr_err("ctx is null\n");
		return -1;
	}

	if (type == LHBM_TYPE_OFF) {
		/*
		 * The off sequence restores the backlight itself - the spot was
		 * drawn with the panel's own dimming, so leaving it would darken
		 * the screen until userspace pushes a level.
		 */
		lhbm_off[0].para_list[0] = (bl_level >> 8) & 0xff;
		lhbm_off[0].para_list[1] = bl_level & 0xff;

		return mi_disp_panel_ddic_send_cmd(lhbm_off, ARRAY_SIZE(lhbm_off),
						   FORMAT_LP_MODE | FORMAT_BLOCK);
	}

	hlpm = (type == LHBM_TYPE_HLPM_WHITE_1300 ||
		type == LHBM_TYPE_HLPM_WHITE_250);
	if (!hlpm && type > LHBM_TYPE_GREEN_500) {
		pr_err("unsuppport cmd\n");
		return -EINVAL;
	}

	if (bl_level >= LHBM_BL_MIN && bl_level <= LHBM_BL_INTERVAL1_MAX) {
		table = hlpm ? lhbm_hlpm_80nit_2nit : lhbm_normal_80nit_2nit;
		count = hlpm ? ARRAY_SIZE(lhbm_hlpm_80nit_2nit)
			     : ARRAY_SIZE(lhbm_normal_80nit_2nit);
	} else if (bl_level <= LHBM_BL_INTERVAL2_MAX) {
		table = hlpm ? lhbm_hlpm_700nit_80nit : lhbm_normal_700nit_80nit;
		count = hlpm ? ARRAY_SIZE(lhbm_hlpm_700nit_80nit)
			     : ARRAY_SIZE(lhbm_normal_700nit_80nit);
	} else if (bl_level <= LHBM_BL_MAX) {
		table = hlpm ? lhbm_hlpm_1600nit_700nit : lhbm_normal_1600nit_700nit;
		count = hlpm ? ARRAY_SIZE(lhbm_hlpm_1600nit_700nit)
			     : ARRAY_SIZE(lhbm_normal_1600nit_700nit);
	} else {
		pr_info("Error--lhbm_cmd_type:%d , %d backlight is Out of range\n",
			type, bl_level);
		return -EINVAL;
	}

	return mi_disp_panel_ddic_send_cmd(table, count,
					   FORMAT_LP_MODE | FORMAT_BLOCK);
}

static int panel_set_lhbm_fod(struct mtk_dsi *dsi, enum local_hbm_state lhbm_state)
{
	struct mi_dsi_panel_cfg *mi_cfg;
	struct lcm *ctx;
	int bl_level, type;

	if (!dsi || !dsi->panel) {
		pr_err("%s: panel is NULL\n", __func__);
		return -1;
	}

	ctx = panel_to_lcm(dsi->panel);
	if (!ctx->enabled) {
		pr_err("%s: panel isn't enabled\n", __func__);
		return -1;
	}

	mi_cfg = &dsi->mi_cfg;
	bl_level = mi_cfg->last_bl_level;

	pr_info("%s local hbm_state :%d \n", __func__, lhbm_state);

	switch (lhbm_state) {
	case LOCAL_HBM_OFF_TO_NORMAL:
		pr_info("LOCAL_HBM_NORMAL off\n");
		type = LHBM_TYPE_OFF;
		bl_level = mi_cfg->last_no_zero_bl_level;
		ctx->lhbm_en = false;
		break;
	case LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT:
		pr_info("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT\n");
		type = LHBM_TYPE_OFF;
		ctx->lhbm_en = false;
		break;
	case LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE:
		pr_info("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE\n");
		type = LHBM_TYPE_OFF;
		bl_level = mi_cfg->last_no_zero_bl_level;
		ctx->lhbm_en = false;
		break;
	case LOCAL_HBM_NORMAL_WHITE_1000NIT:
		pr_info("LOCAL_HBM_NORMAL_WHITE_1300NIT in HBM\n");
		type = atomic_read(&doze_enable) ? LHBM_TYPE_HLPM_WHITE_1300
						 : LHBM_TYPE_WHITE_1300;
		ctx->lhbm_en = true;
		break;
	case LOCAL_HBM_NORMAL_WHITE_110NIT:
		type = atomic_read(&doze_enable) ? LHBM_TYPE_HLPM_WHITE_250
						 : LHBM_TYPE_WHITE_250;
		ctx->lhbm_en = true;
		break;
	case LOCAL_HBM_NORMAL_GREEN_500NIT:
		pr_info("LOCAL_HBM_NORMAL_GREEN_500NIt\n");
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
		type = LHBM_TYPE_GREEN_500;
		ctx->lhbm_en = true;
		break;
	case LOCAL_HBM_HLPM_WHITE_1000NIT:
		type = LHBM_TYPE_HLPM_WHITE_1300;
		ctx->lhbm_en = true;
		break;
	case LOCAL_HBM_HLPM_WHITE_110NIT:
		pr_info("LOCAL_HBM_HLPM_WHITE_250NIT\n");
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
		type = LHBM_TYPE_HLPM_WHITE_250;
		ctx->lhbm_en = true;
		break;
	default:
		pr_info("invalid local hbm value\n");
		return 0;
	}

	if (type != LHBM_TYPE_OFF && atomic_read(&doze_enable))
		bl_level = mi_cfg->last_no_zero_bl_level;

	pr_info("bl_level:%d  flat_mode:%d\n", bl_level, ctx->gir_status);

	return mi_disp_panel_send_lhbm(dsi, type, bl_level);
}

static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.ext_param_set = mtk_panel_ext_param_set,
	.ext_param_get = mtk_panel_ext_param_get,
	.mode_switch = mode_switch,
	.set_bl_elvss_cmdq = lcm_set_bl_elvss_cmdq,
	.set_backlight_cmdq = lcm_setbacklight_cmdq,
	.esd_restore_backlight = lcm_esd_restore_backlight,
	.get_panel_initialized = get_panel_initialized,
	.get_panel_info = panel_get_panel_info,
	.get_panel_max_brightness_clone = panel_get_max_brightness_clone,
	.get_panel_factory_max_brightness = panel_get_factory_max_brightness,
	.doze_enable = panel_doze_enable,
	.doze_disable = panel_doze_disable,
	.doze_suspend = panel_doze_suspend,
	.set_doze_brightness = panel_set_doze_brightness,
	.get_doze_brightness = panel_get_doze_brightness,
	.panel_set_gir_on = panel_set_gir_on,
	.panel_set_gir_off = panel_set_gir_off,
	.panel_get_gir_status = panel_get_gir_status,
	.get_wp_info = panel_get_wp_info,
	.panel_fod_lhbm_init = panel_fod_lhbm_init,
	.set_lhbm_fod = panel_set_lhbm_fod,
	.panel_poweron = lcm_panel_poweron,
	.panel_poweroff = lcm_panel_poweroff,
};

static int lcm_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	static const struct drm_display_mode * const modes[] = {
		&mode_60hz, &mode_90hz, &mode_120hz, &mode_144hz,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, modes[i]);
		if (!mode) {
			dev_err(connector->dev->dev, "failed to add mode %ux%u@%u\n",
				modes[i]->hdisplay, modes[i]->vdisplay,
				drm_mode_vrefresh(modes[i]));
			return -ENOMEM;
		}
		drm_mode_set_name(mode);
		mode->type = DRM_MODE_TYPE_DRIVER;
		if (i == 0)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = PHYSICAL_WIDTH / 1000;
	connector->display_info.height_mm = PHYSICAL_HEIGHT / 1000;

	return 1;
}

static const struct drm_panel_funcs lcm_drm_funcs = {
	.disable = lcm_disable,
	.unprepare = lcm_unprepare,
	.prepare = lcm_prepare,
	.enable = lcm_enable,
	.get_modes = lcm_get_modes,
};

static int lcm_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;
	struct lcm *ctx;
	struct device_node *backlight;
	int ret;

	dsi_node = of_get_parent(dev->of_node);
	if (dsi_node) {
		endpoint = of_graph_get_next_endpoint(dsi_node, NULL);
		if (endpoint) {
			remote_node = of_graph_get_remote_port_parent(endpoint);
			if (!remote_node) {
				pr_info("No panel connected, skip probe lcm\n");
				return -ENODEV;
			}
			if (remote_node != dev->of_node) {
				pr_info("%s+ skip probe due to not current lcm\n", __func__);
				return -ENODEV;
			}
		}
	}

	ctx = devm_kzalloc(dev, sizeof(struct lcm), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	/* Command mode: no MIPI_DSI_MODE_VIDEO, unlike the video mode panels. */
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_NO_EOT_PACKET
			| MIPI_DSI_CLOCK_NON_CONTINUOUS;

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

	ctx->reset_gpio = devm_gpiod_get_index(dev, "reset", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "%s: cannot get reset-gpios %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	devm_gpiod_put(dev, ctx->reset_gpio);

	ctx->dvdd_gpio = devm_gpiod_get_index(dev, "dvdd", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->dvdd_gpio)) {
		dev_err(dev, "%s: cannot get dvdd-gpios %ld\n",
			__func__, PTR_ERR(ctx->dvdd_gpio));
		return PTR_ERR(ctx->dvdd_gpio);
	}
	devm_gpiod_put(dev, ctx->dvdd_gpio);

	ret = lcm_panel_vci_regulator_init(dev);
	if (!ret)
		lcm_panel_vci_enable(dev);
	else
		pr_err("%s: vci regulator init failed %d\n", __func__, ret);

	ret = lcm_panel_vddi_regulator_init(dev);
	if (!ret)
		lcm_panel_vddi_enable(dev);
	else
		pr_err("%s: vddi regulator init failed %d\n", __func__, ret);

	ctx->prepared = true;
	ctx->enabled = true;
	ctx->dynamic_fps = 60;
	ctx->gir_status = 1;
	mutex_init(&ctx->panel_lock);

	if (kstrtouint(build_id_cmdline, 0, &panel_build_id))
		panel_build_id = 0;

	drm_panel_init(&ctx->panel, dev, &lcm_drm_funcs, DRM_MODE_CONNECTOR_DSI);
	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
	ret = mtk_panel_ext_create(dev, &ext_params, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;
#endif

	panel_ctx = ctx;
	pr_info("%s- lcm, %s\n", __func__, dev_name(dev));

	return ret;
}

static void lcm_remove(struct mipi_dsi_device *dsi)
{
	struct lcm *ctx = mipi_dsi_get_drvdata(dsi);
#if defined(CONFIG_MTK_PANEL_EXT)
	struct mtk_panel_ctx *ext_ctx = find_panel_ctx(&ctx->panel);
#endif

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_detach(ext_ctx);
	mtk_panel_remove(ext_ctx);
#endif
}

static const struct of_device_id lcm_of_match[] = {
	{ .compatible = "n12a_42_02_0a_dsc_cmd,lcm", },
	{ }
};

MODULE_DEVICE_TABLE(of, lcm_of_match);

static struct mipi_dsi_driver lcm_driver = {
	.probe = lcm_probe,
	.remove = lcm_remove,
	.driver = {
		.name = "panel-n12a-42-02-0a-dsc-cmd",
		.owner = THIS_MODULE,
		.of_match_table = lcm_of_match,
	},
};

module_mipi_dsi_driver(lcm_driver);

MODULE_AUTHOR("Yuan Chen <chenyuan8@xiaomi.com>");
MODULE_DESCRIPTION("n12a_42_02_0a_dsc_cmd oled panel driver");
MODULE_LICENSE("GPL v2");
