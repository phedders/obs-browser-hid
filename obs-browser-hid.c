/*
 * obs-browser-hid
 *
 * An OBS plugin that lets you send keyboard input, text and mouse clicks to a
 * *named browser source* through obs-websocket.
 *
 * How it works:
 *   - obs-websocket (v5.x+, bundled with OBS) has a "vendor" API which lets
 *     third-party plugins register extra `CallVendorRequest` request types.
 *   - libobs already has `obs_source_send_key_click()`, and the browser source
 *     implementation (obs-browser) routes that to `CEF SendKeyEvent()`, i.e.
 *     real keyboard events inside the browser source's web contents.
 *   - This plugin bridges the two: it registers the vendor "obs-browser-hid"
 *     with three requests, `SendKey`, `SendText` and `SendMouseClick`.
 *
 * Registering the vendor happens from `obs_module_post_load()`, because
 * obs-websocket might load after this plugin and only becomes reachable once
 * all modules are loaded (this is also what the obs-websocket API header
 * requires).
 *
 * Modifier/key encoding:
 *   The browser source on Linux consumes `obs_key_event.native_vkey` as an
 *   X11 keysym (obs-browser maps it to a Chromium windows key code) and
 *   `native_modifiers` as an X11 core protocol modifiers bitmask (same format
 *   QKeyEvent::nativeModifiers() yields on X11 - the bit layout obs-browser
 *   intentionally forwards into CEF). This plugin generates both directly via
 *   libxkbcommon.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-module.h>
#include <obs-interaction.h>
#include <obs-frontend-api.h>

#include "obs-websocket-api.h"
#include "obs-browser-hid.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#ifdef __linux__
#include <xkbcommon/xkbcommon.h>
#endif

OBS_DECLARE_MODULE()

#define VENDOR_NAME         "obs-browser-hid"
#define REQUEST_TYPE_SEND_KEY  "SendKey"
#define REQUEST_TYPE_SEND_TEXT "SendText"
#define REQUEST_TYPE_SEND_MOUSE_CLICK "SendMouseClick"

#define BROWSER_SOURCE_ID "browser_source"

/* Input limits. All of these fields come straight from untrusted websocket
 * request data (obs-websocket only authenticates the connection, it does not
 * sanitize vendor request payloads), so every one of them is clamped before
 * use. */
#define BKEY_NAME_MAX     128   /* key name (xkb keysym name) length cap */
#define BSPEC_MAX         255   /* modifiers string cap */
#define BTEXT_MAX         4096  /* SendText UTF-8 byte cap */
#define BMOUSE_COORD_MAX  1000000L /* clamp for x/y around 8K canvases */
#define BCOUNTCLICK_MAX   16     /* mouse clickCount cap */
#define BSOURCE_NAME_MAX  1024   /* source name lookup cap */

#define SAFE_STR(s) ((const char *)(s) ? (const char *)(s) : "")

static obs_websocket_vendor ws_vendor = NULL;

/*
 * ---------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------
 */

/** Formats `fmt` into buf, replacing control characters with '?' (the
 *  messages can echo untrusted request fields; OBS log-injection guard). */
static void format_err(char *buf, size_t size, const char *fmt, va_list args)
{
	vsnprintf(buf, size, fmt, args);
	for (char *c = buf; *c; c++) {
		unsigned char u = (unsigned char)*c;
		if (u < 0x20 || u == 0x7f)
			*c = '?';
	}
}

/** Core-side error: write into the caller's buffer and log it. */
static void set_err(char *err, size_t err_size, const char *fmt, ...)
{
	char buf[512];
	va_list args;

	va_start(args, fmt);
	format_err(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (err && err_size)
		snprintf(err, err_size, "%s", buf);
	blog(LOG_WARNING, "[obs-browser-hid] %s", buf);
}

/** Vendor-side error: goes into `responseData.error` and the log. */
static void set_error(obs_data_t *response_data, const char *fmt, ...)
{
	char buf[512];
	va_list args;

	va_start(args, fmt);
	format_err(buf, sizeof(buf), fmt, args);
	va_end(args);

	obs_data_set_string(response_data, "error", buf);
	blog(LOG_WARNING, "[obs-browser-hid] %s", buf);
}

/* ---------------------------------------------------------------------
 * Virtual source names
 * ---------------------------------------------------------------------
 *
 *   "current"  - the first browser source found (recursively) inside the
 *                scene that is currently in the program output (in Studio
 *                Mode the program scene; otherwise the current scene).
 *   "interact" - the browser source that currently has an OBS "interact"
 *                window open (resolved via the frontend's interact dialog).
 */

/** Declared/implemented in obs-browser-hid-interact.cpp: returns a bfree()-able
 *  strdup of the browser-source name currently shown in an open "interact"
 *  window, or NULL. Must only be called from the OBS (UI) process. */
extern char *obs_browser_hid_interact_source_name(void);

static bool is_browser_source_id(obs_source_t *src)
{
	return strcmp(obs_source_get_unversioned_id(src), BROWSER_SOURCE_ID) == 0;
}

static bool scene_iter_find_browser(obs_scene_t *scene, obs_sceneitem_t *item, void *ctx)
{
	UNUSED_PARAMETER(scene);
	obs_source_t **res = ctx;

	obs_source_t *item_src = obs_sceneitem_get_source(item);
	if (!item_src)
		return true;

	if (is_browser_source_id(item_src)) {
		*res = obs_source_get_ref(item_src);
		return false; /* stop enumerating */
	}

	/* recurse into nested scenes and groups */
	obs_scene_t *sub = NULL;
	if (obs_source_is_scene(item_src)) {
		sub = obs_scene_from_source(item_src);
	} else if (obs_sceneitem_is_group(item)) {
		sub = obs_sceneitem_group_get_scene(item);
	}
	if (sub)
		obs_scene_enum_items(sub, scene_iter_find_browser, ctx);

	return *res == NULL; /* stop as soon as a match has been found */
}

static obs_source_t *resolve_current_browser_source(void)
{
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	if (!scene_src)
		return NULL;

	obs_source_t *res = NULL;
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (scene)
		obs_scene_enum_items(scene, scene_iter_find_browser, &res);

	obs_source_release(scene_src);
	return res;
}

static obs_source_t *resolve_interact_browser_source(void)
{
	char *name = obs_browser_hid_interact_source_name();
	if (!name)
		return NULL;

	obs_source_t *src = obs_get_source_by_name(name);
	bfree(name);
	return src;
}

/** Copies a string into buf for logging, replacing control characters with
 *  '?' (log-injection guard; request data is untrusted websocket input). */
static void copy_logstr(char *out, size_t size, const char *s)
{
	size_t j = 0;
	if (s) {
		for (size_t i = 0; s[i]; i++) {
			unsigned char c = (unsigned char)s[i];
			if (c >= 0x20 && c != 0x7f) {
				if (j >= size - 1)
					break; /* truncate, stays NUL-terminated */
				out[j++] = (char)c;
			}
		}
	}
	out[j] = 0;
}

/*
 * ---------------------------------------------------------------------
 * UTF-8 <-> UTF-32
 * ---------------------------------------------------------------------
 */

/** Decodes the UTF-8 sequence at *p (<= end) to a codepoint, advances *p. */
static uint32_t utf8_decode(const char **p, const char *end)
{
	uint32_t c = 0;
	unsigned char b = *(*p)++;

	if (b < 0x80)
		return b;

	if ((b & 0xE0) == 0xC0) {
		c = b & 0x1F;
	} else if ((b & 0xF0) == 0xE0) {
		c = b & 0x0F;
	} else if ((b & 0xF8) == 0xF0) {
		c = b & 0x07;
	} else {
		return 0xFFFD; /* invalid */
	}

	for (int extra = (b >= 0xF0) ? 3 : (b >= 0xE0) ? 2 : 1; extra > 0; extra--) {
		if (*p >= end || (**p & 0xC0) != 0x80)
			return 0xFFFD;
		c = (c << 6) | (**p & 0x3F);
		(*p)++;
	}

	return c;
}

/** Encodes a codepoint to UTF-8 in buf (at least 5 bytes), NUL-terminates. */
static size_t utf8_encode(uint32_t cp, char *buf)
{
	if (cp < 0x80) {
		buf[0] = (char)cp;
		buf[1] = 0;
		return 1;
	}
	if (cp < 0x800) {
		buf[0] = 0xC0 | (cp >> 6);
		buf[1] = 0x80 | (cp & 0x3F);
		buf[2] = 0;
		return 2;
	}
	if (cp < 0x10000) {
		buf[0] = 0xE0 | (cp >> 12);
		buf[1] = 0x80 | ((cp >> 6) & 0x3F);
		buf[2] = 0x80 | (cp & 0x3F);
		buf[3] = 0;
		return 3;
	}
	buf[0] = 0xF0 | (cp >> 18);
	buf[1] = 0x80 | ((cp >> 12) & 0x3F);
	buf[2] = 0x80 | ((cp >> 6) & 0x3F);
	buf[3] = 0x80 | (cp & 0x3F);
	buf[4] = 0;
	return 4;
}

/*
 * ---------------------------------------------------------------------
 * Key event construction
 * ---------------------------------------------------------------------
 *
 * We fill struct obs_key_event the same way the OBS frontend does when a real
 * key press reaches a source via the "interact" window:
 *
 *   - `modifiers`      : INTERACT_* flags (obs-interaction.h), used by
 *                        obs-browser on Windows/macOS.
 *   - `native_vkey`    : Linux - X11 keysym; Windows - virtual key code;
 *                        macOS - NSEvent keyCode (hardware key code).
 *   - `native_modifiers`:
 *                        Linux - X11 core protocol modifier mask (Shift=0x01,
 *                        Lock=0x02, Control=0x04, Mod1/Alt=0x08, Mod2/NumLock=
 *                        0x10, Mod3=0x20, Mod4/Super=0x40, Mod5/AltGr=0x80).
 *                        This is what obs-browser forwards into CEF on Linux.
 *   - `text`           : UTF-8 character the key produces (drives the
 *                        KEYEVENT_CHAR injection; empty for modifier combos).
 */

struct key_event_info {
	struct obs_key_event event;
	char text_buf[8];
	bool valid;
};

struct modifier_mask {
	uint32_t native;    /* X11 core modifiers (Linux) */
	uint32_t interact;  /* INTERACT_* flags (all platforms) */
};

static void modifier_mask_init(struct modifier_mask *m)
{
	m->native = 0;
	m->interact = 0;
}

/** Parses "ctrl+shift+alt" (+"ctrl", "alt", "shift", "super"/"meta"/"win",
 *  "altgr", "numlock", "capslock"), accumulating into an existing mask. */
static void modifier_mask_parse(struct modifier_mask *m, const char *spec)
{
	if (!spec || !*spec)
		return;

	char buf[256];
	size_t len = strlen(spec);
	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	memcpy(buf, spec, len);
	buf[len] = 0;

	/* strtok_r, not strtok: vendor requests can arrive on more than one
	 * websocket thread and strtok's static state is not reentrant. */
	char *save = NULL;
	for (char *tok = strtok_r(buf, "+, ", &save); tok; tok = strtok_r(NULL, "+, ", &save)) {
		for (char *c = tok; *c; c++)
			*c = (*c >= 'A' && *c <= 'Z') ? (*c + 32) : *c;
		if (strcmp(tok, "shift") == 0) {
			m->native |= 1 << 0;
			m->interact |= INTERACT_SHIFT_KEY;
		} else if (strcmp(tok, "ctrl") == 0 || strcmp(tok, "control") == 0) {
			m->native |= 1 << 2;
			m->interact |= INTERACT_CONTROL_KEY;
		} else if (strcmp(tok, "alt") == 0) {
			m->native |= 1 << 3; /* Mod1 */
			m->interact |= INTERACT_ALT_KEY;
		} else if (strcmp(tok, "super") == 0 || strcmp(tok, "meta") == 0 ||
			   strcmp(tok, "win") == 0) {
			m->native |= 1 << 6; /* Mod4 */
			m->interact |= INTERACT_COMMAND_KEY;
		} else if (strcmp(tok, "altgr") == 0) {
			m->native |= 1 << 7; /* Mod5 */ 
			m->interact |= INTERACT_ALT_KEY;
		} else if (strcmp(tok, "numlock") == 0) {
			m->native |= 1 << 4; /* Mod2 */
			m->interact |= INTERACT_NUMLOCK_KEY;
		} else if (strcmp(tok, "capslock") == 0) {
			m->native |= 1 << 1; /* Lock */
			m->interact |= INTERACT_CAPS_KEY;
		} else {
			char tok_log[BSPEC_MAX + 1];
			copy_logstr(tok_log, sizeof(tok_log), tok);
			blog(LOG_WARNING, "[obs-browser-hid] Unknown modifier token: %s", tok_log);
		}
	}
}

/** Resolves the user-facing key name into native fields + the text it types. */
static bool resolve_key(const char *key_name, struct modifier_mask *mods,
			const char *text_override, struct key_event_info *out)
{
	struct obs_key_event *ev = &out->event;
	out->valid = false;
	out->text_buf[0] = 0;

	memset(ev, 0, sizeof(*ev));

	if (!key_name || !*key_name) {
		blog(LOG_WARNING, "[obs-browser-hid] No key given");
		return false;
	}

	if (strlen(key_name) > BKEY_NAME_MAX) {
		blog(LOG_WARNING, "[obs-browser-hid] Key name too long (%zu bytes, max %u)",
		     strlen(key_name), BKEY_NAME_MAX);
		return false;
	}

#ifdef __linux__
	xkb_keysym_t keysym = xkb_keysym_from_name(key_name, XKB_KEYSYM_CASE_INSENSITIVE);

	/* Fall back: a single-char key name that is not a known keysym name
	 * (e.g. "!") is treated as the character itself. */
	if (!keysym && strlen(key_name) == 1) {
		uint32_t cp = (unsigned char)key_name[0];
		keysym = xkb_utf32_to_keysym(cp);
	}

	if (!keysym) {
		char key_log[BKEY_NAME_MAX + 1];
		copy_logstr(key_log, sizeof(key_log), key_name);
		blog(LOG_WARNING, "[obs-browser-hid] Unknown key name: %s", key_log);
		return false;
	}

	/* The character this keysym produces when typed. */
	char typed[8];
	int utf8len = xkb_keysym_to_utf8(keysym, typed, sizeof(typed));
	size_t text_len = utf8len > 0 ? (size_t)utf8len : 0;

	/* Extra "shift" when emitting an upper-case letter that wasn't
	 * requested with an explicit shift modifier (matches real hardware,
	 * where Shift+A raises keysym 'A' with the Shift bit set). */
	const char *t = text_len ? typed : "";
	if (t[0] >= 'A' && t[0] <= 'Z' && !(mods->native & 1)) {
		mods->native |= 1 << 0;
		mods->interact |= INTERACT_SHIFT_KEY;
	}

	/* With ctrl/alt/super/altgr held, browsers treat the event as a
	 * shortcut; injecting a character then only gets in the way
	 * (real key events with these modifiers carry no text). */
	bool suppress_text = (mods->native & ((1 << 2) | (1 << 3) | (1 << 6) | (1 << 7)));

	if (text_override && *text_override) {
		/* Take the first Unicode character of the override text. */
		const char *p = text_override;
		uint32_t cp = utf8_decode(&p, text_override + strlen(text_override));
		utf8_encode(cp, out->text_buf);
	} else if (!suppress_text && text_len) {
		size_t n = text_len < sizeof(out->text_buf) - 1 ? text_len : sizeof(out->text_buf) - 1;
		memcpy(out->text_buf, typed, n);
		out->text_buf[sizeof(out->text_buf) - 1] = 0;
	}

	ev->native_vkey = keysym;
	ev->native_modifiers = mods->native;
	ev->native_scancode = 0;
#else
	UNUSED_PARAMETER(mods);
	UNUSED_PARAMETER(text_override);
	blog(LOG_WARNING,
	     "[obs-browser-hid] Key-name mapping is only implemented on Linux; "
	     "SendKey is unavailable in this build (SendText/SendMouseClick "
	     "still work).");
	return false;
#endif

	ev->modifiers = mods->interact;
	ev->text = out->text_buf;
	out->valid = true;
	return true;
}

/*
 * ---------------------------------------------------------------------
 * Vendor request implementations
 * ---------------------------------------------------------------------
 */

/** Common plumbing for all send paths (vendor + UI): locate + validate the
 *  browser source. Besides literal source names, two virtual names are
 *  understood:
 *    "current"  - first browser source in the active program scene
 *    "interact" - the browser source with an open interact window        */
static obs_source_t *resolve_target_source(const char *name, const char *uuid,
					   char *err, size_t err_size)
{
	obs_source_t *src = NULL;

	if (name && *name) {
		if (strlen(name) > BSOURCE_NAME_MAX) {
			set_err(err, err_size, "sourceName is too long (max %u bytes)", BSOURCE_NAME_MAX);
			return NULL;
		}
		if (strcmp(name, "current") == 0) {
			src = resolve_current_browser_source();
			if (!src) {
				set_err(err, err_size, "'current': no browser source found in the active program scene");
				return NULL;
			}
		} else if (strcmp(name, "interact") == 0) {
			src = resolve_interact_browser_source();
			if (!src) {
				set_err(err, err_size, "'interact': no interact window is open (or its source is not a browser source)");
				return NULL;
			}
		} else {
			src = obs_get_source_by_name(name);
		}
	}
	if (!src && uuid && *uuid)
		src = obs_get_source_by_uuid(uuid);

	if (!src) {
		set_err(err, err_size, "No source found with the given sourceName/sourceUuid");
		return NULL;
	}

	const char *id = obs_source_get_unversioned_id(src);
	if (!id || strcmp(id, BROWSER_SOURCE_ID) != 0) {
		set_err(err, err_size, "Source '%s' is not a browser source (id=%s)",
			SAFE_STR(obs_source_get_name(src)), SAFE_STR(id));
		obs_source_release(src);
		return NULL;
	}

	return src; /* caller owns (release) */
}

bool obs_browser_hid_open_interact(const char *source_name, const char *source_uuid,
				   char *err, size_t err_size)
{
	obs_source_t *src = resolve_target_source(source_name, source_uuid, err, err_size);
	if (!src)
		return false;

	/* The frontend API marshals this to OBSBasic::OpenInteraction, which
	 * creates (or re-creates) and raises the interact window. */
	obs_frontend_open_source_interaction(src);
	obs_source_release(src);
	return true;
}

/*
 * ---------------------------------------------------------------------
 * Input dispatch (shared by the websocket vendor and the dock UI)
 * ---------------------------------------------------------------------
 */

bool obs_browser_hid_send_key(const char *source_name, const char *source_uuid,
			      const char *key, const char *modifiers, int mode,
			      const char *text_override, char *err, size_t err_size)
{
	obs_source_t *src = resolve_target_source(source_name, source_uuid, err, err_size);
	if (!src)
		return false;

	struct key_event_info info;
	struct modifier_mask mods;
	modifier_mask_init(&mods);
	modifier_mask_parse(&mods, modifiers);

	if (!resolve_key(key, &mods, text_override, &info)) {
		set_err(err, err_size, "Could not resolve key '%s'", SAFE_STR(key));
		obs_source_release(src);
		return false;
	}

	if (mode != OBS_BROWSER_HID_RELEASE)
		obs_source_send_key_click(src, &info.event, false);
	if (mode != OBS_BROWSER_HID_PRESS)
		obs_source_send_key_click(src, &info.event, true);

	char key_log[BKEY_NAME_MAX + 1], mods_log[BSPEC_MAX + 1], src_log[256];
	copy_logstr(key_log, sizeof(key_log), key);
	copy_logstr(mods_log, sizeof(mods_log), modifiers);
	copy_logstr(src_log, sizeof(src_log), obs_source_get_name(src));
	blog(LOG_INFO, "[obs-browser-hid] SendKey '%s' (modifiers '%s') to '%s'", key_log, mods_log, src_log);

	obs_source_release(src);
	return true;
}

/**
 * `SendKey` request fields:
 *   sourceName  : string, required - browser source name
 *   sourceUuid  : string, optional - fallback lookup if no `sourceName`
 *   key         : string, required - key name; on Linux an XKB keysym name,
 *                 e.g. "a", "Return", "Escape", "F5", "Left", "plus", "space"
 *   modifiers   : string, optional - "+"-separated modifier list:
 *                 ctrl / alt / shift / super|meta|win / altgr / numlock /
 *                 capslock, e.g. "ctrl+shift"
 *   text        : string, optional - single Unicode character to inject as
 *                 the character event instead of the derived one
 *   keyUp       : bool, optional - true: send only a key release,
 *                 false: send only a key press, omit: press + release
 *
 * Response fields: `error` requires attention (obs-websocket always reports
 * vendor requests as "success"; the error text is in responseData).
 */
static void send_key_request(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);

	bool have_key_up = obs_data_has_user_value(request_data, "keyUp");
	bool key_up = obs_data_get_bool(request_data, "keyUp");
	int mode = !have_key_up ? OBS_BROWSER_HID_BOTH
				: (key_up ? OBS_BROWSER_HID_RELEASE : OBS_BROWSER_HID_PRESS);

	char err[512];
	if (!obs_browser_hid_send_key(obs_data_get_string(request_data, "sourceName"),
				      obs_data_get_string(request_data, "sourceUuid"),
				      obs_data_get_string(request_data, "key"),
				      obs_data_get_string(request_data, "modifiers"), mode,
				      obs_data_get_string(request_data, "text"), err, sizeof(err)))
		set_error(response_data, "%s", err);
}

/**
 * `SendText` request fields:
 *   sourceName  : string, required - browser source name
 *   sourceUuid  : string, optional - fallback lookup
 *   text        : string, required - the text to type (UTF-8)
 *
 * Types the entire string, character by character (press + release each).
 */
bool obs_browser_hid_send_text(const char *source_name, const char *source_uuid,
			       const char *text, char *err, size_t err_size)
{
	obs_source_t *src = resolve_target_source(source_name, source_uuid, err, err_size);
	if (!src)
		return false;

	if (!text) {
		set_err(err, err_size, "Field 'text' is required");
		obs_source_release(src);
		return false;
	}
	if (strlen(text) > BTEXT_MAX) {
		set_err(err, err_size, "text is too long (%zu bytes, max %u)", strlen(text), BTEXT_MAX);
		obs_source_release(src);
		return false;
	}

	struct key_event_info info;
	struct modifier_mask mods;
	modifier_mask_init(&mods);

	const char *p = text;
	const char *end = text + strlen(text);
	size_t count = 0;

	while (p < end) {
		uint32_t cp = utf8_decode(&p, end);
		utf8_encode(cp, info.text_buf);

		memset(&info.event, 0, sizeof(info.event));
		info.event.text = info.text_buf;
		info.event.modifiers = 0;

#ifdef __linux__
		xkb_keysym_t keysym = xkb_utf32_to_keysym(cp);
		info.event.native_vkey = keysym;
		info.event.native_modifiers = 0;
#endif

		obs_source_send_key_click(src, &info.event, false);
		obs_source_send_key_click(src, &info.event, true);

		count++;
	}

	char src_log[256];
	copy_logstr(src_log, sizeof(src_log), obs_source_get_name(src));
	blog(LOG_INFO, "[obs-browser-hid] SendText %zu chars to '%s'", count, src_log);

	obs_source_release(src);
	return true;
}

static void send_text_request(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);

	char err[512];
	if (!obs_browser_hid_send_text(obs_data_get_string(request_data, "sourceName"),
				       obs_data_get_string(request_data, "sourceUuid"),
				       obs_data_get_string(request_data, "text"), err, sizeof(err)))
		set_error(response_data, "%s", err);
}

/**
 * `SendMouseClick` request fields:
 *   sourceName  : string, required - browser source name
 *   sourceUuid  : string, optional - fallback lookup
 *   x, y        : int, required - click position in the source's own canvas,
 *                 (0,0) = top-left of the source (the same coordinate space
 *                 the "interact" window uses)
 *   button      : string, optional - "left" (default), "middle", "right"
 *                 ("l"/"m"/"r" work too)
 *   clickCount  : int, optional - 1 (default); 2 = double click etc.
 *   modifiers   : string, optional - "shift"/"ctrl"/"alt"(+"..."), same as
 *                 SendKey  (e.g. "shift+click" -> range/end selection)
 *   mouseUp     : bool, optional - true: send only button release;
 *                 false: send only button press; omit: press + release
 *   noMove      : bool, optional - true: skip the cursor-move event that
 *                 is normally sent to the position before clicking
 */
bool obs_browser_hid_send_mouse_click(const char *source_name, const char *source_uuid,
				      long long x, long long y, const char *button_str,
				      long long click_count, const char *modifiers,
				      int mode, bool no_move, char *err, size_t err_size)
{
	obs_source_t *src = resolve_target_source(source_name, source_uuid, err, err_size);
	if (!src)
		return false;

	struct obs_mouse_event ev;
	memset(&ev, 0, sizeof(ev));

	/* Coordinates may come from untrusted int64s; clamp to a sane window
	 * (8K canvases are far below this) before casting to int32. */
	if (x < -BMOUSE_COORD_MAX)
		x = -BMOUSE_COORD_MAX;
	else if (x > BMOUSE_COORD_MAX)
		x = BMOUSE_COORD_MAX;
	if (y < -BMOUSE_COORD_MAX)
		y = -BMOUSE_COORD_MAX;
	else if (y > BMOUSE_COORD_MAX)
		y = BMOUSE_COORD_MAX;
	ev.x = (int32_t)x;
	ev.y = (int32_t)y;

	const char *button_log = button_str && *button_str ? button_str : "left";
	int32_t button;
	if (!button_str || !*button_str || strcmp(button_str, "left") == 0 || strcmp(button_str, "l") == 0) {
		button = MOUSE_LEFT;
	} else if (strcmp(button_str, "middle") == 0 || strcmp(button_str, "m") == 0) {
		button = MOUSE_MIDDLE;
	} else if (strcmp(button_str, "right") == 0 || strcmp(button_str, "r") == 0) {
		button = MOUSE_RIGHT;
	} else {
		set_err(err, err_size, "Unknown button '%s' (left|middle|right)", button_str);
		obs_source_release(src);
		return false;
	}

	if (click_count <= 0)
		click_count = 1;
	else if (click_count > BCOUNTCLICK_MAX)
		click_count = BCOUNTCLICK_MAX;
	uint32_t clicks = (uint32_t)click_count;

	struct modifier_mask mods;
	modifier_mask_init(&mods);
	modifier_mask_parse(&mods, modifiers);
	/* Mouse events carry the INTERACT_* flags on all platforms. */
	ev.modifiers = mods.interact;

	if (!no_move)
		obs_source_send_mouse_move(src, &ev, false);
	if (mode != OBS_BROWSER_HID_RELEASE)
		obs_source_send_mouse_click(src, &ev, button, false, clicks);
	if (mode != OBS_BROWSER_HID_PRESS)
		obs_source_send_mouse_click(src, &ev, button, true, clicks);

	char src_log[256];
	copy_logstr(src_log, sizeof(src_log), obs_source_get_name(src));
	blog(LOG_INFO, "[obs-browser-hid] SendMouseClick %s%s at (%ld,%d) to '%s'", button_log,
	     clicks > 1 ? " (multi)" : "", (long)ev.x, ev.y, src_log);

	obs_source_release(src);
	return true;
}

static void send_mouse_click_request(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(priv);

	bool have_x = obs_data_has_user_value(request_data, "x");
	bool have_y = obs_data_has_user_value(request_data, "y");
	if (!have_x || !have_y) {
		set_error(response_data, "Fields 'x' and 'y' are required");
		return;
	}

	bool have_mouse_up = obs_data_has_user_value(request_data, "mouseUp");
	bool mouse_up = obs_data_get_bool(request_data, "mouseUp");
	int mode = !have_mouse_up ? OBS_BROWSER_HID_BOTH
				  : (mouse_up ? OBS_BROWSER_HID_RELEASE : OBS_BROWSER_HID_PRESS);
	bool no_move = obs_data_has_user_value(request_data, "noMove") &&
		       obs_data_get_bool(request_data, "noMove");

	char err[512];
	if (!obs_browser_hid_send_mouse_click(
		    obs_data_get_string(request_data, "sourceName"),
		    obs_data_get_string(request_data, "sourceUuid"),
		    obs_data_get_int(request_data, "x"), obs_data_get_int(request_data, "y"),
		    obs_data_get_string(request_data, "button"),
		    obs_data_get_int(request_data, "clickCount"),
		    obs_data_get_string(request_data, "modifiers"), mode, no_move, err, sizeof(err)))
		set_error(response_data, "%s", err);
}

/*
 * ---------------------------------------------------------------------
 * Module lifecycle
 * ---------------------------------------------------------------------
 */

bool obs_module_load(void)
{
	blog(LOG_INFO, "[obs-browser-hid] loaded");
	return true;
}

/** Runs once, after ALL modules are loaded - this is the only safe place to
 *  register an obs-websocket vendor. */
void obs_module_post_load(void)
{
	/* The dock does not depend on obs-websocket; register it first so the
	 * direct (non-websocket) path is usable even if the vendor is not. */
	obs_browser_hid_ui_init();

	ws_vendor = obs_websocket_register_vendor(VENDOR_NAME);
	if (!ws_vendor) {
		blog(LOG_WARNING,
		     "[obs-browser-hid] Could not register vendor '%s'. "
		     "obs-websocket not present/disabled? Requests will be unavailable.",
		     VENDOR_NAME);
		return;
	}

	if (!obs_websocket_vendor_register_request(ws_vendor, REQUEST_TYPE_SEND_KEY, send_key_request, NULL))
		blog(LOG_ERROR, "[obs-browser-hid] Failed to register request '%s'", REQUEST_TYPE_SEND_KEY);
	if (!obs_websocket_vendor_register_request(ws_vendor, REQUEST_TYPE_SEND_TEXT, send_text_request, NULL))
		blog(LOG_ERROR, "[obs-browser-hid] Failed to register request '%s'", REQUEST_TYPE_SEND_TEXT);
	if (!obs_websocket_vendor_register_request(ws_vendor, REQUEST_TYPE_SEND_MOUSE_CLICK,
						   send_mouse_click_request, NULL))
		blog(LOG_ERROR, "[obs-browser-hid] Failed to register request '%s'", REQUEST_TYPE_SEND_MOUSE_CLICK);

	blog(LOG_INFO, "[obs-browser-hid] Registered obs-websocket requests '%s', '%s' and '%s'",
	     REQUEST_TYPE_SEND_KEY, REQUEST_TYPE_SEND_TEXT, REQUEST_TYPE_SEND_MOUSE_CLICK);
}

void obs_module_unload(void)
{
	/* nop: obs-websocket tears its vendors down itself */
}
