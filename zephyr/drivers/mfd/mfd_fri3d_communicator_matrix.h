/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Between the communicator's MFD driver and its input driver (the MFD's child,
 * drivers/input/input_fri3d_communicator.c): the key matrix. Not for
 * applications, which take the input events.
 */

#ifndef ZEPHYR_DRIVERS_MFD_MFD_FRI3D_COMMUNICATOR_MATRIX_H_
#define ZEPHYR_DRIVERS_MFD_MFD_FRI3D_COMMUNICATOR_MATRIX_H_

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/mfd/fri3d_communicator.h>

/*
 * Called from the system work queue with every key matrix read that differs
 * from the previous one: FRI3D_COMMUNICATOR_MATRIX_COLS bytes, bit r of byte c
 * set while the key in row r, column c is pressed.
 */
typedef void (*mfd_fri3d_communicator_matrix_cb_t)(const struct device *dev,
						   const uint8_t *matrix, void *user_data);

/* Set the key matrix callback (one per device: the input driver's) */
void mfd_fri3d_communicator_set_matrix_callback(const struct device *dev,
						mfd_fri3d_communicator_matrix_cb_t cb,
						void *user_data);

#endif /* ZEPHYR_DRIVERS_MFD_MFD_FRI3D_COMMUNICATOR_MATRIX_H_ */
