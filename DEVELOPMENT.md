# Development notes

How `obs-browser-hid` (the OBS plugin) and `ws-obs` (the Rust CLI) were
built, what was decided along the way, and where to pick up next.

---

## 1. How this was developed

### Tooling

- **Agent harness**: [OpenCode](https://opencode.ai) (AI coding agent, shell +
  file tools).
- **Model**: the bulk of the initial implementation was produced by an OpenCode
  session running **GLM-5.3-Flash**; the input-hardening, code-audit and
  documentation pass ran on **`opencode/deepseek-v4.1-flash-free`**
  (DeepSeek V4.1 Flash Free). The exact model/variant of a session is shown in
  the OpenCode UI.
- **Target tree**: a local obs-studio checkout at `/home/prh/gits/obs-studio`,
  version 33.0.1/master, used both as the API reference for `libobs` /
  `libobs-frontend-api` / obs-browser internals and as the CMake link target.
- **Verification**: no OBS instance was available inside the agent environment,
  so the plugin was compile-verified against the real libobs headers and
  runtime-tested by the user in real OBS. The CLI was tested end-to-end against
  a fake obs-websocket v5 server (see `tests/`), including authentication.

### Naming

The project, plugin module and vendor were originally called
`obs-websocket-keys` / `obs-keypress`. Once mouse support made "keypress"
inaccurate, they were renamed to **`obs-browser-hid`** ("HID" = synthetic
human-interface-device input, the same shorthand tools like `uhid`/`ydotool`
use). Consequences of the rename:

- the module is now `obs-browser-hid.so` and the vendor is `obs-browser-hid`;
- `ws-obs` still accepts `obs-keypress` as a **deprecated alias** and maps it
  to the new vendor name (the plugin itself only registers the new one), so
  existing scripts keep working;
- when upgrading, delete the old `obs-websocket-keys.so` from the OBS module
  directory — otherwise OBS loads both modules and registers two vendors.

### Prompt history (summary)

1. **Initial goal** - "Build an OBS plugin plus a companion CLI so an external
   process can send keypresses to a *named browser source* over obs-websocket."
   Constraints: must work as an OBS plugin built against the user's own
   obs-studio checkout; the CLI should look like
   `./ws-obs <vendor> <args>`, e.g. `./ws-obs obs-browser-hid browsersource2 Return`.
2. **Mouse support** - "Mouse clicks need a selectable position and click type,
   also accessible from `ws-obs`." Led to `SendMouseClick` with `x`/`y`,
   `button`, `clickCount`, modifiers and partial press/release.
3. **Virtual sources** - "Make it possible to address `current` (first browser
   source in the active program scene) and `interact` (browser source with an
   open interact window) instead of literal names."
4. **Bug report** - "`interact` is not working - seems to send nothing now:
   `'interact': no interact window is open ...`". Root cause: the OBS interact
   dialog is created *parented* to the main window, and `QApplication::
   topLevelWidgets()` only lists parentless widgets, so the scan never saw it.
   Fixed by scanning `allWidgets()` and filtering with `isWindow()` +
   `inherits("OBSBasicInteraction")`.
5. **Hardening + documentation** - "Harden the `ws-obs` tool's inputs against
   buffer overflows; check the plugin for ways websocket data could cause
   errors or security risks; write markdown documentation about how this was
   developed, the prompt summary, the model used, and useful expansion notes."

### Working loop that proved effective

1. Read the exact API surface in the obs-studio tree (`obs_source_send_*`,
   `obs_key_event`, `obs-browser-source.cpp`, `OBSBasicInteraction.cpp`,
   obs-websocket vendor header) before writing code.
2. Write the plugin; build with the standalone CMake project against the
   existing obs-studio build tree; treat warnings as bugs (`-Wall -Wextra`).
3. Mirror the same requests in the Rust CLI.
4. Test the CLI against a scripted fake obs-websocket server: verify the auth
   handshake byte-for-byte, then assert on the JSON payloads the CLI emits.
5. Hand the rebuilt `.so` to a real OBS instance for the parts that cannot be
   simulated (CEF input delivery, Qt/frontend interaction).

---

## 2. Architecture

```
external process                                        Browser HID dock (Qt)
   │  ws-obs ...  (or any obs-websocket client)           │  quick-action buttons
   ▼                                                      ▼
obs-websocket  ── CallVendorRequest ──►  vendor handlers  │
                                         │                │
                                         └──────┬─────────┘
                                                ▼
                                 obs_browser_hid_send_key/_text/_mouse_click()
                                 (obs-browser-hid.h - shared dispatch)
                                                ▼
                               resolve source by name / UUID / "current" /
                               "interact"
                                                ▼
                              obs_source_send_key_click()
                              obs_source_send_mouse_move()
                              obs_source_send_mouse_click()
                                                ▼
                             obs-browser  →  CEF SendKeyEvent /
                                             SendMouseClickEvent
                                                ▼
                                  page DOM events (keydown, ...)
```

Both entry points go through the same `extern "C"` functions declared in
`obs-browser-hid.h`, so the dock and the websocket path cannot drift apart
(validation, clamping, logging, virtual source resolution and key mapping are
shared).

### Why vendor requests?

obs-websocket has no built-in request to send input to a specific source. Its
*vendor* API (`obs_websocket_register_vendor()` + request registration) is the
supported extension point: clients call the ordinary `CallVendorRequest` with
`vendorName: "obs-browser-hid"`, so no extra socket/port/auth layer is needed.

Registration happens in `obs_module_post_load()`, not `obs_module_load()`:
obs-websocket may load after this plugin, and the vendor API header explicitly
requires registration once all modules are loaded.

### Key event encoding (Linux)

`struct obs_key_event` is filled the way the OBS frontend fills it for real
keyboard input to an interact window:

- `native_vkey` = **X11 keysym** (from `libxkbcommon`:
  `xkb_keysym_from_name`, fallback `xkb_utf32_to_keysym` for single characters).
- `native_modifiers` = **X11 core modifier mask** (`Shift=0x01`, `Lock=0x02`,
  `Control=0x04`, `Mod1/Alt=0x08`, `Mod2=NumLock=0x10`, `Mod4/Super=0x40`,
  `Mod5/AltGr=0x80`). This is what obs-browser forwards to CEF on Linux.
- `modifiers` = `INTERACT_*` flags (`obs-interaction.h`), used by obs-browser
  on Windows/macOS; harmless-but-correct on Linux.
- `text` = UTF-8 character the key produces, which drives the character event.
  Suppressed when `ctrl`/`alt`/`super`/`altgr` is held (matching real browsers
  where such combos are shortcuts, not text). Upper-case letters auto-add the
  Shift bit, exactly like hardware.

Mouse events always carry `INTERACT_*` flags in `event->modifiers` on every
platform. A cursor-move is sent to the target position before the press so
hover state is correct (skip with `noMove`).

### Why modifiers is a string, not an array

obs-websocket converts request JSON to `obs_data_t`; its converter silently
drops arrays whose elements aren't objects. `"modifiers": ["ctrl","shift"]`
therefore arrives empty. A `"+"`-separated string (`"ctrl+shift"`) survives.
This is documented in the README and is easy to trip over.

### Virtual source names

- `current` → `obs_frontend_get_current_scene()` (the *program* scene in Studio
  Mode) and a recursive `obs_scene_enum_items()` walk through nested scenes and
  groups, returning the first `browser_source`.
- `interact` → `obs-browser-hid-interact.cpp` scans Qt widgets from the GUI
  thread, finds the visible `OBSBasicInteraction` window, strips the localized
  title prefix (`Basic.InteractionWindow`, e.g. `Interacting with '%1'`), and
  returns the source name. Fallbacks: trailing-quote trim and a source-name
  suffix match.

Two non-obvious traps:

1. **Parented dialog** - `new OBSBasicInteraction(this, source)` gives the
   dialog the main window as Qt parent, so it is *not* returned by
   `QApplication::topLevelWidgets()`. Use `QApplication::allWidgets()` and
   filter on `isWindow()`.
2. **Threading** - vendor callbacks run off the Qt GUI thread, so any Qt access
   must be marshalled with `Qt::BlockingQueuedConnection`. The UI calls the
   same helper *on* the GUI thread, where that connection type would wait for
   itself and freeze OBS, so it runs the scan directly when
   `QThread::currentThread()` is the application thread. Conversely, don't
   call `libobs` mutating functions from arbitrary threads; the source lookup
   here is refcounted and thread-safe.

Virtual names take precedence over literal sources of the same name; a literal
`interact` source can still be addressed via `sourceUuid`.

### Error reporting quirk

obs-websocket always reports vendor requests as `requestStatus: success`; a
vendor failure is only visible as
`responseData.responseData.error`. The plugin therefore puts a human-readable
string there, and `ws-obs` inspects that path and exits non-zero.

### Dock UI (`obs-browser-hid-ui.cpp`)

The dock is added from `obs_module_post_load()` with
`obs_frontend_add_dock_by_id("obs-browser-hid", "Browser HID", widget)`.
That is safe there because modules load from `OBSBasic::OBSInit()` after the
main window (and the frontend API object) exists. The dock starts hidden, as
OBS plugin docks do; users enable it under **View → Docks**.

Design points:

- **No `Q_OBJECT`/moc**: the classes only use lambdas with
  `QObject::connect`, so the build needs no AUTOMOC step. Adding signals or
  `tr()` contexts later means switching the target to AUTOMOC (or moving the
  classes into headers).
- **Shared dispatch**: quick actions call the same
  `obs_browser_hid_send_*()` functions as the vendor handlers, so the dock
  works with obs-websocket disabled and cannot bypass input limits.
- **Source list**: `obs_enum_sources()` filtered on `browser_source`,
  sorted case-insensitively, with "current" / "interact" pinned on top. The
  list is rebuilt on dock creation, on *Rescan*, and on the frontend events
  `FINISHED_LOADING`, `SCENE_LIST_CHANGED`, `SCENE_CHANGED` and
  `PREVIEW_SCENE_CHANGED`; the current selection is preserved when it still
  exists.
- **Settings persistence**: `settings.json` (and `.bak`) in the plugin's
  `plugin_config` directory via `obs_module_config_path()`, saved with
  `obs_data_save_json_pretty_safe()`. Each action carries a stable `uuid`
  (plus `name`, `type`, `key`, `modifiers`, `mode`, `text`, `x`, `y`,
  `button`, `clickCount`); the file also holds the last selected `target`.
  Values are sanitized on load (including UUID normalization), so a
  corrupt/hand-edited file cannot create out-of-range actions. Files from
  before hotkeys existed have no UUIDs; the dock generates them once and
  rewrites the file so the hotkey names stay stable.
- **Editing model**: the settings window is **modeless** (a heap dialog shown
  with `show()`, one instance tracked by the dock), so OBS and interact
  windows stay usable while it is open; it edits a copy and only commits on
  OK. The form applies changes to the selected action immediately (guarded
  by a `loading_` flag while fields are programmatically populated).
- **Reorder/import/export**: the list uses `InternalMove` drag and drop and
  an `ActionListWidget::dropEvent` hook mirrors the visual order back into
  the action vector using the UUIDs stored in `Qt::UserRole`. Import/export
  reuse the same serializer as `settings.json`; import can append (new/
  duplicate UUIDs regenerated) or replace. Import goes through
  `sanitize_action()`, so caps and defaults are the same as for normal load.
- **Quick-action packing**: buttons are fixed to the widest label's size
  hint and placed by a `QGridLayout` whose column count is recomputed from
  the scroll viewport width (`relayout_actions()` on viewport resize), so
  they shrink to text and fit several per row.
- **Compact layout and icons**: margins/spacings are deliberately small
  (4 px at the root, 6/3/6/5 inside group boxes, 4 px grid). The settings
  action is a 22 px auto-raised `QToolButton` in the bottom row next to the
  status label, and Rescan uses the theme refresh icon. Both use OBS's QSS
  classes (`icon-cogs`, `icon-refresh`) and are re-polished so the theme
  supplies the icon; a dark/light resource path is the fallback. The open
  interact button uses OBS's own finger icon (`icon-touch` /
  `interact.svg`).
- **Open interact**: the finger button resolves the selected target with the
  same literal/`current`/`interact` logic and calls
  `obs_frontend_open_source_interaction()` through
  `obs_browser_hid_open_interact()`; OBS then creates and raises the
  standard interact window.
- **Hotkeys**: every action gets
  `obs_hotkey_register_frontend("obs-browser-hid.action.<uuid>", ...)`;
  renames only call `obs_hotkey_set_description()`, so bindings survive.
  Removed actions are unregistered. OBS only restores bindings for its own
  frontend hotkeys, so the plugin reads the active profile's `[Hotkeys]`
  entry (keyed by the hotkey name) and calls `obs_hotkey_load()` after
  registering. Registration happens in `obs_module_post_load()`, before
  `OBSBasic::CreateHotkeys()`. Callbacks are routed to the GUI thread by
  OBSBasic; a thread check makes the fallback safe.
- **Position capture**: Capture hides the modeless dialog and installs an
  application event filter; a small instruction window explains that the
  interact window can be opened now. On a press over an interact window's
  `preview` child, `obs_browser_hid_capture_interact_click()` maps the
  event's widget-local position (not a global cursor position, which is
  unreliable on Wayland) to source-canvas pixels by repeating
  `OBSBasicInteraction::GetSourceRelativeXY()` (device-pixel-ratio scaling,
  then the aspect-fit centre/scale math from `display-helpers.hpp`). The
  press and its release are both swallowed; on release X/Y go through the
  normal spin-box edit path and the dialog is shown again. Esc, the overlay's
  Cancel button, or a 60 s timeout aborts without changes. Source lookup
  splits the localized title pattern around `%1` (so the quoting in
  `Interacting with '%1'` and other wrapping text are both handled) and, as
  a fallback, picks the longest browser-source name contained in the title.
- **Threading**: everything here runs on the Qt GUI thread; blocking CEF
  input calls are the same ones the frontend already makes. Because the dock
  calls the shared dispatch from that thread, the `interact` resolver must not
  self-marshal - it detects the GUI thread and scans directly (see above).

---

## 3. Input hardening (implemented)

Threat model: any client that can authenticate to obs-websocket can call the
vendor with arbitrary data, and obs-websocket does not validate vendor
payloads. All request-derived values are therefore treated as hostile.

### Plugin (`obs-browser-hid.c`)

| Concern | Mitigation |
|---------|-----------|
| Oversized key names | `BKEY_NAME_MAX` (128) before `xkb_keysym_from_name` |
| Non-reentrant parsing | `strtok_r` (callbacks can run on multiple ws threads) |
| Unbounded text | `BTEXT_MAX` (4096) on `SendText` |
| Pathological source lookups | `BSOURCE_NAME_MAX` (1024) |
| Integer abuse | `x`/`y` required, clamped to ±1e6 before the `int32_t` cast; `clickCount` clamped 1..16 |
| Unexpected `button` values | whitelist only, never passed through |
| `xkb_keysym_to_utf8()` errors | negative return mapped to length 0 (prevents a huge `size_t`) |
| Null-pointer formatting | `SAFE_STR()` at every `%s` fed by `obs_source_get_name()` / request data |
| Log injection | `copy_logstr()` strips control chars & truncates request-derived strings; `set_error()` sanitizes its own buffer in place |
| Fixed buffers only | no `strcpy`/`sprintf`; `vsnprintf` into a 512-byte buffer; NUL-termination always explicit |
| Non-Linux `SendKey` | returns a clear error instead of emitting an all-zero key event |

### CLI (`ws-obs-client/src/main.rs`)

- `args_os()` + explicit UTF-8 validation (no panic, no lossy mangling).
- Caps: URL 2048 (must be `ws://`/`wss://`), password 4096, token 1024,
  `--text` 4096, argument count ~262.
- `tungstenite` configured with a 1 MiB message/frame cap and
  `max_redirects = 0` (no silent redirect of an authenticated connection).
- Response frames larger than the cap are rejected **before** buffering;
  `serde_json`'s recursion limit guards nested payloads; at most 1024 unrelated
  frames are skipped while waiting for a response.
- All parse failures exit with usage error (2) or request error (1); no
  `unwrap()` on external input.

### Verified abuse cases

Against `tests/fake-obs-ws.py` (and `tests/fake-huge.py` for the frame cap):

| Case | Result |
|------|--------|
| normal key/click/text | request sent, `ok` |
| `--text` > 4096 | usage error |
| invalid UTF-8 argument | rejected, exit 2 |
| `http://` URL | rejected, exit 2 |
| token > 1024 chars | rejected, exit 2 |
| > ~262 args | rejected, exit 2 |
| click coordinate > i64 | usage error |
| `--text` / `--source` without value | clear usage error |
| `call` with extra args | usage error |
| 2 MiB response frame | `Space limit exceeded: Message too long`, exit 1 |
| plugin error passthrough | relayed, exit 1 |

---

## 4. Building & testing

### Plugin

```sh
cd obs-browser-hid
# against installed OBS 33, or against the source tree:
cmake -B build -S . -DOBS_USE_SYSTEM_LIBS=ON
cmake --build build
sudo cmake --install build --prefix /usr
# -> /usr/lib/<libdir>/obs-modules/plugins/obs-browser-hid.so
# -> /usr/share/obs/obs-modules/plugins/obs-browser-hid/manifest.json
```

For dev runs against the checkout's rundir, copy
`build/obs-browser-hid.so` to
`<obs-build>/rundir/RelWithDebInfo/lib/x86_64-linux-gnu/obs-modules/plugins/`.
A full OBS restart is required after replacing the module.

The plugin is installed in OBS 33's **third-party** module directory
(`obs-modules/plugins`), not the core directory (`obs-modules/core`). The
generated `manifest.json` goes in the matching data directory
(`share/obs/obs-modules/plugins/obs-browser-hid/`).

### CLI

```sh
cd obs-browser-hid/ws-obs-client
cargo build --release
cp target/release/ws-obs ../ws-obs
```

### End-to-end tests without OBS

```sh
python3 tests/fake-obs-ws.py            # listens on 127.0.0.1:4471, password "hunter2"
OBS_WS_URL=ws://127.0.0.1:4471 OBS_WS_PASSWORD=hunter2 \
  ./ws-obs obs-browser-hid current Return
OBS_WS_URL=ws://127.0.0.1:4471 OBS_WS_PASSWORD=hunter2 \
  ./ws-obs obs-browser-hid interact double-click 10 12
WSOBS_DEBUG=1 ...                        # dump raw responses
```

The fake server prints each `CallVendorRequest` payload (`REQ: ...`), so
payload-shape regressions (modifiers string, click fields, virtual names) are
easy to spot. `tests/fake-huge.py` returns a 2 MiB frame on port 4472 to test
the client's frame cap.

---

## 5. Files

| Path | Purpose |
|------|---------|
| `obs-browser-hid.c` | plugin entry point, vendor registration, `SendKey`/`SendText`/`SendMouseClick`, input dispatch, virtual-source resolution, open-interact helper, hardening limits |
| `obs-browser-hid.h` | shared C API used by the vendor handlers and the dock |
| `obs-browser-hid-interact.cpp` | Qt helper: open interact window → browser source name; global click → source-canvas coordinates |
| `obs-browser-hid-ui.cpp` | "Browser HID" dock, source picker and quick-action settings dialog |
| `obs-websocket-api.h` | obs-websocket vendor C API, vendored verbatim from the obs-studio tree |
| `manifest.json.in` | OBS 33 plugin manifest template (version from CMake) |
| `CMakeLists.txt` | standalone build (OBS tree or installed libobs, Qt6 Widgets, libxkbcommon) |
| `debian/` | Debian packaging for the plugin and `ws-obs` |
| `packaging/rpm/` | RPM spec, SRPM helper and shared man page |
| `packaging/README.md` | package build instructions |
| `ws-obs-client/src/main.rs` | Rust CLI: arg parsing, payload building, v5 protocol handshake/auth, hardening |
| `ws-obs` | built CLI binary (local artifact, git-ignored) |
| `build/obs-browser-hid.so` | built plugin (local artifact, git-ignored) |
| `README.md` | user-facing request/CLI documentation |
| `tests/fake-obs-ws.py`, `tests/fake-huge.py` | protocol test servers |

---

## 6. Extending it later

### Highest-value follow-ups

1. **Portable `SendKey`.** The README notes key-name resolution is Linux-only.
   Implement the Windows/macOS branches of `resolve_key()`: on Windows map
   names to Win32 virtual-key codes (`native_vkey`), on macOS to NSEvent
   keyCodes; keep `INTERACT_*` flags in `modifiers`. A portable source of
   names already exists in libobs (`obs_key_from_name()` and the
   `OBS_KEY_*` enum), which would remove the XKB dependency for name lookup.
2. **Integer timing / pacing.** `SendText` emits all characters back-to-back.
   Add an optional `delayMs` field (and cap it) so JS-heavy pages don't drop
   input, or expose a single `SendTextChunk` the automation layer can pace.
3. **Mouse wheel and drag.** `obs_source_send_mouse_wheel()` already exists;
   add `SendMouseWheel` (`x`, `y`, `deltaX`, `deltaY`) and a `SendMouseDrag`
   (`from`, `to`, `button`, `modifiers`) request. Both follow the existing
   clamping pattern.
4. **Normalized coordinates.** A `normalized: true` flag mapping 0..1 to the
   source's canvas size would make scripts resolution-independent.
5. **Source discovery.** A small read-only request (e.g. `ListBrowserSources`)
   returning names/UUIDs and whether a source is in the program scene would
   make `ws-obs` scripts self-configuring.
6. **Dock polish.** Remaining ideas: per-action target overrides, a small
   hotkey editor inside the dialog (instead of sending users to Settings >
   Hotkeys), translated strings (needs AUTOMOC +
   `obs_frontend_push_ui_translation`), and capturing a position from any
   OBS preview (not just an open interact window).

### Design constraints to preserve

- Keep the `modifiers` field a **string**; JSON arrays are dropped by
  obs-websocket's data conversion.
- Keep vendor registration in `obs_module_post_load()`.
- Keep Qt access marshalled to the GUI thread and `libobs` source access
  refcounted (`obs_get_source_by_name` → `obs_source_release`).
- Keep every request-derived value length-capped and clamped; new fields need
  the same treatment (add constants next to the existing `B*_MAX` ones and
  mention them in the README table).
- Log request-derived strings through `copy_logstr()` only.

### Testing ideas

- Rust unit tests for `build_keypress_requests()` (token permutations,
  modifiers de-duplication, `--up`/`--down` mapping, click parsing).
- A CTest harness that compiles the pure helpers (`utf8_decode`, `utf8_encode`,
  `modifier_mask_parse`, `copy_logstr`) standalone and fuzzes them with
  ASan/UBSan; these are the only parser-like functions in the plugin.
- A protocol conformance test using `tests/fake-obs-ws.py` wired into CI
  (spin up, run CLI, assert exit codes + captured payloads).
- An OBS integration test (script that starts OBS with a test scene, calls the
  vendor, and asserts a page-side `keydown` counter) once a headless setup is
  available; that is the only way to cover the CEF delivery path.

### Packaging

Two binary packages are produced from this tree:

- `obs-browser-hid` installs the module in OBS 33's third-party location
  (`<libdir>/obs-modules/plugins/obs-browser-hid.so`) plus
  `<datadir>/obs/obs-modules/plugins/obs-browser-hid/manifest.json`;
- `ws-obs` installs `/usr/bin/ws-obs` and its man page.

Debian files live in `debian/` (`dpkg-buildpackage -b -us -uc`); the RPM spec
and `make-srpm.sh` live in `packaging/rpm/`. `packaging/README.md` documents
the prerequisites (including using an installed OBS 33 or an already-built
source tree) and build commands. Keep the version in `CMakeLists.txt`,
`ws-obs-client/Cargo.toml`, the RPM spec and `debian/changelog` in sync; the
manifest version is generated from the CMake project version.

RPM automatic dependency generation will require the OBS shared libraries;
OBS must satisfy those dependencies (normally the distro `obs-studio`
package). The CLI is independent of the plugin and only recommends it.

### Known upstream quirks (don't relearn these)

- obs-websocket reports vendor failures as `requestStatus: success`; surface
  `responseData.responseData.error`.
- Vendor callbacks are not on the Qt GUI thread and may be concurrent — no
  `strtok`, no static buffers.
- The interact dialog is parented, so `topLevelWidgets()` misses it.
- The interact title pattern has text around `%1` (`Interacting with '%1'`);
  deleting the placeholder yields `Interacting with ''` and never matches.
  Split the pattern around `%1` and compare the prefix/suffix.
- `obs_scene_enum_items()` returns `void` in libobs 33; the callback's `bool`
  return decides whether enumeration continues.
- A stale installed `.so` shows up as `code=600 (No request was found by that
  name.)`; rebuild, reinstall and fully restart OBS.
