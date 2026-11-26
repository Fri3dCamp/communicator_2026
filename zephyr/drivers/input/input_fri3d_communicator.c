/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keyboard of the Fri3d Camp 2026 communicator add-on as input events, like
 * Zephyr's other keyboard drivers: INPUT_EV_KEY, 1 for a press and 0 for a
 * release, modifiers as keys. The MFD hands over every changed key matrix
 * (9 columns of 8 rows, read over I2C); every key whose state changed becomes
 * an event with the code of the devicetree keymap (MATRIX_KEY entries). The
 * events of one matrix read form one group: the sync flag on the last.
 */

#define DT_DRV_COMPAT fri3d_communicator_2026_input

#include <string.h>

#include <zephyr/drivers/mfd/fri3d_communicator.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_hid.h>
#include <zephyr/logging/log.h>

#include "../mfd/mfd_fri3d_communicator_matrix.h"

LOG_MODULE_REGISTER(input_fri3d_communicator, CONFIG_INPUT_LOG_LEVEL);

#define ROWS 8

/* MATRIX_KEY(row, col, code), <zephyr/dt-bindings/input/keymap.h> */
#define KEYMAP_ROW(entry)  (((entry) >> 24) & 0xff)
#define KEYMAP_COL(entry)  (((entry) >> 16) & 0xff)
#define KEYMAP_CODE(entry) ((entry) & 0xffff)

struct input_fri3d_communicator_config {
	const struct device *mfd;
	const uint32_t *keymap;
	size_t keymap_len;
};

struct input_fri3d_communicator_data {
	uint8_t previous[FRI3D_COMMUNICATOR_MATRIX_COLS];
};

/* Is the key of keymap entry @p entry pressed in @p matrix? */
static bool key_pressed(const uint8_t *matrix, uint32_t entry)
{
	return (matrix[KEYMAP_COL(entry)] & BIT(KEYMAP_ROW(entry))) != 0;
}

/*
 * Is @p code pressed in @p matrix: one of its switches is (the space bar has
 * three)?
 */
static bool code_pressed(const struct input_fri3d_communicator_config *config,
			 const uint8_t *matrix, uint16_t code)
{
	for (size_t i = 0; i < config->keymap_len; i++) {
		if (KEYMAP_CODE(config->keymap[i]) == code && key_pressed(matrix, config->keymap[i])) {
			return true;
		}
	}

	return false;
}

/*
 * The codes whose state changed from @p previous to @p matrix, each once. The
 * order makes chords come out right for event-by-event readers: releases of
 * other keys, releases of modifiers, presses of modifiers, presses of other
 * keys (Shift goes down before the A that was pressed with it).
 */
static size_t changed_codes(const struct input_fri3d_communicator_config *config,
			    const uint8_t *previous, const uint8_t *matrix, uint16_t *codes,
			    uint8_t *values)
{
	size_t count = 0;

	for (int pass = 0; pass < 4; pass++) {
		bool press = pass >= 2;
		bool modifiers = pass == 1 || pass == 2;

		for (size_t i = 0; i < config->keymap_len; i++) {
			uint16_t code = KEYMAP_CODE(config->keymap[i]);
			bool now = code_pressed(config, matrix, code);
			bool before = code_pressed(config, previous, code);
			bool seen = false;

			if (now == before || now != press ||
			    (input_to_hid_modifier(code) != 0) != modifiers) {
				continue;
			}
			for (size_t j = 0; j < count; j++) {
				if (codes[j] == code) {
					seen = true;
					break;
				}
			}
			if (!seen) {
				codes[count] = code;
				values[count] = press ? 1 : 0;
				count++;
			}
		}
	}

	return count;
}

static void input_fri3d_communicator_matrix(const struct device *mfd, const uint8_t *matrix,
					    void *user_data)
{
	const struct device *dev = user_data;
	const struct input_fri3d_communicator_config *config = dev->config;
	struct input_fri3d_communicator_data *data = dev->data;
	uint16_t codes[FRI3D_COMMUNICATOR_MATRIX_COLS * ROWS];
	uint8_t values[FRI3D_COMMUNICATOR_MATRIX_COLS * ROWS];
	size_t count;

	ARG_UNUSED(mfd);

	count = changed_codes(config, data->previous, matrix, codes, values);
	for (size_t i = 0; i < count; i++) {
		input_report_key(dev, codes[i], values[i], i == count - 1, K_FOREVER);
	}

	memcpy(data->previous, matrix, sizeof(data->previous));
}

static int input_fri3d_communicator_init(const struct device *dev)
{
	const struct input_fri3d_communicator_config *config = dev->config;

	if (!device_is_ready(config->mfd)) {
		LOG_ERR("%s: communicator not ready", dev->name);
		return -ENODEV;
	}

	for (size_t i = 0; i < config->keymap_len; i++) {
		if (KEYMAP_ROW(config->keymap[i]) >= ROWS ||
		    KEYMAP_COL(config->keymap[i]) >= FRI3D_COMMUNICATOR_MATRIX_COLS) {
			LOG_ERR("%s: keymap entry %zu outside the matrix", dev->name, i);
			return -EINVAL;
		}
	}

	mfd_fri3d_communicator_set_matrix_callback(config->mfd, input_fri3d_communicator_matrix,
						   (void *)dev);

	return 0;
}

#define INPUT_FRI3D_COMMUNICATOR_DEFINE(inst)                                                      \
	static const uint32_t input_fri3d_communicator_keymap##inst[] =                            \
		DT_INST_PROP(inst, keymap);                                                        \
	static const struct input_fri3d_communicator_config input_fri3d_communicator_config##inst = { \
		.mfd = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
		.keymap = input_fri3d_communicator_keymap##inst,                                   \
		.keymap_len = ARRAY_SIZE(input_fri3d_communicator_keymap##inst),                   \
	};                                                                                         \
	static struct input_fri3d_communicator_data input_fri3d_communicator_data##inst;           \
	DEVICE_DT_INST_DEFINE(inst, input_fri3d_communicator_init, NULL,                           \
			      &input_fri3d_communicator_data##inst,                                \
			      &input_fri3d_communicator_config##inst, POST_KERNEL,                 \
			      CONFIG_INPUT_FRI3D_COMMUNICATOR_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(INPUT_FRI3D_COMMUNICATOR_DEFINE)
