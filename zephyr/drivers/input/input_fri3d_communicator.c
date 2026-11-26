/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keyboard of the Fri3d Camp 2026 communicator add-on as input events, like
 * Zephyr's other keyboard drivers: INPUT_EV_KEY, 1 for a press and 0 for a
 * release, modifiers as keys. The MFD hands over every 8-byte HID key report
 * (its report callback: modifiers, reserved, 6 key codes);
 * the keys and modifiers that changed since the previous report become key
 * events with Zephyr's codes (the reverse of input_to_hid_code()).
 */

#define DT_DRV_COMPAT fri3d_communicator_2026_input

#include <string.h>

#include <zephyr/drivers/mfd/fri3d_communicator.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_hid.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(input_fri3d_communicator, CONFIG_INPUT_LOG_LEVEL);

/* key codes per HID usage of the keyboard page */
#define HID_USAGES     256
/* HID usages 0-3: no key, roll over, POST fail, undefined error */
#define HID_FIRST_KEY  4
/* Zephyr key codes that input_to_hid_code() knows lie below this */
#define INPUT_CODE_END 0x300

#define REPORT_KEYS    (FRI3D_COMMUNICATOR_REPORT_SIZE - 2)

struct input_fri3d_communicator_config {
	const struct device *mfd;
};

struct input_fri3d_communicator_data {
	uint8_t previous[FRI3D_COMMUNICATOR_REPORT_SIZE];
};

/* HID usage and modifier bit to Zephyr key code, shared by all instances */
static uint16_t hid_to_input[HID_USAGES];
static uint16_t modifier_to_input[8];

static void input_fri3d_communicator_build_tables(void)
{
	for (uint16_t code = 1; code < INPUT_CODE_END; code++) {
		int16_t hid = input_to_hid_code(code);
		uint8_t modifier = input_to_hid_modifier(code);

		if (hid >= HID_FIRST_KEY && hid < HID_USAGES && hid_to_input[hid] == 0) {
			hid_to_input[hid] = code;
		}
		if (modifier != 0) {
			modifier_to_input[find_lsb_set(modifier) - 1] = code;
		}
	}
}

static bool report_has_key(const uint8_t *report, uint8_t usage)
{
	return memchr(&report[2], usage, REPORT_KEYS) != NULL;
}

/* Report the keys of @p from that are not in @p to, as @p value */
static void input_fri3d_communicator_keys(const struct device *dev, const uint8_t *from,
					  const uint8_t *to, int32_t value)
{
	for (int i = 2; i < FRI3D_COMMUNICATOR_REPORT_SIZE; i++) {
		uint8_t usage = from[i];

		if (usage < HID_FIRST_KEY || report_has_key(to, usage)) {
			continue;
		}
		if (hid_to_input[usage] == 0) {
			LOG_DBG("%s: HID usage 0x%02x has no key code", dev->name, usage);
			continue;
		}
		input_report_key(dev, hid_to_input[usage], value, true, K_FOREVER);
	}
}

static void input_fri3d_communicator_report(const struct device *mfd, const uint8_t *report,
					    void *user_data)
{
	const struct device *dev = user_data;
	struct input_fri3d_communicator_data *data = dev->data;
	uint8_t changed = report[0] ^ data->previous[0];

	ARG_UNUSED(mfd);

	/* an error report (roll over: too many keys) keeps the previous state */
	if (report[2] > 0 && report[2] < HID_FIRST_KEY) {
		return;
	}

	/* releases first, so a key held through Shift changes comes out right */
	input_fri3d_communicator_keys(dev, data->previous, report, 0);
	for (int bit = 0; bit < 8; bit++) {
		if ((changed & BIT(bit)) != 0 && modifier_to_input[bit] != 0) {
			input_report_key(dev, modifier_to_input[bit], (report[0] & BIT(bit)) != 0,
					 true, K_FOREVER);
		}
	}
	input_fri3d_communicator_keys(dev, report, data->previous, 1);

	memcpy(data->previous, report, sizeof(data->previous));
}

static int input_fri3d_communicator_init(const struct device *dev)
{
	const struct input_fri3d_communicator_config *config = dev->config;

	if (!device_is_ready(config->mfd)) {
		LOG_ERR("%s: communicator not ready", dev->name);
		return -ENODEV;
	}

	if (modifier_to_input[0] == 0) {
		input_fri3d_communicator_build_tables();
	}

	mfd_fri3d_communicator_set_report_callback(config->mfd, input_fri3d_communicator_report,
						   (void *)dev);

	return 0;
}

#define INPUT_FRI3D_COMMUNICATOR_DEFINE(inst)                                                      \
	static const struct input_fri3d_communicator_config input_fri3d_communicator_config##inst = { \
		.mfd = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
	};                                                                                         \
	static struct input_fri3d_communicator_data input_fri3d_communicator_data##inst;           \
	DEVICE_DT_INST_DEFINE(inst, input_fri3d_communicator_init, NULL,                           \
			      &input_fri3d_communicator_data##inst,                                \
			      &input_fri3d_communicator_config##inst, POST_KERNEL,                 \
			      CONFIG_INPUT_FRI3D_COMMUNICATOR_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(INPUT_FRI3D_COMMUNICATOR_DEFINE)
