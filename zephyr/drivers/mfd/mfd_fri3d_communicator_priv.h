/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared by the communicator's MFD driver (mfd_fri3d_communicator.c) and its
 * firmware update over RVSWD (mfd_fri3d_communicator_update.c).
 */

#ifndef ZEPHYR_DRIVERS_MFD_MFD_FRI3D_COMMUNICATOR_PRIV_H_
#define ZEPHYR_DRIVERS_MFD_MFD_FRI3D_COMMUNICATOR_PRIV_H_

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mfd/fri3d_communicator.h>
#include <zephyr/kernel.h>

/* key reports waiting for the work queue */
#define REPORT_QUEUE_LEN 4

struct mfd_fri3d_communicator_config {
	struct i2c_dt_spec i2c;
	/* UART that receives the key reports, or NULL */
	const struct device *uart;
	uint32_t report_gap_ms;
	uint32_t boot_timeout_ms;
#ifdef CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE
	/* RVSWD interface on the add-on's I2C pins, or NULL */
	const struct device *rvswd;
#endif
};

struct mfd_fri3d_communicator_data {
	const struct device *dev;
	/* delivers the queued key reports */
	struct k_work report_work;
	struct k_msgq reports;
	uint8_t reports_buf[REPORT_QUEUE_LEN * FRI3D_COMMUNICATOR_REPORT_SIZE];
	/* a firmware update is running: the I2C pins are SWD */
	bool updating;
	/* UART receive state (interrupt) */
	uint8_t rx[FRI3D_COMMUNICATOR_REPORT_SIZE];
	size_t rx_len;
	uint32_t rx_last;
	uint8_t version[3];
	struct mfd_fri3d_communicator_stats stats;
	mfd_fri3d_communicator_report_cb_t cb;
	void *user_data;
#ifdef CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE
	mfd_fri3d_communicator_pre_update_cb_t pre_update;
	mfd_fri3d_communicator_post_update_cb_t post_update;
	void *update_user_data;
#endif
};


/* Wait until the add-on answers on I2C, and read its version */
int mfd_fri3d_communicator_wait(const struct device *dev, k_timepoint_t timeout);

/* What the update code holds, for the shell (CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE) */
struct mfd_fri3d_communicator_update_info {
	/* bundled image (CONFIG_MFD_FRI3D_COMMUNICATOR_FIRMWARE): size, 0 without one */
	size_t bundled_len;
	/* successful updates since boot */
	unsigned int updates;
};

void mfd_fri3d_communicator_get_update_info(struct mfd_fri3d_communicator_update_info *info);

#endif /* ZEPHYR_DRIVERS_MFD_MFD_FRI3D_COMMUNICATOR_PRIV_H_ */
