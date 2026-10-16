/* Copyright 2021 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Primus specific PWM LED settings. */

#include "battery.h"
#include "charge_manager.h"
#include "charge_state.h"
#include "chipset.h"
#include "common.h"
#include "compile_time_macros.h"
#include "console.h"
#include "ec_commands.h"
#include "extpower.h"
#include "hooks.h"
#include "led_common.h"
#include "power.h"
#include "pwm.h"
#include "task.h"
#include "timer.h"
#include "util.h"

#include <stdint.h>
#define CPRINTS(format, args...) cprints(CC_LOGOLED, format, ##args)

#define LED_ON_LVL 100
#define LED_OFF_LVL 0
#define LED_BAT_S3_OFF_TIME_MS 3000
#define LED_BAT_S3_TICK_MS 25
#define LED_TOTAL_TICKS 6
#define TICKS_STEP1_BRIGHTER 0
#define TICKS_STEP2_DIMMER (1000 / LED_BAT_S3_TICK_MS)
#define TICKS_STEP3_OFF (2 * TICKS_STEP2_DIMMER)
#define LED_ONE_SEC (1000 / HOOK_TICK_INTERVAL_MS)
#define LED_LOGO_TICK_SEC (LED_ONE_SEC / 4)
/* Total on/off duration in a period */
#define PERIOD (LED_LOGO_TICK_SEC * 2)
#define LED_ON 1
#define LED_OFF EC_LED_COLOR_COUNT
#define LED_EVENT_SUSPEND TASK_EVENT_CUSTOM_BIT(0)
#define LED_EVENT_200MS_TICK TASK_EVENT_CUSTOM_BIT(1)
#define BATT_NEAR_FULL 900

static int tick;

/*
 * The lid logo (the red dot on the A cover) is its own LED to the host,
 * under EC_LED_ID_LEFT_LED: the host command has no logo ID, and Linux's
 * cros_ec LED driver knows only the existing IDs, so it appears there as
 * chromeos:red:left. Under EC control it does what the power LED does, as
 * Google's code always had it; under host control each moves alone, so a
 * host flashing the power LED leaves the logo be, and the reverse.
 */
#define LOGO_LED EC_LED_ID_LEFT_LED

const enum ec_led_id supported_led_ids[] = { EC_LED_ID_BATTERY_LED,
					     EC_LED_ID_POWER_LED, LOGO_LED };
const int supported_led_ids_count = ARRAY_SIZE(supported_led_ids);

static void led_set_color_battery(enum ec_led_colors color)
{
	switch (color) {
	case EC_LED_COLOR_AMBER:
		pwm_set_duty(PWM_CH_LED1_AMBER, LED_ON_LVL);
		pwm_set_duty(PWM_CH_LED2_WHITE, LED_OFF_LVL);
		break;
	case EC_LED_COLOR_WHITE:
		pwm_set_duty(PWM_CH_LED2_WHITE, LED_ON_LVL);
		pwm_set_duty(PWM_CH_LED1_AMBER, LED_OFF_LVL);
		break;
	default: /* LED_OFF and other unsupported colors */
		pwm_set_duty(PWM_CH_LED1_AMBER, LED_OFF_LVL);
		pwm_set_duty(PWM_CH_LED2_WHITE, LED_OFF_LVL);
		break;
	}
}

static void led_set_battery(void)
{
	/*
	 * On AC with the battery held at the host's charge limit (the battery
	 * sustainer, e.g. 75-80%), show white, as for a full battery, through
	 * the sustainer's idle, top-up and discharge phases alike. Google's
	 * code below shows amber for charging under 90% and leaves the LED
	 * as it was in idle, so a capped battery stayed amber for good.
	 */
	if (extpower_is_present() && battery_sustainer_holding()) {
		led_set_color_battery(EC_LED_COLOR_WHITE);
		return;
	}

	switch (led_pwr_get_state()) {
	case LED_PWRS_CHARGE:
		/*
		 * Always indicate when charging, even in suspend.
		 * When the battery RSOC > 90, set LED to white.
		 */
		if (charge_get_display_charge() > BATT_NEAR_FULL)
			led_set_color_battery(EC_LED_COLOR_WHITE);
		else
			led_set_color_battery(EC_LED_COLOR_AMBER);

		break;
	case LED_PWRS_DISCHARGE:
		led_set_color_battery(LED_OFF);
		break;
	case LED_PWRS_CHARGE_NEAR_FULL:
		led_set_color_battery(EC_LED_COLOR_WHITE);
		break;
	case LED_PWRS_IDLE:
	case LED_PWRS_FORCED_IDLE:
		/* On AC and not charging: as charged as it will get. */
		if (extpower_is_present())
			led_set_color_battery(EC_LED_COLOR_WHITE);
		break;
	default:
		/* Other states don't alter LED behavior */
		break;
	}
}

/* Duty for whichever of the power LED and the logo the EC controls. */
static void led_set_duty_auto(int duty)
{
	if (led_auto_control_is_enabled(EC_LED_ID_POWER_LED))
		pwm_set_duty(PWM_CH_LED4, duty);
	if (led_auto_control_is_enabled(LOGO_LED))
		pwm_set_duty(PWM_CH_TKP_A_LED_N, duty);
}

/*
 * The suspend breathing ramp, one entry per tick from off to full, as raw
 * duty (0-0xffff): 65535 * (i / 40)^2.2. Equal steps in duty look like a
 * fast rise and a long plateau, because the eye's response is roughly a
 * power law; a gamma curve makes the brightness look like it changes at an
 * even rate. The PWM's 64 steps (pwm.c) resolve 33 distinct levels of it.
 */
static const uint16_t suspend_ramp[TICKS_STEP2_DIMMER + 1] = {
	0,     20,    90,    220,   413,   676,   1009,  1416,  1900,
	2462,  3104,  3828,  4636,  5529,  6508,  7574,  8730,  9975,
	11312, 12741, 14263, 15879, 17590, 19397, 21301, 23303, 25403,
	27602, 29901, 32301, 34802, 37406, 40112, 42921, 45835, 48853,
	51976, 55206, 58542, 61985, 65535,
};

static void led_set_raw_duty_auto(uint16_t duty)
{
	if (led_auto_control_is_enabled(EC_LED_ID_POWER_LED))
		pwm_set_raw_duty(PWM_CH_LED4, duty);
	if (led_auto_control_is_enabled(LOGO_LED))
		pwm_set_raw_duty(PWM_CH_TKP_A_LED_N, duty);
}

void led_set_color_power(int onoff_status)
{
	/* LED_OFF and unsupported colors are off. */
	led_set_duty_auto(onoff_status == LED_ON ? LED_ON_LVL : LED_OFF_LVL);
}

#define AC_DISCONNECTED (-1)

static void led_set_power(void)
{
	static int plug_ac_countdown;
	static int ticks;

	if (plug_ac_countdown > 0) {
		ticks = (ticks + 1) % PERIOD;
		plug_ac_countdown--;
		if (ticks < LED_LOGO_TICK_SEC)
			led_set_color_power(LED_OFF);
		else
			led_set_color_power(LED_ON);
	} else if (chipset_in_state(CHIPSET_STATE_ON))
		led_set_color_power(LED_ON);
	else if (chipset_in_state(CHIPSET_STATE_ANY_OFF))
		led_set_color_power(LED_OFF);

	/* Check AC_PRESENT */
	if (!extpower_is_present())
		plug_ac_countdown = AC_DISCONNECTED;
	else if (plug_ac_countdown == AC_DISCONNECTED)
		/* AC power was plugged in (previous state was "disconnected"),
		 * set plug_ac_countdown for amount of ticks left
		 */
		plug_ac_countdown = LED_TOTAL_TICKS;
}

void led_get_brightness_range(enum ec_led_id led_id, uint8_t *brightness_range)
{
	if (led_id == EC_LED_ID_BATTERY_LED) {
		brightness_range[EC_LED_COLOR_AMBER] = 1;
		brightness_range[EC_LED_COLOR_WHITE] = 1;
	} else if (led_id == EC_LED_ID_POWER_LED) {
		brightness_range[EC_LED_COLOR_RED] = 1;
	} else if (led_id == LOGO_LED) {
		/* PWM, so the host may dim it: brightness is the duty. */
		brightness_range[EC_LED_COLOR_RED] = LED_ON_LVL;
	}
}

int led_set_brightness(enum ec_led_id led_id, const uint8_t *brightness)
{
	if (led_id == EC_LED_ID_BATTERY_LED) {
		if (brightness[EC_LED_COLOR_AMBER] != 0)
			led_set_color_battery(EC_LED_COLOR_AMBER);
		else if (brightness[EC_LED_COLOR_WHITE] != 0)
			led_set_color_battery(EC_LED_COLOR_WHITE);
		else
			led_set_color_battery(LED_OFF);
	} else if (led_id == EC_LED_ID_POWER_LED) {
		pwm_set_duty(PWM_CH_LED4, brightness[EC_LED_COLOR_RED] ?
						  LED_ON_LVL :
						  LED_OFF_LVL);
	} else if (led_id == LOGO_LED) {
		pwm_set_duty(PWM_CH_TKP_A_LED_N,
			     MIN(brightness[EC_LED_COLOR_RED], LED_ON_LVL));
	}

	return EC_SUCCESS;
}

/* Called by hook task every 200 ms */
static void led_tick(void)
{
	task_set_event(TASK_ID_LOGOLED, LED_EVENT_200MS_TICK);
}
DECLARE_HOOK(HOOK_TICK, led_tick, HOOK_PRIO_DEFAULT);

static void suspend_led_update(void)
{
	while (1) {
		tick++;

		/* HOOK_CHIPSET_SUSPEND will be called when POWER_S0S0ix,
		 * if we are not transitioning to suspend, we should break here.
		 */
		if (!chipset_in_or_transitioning_to_state(
			    CHIPSET_STATE_ANY_SUSPEND))
			break;

		/* 1s gradual on, 1s gradual off, 3s off */
		/*
		 * The A-cover logo and the power button LED breathe together,
		 * each while the EC controls it: up the ramp over a second,
		 * down it over the next.
		 */
		if (tick <= TICKS_STEP2_DIMMER) {
			led_set_raw_duty_auto(suspend_ramp[tick]);
			crec_msleep(LED_BAT_S3_TICK_MS);
		} else if (tick <= TICKS_STEP3_OFF) {
			led_set_raw_duty_auto(
				suspend_ramp[TICKS_STEP3_OFF - tick]);
			crec_msleep(LED_BAT_S3_TICK_MS);
		} else {
			tick = TICKS_STEP1_BRIGHTER;
			crec_msleep(LED_BAT_S3_OFF_TIME_MS);
		}
	}
}

static void suspend_led_init(void)
{
	task_set_event(TASK_ID_LOGOLED, LED_EVENT_SUSPEND);
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, suspend_led_init, HOOK_PRIO_DEFAULT);

void logoled_task(void *u)
{
	uint32_t evt;

	while (1) {
		evt = task_wait_event(-1);

		if (evt & LED_EVENT_SUSPEND) {
			tick = TICKS_STEP2_DIMMER;
			suspend_led_update();
		}

		if (evt & LED_EVENT_200MS_TICK) {
			if (led_auto_control_is_enabled(EC_LED_ID_POWER_LED) ||
			    led_auto_control_is_enabled(LOGO_LED))
				led_set_power();
			if (led_auto_control_is_enabled(EC_LED_ID_BATTERY_LED))
				led_set_battery();
		}
	}
}
