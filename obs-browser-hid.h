/*
 * obs-browser-hid - shared API between the vendor handlers and the UI.
 *
 * The core (obs-browser-hid.c) resolves the target source and emits the
 * input events; the dock/settings UI (obs-browser-hid-ui.cpp) calls the same
 * functions directly, so both entry points behave identically.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What part of a key/mouse interaction to emit. */
enum obs_browser_hid_mode {
	OBS_BROWSER_HID_BOTH = 0,  /* press + release */
	OBS_BROWSER_HID_PRESS = 1, /* press only */
	OBS_BROWSER_HID_RELEASE = 2
};

/**
 * All send functions resolve `source_name` exactly like the vendor requests:
 * literal source name, or the virtual names "current" / "interact" (the
 * UUID is only used as a fallback). On failure they return false and write a
 * human-readable message into `err` (may be NULL).
 */
bool obs_browser_hid_send_key(const char *source_name, const char *source_uuid,
			      const char *key, const char *modifiers, int mode,
			      const char *text_override, char *err, size_t err_size);

bool obs_browser_hid_send_text(const char *source_name, const char *source_uuid,
			       const char *text, char *err, size_t err_size);

bool obs_browser_hid_send_mouse_click(const char *source_name, const char *source_uuid,
				      long long x, long long y, const char *button,
				      long long click_count, const char *modifiers,
				      int mode, bool no_move, char *err, size_t err_size);

/** Creates the "Browser HID" dock. No-op when there is no Qt frontend. */
void obs_browser_hid_ui_init(void);

#ifdef __cplusplus
}
#endif
