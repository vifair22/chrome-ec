/* Copyright 2021 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "chipset.h"
#include "common.h"
#include "console.h"
#include "fan.h"
#include "hooks.h"
#include "host_command.h"
#include "temp_sensor.h"
#include "thermal.h"
#include "util.h"

/*
 * Fan profile, a board-specific host command. coreboot's primus code
 * (mb/google/brya/variants/primus) sends the same number and layout; keep
 * them in step.
 */
#define EC_CMD_PRIMUS_FAN_PROFILE EC_PRIVATE_HOST_COMMAND_VALUE(0x01)
enum primus_fan_profile {
	PRIMUS_FAN_PROFILE_QUIET = 0,
	PRIMUS_FAN_PROFILE_BALANCED = 1,
	PRIMUS_FAN_PROFILE_PERFORMANCE = 2,
	PRIMUS_FAN_PROFILE_QUERY = 0xff,
};
struct ec_params_primus_fan_profile {
	uint8_t profile;
} __ec_align1;
struct ec_response_primus_fan_profile {
	uint8_t profile;
} __ec_align1;
/* Console output macros */
#define CPUTS(outstr) cputs(CC_THERMAL, outstr)
#define CPRINTS(format, args...) cprints(CC_THERMAL, format, ##args)

struct fan_step {
	/*
	 * Sensor 1~4 trigger point, set -1 if we're not using this
	 * sensor to determine fan speed.
	 */
	int8_t on[TEMP_SENSOR_COUNT];
	/*
	 * Sensor 1~4 trigger point, set -1 if we're not using this
	 * sensor to determine fan speed.
	 */
	int8_t off[TEMP_SENSOR_COUNT];
	/* Fan rpm */
	uint16_t rpm[FAN_CH_COUNT];
};

/*
 * Fan profiles, chosen by the host with EC_CMD_PRIMUS_FAN_PROFILE (coreboot
 * sends setup's "Fan profile" option on every boot). Each table has the same
 * number of levels; Balanced is Google's table, unchanged, and the default
 * after an EC reset until the host says otherwise.
 */
static const struct fan_step fan_table_balanced[] = {
	{
		/* level 0 */
		.on = { 40, -1, -1, -1, -1 },
		.off = { 0, -1, -1, -1, -1 },
		.rpm = { 0 },
	},
	{
		/* level 1 */
		.on = { 42, -1, -1, -1, -1 },
		.off = { 40, -1, -1, -1, -1 },
		.rpm = { 1800 },
	},
	{
		/* level 2 */
		.on = { 43, -1, -1, -1, -1 },
		.off = { 42, -1, -1, -1, -1 },
		.rpm = { 2000 },
	},
	{
		/* level 3 */
		.on = { 44, -1, -1, -1, -1 },
		.off = { 43, -1, -1, -1, -1 },
		.rpm = { 2200 },
	},
	{
		/* level 4 */
		.on = { 45, -1, -1, -1, -1 },
		.off = { 44, -1, -1, -1, -1 },
		.rpm = { 2500 },
	},
	{
		/* level 5 */
		.on = { 46, -1, -1, -1, -1 },
		.off = { 45, -1, -1, -1, -1 },
		.rpm = { 2800 },
	},
	{
		/* level 6 */
		.on = { 47, -1, -1, -1, -1 },
		.off = { 46, -1, -1, -1, -1 },
		.rpm = { 3000 },
	},
	{
		/* level 7 */
		.on = { 75, -1, -1, -1, -1 },
		.off = { 72, -1, -1, -1, -1 },
		.rpm = { 3200 },
	},
};

/*
 * Quiet: the fan stays off until the DDR/SoC sensor passes 55C (it reads
 * 34C idle and 46-47C at a sustained 15 W with the fan running), then
 * climbs the same RPM steps as Balanced two degrees apart, reaching the top
 * step above 67C, before the 70C at which the EC asks the host to throttle.
 */
static const struct fan_step fan_table_quiet[] = {
	{
		/* level 0 */
		.on = { 55, -1, -1, -1, -1 },
		.off = { 0, -1, -1, -1, -1 },
		.rpm = { 0 },
	},
	{
		/* level 1 */
		.on = { 57, -1, -1, -1, -1 },
		.off = { 52, -1, -1, -1, -1 },
		.rpm = { 1800 },
	},
	{
		/* level 2 */
		.on = { 59, -1, -1, -1, -1 },
		.off = { 55, -1, -1, -1, -1 },
		.rpm = { 2000 },
	},
	{
		/* level 3 */
		.on = { 61, -1, -1, -1, -1 },
		.off = { 57, -1, -1, -1, -1 },
		.rpm = { 2200 },
	},
	{
		/* level 4 */
		.on = { 63, -1, -1, -1, -1 },
		.off = { 59, -1, -1, -1, -1 },
		.rpm = { 2500 },
	},
	{
		/* level 5 */
		.on = { 65, -1, -1, -1, -1 },
		.off = { 61, -1, -1, -1, -1 },
		.rpm = { 2800 },
	},
	{
		/* level 6 */
		.on = { 67, -1, -1, -1, -1 },
		.off = { 63, -1, -1, -1, -1 },
		.rpm = { 3000 },
	},
	{
		/* level 7 */
		.on = { 99, -1, -1, -1, -1 },
		.off = { 65, -1, -1, -1, -1 },
		.rpm = { 3200 },
	},
};

/*
 * Performance: starts at 36C, at idle temperatures, and climbs faster and
 * higher than Balanced, to 4400 RPM (the fan's rpm_max is 4800).
 */
static const struct fan_step fan_table_performance[] = {
	{
		/* level 0 */
		.on = { 36, -1, -1, -1, -1 },
		.off = { 0, -1, -1, -1, -1 },
		.rpm = { 0 },
	},
	{
		/* level 1 */
		.on = { 38, -1, -1, -1, -1 },
		.off = { 34, -1, -1, -1, -1 },
		.rpm = { 2000 },
	},
	{
		/* level 2 */
		.on = { 40, -1, -1, -1, -1 },
		.off = { 36, -1, -1, -1, -1 },
		.rpm = { 2400 },
	},
	{
		/* level 3 */
		.on = { 42, -1, -1, -1, -1 },
		.off = { 38, -1, -1, -1, -1 },
		.rpm = { 2800 },
	},
	{
		/* level 4 */
		.on = { 44, -1, -1, -1, -1 },
		.off = { 40, -1, -1, -1, -1 },
		.rpm = { 3200 },
	},
	{
		/* level 5 */
		.on = { 46, -1, -1, -1, -1 },
		.off = { 42, -1, -1, -1, -1 },
		.rpm = { 3600 },
	},
	{
		/* level 6 */
		.on = { 50, -1, -1, -1, -1 },
		.off = { 44, -1, -1, -1, -1 },
		.rpm = { 4000 },
	},
	{
		/* level 7 */
		.on = { 99, -1, -1, -1, -1 },
		.off = { 48, -1, -1, -1, -1 },
		.rpm = { 4400 },
	},
};

BUILD_ASSERT(ARRAY_SIZE(fan_table_quiet) == ARRAY_SIZE(fan_table_balanced));
BUILD_ASSERT(ARRAY_SIZE(fan_table_performance) ==
	     ARRAY_SIZE(fan_table_balanced));

static const struct fan_step *const fan_tables[] = {
	[PRIMUS_FAN_PROFILE_QUIET] = fan_table_quiet,
	[PRIMUS_FAN_PROFILE_BALANCED] = fan_table_balanced,
	[PRIMUS_FAN_PROFILE_PERFORMANCE] = fan_table_performance,
};
static uint8_t fan_profile = PRIMUS_FAN_PROFILE_BALANCED;
#define fan_table (fan_tables[fan_profile])
const int num_fan_levels = ARRAY_SIZE(fan_table_balanced);

/* The level state, at file scope so a profile change can restart it. */
static int current_level;
static int prev_current_level;
static int prev_temp[TEMP_SENSOR_COUNT];

int fan_table_to_rpm(int fan, int *temp, enum temp_sensor_id temp_sensor)
{
	int i;
	int new_rpm = 0;

	/*
	 * Compare the current and previous temperature, we have
	 * the three paths :
	 *  1. decreasing path. (check the release point)
	 *  2. increasing path. (check the trigger point)
	 *  3. invariant path. (return the current RPM)
	 */
	if (temp[temp_sensor] < prev_temp[temp_sensor]) {
		for (i = current_level; i > 0; i--) {
			if (temp[temp_sensor] < fan_table[i].off[temp_sensor])
				current_level = i - 1;
			else
				break;
		}
	} else if (temp[temp_sensor] > prev_temp[temp_sensor]) {
		for (i = current_level; i < num_fan_levels; i++) {
			if (temp[temp_sensor] > fan_table[i].on[temp_sensor])
				current_level = i + 1;
			else
				break;
		}
	}
	if (current_level < 0)
		current_level = 0;

	if (current_level >= num_fan_levels)
		current_level = num_fan_levels - 1;

	if (current_level != prev_current_level) {
		CPRINTS("temp: %d, prev_temp: %d", temp[temp_sensor],
			prev_temp[temp_sensor]);
		CPRINTS("current_level: %d", current_level);
	}

	prev_temp[temp_sensor] = temp[temp_sensor];
	prev_current_level = current_level;

	switch (fan) {
	case FAN_CH_0:
		new_rpm = fan_table[current_level].rpm[FAN_CH_0];
		break;
	default:
		break;
	}
	return new_rpm;
}
void board_override_fan_control(int fan, int *temp)
{
	if (chipset_in_state(CHIPSET_STATE_ON)) {
		fan_set_rpm_mode(FAN_CH(fan), 1);
		fan_set_rpm_target(FAN_CH(fan),
				   fan_table_to_rpm(FAN_CH(fan), temp,
						    TEMP_SENSOR_1_DDR_SOC));
	} else if (chipset_in_state(CHIPSET_STATE_ANY_SUSPEND)) {
		/* Stop fan when enter S0ix */
		fan_set_rpm_mode(FAN_CH(fan), 1);
		fan_set_rpm_target(FAN_CH(fan), 0);
	}
}

/*
 * Set (profile 0-2) or query (0xff) the fan profile. A change restarts the
 * level from 0 with no previous temperature, so the next control tick climbs
 * the new table to the current temperature instead of keeping a level the
 * old table chose.
 */
static enum ec_status host_cmd_fan_profile(struct host_cmd_handler_args *args)
{
	const struct ec_params_primus_fan_profile *p = args->params;
	struct ec_response_primus_fan_profile *r = args->response;

	if (p->profile != PRIMUS_FAN_PROFILE_QUERY) {
		if (p->profile >= ARRAY_SIZE(fan_tables))
			return EC_RES_INVALID_PARAM;
		if (p->profile != fan_profile) {
			fan_profile = p->profile;
			current_level = 0;
			memset(prev_temp, 0, sizeof(prev_temp));
			CPRINTS("fan profile %d", fan_profile);
		}
	}
	r->profile = fan_profile;
	args->response_size = sizeof(*r);
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_PRIMUS_FAN_PROFILE, host_cmd_fan_profile,
		     EC_VER_MASK(0));
