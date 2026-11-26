/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Between the communicator's MFD driver and its input driver (the MFD's child,
 * drivers/input/input_fri3d_communicator.c): the HID key reports. Not for
 * applications, which take the input events.
 */

#ifndef ZEPHYR_DRIVERS_MFD_MFD_FRI3D_COMMUNICATOR_REPORT_H_
#define ZEPHYR_DRIVERS_MFD_MFD_FRI3D_COMMUNICATOR_REPORT_H_

#include <stdint.h>

#include <zephyr/device.h>

/*
 * Called from the system work queue with every HID key report that differs
 * from the previous one: report[0] the modifier bits, report[2..7] the key
 * codes (HID usage IDs, keyboard page) of the keys held.
 */
typedef void (*mfd_fri3d_communicator_report_cb_t)(const struct device *dev,
						   const uint8_t *report, void *user_data);

/* Set the key report callback (one per device: the input driver's) */
void mfd_fri3d_communicator_set_report_callback(const struct device *dev,
						mfd_fri3d_communicator_report_cb_t cb,
						void *user_data);

#endif /* ZEPHYR_DRIVERS_MFD_MFD_FRI3D_COMMUNICATOR_REPORT_H_ */
