#!/usr/bin/env python3
"""Client for piHPSDR's RADE test-harness socket (src/rade_native_test.h).

  rade_ctl.py status
  rade_ctl.py set wave=1 dial=28645000 drive_db=-3
  rade_ctl.py capture on | off          rade_ctl.py log on | off
  rade_ctl.py play file.wav [gain_db=0]
  rade_ctl.py abort

Nothing that keys the transmitter works until the operator ticks "Test harness" in the RADE window.
"""
import os
import socket
import sys


def socket_path():
    base = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(base, "pihpsdr-rade", "control.sock")


def parse(reply):
    """'ok a=1 b=x' / 'err text' -> (ok, {a: '1', b: 'x'} or {'error': 'text'})"""
    reply = reply.strip()
    if reply.startswith("ok"):
        d = {}
        for t in reply[2:].split():
            k, _, v = t.partition("=")
            d[k] = v
        return True, d
    return False, {"error": reply[3:] if reply.startswith("err") else reply}


class Ctl:
    def __init__(self, path=None, timeout=15.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(path or socket_path())
        self.buf = b""

    def close(self):
        self.s.close()

    def request(self, line, timeout=None):
        if timeout is not None:
            self.s.settimeout(timeout)
        self.s.sendall(line.encode() + b"\n")
        while b"\n" not in self.buf:
            d = self.s.recv(4096)
            if not d:
                raise ConnectionError("piHPSDR closed the control socket")
            self.buf += d
        out, _, self.buf = self.buf.partition(b"\n")
        return parse(out.decode())

    # conveniences; each returns the reply dict and raises on "err"
    def _ok(self, line, timeout=None):
        ok, d = self.request(line, timeout)
        if not ok:
            raise RuntimeError("%s: %s" % (line.split()[0], d["error"]))
        return d

    def status(self):
        return self._ok("status")

    def set(self, **kw):
        return self._ok("set " + " ".join("%s=%s" % (k, v) for k, v in kw.items()))

    def capture(self, on):
        return self._ok("capture " + ("on" if on else "off"))

    def log(self, on):
        return self._ok("log " + ("on" if on else "off"))

    def abort(self):
        return self._ok("abort")

    def play(self, path, gain_db=0.0, timeout=150.0):
        """Key, send the file as the mic, unkey. Returns the times (unix seconds) as floats where numeric."""
        ok, d = self.request("play file=%s gain_db=%s" % (path, gain_db), timeout)
        if not ok:
            raise RuntimeError("play: %s" % d["error"])
        return {k: (float(v) if k.startswith(("t_", "ev_")) or k in ("duration_s", "consumed") else v) for k, v in d.items()}


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    c = Ctl()
    try:
        ok, d = c.request(" ".join(argv[1:]), timeout=150.0)
    finally:
        c.close()
    for k, v in d.items():
        print("%-18s %s" % (k, v))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
