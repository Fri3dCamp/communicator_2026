/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware update of the Fri3d Camp 2026 communicator's keyboard controller
 * over RVSWD (CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE): it gives its I2C pins to
 * SWD on request (remap bit), the zephyr-rvswd module programs it, and the
 * pins go back to I2C. Also the bundled image and its shell command.
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
#define REMAP_DELAY_MS 200
/* Debug module status register: answers when a target is there */
#define DMSTATUS       0x11

/*
 * Find the add-on on SWD and halt it. A target that just powered up needs the
 * interface reset of rvswd_connect() before it answers, so redo it for every
 * probe.
 */
static int mfd_fri3d_communicator_find(const struct device *rvswd, k_timeout_t wait,
				       uint32_t vendor[4])
{
	k_timepoint_t end = sys_timepoint_calc(wait);

	do {
		uint32_t status;

		if (rvswd_connect(rvswd) == 0 && rvswd_read(rvswd, DMSTATUS, &status) == 0 &&
		    ch32_identify(rvswd, vendor) == 0) {
			return 0;
		}
		k_msleep(2);
	} while (!sys_timepoint_expired(end));

	return -ETIMEDOUT;
}

int mfd_fri3d_communicator_update(const struct device *dev, const void *image, size_t len,
				  mfd_fri3d_communicator_progress_cb_t cb, void *user_data,
				  k_timeout_t wait)
{
	const struct mfd_fri3d_communicator_config *config = dev->config;
	struct mfd_fri3d_communicator_data *data = dev->data;
	struct k_work_sync sync;
	uint32_t vendor[4];
	const char *chip;
	int ret;

	if (config->rvswd == NULL) {
		return -ENOTSUP;
	}
	if (!device_is_ready(config->rvswd)) {
		return -ENODEV;
	}

	k_work_cancel_delayable_sync(&data->connect_work, &sync);
	if (data->connected) {
		/* the running firmware gives its pins to SWD */
		mfd_fri3d_communicator_config_update(dev, FRI3D_COMMUNICATOR_CONFIG_REMAP,
						     FRI3D_COMMUNICATOR_CONFIG_REMAP);
		data->connected = false;
		k_msleep(REMAP_DELAY_MS);
	}
	data->updating = true;

	/* the application keeps what must not see the SWD traffic away from it */
	if (data->pre_update != NULL) {
		data->pre_update(dev, data->update_user_data);
	}

	ret = mfd_fri3d_communicator_find(config->rvswd, wait, vendor);
	if (ret != 0) {
		LOG_ERR("%s: no communicator on SWD", dev->name);
	} else {
		chip = ch32_chip_name(vendor);
		if (chip == NULL || strncmp(chip, "CH32X03", 7) != 0) {
			LOG_ERR("%s: not a CH32X03x (vendor bytes %08x %08x)", dev->name,
				vendor[0], vendor[1]);
			ret = -ENODEV;
		} else {
			LOG_INF("%s: programming %s (%zu bytes)", dev->name, chip, len);
			/* ends with a reset into the new firmware */
			ret = ch32x03x_program(config->rvswd, image, len, cb, user_data);
			if (ret != 0) {
				LOG_ERR("%s: programming failed: %d", dev->name, ret);
			}
		}
		if (ret != 0) {
			ch32_reset_and_run(config->rvswd);
		}
	}

	/* the pins go back to I2C */
	rvswd_disconnect(config->rvswd);
	if (data->post_update != NULL) {
		data->post_update(dev, ret, data->update_user_data);
	}
	data->updating = false;
	/* connect again once the firmware is up */
	k_work_reschedule(&data->connect_work, K_MSEC(MFD_FRI3D_COMMUNICATOR_START_MS));

	return ret;
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

#if defined(CONFIG_SHELL) && defined(FRI3D_COMMUNICATOR_HAS_FIRMWARE)
#include <zephyr/shell/shell.h>

static const struct device *const shell_communicator = DEVICE_DT_INST_GET(0);

static const uint8_t mfd_fri3d_communicator_firmware[] = {
#include "fri3d_communicator_firmware.inc"
};

static void mfd_fri3d_communicator_shell_progress(size_t done, size_t total, void *user_data)
{
	const struct shell *sh = user_data;

	if (done == total || done % 4096 == 0) {
		shell_print(sh, "%zu/%zu", done, total);
	}
}

static int cmd_communicator_update(const struct shell *sh, size_t argc, char **argv)
{
	int wait_s = argc > 1 ? atoi(argv[1]) : 0;
	int ret;

	shell_print(sh, "installing v%s (%zu bytes)%s",
		    CONFIG_MFD_FRI3D_COMMUNICATOR_FIRMWARE_VERSION,
		    sizeof(mfd_fri3d_communicator_firmware),
		    wait_s > 0 ? ", connect the communicator now" : "");
	ret = mfd_fri3d_communicator_update(shell_communicator, mfd_fri3d_communicator_firmware,
					    sizeof(mfd_fri3d_communicator_firmware),
					    mfd_fri3d_communicator_shell_progress, (void *)sh,
					    K_SECONDS(MAX(wait_s, 1)));
	if (ret != 0) {
		shell_error(sh, "update failed: %d", ret);
	} else {
		shell_print(sh, "done");
	}
	return ret;
}

SHELL_SUBCMD_ADD((communicator), update, NULL,
		 "Install the bundled firmware: update [seconds to wait for the add-on]",
		 cmd_communicator_update, 1, 1);
#endif
