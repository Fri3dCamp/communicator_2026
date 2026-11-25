/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shell commands of the Fri3d Camp 2026 communicator add-on
 * (CONFIG_MFD_FRI3D_COMMUNICATOR_SHELL): "communicator status|backlight", and
 * with a bundled firmware image "communicator update".
 */

#define DT_DRV_COMPAT fri3d_communicator_2026

#include <stdlib.h>

#include <zephyr/drivers/mfd/fri3d_communicator.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "mfd_fri3d_communicator_priv.h"

static const struct device *const shell_communicator = DEVICE_DT_GET(DT_DRV_INST(0));

static int shell_communicator_ready(const struct shell *sh)
{
	if (!device_is_ready(shell_communicator)) {
		shell_error(sh, "%s not ready (no add-on at boot)", shell_communicator->name);
		return -ENODEV;
	}

	return 0;
}

static int cmd_communicator_status(const struct shell *sh, size_t argc, char **argv)
{
	struct mfd_fri3d_communicator_stats st;
	uint8_t version[3];
	uint8_t cfg;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = shell_communicator_ready(sh);
	if (ret != 0) {
		return ret;
	}

	mfd_fri3d_communicator_get_version(shell_communicator, version);
	mfd_fri3d_communicator_get_stats(shell_communicator, &st);
	shell_print(sh, "firmware v%u.%u.%u", version[0], version[1], version[2]);

#ifdef CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE
	struct mfd_fri3d_communicator_update_info info;

	mfd_fri3d_communicator_get_update_info(&info);
	if (info.bundled_len > 0) {
		shell_print(sh, "bundled v%u.%u.%u (%zu bytes)%s", info.bundled_version[0],
			    info.bundled_version[1], info.bundled_version[2], info.bundled_len,
			    info.updated_at_boot ? ", installed at boot" : "");
	}
	shell_print(sh, "updates since boot: %u", info.updates);
#endif

	if (mfd_fri3d_communicator_reg_read(shell_communicator, FRI3D_COMMUNICATOR_REG_CONFIG, &cfg,
					    1) == 0) {
		shell_print(sh, "configuration 0x%02x", cfg);
	}
	shell_print(sh, "UART %u bytes, %u key reports, %u dropped", st.uart_bytes, st.reports,
		    st.dropped);

	return 0;
}

static int cmd_communicator_backlight(const struct shell *sh, size_t argc, char **argv)
{
	int percent = atoi(argv[1]);
	int ret;

	ARG_UNUSED(argc);

	ret = shell_communicator_ready(sh);
	if (ret != 0) {
		return ret;
	}

	if (percent < 0 || percent > 100) {
		shell_error(sh, "0-100");
		return -EINVAL;
	}
	ret = mfd_fri3d_communicator_set_backlight(shell_communicator, (uint8_t)percent);
	if (ret != 0) {
		shell_error(sh, "failed: %d", ret);
	}

	return ret;
}

#ifdef FRI3D_COMMUNICATOR_HAS_FIRMWARE
static void shell_communicator_progress(size_t done, size_t total, void *user_data)
{
	const struct shell *sh = user_data;

	/* every 25 % */
	if (done == total || (done * 4 / total) != ((done + 256) * 4 / total)) {
		shell_print(sh, "  %u %%", (unsigned int)(done * 100 / total));
	}
}

static int cmd_communicator_update(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = shell_communicator_ready(sh);
	if (ret != 0) {
		return ret;
	}

	shell_print(sh, "installing the bundled communicator firmware");
	ret = mfd_fri3d_communicator_update_bundled(shell_communicator,
						    shell_communicator_progress, (void *)sh);
	if (ret != 0) {
		shell_error(sh, "update failed: %d", ret);
		return ret;
	}

	return cmd_communicator_status(sh, 0, NULL);
}
#endif /* FRI3D_COMMUNICATOR_HAS_FIRMWARE */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_communicator,
	SHELL_CMD_ARG(status, NULL, "Firmware versions, configuration and counters",
		      cmd_communicator_status, 1, 0),
	SHELL_CMD_ARG(backlight, NULL, "Keyboard backlight: backlight <0-100>",
		      cmd_communicator_backlight, 2, 0),
#ifdef FRI3D_COMMUNICATOR_HAS_FIRMWARE
	SHELL_CMD_ARG(update, NULL, "Install the bundled add-on firmware", cmd_communicator_update,
		      1, 0),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(communicator, &sub_communicator, "Communicator add-on", NULL);
