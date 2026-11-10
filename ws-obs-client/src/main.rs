/*
 * ws-obs - a tiny CLI for obs-websocket vendor requests.
 *
 * Purpose-built for the `obs-browser-hid` vendor (from the `obs-browser-hid`
 * OBS plugin in this repo), which sends real keypresses to a *named browser
 * source*:
 *
 *    ws-obs [--url ws://host:port] [--password pass] <vendor> [tokens...]
 *
 *    ws-obs obs-browser-hid browsersource2 Return
 *    ws-obs obs-browser-hid browsersource2 ctrl shift Tab
 *    ws-obs obs-browser-hid browsersource2 --up Return     (release only)
 *    ws-obs obs-browser-hid browsersource2 --down a        (press only)
 *    ws-obs obs-browser-hid browsersource2 --text "Hi :)"  (type text)
 *    ws-obs obs-browser-hid browsersource2 ctrl c          (modifier + key)
 *
 * For other vendors a generic JSON escape hatch exists:
 *
 *    ws-obs call <vendor> <requestType> [request-data-json]
 *    ws-obs call vendor-foo DoThing '{"foo": 1}'
 *
 * Connection params come from OBS_WS_URL (default ws://127.0.0.1:4455) and
 * OBS_WS_PASSWORD (if the server requires auth), or --url/--password flags.
 *
 * Exit codes: 0 success, 1 connection/request error, 2 usage error.
 */

use base64::Engine;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::process::ExitCode;
use tungstenite::client::connect_with_config;
use tungstenite::protocol::WebSocketConfig;
use tungstenite::{Message, WebSocket};

const DEFAULT_URL: &str = "ws://127.0.0.1:4455";
const VENDOR: &str = "obs-browser-hid";
/// Pre-rename vendor name; still accepted so existing scripts keep working.
const LEGACY_VENDOR: &str = "obs-keypress";

const KEY_MODIFIERS: &[&str] = &[
	"ctrl", "control", "alt", "shift", "super", "meta", "win", "altgr",
];

const USAGE: &str = "\
ws-obs - send a websocket control packet to OBS (obs-websocket vendor request)

USAGE:
  ws-obs [--url ws://host:port] [--password pass] <vendor> [tokens...]
  ws-obs call <vendor> <requestType> [requestData-json]

obs-browser-hid vendor tokens (first token = source name):
  <sourceName>        browser source to feed (first token, or --source)
  <key>               key name: Return, a, F5, Escape, Left, plus, 'A', ...
  [modifiers]         ctrl|control, alt, shift, super|meta|win, altgr
  --up | --down       only send the release / only send the press
  --text <string>     type a whole string instead (SendText)
  --source <name>     alternative to positional source name

mouse tokens (position is in the source's own canvas; (0,0) = top-left):
  click  <x> <y>              left click at position
  left-click <x> <y>          same as 'click'
  middle-click <x> <y>        middle button
  right-click <x> <y>         right button
  double-click <x> <y>        left button, double click

Examples:
  OBS_WS_PASSWORD=hunter2 ws-obs obs-browser-hid browsersource2 Return
  ws-obs obs-browser-hid browsersource2 ctrl shift Tab
  ws-obs obs-browser-hid browsersource2 --text 'hello'
  ws-obs obs-browser-hid browsersource2 click 120 80
  ws-obs obs-browser-hid browsersource2 right-click 55 12
  ws-obs obs-browser-hid browsersource2 double-click 10 10
  ws-obs call vendor-foo DoThing '{\"arg\":1}'

Env: OBS_WS_URL (default ws://127.0.0.1:4455), OBS_WS_PASSWORD.
'obs-keypress' is accepted as a deprecated alias of 'obs-browser-hid'.
";

/* ---------------------------------------------------------------- */

fn main() -> ExitCode {
	// args() panics on non-UTF-8 arguments; convert explicitly and reject
	// rather than mangling invalid bytes (they could turn into U+FFFD and
	// silently mismatch a source name).
	let args: Vec<String> =
		match std::env::args_os().skip(1).map(|a| a.into_string()).collect::<Result<Vec<String>, _>>() {
			Ok(a) => a,
			Err(bad) => {
				eprintln!("ws-obs error: argument {bad:?} is not valid UTF-8; refusing to guess");
				return ExitCode::from(2);
			}
		};

	match run(&args) {
		Ok(()) => ExitCode::SUCCESS,
		Err(UsageError(e)) => {
			eprintln!("ws-obs: {e}\n\n{USAGE}");
			ExitCode::from(2)
		}
		Err(Fatal(e)) => {
			eprintln!("ws-obs error: {e}");
			ExitCode::FAILURE
		}
	}
}

enum Failure {
	UsageError(String),
	Fatal(String),
}
use Failure::{Fatal, UsageError};

impl From<String> for Failure {
	fn from(e: String) -> Self {
		Fatal(e)
	}
}

type Res = Result<(), Failure>;

/* ------------------------------------------------------------------ */
/* Input hardening limits (web-sockets + CLI are an attack surface)     */

const URL_MAX: usize = 2048; // sane scheme://host:port upper bound
const PASSWORD_MAX: usize = 4096;
const TOKENS_MAX: usize = 256; // CLI tokens after the vendor name
const TOKEN_MAX: usize = 1024; // per-token cap (keys, modifier lists, ...)
const TEXT_MAX: usize = 4096; // --text payload cap (SendText is capped plugin-side too)
const RESPONSE_MAX: usize = 1 << 20; // 1 MiB cap on obs-websocket text frames

fn check_url(url: &str) -> Result<(), Failure> {
	if url.chars().count() > URL_MAX {
		return Err(UsageError("--url/OBS_WS_URL is too long".into()));
	}
	let lower = url.to_ascii_lowercase();
	if !(lower.starts_with("ws://") || lower.starts_with("wss://")) {
		return Err(UsageError(format!(
			"'{url}' is not a ws:// or wss:// URL"
		)));
	}
	Ok(())
}

fn run(args: &[String]) -> Res {
	let mut url = std::env::var("OBS_WS_URL").unwrap_or_else(|_| DEFAULT_URL.to_string());
	let mut password = std::env::var("OBS_WS_PASSWORD").unwrap_or_default();
	check_url(&url)?;
	if password.chars().count() > PASSWORD_MAX {
		return Err(UsageError("--password/OBS_WS_PASSWORD is too long".into()));
	}

	// Length-bounded scan over args. Values after --url / --password /
	// --text get bigger caps instead of the per-token one; enforcement
	// happens here once (url/password are re-checked where parsed).
	let mut exempt = false;
	for arg in args.iter() {
		let len = arg.chars().count();
		if exempt {
			exempt = false;
			continue;
		}
		match arg.as_str() {
			"--url" | "--password" | "--text" => {
				exempt = true;
				continue;
			}
			_ => {}
		}
		if len > TOKEN_MAX {
			return Err(UsageError("argument too long".into()));
		}
	}
	// Cap the exempted values to their own limits.
	for (idx, arg) in args.iter().enumerate() {
		let cap = match arg.as_str() {
			"--url" => URL_MAX,
			"--password" => PASSWORD_MAX,
			"--text" => TEXT_MAX,
			_ => continue,
		};
		let (flag, _) = (arg, idx);
		if idx + 1 < args.len() && args[idx + 1].chars().count() > cap {
			return Err(UsageError(format!("{flag} value too long (max {cap} chars)")));
		}
	}

	if args.len() > TOKENS_MAX + 6 {
		return Err(UsageError("too many arguments".into()));
	}

	let mut i = 0;
	while i < args.len() {
		match args[i].as_str() {
			"--url" if i + 1 < args.len() => {
				url = args[i + 1].clone();
				check_url(&url)?;
				i += 2;
			}
			"--password" if i + 1 < args.len() => {
				password = args[i + 1].clone();
				if password.chars().count() > PASSWORD_MAX {
					return Err(UsageError("--password is too long".into()));
				}
				i += 2;
			}
			"-h" | "--help" => {
				println!("{USAGE}");
				return Ok(());
			}
			_ => break,
		}
	}

	if i >= args.len() {
		return Err(UsageError("missing <vendor> argument".into()));
	}
	if args[i].len() > TOKEN_MAX {
		return Err(UsageError("vendor name too long".into()));
	}
	let vendor = args[i].clone();
	if vendor == LEGACY_VENDOR {
		eprintln!(
			"ws-obs: note: vendor '{LEGACY_VENDOR}' was renamed to '{VENDOR}' (using '{VENDOR}' now)"
		);
	}
	let vendor = if vendor == LEGACY_VENDOR { VENDOR.to_string() } else { vendor };
	let rest = &args[i + 1..];

	let payload_requests = match vendor.as_str() {
		"call" => {
			return build_generic_call(rest, &url, &password);
		}
		VENDOR => build_keypress_requests(rest)?,
		other => {
			return Err(UsageError(format!(
				"unknown vendor '{other}' (known: {VENDOR}; use the `call` subcommand for other vendors)"
			)))
		}
	};

	if payload_requests.is_empty() {
		return Err(UsageError("nothing to send (no requests built)".into()));
	}

	websocket_run(&url, &password, &vendor, &payload_requests).map_err(Fatal)
}

/* ---------------------------------------------------------------- */
/* Vendor payload building (these get wrapped into CallVendorRequest) */

fn vendor_call(vendor: &str, request_type: &str, request_data: Value) -> Value {
	json!({
		"vendorName": vendor,
		"requestType": request_type,
		"requestData": request_data,
	})
}

fn build_keypress_requests(tokens: &[String]) -> Result<Vec<Value>, Failure> {
	let mut source = String::new();
	let mut modifiers: Vec<String> = Vec::new();
	let mut keys: Vec<String> = Vec::new();
	let mut key_up_mode: Option<bool> = None;
	let mut text: Option<String> = None;
	let mut clicks: Vec<(&'static str, u32, i64, i64)> = Vec::new(); // (button, clickCount, x, y)

	let mut idx = 0;
	while idx < tokens.len() {
		let tok = tokens[idx].as_str();
		match tok {
			"--source" | "--sourceName" if idx + 1 < tokens.len() => {
				source = tokens[idx + 1].clone();
				idx += 2;
			}
			"--source" | "--sourceName" => {
				return Err(UsageError(format!("'{tok}' needs a source name")));
			}
			"--text" if idx + 1 < tokens.len() => {
				if text.is_some() {
					return Err(UsageError("--text given twice".into()));
				}
				text = Some(tokens[idx + 1].clone());
				idx += 2;
			}
			"--text" => {
				return Err(UsageError("'--text' needs a string".into()));
			}
			"--up" => {
				key_up_mode = Some(true);
				idx += 1;
			}
			"--down" => {
				key_up_mode = Some(false);
				idx += 1;
			}
			"click" | "left-click" | "middle-click" | "right-click" | "double-click" => {
				if idx + 2 >= tokens.len() {
					return Err(UsageError(format!("'{tok}' needs a position: {tok} <x> <y>")));
				}
				let parse_i64 = |s: &str| {
					s.parse::<i64>().map_err(|_| UsageError(format!("'{s}' is not a number (expected {tok} <x> <y>)")))
				};
				let x = parse_i64(&tokens[idx + 1])?;
				let y = parse_i64(&tokens[idx + 2])?;
				let (button, count): (&'static str, u32) = match tok {
					"middle-click" => ("middle", 1),
					"right-click" => ("right", 1),
					"double-click" => ("left", 2),
					_ => ("left", 1),
				};
				clicks.push((button, count, x, y));
				idx += 3;
			}
			t if t.starts_with("--") => {
				return Err(UsageError(format!("unknown flag '{t}'")));
			}
			t if source.is_empty() => {
				source = t.to_string();
				idx += 1;
			}
			t => {
				let lower = t.to_ascii_lowercase();
				if KEY_MODIFIERS.contains(&lower.as_str()) {
					let m = if lower == "control" { "ctrl" } else { lower.as_str() };
					if !modifiers.iter().any(|x| x == m) {
						modifiers.push(m.to_string());
					}
				} else {
					keys.push(t.to_string());
				}
				idx += 1;
			}
		}
	}

	if source.is_empty() {
		return Err(UsageError("missing <sourceName>".into()));
	}
	if keys.is_empty() && text.is_none() && clicks.is_empty() {
		return Err(UsageError(
			"nothing to send (expected at least one key name or a click token, e.g. Return or 'click 10 20')".into(),
		));
	}

	let mut payload_requests = Vec::new();

	if let Some(text) = text {
		if !modifiers.is_empty() {
			eprintln!("ws-obs: note: SendText ignores modifiers {modifiers:?}");
		}
		payload_requests.push(vendor_call(VENDOR, "SendText", json!({
			"sourceName": source,
			"text": text,
		})));
	}

	for key in &keys {
		let mut rd = json!({ "sourceName": source, "key": key });
		if !modifiers.is_empty() {
			rd["modifiers"] = json!(modifiers.join("+"));
		}
		if let Some(up) = key_up_mode {
			rd["keyUp"] = json!(up);
		}
		payload_requests.push(vendor_call(VENDOR, "SendKey", rd));
	}

	for (button, click_count, x, y) in &clicks {
		let mut rd = json!({
			"sourceName": source,
			"x": x,
			"y": y,
			"button": button,
			"clickCount": click_count,
		});
		if !modifiers.is_empty() {
			rd["modifiers"] = json!(modifiers.join("+"));
		}
		if let Some(up) = key_up_mode {
			rd["mouseUp"] = json!(up);
		}
		payload_requests.push(vendor_call(VENDOR, "SendMouseClick", rd));
	}

	Ok(payload_requests)
}

fn build_generic_call(tokens: &[String], url: &str, password: &str) -> Res {
	if tokens.len() < 2 {
		return Err(UsageError("usage: ws-obs call <vendor> <requestType> [requestData-json]".into()));
	}
	if tokens.len() > 3 {
		return Err(UsageError(
			"too many arguments for `call` (requestData must be a single JSON argument)".into(),
		));
	}

	let request_data: Value = if tokens.len() > 2 {
		serde_json::from_str(&tokens[2])
			.map_err(|e| UsageError(format!("requestData is not valid JSON: {e}")))?
	} else {
		json!({})
	};

	let payload = vendor_call(&tokens[0], &tokens[1], request_data);
	websocket_run(url, password, &tokens[0], &[payload]).map_err(Fatal)
}

/* ---------------------------------------------------------------- */
/* WebSocket transport: v5 protocol handshake, auth, request/response */

fn send_json<S: Read + Write>(socket: &mut WebSocket<S>, v: &Value) -> Result<(), String> {
	socket
		.send(Message::Text(v.to_string()))
		.map_err(|e| format!("websocket write: {e}"))
}

fn next_op<S: Read + Write>(socket: &mut WebSocket<S>, want: Option<u64>) -> Result<Value, String> {
	// Bound the number of unrelated frames we are willing to skip, so a
	// misbehaving server cannot keep us spinning forever on op-5 events.
	const MAX_SKIPPED: usize = 1024;

	let mut skipped = 0usize;
	loop {
		let msg = socket.read().map_err(|e| format!("websocket read: {e}"))?;
		let Message::Text(txt) = msg else {
			continue; // skip binary frames
		};
		if txt.len() > RESPONSE_MAX {
			return Err(format!(
				"obs-websocket frame of {} bytes exceeds the {} byte safety cap",
				txt.len(),
				RESPONSE_MAX
			));
		}
		let v: Value = serde_json::from_str(txt.as_str()).map_err(|e| format!("bad JSON from OBS: {e}"))?;
		let op = v["op"].as_u64().unwrap_or(u64::MAX);
		if want == None || Some(op) == want {
			return Ok(v);
		}
		skipped += 1;
		if skipped > MAX_SKIPPED {
			return Err(format!(
				"gave up after {MAX_SKIPPED} websocket frames without the expected response"
			));
		}
		// skip other ops (e.g. op 5 events) until the expected op arrives
	}
}

fn websocket_run(url: &str, password: &str, _vendor: &str, payload_requests: &[Value]) -> Result<(), String> {
	// Cap frame/message sizes at the protocol layer too, so an oversized
	// frame is rejected before it is fully buffered in memory. No redirect
	// following: the target is explicit (a redirect would silently move the
	// authenticated connection elsewhere).
	let config = WebSocketConfig {
		max_message_size: Some(RESPONSE_MAX),
		max_frame_size: Some(RESPONSE_MAX),
		max_write_buffer_size: 4 * RESPONSE_MAX,
		..WebSocketConfig::default()
	};
	let (mut socket, _response) =
		connect_with_config(url, Some(config), 0).map_err(|e| format!("cannot connect to {url}: {e}"))?;

	// op 0: Hello
	let hello = next_op(&mut socket, Some(0))?;

	let mut identify = json!({ "op": 1, "d": { "rpcVersion": 1 } });
	if let Some(auth) = hello.pointer("/d/authentication") {
		let salt = auth["salt"].as_str().ok_or("Hello: missing authentication salt")?;
		let challenge = auth["challenge"].as_str().ok_or("Hello: missing authentication challenge")?;
		if password.is_empty() {
			return Err(
				"OBS server requires a password; set OBS_WS_PASSWORD or pass --password".to_string(),
			);
		}
		let mut h1 = Sha256::new();
		h1.update(password.as_bytes());
		h1.update(salt.as_bytes());
		let secret = base64::engine::general_purpose::STANDARD.encode(h1.finalize());

		let mut h2 = Sha256::new();
		h2.update(secret.as_bytes());
		h2.update(challenge.as_bytes());
		let answer = base64::engine::general_purpose::STANDARD.encode(h2.finalize());

		identify["d"]["authentication"] = json!(answer);
	}

	// op 1: Identify (expecting op 2: Identified)
	send_json(&mut socket, &identify)?;
	next_op(&mut socket, Some(2))?;

	for (idx, payload) in payload_requests.iter().enumerate() {
		let request_id = format!("ws-obs-{}", idx + 1);
		let frame = json!({
			"op": 6,
			"d": {
				"requestType": "CallVendorRequest",
				"requestId": request_id,
				"requestData": payload,
			}
		});
		send_json(&mut socket, &frame)?;

		// op 7: RequestResponse (events in between are skipped)
		let resp = next_op(&mut socket, Some(7))?;
		if std::env::var("WSOBS_DEBUG").is_ok() {
			eprintln!("debug: response: {resp}");
		}
		let status = &resp["d"]["requestStatus"];
		if status["result"].as_bool() != Some(true) {
			let code = status["code"].as_u64().unwrap_or(0);
			let comment = status["comment"].as_str().unwrap_or("");
			return Err(format!("request {request_id} failed: code={code} ({comment})"));
		}
		if let Some(err) = resp
			.pointer("/d/responseData/responseData/error")
			.and_then(|v| v.as_str())
		{
			return Err(format!("obs-browser-hid plugin error (see OBS log): {err}"));
		}
		eprintln!("{request_id}: ok");
	}

	let _ = socket.close(None); // best-effort
	Ok(())
}
