"""Fake obs-websocket server to smoke-test ws-obs end to end.

Speaks enough of the obs-websocket v5 protocol:
Hello (op 0) -> Identify (op 1) -> Identified (op 2) -> Request (op 6)
-> RequestResponse (op 7), including the optional SHA-256 auth challenge.

Prints every received CallVendorRequest payload to stdout (prefixed "REQ:").
"""
import asyncio
import base64
import hashlib
import json
import sys

import websockets

PASSWORD = "hunter2"
SALT = "s4l7stringxxxx"
CHALLENGE = "ch4ll3ngexxxxx"


def expected_auth():
    inner = hashlib.sha256((PASSWORD + SALT).encode()).digest()
    secret = base64.b64encode(inner).decode()
    return base64.b64encode(hashlib.sha256((secret + CHALLENGE).encode()).digest()).decode()


async def handler(ws):
    await ws.send(json.dumps({
        "op": 0,
        "d": {"obsWebSocketVersion": "5.5.0-fake", "rpcVersion": 1,
              "authentication": {"challenge": CHALLENGE, "salt": SALT}}}))

    ident = json.loads(await ws.recv())
    assert ident["op"] == 1 and ident["d"]["rpcVersion"] == 1, ident
    got = ident["d"].get("authentication")
    ok = got == expected_auth()
    print(f"AUTH: got {got!r} expected {expected_auth()!r} match={ok}", flush=True)
    if not ok:
        await ws.close(code=4009, reason="Authentication failed.")
        return
    await ws.send(json.dumps({"op": 2, "d": {"negotiatedRpcVersion": 1}}))

    async for raw in ws:
        frame = json.loads(raw)
        if frame["op"] != 6:
            continue
        d = frame["d"]
        print(f"REQ: {d['requestType']} {json.dumps(d['requestData'])} id={d['requestId']}", flush=True)
        # First request: answer with a vendor error, others: success.
        vendor = d["requestData"].get("requestData", {})
        if d["requestType"] == "CallVendorRequest" and vendor.get("key") == "BogusKey":
            await ws.send(json.dumps({
                "op": 7,
                "d": {"requestType": "CallVendorRequest", "requestId": d["requestId"],
                      "requestStatus": {"result": True, "code": 100},
                      "responseData": {"vendorName": "obs-browser-hid",
                                       "requestType": "SendKey",
                                       "responseData": {"error": "fake plugin error"}}},
            }))
        else:
            await ws.send(json.dumps({
                "op": 7,
                "d": {"requestType": "CallVendorRequest", "requestId": d["requestId"],
                      "requestStatus": {"result": True, "code": 100}, "responseData": {}}}))
    print("DISCONNECT", flush=True)


async def main():
    async with websockets.serve(handler, "127.0.0.1", 4471):
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
