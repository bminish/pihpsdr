"""A small Socket.IO (protocol 5, Engine.IO 4) client over a WebSocket.

Just enough for the FreeDV Reporter: connect with an auth dictionary, receive
events, emit events, answer pings, and tell the caller when the link drops.
It speaks what freedv-gui's own client speaks (see its util/SocketIoClient.cpp):
ws(s)://host:port/socket.io/?EIO=4&transport=websocket, a "40" packet with the
auth data to open the default namespace, "42[event, data]" for events.

Callbacks run on the client's own thread; hand work to the GUI thread yourself.
"""
import json
import ssl
import threading
import time

import websocket          # websocket-client


class SioClient:
    def __init__(self, host, port, secure, auth, on_event, on_state, path="/socket.io/"):
        self.host, self.port, self.secure, self.auth = host, port, secure, auth
        self.on_event, self.on_state = on_event, on_state
        self.path = path
        self._ws = None
        self._thread = None
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self.connected = False          # the namespace is open
        self.sid = None

    # ------------------------------------------------------------------ api
    def start(self):
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="sio", daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        with self._lock:
            ws = self._ws
        if ws is not None:
            try:
                ws.send("41")           # namespace disconnect, so the server drops us at once
            except Exception:
                pass
            try:
                ws.close()
            except Exception:
                pass
        if self._thread is not None and self._thread is not threading.current_thread():
            self._thread.join(timeout=3)

    def emit(self, event, data=None):
        msg = [event] if data is None else [event, data]
        return self._send("42" + json.dumps(msg, separators=(",", ":")))

    # ------------------------------------------------------------- internals
    def _url(self):
        return "%s://%s:%d%s?EIO=4&transport=websocket" % ("wss" if self.secure else "ws", self.host, self.port, self.path)

    def _send(self, text):
        with self._lock:
            ws = self._ws
        if ws is None:
            return False
        try:
            with self._lock:
                ws.send(text)
            return True
        except Exception:
            return False

    def _run(self):
        backoff = 2.0
        while not self._stop.is_set():
            self.on_state("connecting", "")
            try:
                self._session()
                backoff = 2.0
            except Exception as e:
                if self._stop.is_set():
                    break
                self.on_state("error", str(e))
            self.connected = False
            if self._stop.is_set():
                break
            self.on_state("disconnected", "")
            if self._stop.wait(backoff):
                break
            backoff = min(backoff * 2, 60.0)
        self.on_state("stopped", "")

    def _session(self):
        sslopt = {"cert_reqs": ssl.CERT_REQUIRED} if self.secure else None
        ws = websocket.create_connection(self._url(), timeout=15, sslopt=sslopt,
                                         header=["User-Agent: rade-reporter"])
        with self._lock:
            self._ws = ws
        try:
            opening = ws.recv()
            if not opening.startswith("0"):
                raise RuntimeError("unexpected first packet: " + opening[:60])
            cfg = json.loads(opening[1:])
            # the server pings every pingInterval; allow that and the timeout, then give up
            ws.settimeout((cfg.get("pingInterval", 25000) + cfg.get("pingTimeout", 20000)) / 1000.0 + 5)
            ws.send("40" + json.dumps(self.auth, separators=(",", ":")))
            while not self._stop.is_set():
                pkt = ws.recv()
                if pkt == "":
                    raise RuntimeError("connection closed")
                kind = pkt[0]
                if kind == "2":                    # engine.io ping
                    with self._lock:
                        ws.send("3")
                elif kind == "4":                  # a socket.io packet
                    self._socketio(pkt[1:])
                elif kind == "1":                  # engine.io close
                    raise RuntimeError("closed by server")
        finally:
            with self._lock:
                self._ws = None
            try:
                ws.close()
            except Exception:
                pass

    def _socketio(self, p):
        kind = p[0] if p else ""
        if kind == "0":                            # connect
            try:
                self.sid = json.loads(p[1:]).get("sid")
            except Exception:
                self.sid = None
            self.connected = True
            self.on_state("connected", "")
        elif kind == "1":                          # disconnect
            raise RuntimeError("server disconnected the namespace")
        elif kind == "4":                          # connect error
            raise RuntimeError("connect refused: " + p[1:200])
        elif kind == "2":                          # event
            try:
                arr = json.loads(p[1:])
            except ValueError:
                return
            if isinstance(arr, list) and arr and isinstance(arr[0], str):
                self.on_event(arr[0], arr[1] if len(arr) > 1 else None)
