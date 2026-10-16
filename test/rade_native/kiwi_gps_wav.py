#!/usr/bin/env python3
"""Reader for KiwiSDR IQ recordings made with kiwirecorder.py --kiwi-wav (-w): a RIFF/WAVE file in which every block
of samples is preceded by a 'kiwi' chunk holding the GNSS time of its first sample (GPS seconds of the week, to the
nanosecond) and a flag for whether the Kiwi had a GPS solution. Ordinary WAV readers see only the first block.

  read_kiwi_gps_wav(path, ref_unix) -> KiwiRec
      .z          complex64 samples (full scale 1.0 = 32768; the Kiwi's 16-bit IQ)
      .fs         sample rate fitted from the time stamps (Hz; the nominal 12000 is really 11999.x or so)
      .t0_unix    UTC time (unix seconds, float) of sample 0, from a fit of the stamps of the blocks with a solution
      .time(i)    UTC unix time of sample index i (array ok)
      .index(t)   sample index of unix time t
      .blocks     number of blocks, .gaps list of (block, missing_samples) where the stamps jump more than a block
      .solution   fraction of blocks that had a GPS solution
      .resid_us   rms of the fit residual in microseconds (the stamps' own scatter; the real timing accuracy is the GPS's)

GPS time runs ahead of UTC by the leap seconds (18 since 2017). The week is not in the file; it is taken so that the
time lands within 3.5 days of ref_unix (the recording was made then, on a clock good to a day).
"""
import struct
import numpy as np

GPS_EPOCH_UNIX = 315964800.0           # 1980-01-06 00:00:00 UTC
LEAP_S = 18.0                          # GPS - UTC, unchanged since 2017-01-01
WEEK = 604800.0


def gps_to_unix(sow, ref_unix, leap=LEAP_S):
    """GPS seconds of week -> unix UTC, choosing the week nearest ref_unix. sow may be an array."""
    sow = np.asarray(sow, dtype=np.float64)
    ref_gps = ref_unix - GPS_EPOCH_UNIX + leap
    week = np.round((ref_gps - sow) / WEEK)
    return GPS_EPOCH_UNIX + week * WEEK + sow - leap


class KiwiRec:
    pass


def read_kiwi_gps_wav(path, ref_unix):
    z_parts, sow, ns, sol = [], [], [], []
    with open(path, "rb") as f:
        hdr = f.read(12)
        if hdr[:4] != b"RIFF" or hdr[8:12] != b"WAVE":
            raise ValueError("not a WAVE file")
        nominal = None
        while True:
            h = f.read(8)
            if len(h) < 8:
                break
            name, size = h[:4], struct.unpack("<I", h[4:])[0]
            if name == b"fmt ":
                b = f.read(size)
                tag, ch, nominal, _, align = struct.unpack("<HHLLH", b[:14])
                if tag != 1 or ch != 2 or align != 4:
                    raise ValueError("not a Kiwi IQ wav")
            elif name == b"kiwi":
                b = f.read(size)
                s, _, gs, gn = struct.unpack("<BBII", b[:10])
                sol.append(s); sow.append(gs); ns.append(gn)
            elif name == b"data":
                d = f.read(size)
                d = d[: len(d) - len(d) % 4]
                if len(sow) > len(z_parts) + 1 or len(sow) == len(z_parts):
                    # a data block without its own kiwi chunk (the Kiwi wrote the file with -w, so this should not happen)
                    raise ValueError("data block without a GNSS time stamp: not a --kiwi-wav recording")
                x = np.frombuffer(d, dtype="<i2").astype(np.float32)
                z_parts.append((x[0::2] + 1j * x[1::2]).astype(np.complex64) / 32768.0)
            else:
                f.seek(size + (size & 1), 1)
            if size & 1 and name in (b"fmt ", b"kiwi", b"data"):
                f.seek(1, 1)
    if not z_parts:
        raise ValueError("no data blocks")
    # the first blocks after connecting carry no valid stamp (kiwirecorder itself skips two); drop them and any with a zero stamp
    drop = 2
    while drop < len(sow) and sow[drop] == 0:
        drop += 1
    z_parts, sow, ns, sol = z_parts[drop:], sow[drop:], ns[drop:], sol[drop:]
    if len(z_parts) < 4:
        raise ValueError("too few stamped blocks")
    n = np.array([len(p) for p in z_parts])
    sow = np.array(sow[: len(n)], dtype=np.float64) + 1e-9 * np.array(ns[: len(n)], dtype=np.float64)
    sol = np.array(sol[: len(n)])
    # continuous GPS seconds across a week rollover
    sow = np.unwrap(sow, period=WEEK) if len(sow) > 1 else sow
    start = np.concatenate([[0], np.cumsum(n)[:-1]])
    ok = sol == 1
    if ok.sum() < 3:
        ok = np.ones(len(n), dtype=bool)
    # fit  t = t0 + idx / fs  over the blocks with a solution; drop outliers once (stamps that jump)
    for _ in range(2):
        A = np.vstack([np.ones(ok.sum()), start[ok]]).T
        c, *_ = np.linalg.lstsq(A, sow[ok], rcond=None)
        r = sow - (c[0] + c[1] * start)
        sd = np.std(r[ok])
        ok = ok & (np.abs(r) < max(5 * sd, 1e-4))
    rec = KiwiRec()
    rec.z = np.concatenate(z_parts)
    rec.fs = 1.0 / c[1]
    rec.nominal_fs = nominal
    rec.blocks = len(n)
    rec.solution = float(np.mean(sol == 1))
    rec.resid_us = float(np.std(r[ok]) * 1e6)
    rec._sow0 = c[0]
    unix0 = float(gps_to_unix(c[0] % WEEK, ref_unix))
    rec.t0_unix = unix0 + (c[0] - c[0] % WEEK) * 0.0
    rec._c1 = c[1]
    gaps = []
    for b in range(1, len(n)):
        missing = (sow[b] - sow[b - 1]) / c[1] - n[b - 1]
        if abs(missing) > 2:
            gaps.append((b, int(round(missing))))
    rec.gaps = gaps
    return rec


def _time(self, i):
    return self.t0_unix + np.asarray(i, dtype=np.float64) * self._c1


def _index(self, t):
    return (np.asarray(t, dtype=np.float64) - self.t0_unix) / self._c1


KiwiRec.time = _time
KiwiRec.index = _index

if __name__ == "__main__":
    import sys, time
    p = sys.argv[1]
    ref = float(sys.argv[2]) if len(sys.argv) > 2 else time.time()
    r = read_kiwi_gps_wav(p, ref)
    print("samples %d (%.2f s), blocks %d, nominal fs %s, fitted fs %.4f Hz, GPS solution in %.0f%% of blocks, "
          "stamp scatter %.1f us, gaps %s" % (len(r.z), len(r.z) / r.fs, r.blocks, r.nominal_fs, r.fs, 100 * r.solution, r.resid_us, r.gaps[:5]))
    print("first sample at unix %.6f (%s UTC); reference %.1f: difference %.2f s" % (
        r.t0_unix, time.strftime("%H:%M:%S", time.gmtime(r.t0_unix)), ref, r.t0_unix - ref))
