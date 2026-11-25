/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CH32X035 keyboard controller of the Fri3d Camp 2026 communicator add-on
 * (communicator_2026 firmware): I2C registers, and the 8-byte HID key reports
 * it sends on its UART on every change, as one burst without framing (a pause
 * starts a new report; byte 1 is reserved and 0). With
 * CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE its firmware can be updated over
 * RVSWD (mfd_fri3d_communicator_update.c).
 */

#define DT_DRV_COMPAT fri3d_communicator_2026

#include <stdlib.h>
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

bool mfd_fri3d_communicator_is_connected(const struct device *dev)
{
	struct mfd_fri3d_communicator_data *data = dev->data;

	return data->connected;
}

void mfd_fri3d_communicator_get_version(const struct device *dev, uint8_t version[3])
{
	struct mfd_fri3d_communicator_data *data = dev->data;

	memcpy(version, data->version, sizeof(data->version));
}

void mfd_fri3d_communicator_set_report_callback(const struct device *dev,
						mfd_fri3d_communicator_report_cb_t cb,
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

static int mfd_fri3d_communicator_connect(const struct device *dev)
{
	struct mfd_fri3d_communicator_data *data = dev->data;
	int ret;

	ret = mfd_fri3d_communicator_reg_read(dev, FRI3D_COMMUNICATOR_REG_VERSION, data->version,
					      sizeof(data->version));
	if (ret != 0) {
		return ret;
	}
	data->connected = true;
	LOG_INF("%s: connected, firmware v%u.%u.%u", dev->name, data->version[0],
		data->version[1], data->version[2]);

	return 0;
}

static void mfd_fri3d_communicator_connect_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct mfd_fri3d_communicator_data *data =
		CONTAINER_OF(dwork, struct mfd_fri3d_communicator_data, connect_work);
	const struct device *dev = data->dev;
	const struct mfd_fri3d_communicator_config *config = dev->config;

	if (data->updating || data->connected) {
		return;
	}
	if (mfd_fri3d_communicator_connect(dev) != 0) {
		if (sys_timepoint_expired(data->connect_end)) {
			LOG_INF("%s: no communicator", dev->name);
		} else {
			k_work_reschedule(dwork, K_MSEC(config->probe_period_ms));
		}
	}
}

static void mfd_fri3d_communicator_report_work(struct k_work *work)
{
	struct mfd_fri3d_communicator_data *data =
		CONTAINER_OF(work, struct mfd_fri3d_communicator_data, report_work);
	uint8_t report[FRI3D_COMMUNICATOR_REPORT_SIZE];

	while (k_msgq_get(&data->reports, report, K_NO_WAIT) == 0) {
		mfd_fri3d_communicator_report_cb_t cb = data->cb;

		data->stats.reports++;
		if (cb != NULL) {
			cb(data->dev, report, data->user_data);
		}
	}
}

/* Key report bytes on the UART: a pause starts a new report */
static void mfd_fri3d_communicator_uart_isr(const struct device *uart, void *user_data)
{
	const struct device *dev = user_data;
	const struct mfd_fri3d_communicator_config *config = dev->config;
	struct mfd_fri3d_communicator_data *data = dev->data;
	uint8_t c;

	uart_irq_update(uart);
	while (uart_irq_rx_ready(uart) > 0 && uart_fifo_read(uart, &c, 1) == 1) {
		uint32_t now = k_uptime_get_32();

		data->stats.uart_bytes++;
		if (data->rx_len > 0 && now - data->rx_last > config->report_gap_ms) {
			data->rx_len = 0;
		}
		data->rx_last = now;
		data->rx[data->rx_len++] = c;
		if (data->rx_len < FRI3D_COMMUNICATOR_REPORT_SIZE) {
			continue;
		}
		data->rx_len = 0;
		/* byte 1 is reserved (0): anything else is out of step */
		if (data->updating || data->rx[1] != 0 ||
		    k_msgq_put(&data->reports, data->rx, K_NO_WAIT) != 0) {
			data->stats.dropped++;
			continue;
		}
		k_work_submit(&data->report_work);
	}
}


static int mfd_fri3d_communicator_init(const struct device *dev)
{
	const struct mfd_fri3d_communicator_config *config = dev->config;
	struct mfd_fri3d_communicator_data *data = dev->data;

	data->dev = dev;
	k_work_init_delayable(&data->connect_work, mfd_fri3d_communicator_connect_work);
	k_work_init(&data->report_work, mfd_fri3d_communicator_report_work);
	k_msgq_init(&data->reports, (char *)data->reports_buf, FRI3D_COMMUNICATOR_REPORT_SIZE,
		    REPORT_QUEUE_LEN);

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("%s: I2C bus not ready", dev->name);
		return -ENODEV;
	}

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

	/* look for the add-on from the work queue: it may not be connected */
	data->connect_end = sys_timepoint_calc(K_MSEC(config->connect_timeout_ms));
	k_work_schedule(&data->connect_work, K_NO_WAIT);

	return 0;
}

#define MFD_FRI3D_COMMUNICATOR_DEFINE(inst)                                                        \
	static const struct mfd_fri3d_communicator_config mfd_fri3d_communicator_config##inst = {  \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.uart = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, uart),                             \
				    (DEVICE_DT_GET(DT_INST_PHANDLE(inst, uart))), (NULL)),         \
		.report_gap_ms = DT_INST_PROP(inst, report_gap_ms),                                \
		.probe_period_ms = DT_INST_PROP(inst, probe_period_ms),                            \
		.connect_timeout_ms = DT_INST_PROP(inst, connect_timeout_ms),                      \
		IF_ENABLED(CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE,                                   \
			   (.rvswd = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, rvswd),               \
						 (DEVICE_DT_GET(DT_INST_PHANDLE(inst, rvswd))),     \
						 (NULL)),                                           \
			    .update_power = COND_CODE_1(                                           \
				    DT_INST_NODE_HAS_PROP(inst, update_power_domain),              \
				    (GPIO_DT_SPEC_GET(DT_INST_PHANDLE(inst, update_power_domain),  \
						      enable_gpios)),                              \
				    ({0})),))                                                      \
	};                                                                                         \
                                                                                                   \
	static struct mfd_fri3d_communicator_data mfd_fri3d_communicator_data##inst;               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, mfd_fri3d_communicator_init, NULL,                             \
			      &mfd_fri3d_communicator_data##inst,                                  \
			      &mfd_fri3d_communicator_config##inst, POST_KERNEL,                   \
			      CONFIG_MFD_FRI3D_COMMUNICATOR_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MFD_FRI3D_COMMUNICATOR_DEFINE)

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>

static const struct device *const shell_communicator = DEVICE_DT_INST_GET(0);

static int cmd_communicator_status(const struct shell *sh, size_t argc, char **argv)
{
	struct mfd_fri3d_communicator_stats st;
	uint8_t version[3];
	uint8_t cfg;

	if (!mfd_fri3d_communicator_is_connected(shell_communicator)) {
		shell_print(sh, "no communicator");
		return 0;
	}
	mfd_fri3d_communicator_get_version(shell_communicator, version);
	mfd_fri3d_communicator_get_stats(shell_communicator, &st);
	shell_print(sh, "firmware v%u.%u.%u", version[0], version[1], version[2]);
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


SHELL_SUBCMD_SET_CREATE(sub_communicator, (communicator));
SHELL_CMD_REGISTER(communicator, &sub_communicator, "Communicator add-on", NULL);

SHELL_SUBCMD_ADD((communicator), status, NULL, "Firmware version, configuration and counters",
		 cmd_communicator_status, 1, 0);
SHELL_SUBCMD_ADD((communicator), backlight, NULL, "Keyboard backlight: backlight <0-100>",
		 cmd_communicator_backlight, 2, 0);
#endif /* CONFIG_SHELL */
