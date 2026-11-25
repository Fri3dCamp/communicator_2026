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
#ifdef CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE
	/* RVSWD interface on the add-on's I2C pins, or NULL */
	const struct device *rvswd;
#endif
};

struct mfd_fri3d_communicator_data {
	const struct device *dev;
	/* connects to the add-on */
	struct k_work_delayable connect_work;
	/* delivers the queued key reports */
	struct k_work report_work;
	struct k_msgq reports;
	uint8_t reports_buf[REPORT_QUEUE_LEN * FRI3D_COMMUNICATOR_REPORT_SIZE];
	bool connected;
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

/*
 * The add-on's firmware answers on I2C shortly after it powers up: the driver
 * connects to it once this long after boot, and after an update.
 */
#define MFD_FRI3D_COMMUNICATOR_START_MS 1500

#endif /* ZEPHYR_DRIVERS_MFD_MFD_FRI3D_COMMUNICATOR_PRIV_H_ */
