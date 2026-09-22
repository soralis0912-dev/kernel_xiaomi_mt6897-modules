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

static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.ext_param_set = mtk_panel_ext_param_set,
	.ext_param_get = mtk_panel_ext_param_get,
	.mode_switch = mode_switch,
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
