// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2014 NVIDIA Corporation
 * Copyright (c) 2024 Svyatoslav Ryhel <clamor95@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

struct sharp_panel {
	struct drm_panel base;

	struct mipi_dsi_device *link1;
	struct mipi_dsi_device *link2;

	struct regulator *avdd;
	struct regulator *vddio;
	struct regulator *vsp;
	struct regulator *vsn;

	struct gpio_desc *reset_gpio;

	const struct drm_display_mode *mode;
};

static inline struct sharp_panel *to_sharp_panel(struct drm_panel *panel)
{
	return container_of(panel, struct sharp_panel, base);
}

#define dsi_generic_write_seq(dsi, cmd, seq...) do {			\
		static const u8 b[] = { cmd, seq };			\
		int ret;						\
		ret = mipi_dsi_dcs_write_buffer(dsi, b, ARRAY_SIZE(b));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static void sharp_panel_reset(struct sharp_panel *sharp)
{
	gpiod_set_value_cansleep(sharp->reset_gpio, 1);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(sharp->reset_gpio, 0);
	usleep_range(2000, 3000);
}

static int sharp_panel_prepare(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);
	struct device *dev = panel->dev;
	int err;

	err = regulator_enable(sharp->vddio);
	if (err < 0) {
		dev_err(dev, "failed to enable vddio power supply\n");
		return err;
	}

	err = regulator_enable(sharp->avdd);
	if (err < 0) {
		dev_err(dev, "failed to enable avdd power supply\n");
		return err;
	}
	msleep(12);

	err = regulator_enable(sharp->vsp);
	if (err < 0) {
		dev_err(dev, "failed to enable vsp power supply\n");
		return err;
	}
	msleep(12);

	err = regulator_enable(sharp->vsn);
	if (err < 0) {
		dev_err(dev, "failed to enable vsn power supply\n");
		return err;
	}
	msleep(24);

	sharp_panel_reset(sharp);
	msleep(32);

	return 0;
}

static int sharp_panel_enable(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);
	struct device *dev = panel->dev;
	int err;

	return 0;

	err = mipi_dsi_dcs_exit_sleep_mode(sharp->link1);
	if (err < 0) {
		dev_err(dev, "failed to exit sleep mode link1: %d\n", err);
		return err;
	}

	err = mipi_dsi_dcs_exit_sleep_mode(sharp->link2);
	if (err < 0) {
		dev_err(dev, "failed to exit sleep mode link2: %d\n", err);
		return err;
	}

	msleep(120);

	dsi_generic_write_seq(sharp->link1, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0xff);
	dsi_generic_write_seq(sharp->link2, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0xff);

	dsi_generic_write_seq(sharp->link1, MIPI_DCS_WRITE_POWER_SAVE, 0x01);
	dsi_generic_write_seq(sharp->link2, MIPI_DCS_WRITE_POWER_SAVE, 0x01);

	dsi_generic_write_seq(sharp->link1, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x2C);
	dsi_generic_write_seq(sharp->link2, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x2C);

	err = mipi_dsi_dcs_set_display_on(sharp->link1);
	if (err < 0) {
		dev_err(dev, "failed to set display on link1: %d\n", err);
		return err;
	}

	err = mipi_dsi_dcs_set_display_on(sharp->link2);
	if (err < 0) {
		dev_err(dev, "failed to set display on link2: %d\n", err);
		return err;
	}

	return 0;
}

static int sharp_panel_disable(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);
	struct device *dev = panel->dev;
	int err;

	err = mipi_dsi_dcs_set_display_off(sharp->link1);
	if (err < 0) {
		dev_err(dev, "failed to set display off link1: %d\n", err);
		return err;
	}

	err = mipi_dsi_dcs_set_display_off(sharp->link2);
	if (err < 0) {
		dev_err(dev, "failed to set display off link2: %d\n", err);
		return err;
	}

	msleep(100);

	err = mipi_dsi_dcs_enter_sleep_mode(sharp->link1);
	if (err < 0) {
		dev_err(dev, "failed to enter sleep mode link1: %d\n", err);
		return err;
	}

	err = mipi_dsi_dcs_enter_sleep_mode(sharp->link2);
	if (err < 0) {
		dev_err(dev, "failed to enter sleep mode link2: %d\n", err);
		return err;
	}

	msleep(150);
	return 0;
}

static int sharp_panel_unprepare(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);

	gpiod_set_value_cansleep(sharp->reset_gpio, 1);

	regulator_disable(sharp->avdd);
	regulator_disable(sharp->vddio);
	regulator_disable(sharp->vsp);
	regulator_disable(sharp->vsn);

	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock = (1536 + 136 + 28 + 28) * (2048 + 14 + 8 + 2) * 60 / 1000,
	.hdisplay = 1536,
	.hsync_start = 1536 + 136,
	.hsync_end = 1536 + 136 + 28,
	.htotal = 1536 + 136 + 28 + 28,
	.vdisplay = 2048,
	.vsync_start = 2048 + 14,
	.vsync_end = 2048 + 14 + 8,
	.vtotal = 2048 + 14 + 8 + 2,
	.width_mm = 120,
	.height_mm = 160,
};

static int sharp_panel_get_modes(struct drm_panel *panel,
				 struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs sharp_panel_funcs = {
	.unprepare = sharp_panel_unprepare,
	.enable = sharp_panel_enable,
	.disable = sharp_panel_disable,
	.prepare = sharp_panel_prepare,
	.get_modes = sharp_panel_get_modes,
};

static int sharp_panel_add(struct sharp_panel *sharp)
{
	struct device *dev = &sharp->link1->dev;
	int ret;

	sharp->mode = &default_mode;

	sharp->avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(sharp->avdd))
		return PTR_ERR(sharp->avdd);

	sharp->vddio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(sharp->vddio))
		return PTR_ERR(sharp->vddio);

	sharp->vsp = devm_regulator_get(dev, "vsp");
	if (IS_ERR(sharp->vsp))
		return PTR_ERR(sharp->vsp);

	sharp->vsn = devm_regulator_get(dev, "vsn");
	if (IS_ERR(sharp->vsn))
		return PTR_ERR(sharp->vsn);

	sharp->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						    GPIOD_OUT_LOW);
	if (IS_ERR(sharp->reset_gpio))
		return PTR_ERR(sharp->reset_gpio);

	drm_panel_init(&sharp->base, dev, &sharp_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&sharp->base);
	if (ret)
		return ret;

	drm_panel_add(&sharp->base);

	return 0;
}

static void sharp_panel_del(struct sharp_panel *sharp)
{
	if (sharp->base.dev)
		drm_panel_remove(&sharp->base);

	if (sharp->link2)
		put_device(&sharp->link2->dev);
}

static int sharp_panel_probe(struct mipi_dsi_device *dsi)
{
	struct mipi_dsi_device *secondary = NULL;
	struct sharp_panel *sharp;
	struct device_node *np;
	int err;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags =  MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM;

	/* Find DSI-LINK1 */
	np = of_parse_phandle(dsi->dev.of_node, "link2", 0);
	if (np) {
		secondary = of_find_mipi_dsi_device_by_node(np);
		of_node_put(np);

		if (!secondary)
			return -EPROBE_DEFER;
	}

	/* register a panel for only the DSI-LINK1 interface */
	if (secondary) {
		sharp = devm_kzalloc(&dsi->dev, sizeof(*sharp), GFP_KERNEL);
		if (!sharp) {
			put_device(&secondary->dev);
			return -ENOMEM;
		}

		mipi_dsi_set_drvdata(dsi, sharp);

		sharp->link2 = secondary;
		sharp->link1 = dsi;

		err = sharp_panel_add(sharp);
		if (err < 0) {
			put_device(&secondary->dev);
			return err;
		}
	}

	err = mipi_dsi_attach(dsi);
	if (err < 0) {
		if (secondary)
			sharp_panel_del(sharp);

		return err;
	}

	return 0;
}

static void sharp_panel_remove(struct mipi_dsi_device *dsi)
{
	struct sharp_panel *sharp = mipi_dsi_get_drvdata(dsi);
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	/* only detach from host for the DSI-LINK2 interface */
	if (sharp)
		sharp_panel_del(sharp);
}

static const struct of_device_id sharp_of_match[] = {
	{ .compatible = "sharp,lq079l1sx01", },
	{ }
};
MODULE_DEVICE_TABLE(of, sharp_of_match);

static struct mipi_dsi_driver sharp_panel_driver = {
	.probe = sharp_panel_probe,
	.remove = sharp_panel_remove,
	.driver = {
		.name = "panel-sharp-lq079l1sx01",
		.of_match_table = sharp_of_match,
	},
};
module_mipi_dsi_driver(sharp_panel_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("Sharp LQ079L1SX01 panel driver");
MODULE_LICENSE("GPL v2");
