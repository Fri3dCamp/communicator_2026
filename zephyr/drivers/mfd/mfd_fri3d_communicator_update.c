/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware update of the Fri3d Camp 2026 communicator's keyboard controller
 * over RVSWD (CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE): it gives its I2C pins to
 * SWD on request (remap bit), the zephyr-rvswd module programs it, and the
 * pins go back to I2C. Also the bundled image, its installation at boot and the
 * shell.
 */

#define DT_DRV_COMPAT fri3d_communicator_2026

#include <stdlib.h>
#include <string.h>

#include <rvswd/ch32.h>
#include <rvswd/rvswd.h>
#include <zephyr/logging/log.h>

#include "mfd_fri3d_communicator_priv.h"

LOG_MODULE_DECLARE(mfd_fri3d_communicator, CONFIG_MFD_LOG_LEVEL);

/* Delay for the add-on to give its I2C pins to SWD after the remap command */
#define REMAP_DELAY_MS     200
/* After the reset into the new firmware: I2C starts after one second */
#define RESTART_TIMEOUT_MS 4000

/* successful updates since boot (for the shell) */
static unsigned int mfd_fri3d_communicator_updates;

static int mfd_fri3d_communicator_program(const struct device *dev, const void *image, size_t len,
					  mfd_fri3d_communicator_progress_cb_t cb,
					  void *user_data)
{
	const struct mfd_fri3d_communicator_config *config = dev->config;
	uint32_t vendor[4];
	const char *chip;
	int ret;

	ret = rvswd_connect(config->rvswd);
	if (ret == 0) {
		/* halts it before it runs any code */
		ret = ch32_identify(config->rvswd, vendor);
	}
	if (ret != 0) {
		LOG_ERR("%s: no answer on SWD: %d", dev->name, ret);
		return ret;
	}

	chip = ch32_chip_name(vendor);
	if (chip == NULL || strncmp(chip, "CH32X03", 7) != 0) {
		LOG_ERR("%s: not a CH32X03x (vendor bytes %08x %08x)", dev->name, vendor[0],
			vendor[1]);
		ch32_reset_and_run(config->rvswd);
		return -ENODEV;
	}

	LOG_INF("%s: programming %s (%zu bytes)", dev->name, chip, len);
	/* ends with a reset into the new firmware */
	ret = ch32x03x_program(config->rvswd, image, len, cb, user_data);
	if (ret != 0) {
		LOG_ERR("%s: programming failed: %d", dev->name, ret);
		ch32_reset_and_run(config->rvswd);
	}

	return ret;
}

int mfd_fri3d_communicator_update(const struct device *dev, const void *image, size_t len,
				  mfd_fri3d_communicator_progress_cb_t cb, void *user_data)
{
	const struct mfd_fri3d_communicator_config *config = dev->config;
	struct mfd_fri3d_communicator_data *data = dev->data;
	int ret, restart;

	if (config->rvswd == NULL) {
		return -ENOTSUP;
	}
	if (!device_is_ready(config->rvswd)) {
		return -ENODEV;
	}

	/*
	 * The firmware gives its I2C pins to SWD. Without firmware it does not
	 * answer, and the pins are SWD anyway.
	 */
	mfd_fri3d_communicator_config_update(dev, FRI3D_COMMUNICATOR_CONFIG_REMAP,
					     FRI3D_COMMUNICATOR_CONFIG_REMAP);
	data->updating = true;
	k_msleep(REMAP_DELAY_MS);

	/* the application keeps what must not see the SWD traffic away from it */
	if (data->pre_update != NULL) {
		data->pre_update(dev, data->update_user_data);
	}

	ret = mfd_fri3d_communicator_program(dev, image, len, cb, user_data);
	if (ret == 0) {
		mfd_fri3d_communicator_updates++;
	}

	/* the pins go back to I2C */
	rvswd_disconnect(config->rvswd);
	if (data->post_update != NULL) {
		data->post_update(dev, ret, data->update_user_data);
	}

	restart = mfd_fri3d_communicator_wait(dev, sys_timepoint_calc(K_MSEC(RESTART_TIMEOUT_MS)));
	if (restart == 0) {
		LOG_INF("%s: firmware v%u.%u.%u", dev->name, data->version[0], data->version[1],
			data->version[2]);
	} else {
		LOG_ERR("%s: no answer after the update", dev->name);
	}
	data->updating = false;

	return ret != 0 ? ret : restart;
}

void mfd_fri3d_communicator_set_update_callbacks(const struct device *dev,
						 mfd_fri3d_communicator_pre_update_cb_t pre,
						 mfd_fri3d_communicator_post_update_cb_t post,
						 void *user_data)
{
	struct mfd_fri3d_communicator_data *data = dev->data;

	data->pre_update = pre;
	data->post_update = post;
	data->update_user_data = user_data;
}

#ifdef FRI3D_COMMUNICATOR_HAS_FIRMWARE
/* the bundled firmware was installed at boot (for the shell) */
static bool mfd_fri3d_communicator_updated_at_boot;

static const uint8_t mfd_fri3d_communicator_firmware[] = {
#include "fri3d_communicator_firmware.inc"
};

/* Version of the bundled firmware from CONFIG_MFD_FRI3D_COMMUNICATOR_FIRMWARE_VERSION */
static void mfd_fri3d_communicator_bundled_version(uint8_t version[3])
{
	const char *p = CONFIG_MFD_FRI3D_COMMUNICATOR_FIRMWARE_VERSION;

	for (int i = 0; i < 3; i++) {
		char *end;

		version[i] = strtoul(p, &end, 10);
		p = *end == '.' ? end + 1 : end;
	}
}

static int mfd_fri3d_communicator_update_bundled(const struct device *dev,
						 mfd_fri3d_communicator_progress_cb_t cb,
						 void *user_data)
{
	return mfd_fri3d_communicator_update(dev, mfd_fri3d_communicator_firmware,
					     sizeof(mfd_fri3d_communicator_firmware), cb,
					     user_data);
}
#endif /* FRI3D_COMMUNICATOR_HAS_FIRMWARE */

#ifdef CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE_AT_BOOT
/*
 * At init: install the bundled firmware when the add-on runs an older version,
 * or does not answer at all (no firmware). Debug builds report 255.255.255
 * and are left alone.
 */
int mfd_fri3d_communicator_update_at_boot(const struct device *dev, int answered)
{
#ifdef FRI3D_COMMUNICATOR_HAS_FIRMWARE
	struct mfd_fri3d_communicator_data *data = dev->data;
	uint8_t bundled[3];

	mfd_fri3d_communicator_bundled_version(bundled);

	if (answered == 0) {
		if (memcmp(data->version, bundled, sizeof(bundled)) >= 0) {
			return 0;
		}
		LOG_INF("%s: firmware v%u.%u.%u is older than v%u.%u.%u, updating", dev->name,
			data->version[0], data->version[1], data->version[2], bundled[0],
			bundled[1], bundled[2]);
	} else {
		LOG_WRN("%s: no answer, installing firmware v%u.%u.%u", dev->name, bundled[0],
			bundled[1], bundled[2]);
	}

	if (mfd_fri3d_communicator_update_bundled(dev, NULL, NULL) != 0) {
		return -EIO;
	}
	mfd_fri3d_communicator_updated_at_boot = true;

	return 0;
#else
	ARG_UNUSED(dev);

	return answered;
#endif
}
#endif /* CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE_AT_BOOT */

#if defined(CONFIG_SHELL) && defined(FRI3D_COMMUNICATOR_HAS_FIRMWARE)
#include <zephyr/shell/shell.h>

static const struct device *const shell_communicator = DEVICE_DT_INST_GET(0);

static void mfd_fri3d_communicator_shell_progress(size_t done, size_t total, void *user_data)
{
	const struct shell *sh = user_data;

	/* every 25 % */
	if (done == total || (done * 4 / total) != ((done + 256) * 4 / total)) {
		shell_print(sh, "  %u %%", (unsigned int)(done * 100 / total));
	}
}

static int cmd_communicator_firmware(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t version[3], bundled[3];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	mfd_fri3d_communicator_get_version(shell_communicator, version);
	mfd_fri3d_communicator_bundled_version(bundled);
	shell_print(sh, "communicator firmware v%u.%u.%u, bundled v%u.%u.%u (%u bytes)%s, "
		    "updates since boot: %u",
		    version[0], version[1], version[2], bundled[0], bundled[1], bundled[2],
		    (unsigned int)sizeof(mfd_fri3d_communicator_firmware),
		    mfd_fri3d_communicator_updated_at_boot ? ", installed at boot" : "",
		    mfd_fri3d_communicator_updates);

	return 0;
}

static int cmd_communicator_update(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "installing the bundled communicator firmware");
	ret = mfd_fri3d_communicator_update_bundled(shell_communicator,
						    mfd_fri3d_communicator_shell_progress,
						    (void *)sh);
	if (ret != 0) {
		shell_error(sh, "update failed: %d", ret);
		return ret;
	}

	cmd_communicator_firmware(sh, 0, NULL);

	return 0;
}

SHELL_SUBCMD_ADD((communicator), firmware, NULL, "Add-on and bundled firmware versions",
		 cmd_communicator_firmware, 1, 0);
SHELL_SUBCMD_ADD((communicator), update, NULL, "Install the bundled add-on firmware",
		 cmd_communicator_update, 1, 0);
#endif
