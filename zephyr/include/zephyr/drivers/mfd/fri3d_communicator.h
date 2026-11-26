/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Fri3d Camp 2026 communicator add-on (multi-function device)
 *
 * The CH32X035 keyboard controller of the communicator, compatible
 * "fri3d,communicator-2026": I2C registers of the communicator_2026 firmware.
 * The add-on sends its HID key report on its UART on every change; the driver
 * takes that only as the trigger to read the key matrix over I2C (firmware
 * v1.1.0 and later).
 * Applications get the keys from the input child (fri3d,communicator-2026-input).
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MFD_FRI3D_COMMUNICATOR_H_
#define ZEPHYR_INCLUDE_DRIVERS_MFD_FRI3D_COMMUNICATOR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Firmware version: major, minor, patch (debug builds: 255.255.255) */
#define FRI3D_COMMUNICATOR_REG_VERSION   0x00
/** Current HID key report (8 bytes: modifiers, reserved, 6 key codes) */
#define FRI3D_COMMUNICATOR_REG_REPORT    0x03
/** Configuration (1 byte, FRI3D_COMMUNICATOR_CONFIG_*) */
#define FRI3D_COMMUNICATOR_REG_CONFIG    0x0b
/** Keyboard backlight, 0-100 (2 bytes, little endian) */
#define FRI3D_COMMUNICATOR_REG_BACKLIGHT 0x0c
/** Key matrix of the current report (FRI3D_COMMUNICATOR_MATRIX_COLS bytes, firmware v1.1.0 and later) */
#define FRI3D_COMMUNICATOR_REG_MATRIX    0x0e

/** @name Configuration register bits (firmware v1.0.3 and later)
 * @{
 */
/** Reboot into the WCH bootloader */
#define FRI3D_COMMUNICATOR_CONFIG_REBOOT       BIT(0)
/** Give the I2C pins to SWD (firmware update) */
#define FRI3D_COMMUNICATOR_CONFIG_REMAP        BIT(1)
/** Shut the speaker amplifier down (communicator rev 01 and later) */
#define FRI3D_COMMUNICATOR_CONFIG_AMP_SHUTDOWN BIT(2)
/** @} */

/** Size of a HID key report */
#define FRI3D_COMMUNICATOR_REPORT_SIZE 8

/** Columns of the key matrix: one byte each, bit r = row r pressed */
#define FRI3D_COMMUNICATOR_MATRIX_COLS 9

/** Counters, for diagnostics */
struct mfd_fri3d_communicator_stats {
	/** Bytes received on the UART (each one a trigger to read) */
	uint32_t uart_bytes;
	/** I2C reads of the key matrix */
	uint32_t reads;
	/** I2C reads that failed */
	uint32_t read_errors;
	/** Reads with a changed key matrix (handed to the input driver) */
	uint32_t changes;
};

/**
 * @brief Read registers.
 *
 * @retval 0 Success.
 * @retval -EBUSY A firmware update is running.
 * @retval <0 I2C error.
 */
int mfd_fri3d_communicator_reg_read(const struct device *dev, uint8_t reg, void *data, size_t len);

/**
 * @brief Write registers.
 *
 * @retval 0 Success.
 * @retval -EBUSY A firmware update is running.
 * @retval <0 I2C error.
 */
int mfd_fri3d_communicator_reg_write(const struct device *dev, uint8_t reg, const void *data,
				     size_t len);

/**
 * @brief Change configuration bits (read, modify, write).
 *
 * @param dev MFD device.
 * @param mask FRI3D_COMMUNICATOR_CONFIG_* bits to change.
 * @param value Their new values.
 *
 * @retval 0 Success.
 * @retval <0 As mfd_fri3d_communicator_reg_read() and _reg_write().
 */
int mfd_fri3d_communicator_config_update(const struct device *dev, uint8_t mask, uint8_t value);

/**
 * @brief Set the keyboard backlight.
 *
 * @param dev MFD device.
 * @param percent Brightness, 0-100.
 *
 * @retval 0 Success.
 * @retval -EINVAL Brightness above 100.
 * @retval <0 As mfd_fri3d_communicator_reg_write().
 */
int mfd_fri3d_communicator_set_backlight(const struct device *dev, uint8_t percent);

/**
 * @brief Firmware version the add-on reported at init (or after an update).
 *
 * @param dev MFD device.
 * @param version Major, minor, patch.
 */
void mfd_fri3d_communicator_get_version(const struct device *dev, uint8_t version[3]);

/**
 * @brief Get the last key matrix read.
 *
 * Read over I2C on every key change (firmware v1.1.0 and later): one byte per
 * column, bit r set while the key in row r is pressed.
 *
 * @param dev MFD device.
 * @param matrix Buffer for FRI3D_COMMUNICATOR_MATRIX_COLS bytes.
 *
 * @retval 0 Success.
 * @retval -ENOTSUP The add-on's firmware has no key matrix register.
 */
int mfd_fri3d_communicator_get_matrix(const struct device *dev,
				      uint8_t matrix[FRI3D_COMMUNICATOR_MATRIX_COLS]);

/**
 * @brief Get the counters.
 */
void mfd_fri3d_communicator_get_stats(const struct device *dev,
				      struct mfd_fri3d_communicator_stats *stats);

/**
 * @brief Update progress callback.
 *
 * @param done Bytes programmed.
 * @param total Bytes to program.
 * @param user_data As given to mfd_fri3d_communicator_update().
 */
typedef void (*mfd_fri3d_communicator_progress_cb_t)(size_t done, size_t total,
						     void *user_data);

/**
 * @brief Called by mfd_fri3d_communicator_update() before the SWD traffic starts.
 *
 * The add-on has given its I2C pins to SWD: switch off or keep away what must
 * not see that traffic (it starts like an I2C start condition), e.g. the other
 * devices on the bus and the bus pull-ups.
 *
 * @param dev MFD device.
 * @param user_data As given to mfd_fri3d_communicator_set_update_callbacks().
 */
typedef void (*mfd_fri3d_communicator_pre_update_cb_t)(const struct device *dev,
						       void *user_data);

/**
 * @brief Called by mfd_fri3d_communicator_update() after the SWD session.
 *
 * The pins are I2C again: undo what the pre-update callback did, e.g. switch
 * the other devices on the bus on again and restart them.
 *
 * @param dev MFD device.
 * @param result 0 when the add-on runs the new firmware, else a negative errno.
 * @param user_data As given to mfd_fri3d_communicator_set_update_callbacks().
 */
typedef void (*mfd_fri3d_communicator_post_update_cb_t)(const struct device *dev, int result,
							void *user_data);

/**
 * @brief Set the callbacks around a firmware update (one pair per device).
 *
 * Needs CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE. Both are optional (NULL).
 *
 * @param dev MFD device.
 * @param pre Called before the SWD traffic starts.
 * @param post Called after the SWD session.
 * @param user_data Passed to both.
 */
void mfd_fri3d_communicator_set_update_callbacks(const struct device *dev,
						 mfd_fri3d_communicator_pre_update_cb_t pre,
						 mfd_fri3d_communicator_post_update_cb_t post,
						 void *user_data);

/**
 * @brief Program a firmware image into the add-on over RVSWD.
 *
 * Asks the firmware to give its I2C pins to SWD (remap), programs the image,
 * restarts the add-on and reads its version again; the callbacks of
 * mfd_fri3d_communicator_set_update_callbacks() run around the SWD session.
 * Blocks for about ten seconds. Needs CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE.
 *
 * @param dev MFD device.
 * @param image Firmware binary (communicator_2026 release build).
 * @param len Its length.
 * @param cb Progress callback, or NULL.
 * @param user_data Passed to @p cb.
 *
 * @retval 0 Success: the add-on runs the new firmware.
 * @retval -ENOTSUP No rvswd property.
 * @retval -ENODEV RVSWD not ready, or not a CH32X03x.
 * @retval -ETIMEDOUT The add-on did not answer on I2C after the update.
 * @retval <0 Programming error.
 */
int mfd_fri3d_communicator_update(const struct device *dev, const void *image, size_t len,
				  mfd_fri3d_communicator_progress_cb_t cb, void *user_data);

/**
 * @brief Get the version of the bundled firmware image.
 *
 * The image comes from CONFIG_MFD_FRI3D_COMMUNICATOR_FIRMWARE, its version from
 * CONFIG_MFD_FRI3D_COMMUNICATOR_FIRMWARE_VERSION. Together with
 * mfd_fri3d_communicator_get_version() an application can keep the add-on on
 * the bundled version, e.g. at start:
 *
 * @code{.c}
 * uint8_t running[3], bundled[3];
 *
 * mfd_fri3d_communicator_get_version(dev, running);
 * if (mfd_fri3d_communicator_get_bundled_version(bundled) == 0 &&
 *     memcmp(running, bundled, sizeof(bundled)) < 0) {
 *         mfd_fri3d_communicator_update_bundled(dev, NULL, NULL);
 * }
 * @endcode
 *
 * Debug builds of the add-on firmware report 255.255.255, so they are left
 * alone. Needs CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE.
 *
 * @param version Buffer for major, minor and patch.
 *
 * @retval 0 Success.
 * @retval -ENOENT No bundled image.
 */
int mfd_fri3d_communicator_get_bundled_version(uint8_t version[3]);

/**
 * @brief Program the bundled firmware image into the add-on.
 *
 * As mfd_fri3d_communicator_update() with the image of
 * CONFIG_MFD_FRI3D_COMMUNICATOR_FIRMWARE. Needs
 * CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE.
 *
 * @param dev MFD device.
 * @param cb Progress callback, or NULL.
 * @param user_data Passed to @p cb.
 *
 * @retval 0 Success: the add-on runs the bundled firmware.
 * @retval -ENOENT No bundled image.
 * @retval <0 As mfd_fri3d_communicator_update().
 */
int mfd_fri3d_communicator_update_bundled(const struct device *dev,
					  mfd_fri3d_communicator_progress_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MFD_FRI3D_COMMUNICATOR_H_ */
