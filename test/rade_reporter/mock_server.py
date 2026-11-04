"""A stand-in for a FreeDV Reporter server: Engine.IO 4 / Socket.IO over a WebSocket.

It records what clients send so a test can check it, and answers the way the
real one does: open packet, namespace connect, then connection_successful.
"""
import asyncio
import json
import threading

import websockets.asyncio.server as wss


class MockServer:
    def __init__(self):
        self.clients = []          # one dict per connection: auth, events, closed
        self.port = None
        self._loop = None
        self._ready = threading.Event()

    async def _handler(self, ws):
        c = {"auth": None, "events": [], "closed": False, "left": False, "ws": ws}
        self.clients.append(c)
        await ws.send('0' + json.dumps({"sid": "eio", "upgrades": [], "pingInterval": 25000,
                                        "pingTimeout": 20000, "maxPayload": 1000000}))
        try:
            async for msg in ws:
                if msg.startswith("40"):
                    c["auth"] = json.loads(msg[2:] or "{}")
                    await ws.send('40' + json.dumps({"sid": "sio%d" % len(self.clients)}))
                    await ws.send('42' + json.dumps(["connection_successful"]))
                    # a station that is already there, to be shown
                    await ws.send('42' + json.dumps(["bulk_update", [
                        ["new_connection", {"sid": "s1", "last_update": "2026-10-15T20:00:00+00:00",
                                            "callsign": "VK5DGR", "grid_square": "PF95", "version": "FreeDV 2.2",
                                            "rx_only": False, "connect_time": "2026-10-15T19:00:00+00:00"}],
                        ["freq_change", {"sid": "s1", "last_update": "2026-10-15T20:00:01+00:00", "freq": 7053000}],
                        ["tx_report", {"sid": "s1", "last_update": "2026-10-15T20:00:02+00:00", "callsign": "VK5DGR",
                                       "grid_square": "PF95", "mode": "RADEV1", "transmitting": True,
                                       "last_tx": "2026-10-15T20:00:02+00:00"}]]]))
                elif msg.startswith("42"):
                    c["events"].append(json.loads(msg[2:]))
                elif msg.startswith("41"):
                    c["left"] = True
                elif msg == "3":
                    pass
        except Exception:
            pass
        c["closed"] = True

    def start(self):
        def run():
            self._loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self._loop)

            async def main():
                async with wss.serve(self._handler, "127.0.0.1", 0) as s:
                    self.port = s.sockets[0].getsockname()[1]
                    self._ready.set()
                    await asyncio.Future()
            try:
                self._loop.run_until_complete(main())
            except asyncio.CancelledError:
                pass
        threading.Thread(target=run, daemon=True).start()
        self._ready.wait(5)
        return self

    def push(self, client, event, data):
        """Send an event to one connected client, as the server does (for tests of what the Reporter does on receiving it)."""
        msg = "42" + json.dumps([event, data])
        asyncio.run_coroutine_threadsafe(client["ws"].send(msg), self._loop).result(5)

    def stop(self):
        if self._loop:
            for t in asyncio.all_tasks(self._loop):
                self._loop.call_soon_threadsafe(t.cancel)
