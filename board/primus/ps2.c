/* Copyright 2021 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cbi_ssfc.h"
#include "chipset.h"
#include "gpio.h"
#include "hooks.h"
#include "keyboard_8042.h"
#include "ps2.h"
#include "ps2_chip.h"
#include "registers.h"
#include "time.h"

#include <stddef.h>
#include <string.h>

#define PS2_TRANSMIT_DELAY_MS 10

static uint8_t queue_data[3];
static int data_count;
/*
 * To make sure Synaptics trackpoint receive full resume commands,
 * use this variable to track resume status. It will block host
 * send command to Synaptics trackpoint during resume process.
 * Suspend: 1
 * Resume: 0
 */
static bool trackpoint_in_suspend;

void send_aux_data_to_device(uint8_t data)
{
	if (!trackpoint_in_suspend)
		ps2_transmit_byte(PRIMUS_PS2_CH, data);
}

static void board_init(void)
{
	ps2_enable_channel(PRIMUS_PS2_CH, 1, send_aux_data_to_host_interrupt);
}
DECLARE_HOOK(HOOK_INIT, board_init, HOOK_PRIO_DEFAULT);

/*
 * Goodix touchpad AVDD need to pull low to 0V when poweroff.
 * Setting PS2 module in GPIO.inc will let AVDD have 0.9V offset.
 * So we need to enable PS2 module later than PLTRST# to avoid the 0.9V
 * offset.
 */
static void enable_ps2(void)
{
	gpio_set_alternate_function(GPIO_PORT_6, BIT(2) | BIT(3),
				    GPIO_ALT_FUNC_DEFAULT);
}
DECLARE_DEFERRED(enable_ps2);

static void ps2_pins_low(void)
{
	gpio_set_flags(GPIO_EC_PS2_SCL_TPAD, GPIO_ODR_LOW);
	gpio_set_flags(GPIO_EC_PS2_SDA_TPAD, GPIO_ODR_LOW);
	gpio_set_alternate_function(GPIO_PORT_6, BIT(2) | BIT(3),
				    GPIO_ALT_FUNC_NONE);
}

static void disable_ps2(void)
{
	ps2_pins_low();
	/* make sure PLTRST# goes high and re-enable PS2.*/
	hook_call_deferred(&enable_ps2_data, 2 * SECOND);
}
DECLARE_HOOK(HOOK_CHIPSET_RESET, disable_ps2, HOOK_PRIO_DEFAULT);

/*
 * The reset hook above is the only place Google's code enables PS2, and it
 * runs on an assertion of PLTRST# the EC sees. An EC that has just booted
 * (power-on, a reset after a new RW from software sync, battery cutoff)
 * never sees one: the AP comes up with PLTRST# already asserted, and the
 * EC only sees it go away. The pins then stay low GPIOs from gpio.inc for
 * the whole boot: the trackpoint is dead, and the AP firmware's PS/2 code
 * stalls on an aux device that cannot answer, so the keyboard fails in
 * edk2 and its time crawls (found on the C14 2026-10-14; a warm reboot
 * cured it, the reset hook having run).
 *
 * So also enable PS2 two seconds after the AP powers on, and at EC init when
 * the AP is already on (software sync jumps to RW mid-boot, after the
 * startup hook ran in RO), and hold the pins low again at shutdown, which
 * keeps the touchpad's AVDD at 0V while the AP is off, as above intends.
 */
static void ps2_chipset_startup(void)
{
	hook_call_deferred(&enable_ps2_data, 2 * SECOND);
}
DECLARE_HOOK(HOOK_CHIPSET_STARTUP, ps2_chipset_startup, HOOK_PRIO_DEFAULT);

static void ps2_chipset_shutdown(void)
{
	hook_call_deferred(&enable_ps2_data, -1);
	ps2_pins_low();
}
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, ps2_chipset_shutdown, HOOK_PRIO_DEFAULT);

static void ps2_init_ap_on(void)
{
	if (chipset_in_state(CHIPSET_STATE_ON))
		hook_call_deferred(&enable_ps2_data, 2 * SECOND);
}
DECLARE_HOOK(HOOK_INIT, ps2_init_ap_on, HOOK_PRIO_DEFAULT + 1);

static void ps2_transmit(uint8_t cmd)
{
	ps2_transmit_byte(PRIMUS_PS2_CH, cmd);
	crec_msleep(PS2_TRANSMIT_DELAY_MS);
}

/* Process the PS2 data at here */
void get_ps2_data(uint8_t data)
{
	/* receive the PS2 data and save in PS2 queue */
	queue_data[data_count++] = data;
}

static void send_command_to_trackpoint(uint8_t command1, uint8_t command2)
{
	/*
	 * Send command to trackpoint and wait,
	 * this will make sure EC get ACK from PS2 device
	 * and send the next command.
	 */

	ps2_transmit(TP_COMMAND);
	ps2_transmit(TP_TOGGLE);
	ps2_transmit(command1);
	ps2_transmit(command2);
}

uint8_t get_trackpoint_id(void)
{
	/*
	 * Three data will be received when asking PS2
	 * device id.
	 */
	ps2_transmit(TP_READ_ID);

	/*
	 * When EC send TP_READ_ID, trackpoint will return ACK(0xFA),
	 * and then return device ID. So return the second data.
	 * Also make sure only return the trackpoint device ID.
	 */
	if (queue_data[1] == TP_VARIANT_ELAN ||
	    queue_data[1] == TP_VARIANT_SYNAPTICS)
		return queue_data[1];
	else
		return 0;
}

/* Called on AP S0 -> S0ix transition */
static void ps2_suspend(void)
{
	uint8_t trackpoint_id;
	/*
	 * When EC send PS2 command to PS2 device,
	 * PS2 device will return ACK(0xFA).
	 * EC will send it to host and cause host wake from suspend.
	 * So disable EC send data to host to avoid it.
	 *
	 * In order to receive the PS2 data and also not to wake host,
	 * use get_ps2_data to process PS2 data.
	 */
	ps2_enable_channel(PRIMUS_PS2_CH, 1, get_ps2_data);
	trackpoint_id = get_trackpoint_id();
	/*
	 * Don't need to read any data from PS2 device now,
	 * so disable it.
	 */
	ps2_enable_channel(PRIMUS_PS2_CH, 1, NULL);

	/*
	 * Send suspend mode to trackpoint
	 * Those commands was provide by Elan and Synaptics
	 */
	if (trackpoint_id == TP_VARIANT_ELAN)
		send_command_to_trackpoint(TP_TOGGLE_BURST,
					   TP_TOGGLE_ELAN_SLEEP);
	else if (trackpoint_id == TP_VARIANT_SYNAPTICS) {
		send_command_to_trackpoint(TP_TOGGLE_SOURCE_TAG,
					   TP_TOGGLE_SNAPTICS_SLEEP);
		trackpoint_in_suspend = 1;
	}

	/* Clear the data in queue and the counter */
	memset(queue_data, 0, ARRAY_SIZE(queue_data));
	data_count = 0;
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, ps2_suspend, HOOK_PRIO_DEFAULT);

/* Called on AP S0ix -> S0 transition */
static void ps2_resume(void)
{
	uint8_t trackpoint_id;

	ps2_enable_channel(PRIMUS_PS2_CH, 1, get_ps2_data);
	trackpoint_id = get_trackpoint_id();
	ps2_enable_channel(PRIMUS_PS2_CH, 1, send_aux_data_to_host_interrupt);
	/*
	 * For Synaptics trackpoint, EC need to send command to it again.
	 * For Elan trackpoint, we just need to touch trackpoint and it wake.
	 */
	if (trackpoint_id == TP_VARIANT_SYNAPTICS) {
		send_command_to_trackpoint(TP_TOGGLE_SOURCE_TAG,
					   TP_TOGGLE_SNAPTICS_SLEEP);
		trackpoint_in_suspend = 0;
	}

	/* Clear the data in queue and the counter */
	memset(queue_data, 0, ARRAY_SIZE(queue_data));
	data_count = 0;
}
DECLARE_HOOK(HOOK_CHIPSET_RESUME, ps2_resume, HOOK_PRIO_DEFAULT);
