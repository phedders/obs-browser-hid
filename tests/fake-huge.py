import asyncio, json
import websockets

async def handler(ws):
    await ws.send(json.dumps({"op": 0, "d": {"obsWebSocketVersion": "5.5.0-fake", "rpcVersion": 1}}))
    await ws.recv()
    await ws.send(json.dumps({"op": 2, "d": {"negotiatedRpcVersion": 1}}))
    async for raw in ws:
        frame = json.loads(raw)
        if frame["op"] != 6:
            continue
        await ws.send(json.dumps({"op": 7, "d": {
            "requestType": "CallVendorRequest", "requestId": frame["d"]["requestId"],
            "requestStatus": {"result": True, "code": 100},
            "responseData": {"junk": "x" * (2 * 1024 * 1024)}}}))
        await ws.close()

async def main():
    async with websockets.serve(handler, "127.0.0.1", 4472):
        await asyncio.Future()

asyncio.run(main())
