/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CH32X035 keyboard controller of the Fri3d Camp 2026 communicator add-on
 * (communicator_2026 firmware): I2C registers. The add-on sends its 8-byte HID
 * key report on its UART on every change; the driver takes the UART data only
 * as the trigger to read the key matrix over I2C (firmware v1.1.0 and later),
 * like the DJ add-on, and hands a changed matrix to its input child. With
 * CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE its firmware can be updated over
 * RVSWD (mfd_fri3d_communicator_update.c); the shell commands are in
 * mfd_fri3d_communicator_shell.c.
 */

#define DT_DRV_COMPAT fri3d_communicator_2026

#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mfd/fri3d_communicator.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "mfd_fri3d_communicator_priv.h"

LOG_MODULE_REGISTER(mfd_fri3d_communicator, CONFIG_MFD_LOG_LEVEL);


int mfd_fri3d_communicator_reg_read(const struct device *dev, uint8_t reg, void *data, size_t len)
{
	const struct mfd_fri3d_communicator_config *config = dev->config;
	struct mfd_fri3d_communicator_data *dev_data = dev->data;

	if (dev_data->updating) {
		return -EBUSY;
	}
	return i2c_burst_read_dt(&config->i2c, reg, data, len);
}

int mfd_fri3d_communicator_reg_write(const struct device *dev, uint8_t reg, const void *data,
				     size_t len)
{
	const struct mfd_fri3d_communicator_config *config = dev->config;
	struct mfd_fri3d_communicator_data *dev_data = dev->data;
	uint8_t buf[1 + 2];

	if (dev_data->updating) {
		return -EBUSY;
	}
	if (len > sizeof(buf) - 1) {
		return -EINVAL;
	}
	buf[0] = reg;
	memcpy(&buf[1], data, len);

	return i2c_write_dt(&config->i2c, buf, 1 + len);
}

int mfd_fri3d_communicator_config_update(const struct device *dev, uint8_t mask, uint8_t value)
{
	uint8_t cfg;
	int ret;

	ret = mfd_fri3d_communicator_reg_read(dev, FRI3D_COMMUNICATOR_REG_CONFIG, &cfg, 1);
	if (ret != 0) {
		return ret;
	}
	cfg = (cfg & ~mask) | (value & mask);

	return mfd_fri3d_communicator_reg_write(dev, FRI3D_COMMUNICATOR_REG_CONFIG, &cfg, 1);
}

int mfd_fri3d_communicator_set_backlight(const struct device *dev, uint8_t percent)
{
	uint8_t buf[2];

	if (percent > 100) {
		return -EINVAL;
	}
	sys_put_le16(percent, buf);

	return mfd_fri3d_communicator_reg_write(dev, FRI3D_COMMUNICATOR_REG_BACKLIGHT, buf,
						sizeof(buf));
}

void mfd_fri3d_communicator_get_version(const struct device *dev, uint8_t version[3])
{
	struct mfd_fri3d_communicator_data *data = dev->data;

	memcpy(version, data->version, sizeof(data->version));
}

void mfd_fri3d_communicator_set_matrix_callback(const struct device *dev,
						mfd_fri3d_communicator_matrix_cb_t cb,
						void *user_data)
{
	struct mfd_fri3d_communicator_data *data = dev->data;

	data->user_data = user_data;
	data->cb = cb;
}

void mfd_fri3d_communicator_get_stats(const struct device *dev,
				      struct mfd_fri3d_communicator_stats *stats)
{
	struct mfd_fri3d_communicator_data *data = dev->data;

	*stats = data->stats;
}

int mfd_fri3d_communicator_wait(const struct device *dev, k_timepoint_t timeout)
{
	const struct mfd_fri3d_communicator_config *config = dev->config;
	struct mfd_fri3d_communicator_data *data = dev->data;

	/* straight on the bus: also during an update */
	while (i2c_burst_read_dt(&config->i2c, FRI3D_COMMUNICATOR_REG_VERSION, data->version,
				 sizeof(data->version)) != 0) {
		if (sys_timepoint_expired(timeout)) {
			return -ETIMEDOUT;
		}
		k_msleep(50);
	}

	return 0;
}

/* Key matrix register: firmware v1.1.0 and later (debug builds report 255.255.255) */
static bool mfd_fri3d_communicator_has_matrix(const struct mfd_fri3d_communicator_data *data)
{
	static const uint8_t first[3] = {1, 1, 0};

	return memcmp(data->version, first, sizeof(first)) >= 0;
}

int mfd_fri3d_communicator_get_matrix(const struct device *dev,
				      uint8_t matrix[FRI3D_COMMUNICATOR_MATRIX_COLS])
{
	struct mfd_fri3d_communicator_data *data = dev->data;

	if (!mfd_fri3d_communicator_has_matrix(data)) {
		return -ENOTSUP;
	}
	memcpy(matrix, data->matrix, sizeof(data->matrix));

	return 0;
}

/* Read the key matrix and hand a changed one on */
static void mfd_fri3d_communicator_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct mfd_fri3d_communicator_data *data =
		CONTAINER_OF(dwork, struct mfd_fri3d_communicator_data, work);
	const struct device *dev = data->dev;
	const struct mfd_fri3d_communicator_config *config = dev->config;
	uint8_t matrix[FRI3D_COMMUNICATOR_MATRIX_COLS];
	mfd_fri3d_communicator_matrix_cb_t cb;
	int ret;

	if (data->updating || !mfd_fri3d_communicator_has_matrix(data)) {
		/* the update reads again when it is done */
		return;
	}

	/* UART data from here on means a change after this read */
	atomic_clear(&data->uart_data);
	data->last_read = k_uptime_get_32();

	ret = mfd_fri3d_communicator_reg_read(dev, FRI3D_COMMUNICATOR_REG_MATRIX, matrix,
					      sizeof(matrix));
	data->stats.reads++;
	if (ret != 0) {
		data->stats.read_errors++;
		/* the next UART data reads again */
		LOG_WRN("%s: reading the key matrix failed: %d", dev->name, ret);
		return;
	}

	cb = data->cb;
	if (!data->matrix_valid || memcmp(matrix, data->matrix, sizeof(matrix)) != 0) {
		data->matrix_valid = true;
		memcpy(data->matrix, matrix, sizeof(matrix));
		data->stats.changes++;
		if (cb != NULL) {
			cb(dev, data->matrix, data->user_data);
		}
	}

	/* a change while reading: read again */
	if (atomic_get(&data->uart_data)) {
		k_work_reschedule(dwork, K_MSEC(config->poll_period_ms));
	}
}

/*
 * The add-on sent its key report on the UART: a key changed. The bytes only
 * trigger a read of the report over I2C, at most once per poll period.
 */
static void mfd_fri3d_communicator_uart_isr(const struct device *uart, void *user_data)
{
	const struct device *dev = user_data;
	const struct mfd_fri3d_communicator_config *config = dev->config;
	struct mfd_fri3d_communicator_data *data = dev->data;
	uint8_t buf[16];
	bool received = false;
	uint32_t elapsed;

	uart_irq_update(uart);
	while (uart_irq_rx_ready(uart) > 0) {
		int len = uart_fifo_read(uart, buf, sizeof(buf));

		if (len <= 0) {
			break;
		}
		data->stats.uart_bytes += len;
		received = true;
	}

	if (!received || data->updating) {
		return;
	}

	atomic_set(&data->uart_data, 1);
	elapsed = k_uptime_get_32() - data->last_read;
	k_work_reschedule(&data->work, elapsed >= config->poll_period_ms
					       ? K_NO_WAIT
					       : K_MSEC(config->poll_period_ms - elapsed));
}

static int mfd_fri3d_communicator_init(const struct device *dev)
{
	const struct mfd_fri3d_communicator_config *config = dev->config;
	struct mfd_fri3d_communicator_data *data = dev->data;
	int ret;

	data->dev = dev;
	k_work_init_delayable(&data->work, mfd_fri3d_communicator_work);

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("%s: I2C bus not ready", dev->name);
		return -ENODEV;
	}

	/* the devicetree says the add-on is there: wait for its firmware to answer */
	ret = mfd_fri3d_communicator_wait(dev,
					  sys_timepoint_calc(K_MSEC(config->boot_timeout_ms)));
	if (ret != 0) {
		LOG_ERR("%s: no answer from the communicator", dev->name);
		return -ENODEV;
	}

	LOG_INF("%s: firmware v%u.%u.%u", dev->name, data->version[0], data->version[1],
		data->version[2]);

	if (config->uart != NULL) {
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
		if (!device_is_ready(config->uart)) {
			LOG_ERR("%s: UART not ready", dev->name);
			return -ENODEV;
		}
		uart_irq_callback_user_data_set(config->uart, mfd_fri3d_communicator_uart_isr,
						(void *)dev);
		uart_irq_rx_enable(config->uart);
#endif
	}

	if (!mfd_fri3d_communicator_has_matrix(data)) {
		LOG_WRN("%s: firmware before v1.1.0 has no key matrix register: no keys", dev->name);
	}
	/* the current matrix, the reference for the changes */
	k_work_schedule(&data->work, K_NO_WAIT);

	return 0;
}

#define MFD_FRI3D_COMMUNICATOR_DEFINE(inst)                                                        \
	static const struct mfd_fri3d_communicator_config mfd_fri3d_communicator_config##inst = {  \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.uart = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, uart),                             \
				    (DEVICE_DT_GET(DT_INST_PHANDLE(inst, uart))), (NULL)),         \
		.poll_period_ms = DT_INST_PROP(inst, poll_period_ms),                              \
		.boot_timeout_ms = DT_INST_PROP(inst, boot_timeout_ms),                            \
		IF_ENABLED(CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE,                                   \
			   (.rvswd = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, rvswd),               \
						 (DEVICE_DT_GET(DT_INST_PHANDLE(inst, rvswd))),     \
						 (NULL)),))                                        \
	};                                                                                         \
                                                                                                   \
	static struct mfd_fri3d_communicator_data mfd_fri3d_communicator_data##inst;               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, mfd_fri3d_communicator_init, NULL,                             \
			      &mfd_fri3d_communicator_data##inst,                                  \
			      &mfd_fri3d_communicator_config##inst, POST_KERNEL,                   \
			      CONFIG_MFD_FRI3D_COMMUNICATOR_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MFD_FRI3D_COMMUNICATOR_DEFINE)
