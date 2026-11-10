# obs-browser-hid

An OBS Studio plugin that lets you send **keyboard input, text and mouse
clicks to a named browser source over WebSocket**, using the built-in
[obs-websocket] server.

obs-websocket's standard request set cannot target input at a single source.
This plugin registers an **obs-websocket vendor** called `obs-browser-hid`
with three extra request types you can call with the standard
[`CallVendorRequest`](https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md#callvendorrequest)
request:

- **`SendKey`** - send a single key (with modifiers) to a browser source.
- **`SendText`** - type a text string into a browser source.
- **`SendMouseClick`** - click a position in a browser source.

> **Naming:** project and vendor were originally `obs-websocket-keys` /
> `obs-keypress`; they were renamed to `obs-browser-hid` once mouse support
> landed. `ws-obs` still accepts `obs-keypress` as a deprecated alias (the
> plugin only registers `obs-browser-hid`).

The same actions are also available from an in-OBS **Browser HID dock**
(target source picker + configurable quick-action buttons); see
[Dock (in-OBS controls)](#dock-in-obs-controls).

## Virtual source names

The `sourceName` field understands two special names, resolved by the plugin
at request time:

| Name       | Resolves to |
|------------|-------------|
| `"current"` | the first browser source found (recursively, incl. nested scenes/groups) inside the scene that is currently **in program output** — in Studio Mode the program scene, otherwise the current scene |
| `"interact"` | the browser source that currently has an OBS **"interact" window** open (the one you opened via right-click → Interact) |

If no matching source exists the request fails with an explanatory `error`
(note: obs-websocket itself still reports `requestStatus: success`; the
message is inside `responseData`). If you have a *literal* source actually
named `current` or `interact`, address it via `sourceUuid` instead — the
virtual name takes precedence for `sourceName`.

## How it works

`libobs` exposes `obs_source_send_key_click()`, and OBS's browser source
(obs-browser/CEF) routes that to `CefBrowserHost::SendKeyEvent()`, i.e. very
real key events inside the pages it renders. The plugin receives the vendor
request from obs-websocket, looks the browser source up by name, and emits
the key event in exactly the format the OBS frontend would produce for a
real key press reaching a source.

So this is *true* key input for the browser source - the page sees
`keydown`/`keyup`/`keypress` (via CEF), JavaScript listeners and text fields
behave like a human typed, and it works even when the OBS window itself is
not focused.

It only *sends synthetic input* into a browser source's page contents. It
never fakes keystrokes in any other application or OS surface (also see
"Limitations").

## Requests

### `SendKey`

```json
{
  "vendorName": "obs-browser-hid",
  "requestType": "SendKey",
  "requestData": {
    "sourceName": "Chat overlay",
    "key": "a",
    "modifiers": "ctrl+shift",
    "keyUp": false,
    "text": "A"
  }
}
```

| Field        | Type    | Req | Meaning |
|--------------|---------|-----|---------|
| `sourceName` | String  | yes*| Name of the browser source |
| `sourceUuid` | String  |     | Alternative lookup (used when `sourceName` didn't match) |
| `key`        | String  | yes | Key name (XKB keysym name on Linux, case-insensitive): `"a"`, `"Return"`, `"Escape"`, `"F5"`, `"Left"`, `"space"`, `"plus"`, `"Dele"`... |
| `modifiers`  | String  |     | `+`-separated list of `ctrl`, `alt`, `shift`, `super`/`meta`/`win`, `altgr`, `numlock`, `capslock` |
| `keyUp`      | Boolean |     | `true`: send only the key **release**; `false`: send only the key **press**; omit: press + release |
| `text`       | String  |     | Optional single character override for the injected character event (first Unicode char is used) |

\*: one of `sourceName` or `sourceUuid` is required.

Notes:

- Upper-case single-letter keys (`key: "A"`) automatically get the Shift bit added (matches real hardware).
- With `ctrl`/`alt`/`super` held, no character event is injected (a real browser does not type into the field for such shortcuts).
- To synthesize true modifier holds for Chromium (when a plain "C with ctrl flag" doesn't trigger a shortcut), you can send the modifier key itself as a second `SendKey` with `keyUp: false`, e.g. `key: "Control_L"`, then `"c"`, then `Control_L` with `keyUp: true`.

### `SendText`

```json
{
  "vendorName": "obs-browser-hid",
  "requestType": "SendText",
  "requestData": {
    "sourceName": "Chat overlay",
    "text": "Hello, world!"
  }
}
```

| Field        | Type   | Req | Meaning |
|--------------|--------|-----|---------|
| `sourceName` | String | yes | Name of the browser source |
| `text`       | String | yes | UTF-8 text to type, character-by-character (max 4096 bytes) |

### `SendMouseClick`

```json
{
  "vendorName": "obs-browser-hid",
  "requestType": "SendMouseClick",
  "requestData": {
    "sourceName": "Chat overlay",
    "x": 120,
    "y": 80,
    "button": "right",
    "clickCount": 1
  }
}
```

| Field        | Type    | Req | Meaning |
|--------------|---------|-----|---------|
| `sourceName` | String  | yes | Name of the browser source |
| `x`, `y`     | Number  | yes | Click position in the source's own canvas, `(0,0)` = top-left (same space as the "interact" window) |
| `button`     | String  |     | `left` (default), `middle`, `right` (short `l`/`m`/`r` accepted) |
| `clickCount` | Number  |     | `1` (default); `2` = double click, etc. |
| `modifiers`  | String  |     | Same format as `SendKey` (`ctrl`, `shift`, `alt`, ...) |
| `mouseUp`    | Boolean |     | `true`: send only button **release**; `false`: only `press`; omit: press + release |
| `noMove`     | Boolean |     | `true`: skip the cursor-move event normally sent before the click |

A cursor-move is issued at the position before the press (matching real
hardware); pages that need hover state see it.

## Usage examples

### Python (obsws-python)

```python
import obsws_python as obs
cl = obs.ReqClient(host="localhost", port=4455, password="yourpass")

cl.send("CallVendorRequest", {
    "vendorName": "obs-browser-hid",
    "requestType": "SendKey",
    "requestData": {"sourceName": "Chat overlay", "key": "Return"},
})

cl.send("CallVendorRequest", {
    "vendorName": "obs-browser-hid",
    "requestType": "SendText",
    "requestData": {"sourceName": "Chat overlay", "text": "Hello!"},
})

cl.send("CallVendorRequest", {
    "vendorName": "obs-browser-hid",
    "requestType": "SendMouseClick",
    "requestData": {"sourceName": "Chat overlay", "x": 120, "y": 80, "button": "right"},
})
```

### JavaScript (@obs-websocket-js)

```js
await ws.call("CallVendorRequest", {
  vendorName: "obs-browser-hid",
  requestType: "SendKey",
  requestData: { sourceName: "Chat overlay", key: "Escape" },
});
```

### CLI: `ws-obs`

A small Rust client (`ws-obs-client/`) wraps all of this. Build it with
`cargo build --release` (or copy `ws-obs-client/target/release/ws-obs` next to
this file); the `ws-obs` file in this folder is a local build artifact, not a
tracked source file.

```sh
ws-obs [--url ws://host:port] [--password pass] obs-browser-hid <source> [tokens...]
ws-obs [--url ws://host:port] [--password pass] call <vendor> <requestType> [json]

  obs-browser-hid browsersource2 Return          # key
  obs-browser-hid browsersource2 ctrl shift Tab  # modifiers + key
  obs-browser-hid browsersource2 --text 'hello'  # type text
  obs-browser-hid browsersource2 click 120 80    # left click at position
  obs-browser-hid browsersource2 right-click 55 12
  obs-browser-hid browsersource2 double-click 10 10
  obs-browser-hid browsersource2 middle-click 5 5 --up
  obs-browser-hid browsersource2 click 10 10 ctrl # click with modifier

  # virtual sources, same surface as normal ones:
  obs-browser-hid current  Return               # first browser src in program
  obs-browser-hid interact ctrl c               # browser src with interact open
  obs-browser-hid interact click 40 60
```

Mouse coordinates are in the source's own canvas ((0,0) = source top-left).

`ws-obs` exit codes: `0` success, `1` connection/request/plugin error, `2`
usage error. Set `WSOBS_DEBUG=1` to dump raw response JSON to stderr.

## Dock (in-OBS controls)

The plugin registers a **Browser HID** dock, so the same actions can be run
from inside OBS without any websocket client:

- open it via **View → Docks → Browser HID** (OBS remembers dock visibility
  per scene collection);
- pick the **target source** from the dropdown (all browser sources found in
  the current profile are listed), or use the **Use current** / **Use
  interact** buttons to select the virtual sources;
- **Rescan** refreshes the list (it also refreshes automatically on scene
  changes and when OBS finishes loading);
- **Settings...** opens the quick-action editor as a **non-modal window** -
  OBS, the scene view and interact windows stay usable while it is open.
  Each entry gets a **name** and is either a **keyboard key** (key name,
  modifiers, optional text override, press/release mode) or a **mouse click**
  (x/y, button, click count, modifiers, press/release mode);
- drag rows in the settings list to **reorder** the buttons, or use
  Up/Down; **Import... / Export...** read/write a JSON action list
  (same format as `settings.json`), with append-or-replace on import;
- **Capture from interact window...** (mouse actions) hides the editor and
  arms capture: open the source's interact window if needed and click inside
  it. The click position is filled into X/Y and the editor reappears. Both
  the press and release are swallowed, so the page does not receive the
  click. Press Esc, use **Cancel capture**, or wait 60 s to abort;
- each action also gets an OBS **frontend hotkey**. Bind keys under
  **Settings → Hotkeys**; entries are listed as `Browser HID: <action name>`
  and run against the currently selected dock target. Bindings are stored in
  the active OBS profile and restored when the plugin registers the hotkeys;
- the quick-action buttons are all sized to the widest label and packed into
  as many columns as the dock width allows, so small docks fit more actions;
- the dock shows one button per configured action; clicking it sends the
  input to the selected target and reports success or the plugin's error
  message at the bottom.

Quick actions (including their stable UUIDs, used for hotkey names) and the
last selected target are stored in
`<obs-config>/plugin_config/obs-browser-hid/settings.json`
(`settings.json.bak` is kept as a backup on save). Older settings files
without UUIDs are upgraded and rewritten on first load. Input goes directly
through libobs, so the dock works even when the obs-websocket server is
disabled.

## Input hardening & threat model

Everything below is deliberately bounded because vendor request payloads are
**untrusted input**: any client that can authenticate to obs-websocket can call
`CallVendorRequest`, and obs-websocket does not sanitize vendor data.

Plugin-side (`obs-browser-hid.c`):

| Input | Limit / handling |
|-------|------------------|
| `key` | 128 bytes max before keysym lookup |
| `modifiers` | 255 bytes max, parsed with `strtok_r` (reentrant) |
| `text` (`SendText`) | 4096 bytes max |
| `sourceName` | 1024 bytes max before lookup |
| `x`, `y` | required, clamped to ±1,000,000 before the `int32_t` cast |
| `clickCount` | clamped to 1..16 |
| `button` | must be one of `left`/`middle`/`right` (+ `l`/`m`/`r`), no passthrough |
| error strings | truncated at 511 bytes, control characters replaced with `?` |
| OBS log lines | request-derived strings passed through `copy_logstr()` (control chars stripped, length-capped) — prevents log-injection via newlines in a key/source name |

All buffers are fixed-size and all writes are length-checked; there is no
`strcpy`/`sprintf`/unchecked `memcpy` on request data. `xkb_keysym_to_utf8()`'s
negative return value is handled (it cannot wrap to a huge `size_t`).
Vendor callbacks may run on more than one websocket thread, so no unsafe
global/static mutable state is used (the only static is the vendor handle,
written once during module load).

Client-side (`ws-obs-client/src/main.rs`):

- Arguments are read via `args_os()` and **rejected** if not valid UTF-8
  (`args()` would panic; lossy conversion could silently mismatch a source).
- Caps: URL ≤ 2048 chars and must be `ws://`/`wss://`, password ≤ 4096, token
  ≤ 1024 chars, `--text` ≤ 4096, ≤ ~262 arguments.
- The WebSocket layer (`tungstenite`) is configured with a 1 MiB
  message/frame cap and redirect following disabled, so a malicious server
  cannot force unbounded memory growth or silently redirect the connection.
- Response JSON is parsed with `serde_json` (default recursion-depth limit
  guards against deeply nested payloads); waiting for a response gives up
  after 1024 unrelated frames.
- No `unwrap()`/`expect()` on attacker-influenced data; all parse failures are
  reported as usage/connection errors with proper exit codes.

Residual risks worth knowing:

- Anyone with obs-websocket credentials can drive the browser sources this
  plugin targets. Protect the websocket port/password accordingly.
- `SendText` emits one CEF key event per character synchronously; a full
  4096-byte string can block the websocket thread for a while. Keep the
  built-in cap small, or split large inputs at the automation layer.
- This software is only as safe as obs-websocket's own auth/transport; use
  `wss://` (via a tunnel/reverse proxy) if the port is exposed beyond
  localhost.


## Build & install

Requires the obs-studio source tree checked out & built at
`/home/prh/gits/obs-studio` (overridable, see CMakeLists.txt).

```sh
cmake -B build -S .
cmake --build build
sudo cmake --install build --prefix /   # -> /usr/lib/x86_64-linux-gnu/obs-modules/core/
```

or drop `build/obs-browser-hid.so` into
`build/rundir/RelWithDebInfo/lib/x86_64-linux-gnu/obs-modules/core/`
in your obs-studio checkout for development runs.

## Limitations / caveats

- **Windows / macOS key names**: this build resolves `key` to native key codes
  via XKB keysyms (Linux). On Windows/macOS `SendKey` returns an explicit error
  (the message is relayed by the CLI); `SendText` and `SendMouseClick` still
  work. A follow-up should map names to Win32 virtual keys / NSEvent keyCodes
  there (see `DEVELOPMENT.md`).
- obs-websocket reports vendor requests as `requestStatus: success` even when
  the vendor plugin failed internally (a limitation of the vendor API);
  failures therefore come back as an `error` field inside `responseData`.
- `SendText` has no inter-keystroke delay; each character is fed back-to-back.
  If a JS-heavy page drops input, add your own pacing by emitting multiple
  `SendKey` calls from your automation layer.
- This is a *plugin* (C, links libobs + libxkbcommon), loaded by OBS like any
  other addon; it does not need separate WebSocket transport of its own -
  everything goes through the regular obs-websocket server/port/auth.
- The dock and settings dialog require a Qt OBS frontend (the normal desktop
  app); they are skipped in headless builds. The quick-action editor is
  English-only for now.
- Action hotkeys are ordinary OBS frontend hotkeys, so they follow OBS's
  normal hotkey focus/rerouting rules rather than being system-global
  shortcuts. They are saved per profile.

## Files

- `obs-browser-hid.c` - the plugin core (vendor requests + input dispatch)
- `obs-browser-hid.h` - shared C API between the core, the interact helper and the dock
- `obs-browser-hid-interact.cpp` - Qt helper that resolves the open "interact" window to a source name
- `obs-browser-hid-ui.cpp` - the "Browser HID" dock and the quick-action settings dialog
- `obs-websocket-api.h` - vendored verbatim from `plugins/obs-websocket/lib/` of the obs-studio repo (header-only vendor C API)
- `CMakeLists.txt` - standalone build against the local obs-studio build tree
- `ws-obs-client/` - Rust CLI source (`cargo build --release`)
- `ws-obs` - built CLI binary, local artifact (git-ignored)
- `DEVELOPMENT.md` - how this plugin/CLI was built, prompt history, design notes and how to extend it

[obs-websocket]: https://github.com/obsproject/obs-websocket
