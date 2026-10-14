/* Copyright 2021 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "ec_commands.h"
#include "keyboard_8042.h"
#include "keyboard_8042_sharedlib.h"
#include "keyboard_protocol.h"
#include "keyboard_scan.h"
#include "timer.h"

/* Keyboard scan setting */
__override struct keyboard_scan_config keyscan_config = {
	.output_settle_us = 50,
	.debounce_down_us = 9 * MSEC,
	.debounce_up_us = 30 * MSEC,
	.scan_period_us = 3 * MSEC,
	.stable_scan_period_us = 9 * MSEC,
	.min_post_scan_delay_us = 1000,
	.poll_timeout_us = 100 * MSEC,
	.actual_key_mask = {
		0x1c, 0xff, 0xff, 0xff, 0xff, 0xf5, 0xff,
		0xa4, 0xff, 0xfe, 0x55, 0xfb, 0xca  /* full set */
	},
};

static const struct ec_response_keybd_config primus_kb = {
	.num_top_row_keys = 14,
	.action_keys = {
		TK_BACK,		/* T1 */
		TK_REFRESH,		/* T2 */
		TK_FULLSCREEN,		/* T3 */
		TK_OVERVIEW,		/* T4 */
		TK_SNAPSHOT,		/* T5 */
		TK_BRIGHTNESS_DOWN,	/* T6 */
		TK_BRIGHTNESS_UP,	/* T7 */
		TK_PREV_TRACK,		/* T8 */
		TK_PLAY_PAUSE,		/* T9 */
		TK_NEXT_TRACK,		/* T10 */
		TK_MICMUTE,		/* T11 */
		TK_VOL_MUTE,		/* T12 */
		TK_VOL_DOWN,		/* T13 */
		TK_VOL_UP,		/* T14 */
	},
	.capabilities = KEYBD_CAP_SCRNLOCK_KEY,
};

/* TK_REFRESH is always T2 above, vivaldi_keys are not overridden. */
BUILD_ASSERT_REFRESH_RC(3, 2);

BUILD_ASSERT(IS_ENABLED(CONFIG_KEYBOARD_VIVALDI));
__override const struct ec_response_keybd_config *
board_vivaldi_keybd_config(void)
{
	return &primus_kb;
}

/*
 * The layout the C14's owner asked for, on top of the Vivaldi top row:
 *
 * - Top row: the mode chosen in firmware setup (KEYBD_TOP_ROW) is the
 *   default; held Shift gives the other mode, with Shift released around
 *   the key so the host sees a plain action key (or F-key). Each key's
 *   release sends what its press sent, whatever Shift did in between.
 * - Search: alone, a tap of Caps Lock; held with another key, Super. The
 *   choice is made by whether another key goes down before Search comes
 *   up, not by a timer, so fast typing cannot turn Super into Caps.
 * - Search+Backspace: Delete, with no Super, since the keyboard has no
 *   Delete key. Every other Search chord stays Super (window tiling on
 *   Super+arrows is in constant use). Backspace's code is rewritten rather
 *   than a Delete injected, so the 8042 typematic repeats Delete as it
 *   would a real key; a Super already sent for an earlier chord is
 *   released first, and sent again by the next Search chord.
 */
#define SCANCODE_LEFT_SHIFT_SET2 0x0012
#define SCANCODE_RIGHT_SHIFT_SET2 0x0059
#define SCANCODE_BACKSPACE_SET2 0x0066
#define SCANCODE_DELETE_SET2 0xe071

static bool left_shift, right_shift;
static uint16_t top_row_sent[MAX_TOP_ROW_KEYS];
static bool search_down, search_chorded, super_sent, delete_down;

static void shifts_to_host(int8_t pressed)
{
	if (left_shift)
		keyboard_inject_scancode(SCANCODE_LEFT_SHIFT_SET2, pressed);
	if (right_shift)
		keyboard_inject_scancode(SCANCODE_RIGHT_SHIFT_SET2, pressed);
}

__override enum ec_error_list keyboard_scancode_callback(uint16_t *make_code,
							 int8_t pressed)
{
	uint16_t code = *make_code;
	int i;

	if (code == SCANCODE_LEFT_WIN) {
		if (pressed) {
			search_down = true;
			search_chorded = false;
			return EC_ERROR_UNIMPLEMENTED; /* held back */
		}
		search_down = false;
		if (super_sent) {
			super_sent = false;
			return EC_SUCCESS; /* the Super break */
		}
		if (search_chorded)
			return EC_ERROR_UNIMPLEMENTED; /* Delete only */
		keyboard_inject_scancode(SCANCODE_CAPSLOCK, 1);
		keyboard_inject_scancode(SCANCODE_CAPSLOCK, 0);
		return EC_ERROR_UNIMPLEMENTED;
	}

	if (code == SCANCODE_BACKSPACE_SET2 && (delete_down ||
						 (pressed && search_down))) {
		if (pressed) {
			search_chorded = true;
			if (super_sent) {
				keyboard_inject_scancode(SCANCODE_LEFT_WIN, 0);
				super_sent = false;
			}
		}
		delete_down = pressed;
		*make_code = SCANCODE_DELETE_SET2;
		return EC_SUCCESS;
	}

	if (pressed && search_down) {
		search_chorded = true;
		if (!super_sent) {
			keyboard_inject_scancode(SCANCODE_LEFT_WIN, 1);
			super_sent = true;
		}
	}

	if (code == SCANCODE_LEFT_SHIFT_SET2)
		left_shift = pressed;
	else if (code == SCANCODE_RIGHT_SHIFT_SET2)
		right_shift = pressed;

	i = vivaldi_top_row_index(code);
	if (i < 0) {
		/* A top-row key whose code a mode change moved: match by
		 * what its press sent. */
		for (i = 0; i < MAX_TOP_ROW_KEYS; i++)
			if (!pressed && top_row_sent[i] &&
			    vivaldi_other_scancode(i) == code)
				break;
		if (i == MAX_TOP_ROW_KEYS)
			return EC_SUCCESS;
	}

	if (pressed) {
		uint16_t other = vivaldi_other_scancode(i);

		if ((left_shift || right_shift) && other) {
			shifts_to_host(0);
			keyboard_inject_scancode(other, 1);
			shifts_to_host(1);
			top_row_sent[i] = other;
			return EC_ERROR_UNIMPLEMENTED;
		}
		top_row_sent[i] = code;
		return EC_SUCCESS;
	}

	if (top_row_sent[i] && top_row_sent[i] != code) {
		keyboard_inject_scancode(top_row_sent[i], 0);
		top_row_sent[i] = 0;
		return EC_ERROR_UNIMPLEMENTED;
	}
	top_row_sent[i] = 0;
	return EC_SUCCESS;
}
