"""Reading and summarising the RADE parameter logs (rade-logs/<run>/ or its .tar.gz).

No GUI here, so it can be tested without a display. Times in a Run are absolute UTC
seconds (Unix time); the files hold seconds since the start of the run.
"""
import csv, io, os, tarfile, tempfile, math
import numpy as np

DIAL_OFFSET_HZ = 1500        # the dial is the suppressed carrier; the energy centre is 1500 Hz from it
GAP_S = 1.5                  # a break longer than this in a table is drawn as a break

TABLES = ("rx", "tx", "events", "overs", "tx_overs")


class Run:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path.rstrip("/"))
        self.meta = {}
        self.t0 = 0.0
        self.rx = {}
        self.tx = {}
        self.events = []          # (t_abs, kind, value)
        self.overs = []           # dicts, t_start / t_end absolute
        self.tx_overs = []
        self.source = path        # what it was opened from (a directory, or a .tar.gz)
        self.rxf = {}             # rx_full.csv: every librade call, column name -> array (t absolute); empty if the run has none
        self.rxf_off = 0          # bytes of rx_full.csv taken so far (a live run is read on from there)
        self.live = False         # a directory whose run has not ended (no ended_utc in meta.txt): it can be updated


def _num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return math.nan


def _rows(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        text = f.read()
    if text and not text.endswith("\n"):          # a run that is still being written: the last line may be half there
        text = text[:text.rfind("\n") + 1]
    return list(csv.DictReader(io.StringIO(text, newline="")))


def _columns(rows, t0):
    cols = {}
    if not rows:
        return cols
    for k in rows[0].keys():
        cols[k] = np.array([_num(r.get(k)) for r in rows], dtype=float)
    if "t" in cols:
        cols["t"] = cols["t"] + t0
    return cols


def load_rxfull(path, t0, prev=None):
    """rx_full.csv as arrays. With prev (the same run, loaded before) only the lines written since are read: at V2's 50 rows a second the
    whole file is too much to parse again at every update. Returns (columns, bytes taken)."""
    cols, off = {}, 0
    if prev is not None and prev.rxf and prev.rxf_off:
        cols, off = prev.rxf, prev.rxf_off
    try:
        size = os.path.getsize(path)
    except OSError:
        return {}, 0
    if size < off:                                   # the file is not the one we read from: start again
        cols, off = {}, 0
    with open(path, "rb") as f:
        head = f.readline().decode().strip()
        if not cols:
            off = f.tell()
        f.seek(off)
        raw = f.read()
    end = raw.rfind(b"\n") + 1                      # a line being written is left for the next time
    if end == 0 or not head:
        return cols, off
    names = head.split(",")
    new = np.genfromtxt(io.StringIO(raw[:end].decode()), delimiter=",", ndmin=2)
    new = new.reshape(-1, len(names))
    out = {}
    for i, n in enumerate(names):
        col = new[:, i] + (t0 if n == "t" else 0.0)
        out[n] = np.concatenate([cols[n], col]) if n in cols else col
    return out, off + end


def resolve(path):
    """A run directory from a directory, a file inside one, or a .tar.gz (extracted to a temp dir)."""
    if os.path.isdir(path):
        return path
    if path.endswith((".tar.gz", ".tgz")):
        dest = tempfile.mkdtemp(prefix="rade-logview-")
        with tarfile.open(path) as tf:
            try:
                tf.extractall(dest, filter="data")
            except TypeError:
                tf.extractall(dest)
        for root, _dirs, files in os.walk(dest):
            if "meta.txt" in files:
                return root
        raise ValueError("no meta.txt in " + path)
    d = os.path.dirname(os.path.abspath(path))
    if os.path.exists(os.path.join(d, "meta.txt")):
        return d
    raise ValueError("%s is not a run of the new log format (a directory with meta.txt, rx.csv ...)" % path)


def signature(path):
    """What changes when a run directory is written to: the size and time of its tables and meta.txt."""
    d = path if os.path.isdir(path) else os.path.dirname(os.path.abspath(path))
    sig = []
    for n in ("meta.txt", "rx.csv", "rx_full.csv", "tx.csv", "events.csv", "overs.csv", "tx_overs.csv"):
        try:
            st = os.stat(os.path.join(d, n))
            sig.append((n, st.st_size, st.st_mtime_ns))
        except OSError:
            sig.append((n, 0, 0))
    return tuple(sig)


def find_runs(path):
    """The runs in path: the run itself if it is one, else every run directory and .tar.gz directly inside it
    (a directory is taken in place of the .tar.gz made from it)."""
    if os.path.isfile(os.path.join(path, "meta.txt")):
        return [path]
    out, dirs = [], set()
    try:
        names = sorted(os.listdir(path))
    except OSError:
        return []
    for n in names:
        q = os.path.join(path, n)
        if os.path.isdir(q) and os.path.isfile(os.path.join(q, "meta.txt")):
            out.append(q)
            dirs.add(n)
    for n in names:
        if n.endswith(".tar.gz") and n[:-len(".tar.gz")] not in dirs:
            out.append(os.path.join(path, n))
    return out


def load_run(path, prev=None):
    d = resolve(path)
    run = Run(d)
    is_tar = path.endswith((".tar.gz", ".tgz"))
    run.source = path if is_tar else d             # a file in a run directory stands for the directory
    run.sig = None if is_tar else signature(d)
    if path.endswith((".tar.gz", ".tgz")):
        run.name = os.path.basename(path)[:-len(".tar.gz")] if path.endswith(".tar.gz") else os.path.basename(path)[:-4]
    mp = os.path.join(d, "meta.txt")
    if not os.path.exists(mp):
        raise ValueError("no meta.txt in " + d)
    with open(mp) as f:
        for line in f:
            if ":" in line:
                k, v = line.split(":", 1)
                run.meta[k.strip()] = v.strip()
    run.live = not is_tar and "ended_utc" not in run.meta
    run.t0 = _num(run.meta.get("started_unix"))
    if math.isnan(run.t0):
        raise ValueError("meta.txt has no started_unix")
    run.rx = _columns(_rows(os.path.join(d, "rx.csv")), run.t0)
    run.tx = _columns(_rows(os.path.join(d, "tx.csv")), run.t0)
    if os.path.exists(os.path.join(d, "rx_full.csv")):
        run.rxf, run.rxf_off = load_rxfull(os.path.join(d, "rx_full.csv"), run.t0, prev if prev is not None and prev.source == run.source else None)
    for r in _rows(os.path.join(d, "events.csv")):
        run.events.append((run.t0 + _num(r.get("t")), r.get("kind", ""), r.get("value", "")))
    run.events.sort(key=lambda e: e[0])
    for name, dest in (("overs", run.overs), ("tx_overs", run.tx_overs)):
        for r in _rows(os.path.join(d, name + ".csv")):
            o = dict(r)
            for k in ("t_start", "t_end"):
                o[k] = run.t0 + _num(r.get(k))
            for k in o:
                if k not in ("utc_start", "mode", "wave", "call", "t_start", "t_end"):
                    o[k] = _num(o[k])
            dest.append(o)
    return run


CALL_WAIT_S = 2.5            # a call may arrive this long after its over ends (V1: the end-of-over frame)


def heard_calls(run):
    """Every call the run has, as dicts: call, v2, t_call, t_start, t_end (None while the over is still going).

    From the call events, so a call is known as soon as it is decoded, before the over's row is written to
    overs.csv; the over is the logged one the call falls in, else the one that sync_on opened.  Once an over
    has a call, a repeat of the same call in it is not listed again."""
    waves = [(t, v.strip()) for t, k, v in run.events if k == "wave"]
    ons = [t for t, k, _v in run.events if k == "sync_on"]
    offs = [t for t, k, _v in run.events if k == "sync_off"]
    out, seen = [], set()
    for t, k, v in run.events:
        if k != "call" or not v.split():
            continue
        call = v.split()[0]
        ov = next((o for o in run.overs if o["t_start"] <= t <= o["t_end"] + CALL_WAIT_S), None)
        if ov is not None:
            t0, t1, wave = ov["t_start"], ov["t_end"], ov.get("wave", "")
        else:
            t0 = max([x for x in ons if x <= t], default=t)
            t1 = min([x for x in offs if x >= t0], default=None)
            wave = ""
        if not wave:
            w = [x for tw, x in waves if tw <= t0]
            wave = "V" + w[-1] if w else ""
        if (call, t0) in seen:
            continue
        seen.add((call, t0))
        out.append({"call": call, "v2": wave == "V2", "t_call": t, "t_start": t0, "t_end": t1})
    return out


def with_gaps(t, y, gap=GAP_S):
    """Insert NaN where the table has a break, so a line is not drawn across silence."""
    if len(t) < 2:
        return t, y
    br = np.where(np.diff(t) > gap)[0]
    if len(br) == 0:
        return t, y
    return np.insert(t, br + 1, np.nan), np.insert(y, br + 1, np.nan)


def energy_centre_hz(dial_hz, invert):
    """Nominal centre of the RADE energy: the carrier +1500 Hz (normal) or -1500 Hz (inverted)."""
    return dial_hz + (-DIAL_OFFSET_HZ if invert else DIAL_OFFSET_HZ)


def window_stats(arr_t, arr_y, t_a, t_b):
    lo, hi = min(t_a, t_b), max(t_a, t_b)
    m = (arr_t >= lo) & (arr_t <= hi) & ~np.isnan(arr_y)
    if not m.any():
        return None
    v = arr_y[m]
    return {"n": int(m.sum()), "mean": float(v.mean()), "min": float(v.min()), "max": float(v.max()),
            "first": float(v[0]), "last": float(v[-1])}


def utc_str(t_abs):
    import datetime
    return datetime.datetime.fromtimestamp(t_abs, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3] + "Z"


def over_text(run, o):
    """Static information of one heard over as a block of text."""
    dial = o.get("dial_hz", math.nan)
    inv = int(o.get("invert", 0)) if not math.isnan(o.get("invert", math.nan)) else 0
    fe = energy_centre_hz(dial, inv)
    L = ["Run              %s" % run.name,
         "Heard over       %s  ->  %s UTC   (%.1f s)" % (utc_str(o["t_start"]), utc_str(o["t_end"])[11:], o["t_end"] - o["t_start"]),
         "Waveform / mode  RADE %s, radio mode %s, Invert %s (%s)" % (o.get("wave", "?"), o.get("mode", "?"), inv,
                                                                    "energy below the carrier" if inv else "energy above the carrier"),
         "Dial (carrier)   %.3f kHz" % (dial / 1e3),
         "Energy centre    %.3f kHz nominal (dial %s 1500 Hz)" % (fe / 1e3, "-" if inv else "+"),
         "Decoder offset   mean %+.1f Hz, sd %.1f Hz (sign convention as logged, not applied to the centre above)" % (
             o.get("foff_mean", math.nan), o.get("foff_sd", math.nan)),
         "SNR              mean %.1f dB, min %.1f, max %.1f   (sync %.0f %% of the over)" % (
             o.get("snr_mean", math.nan), o.get("snr_min", math.nan), o.get("snr_max", math.nan), o.get("sync_pct", math.nan)),
         "End-of-over      %s" % ("not recorded (old-format log)" if math.isnan(o.get("eoo", math.nan)) else "seen" if o["eoo"] == 1 else "not seen"),
         "Call             %s" % ((o.get("call") or ("(not recorded)" if math.isnan(o.get("eoo", math.nan)) else "(none decoded)")) +
                                   ("   (SNR %.1f dB)" % o["call_snr"] if o.get("call") and not math.isnan(o.get("call_snr", math.nan)) else ""))]
    return "\n".join(L)


def tx_over_text(run, o):
    return "\n".join([
        "Run              %s" % run.name,
        "Transmitted over %s  ->  %s UTC   (%.1f s)" % (utc_str(o["t_start"]), utc_str(o["t_end"])[11:], o["t_end"] - o["t_start"]),
        "Waveform         RADE %s, dial (carrier) %.3f kHz" % (o.get("wave", "?"), o.get("dial_hz", math.nan) / 1e3),
        "Drive / trim     %.1f dB / %.1f dB, leveller gain at end %.1f dB" % (o.get("drive_db", math.nan), o.get("trim_db", math.nan), o.get("agc_db_end", math.nan)),
        "Loudness / peak  mean %.1f LUFS, peak max %.1f dBFS, limiting %.0f %% of the time" % (
            o.get("loud_mean", math.nan), o.get("peak_max", math.nan), o.get("limiting_pct", math.nan)),
        "Underruns        %.0f, end-of-over hold %.0f ms" % (o.get("underruns", math.nan), o.get("hold_ms", math.nan))])


def run_text(run):
    """Static information of the whole run: the meta file, a summary, the settings seen and calls."""
    L = ["Run %s   (%s)" % (run.name, run.path)]
    for k, v in run.meta.items():
        L.append("  %-14s %s" % (k, v))
    t_end = max([run.t0] + [e[0] for e in run.events] + [o["t_end"] for o in run.overs + run.tx_overs])
    L.append("Span             %s -> %s UTC  (%.0f s)" % (utc_str(run.t0), utc_str(t_end)[11:], t_end - run.t0))
    L.append("Heard overs      %d, transmitted overs %d, rx rows %d, tx rows %d" % (
        len(run.overs), len(run.tx_overs), len(run.rx.get("t", [])), len(run.tx.get("t", []))))
    dials = sorted({o["dial_hz"] for o in run.overs if not math.isnan(o["dial_hz"])})
    if dials:
        L.append("Dials heard on   " + ", ".join("%.3f" % (d / 1e3) for d in dials) + " kHz")
    calls = [(o["call"], o["t_start"]) for o in run.overs if o.get("call")]
    if calls:
        L.append("Calls            " + ", ".join("%s (%s)" % (c, utc_str(t)[11:19]) for c, t in calls))
    counts = {}
    for _t, k, _v in run.events:
        counts[k] = counts.get(k, 0) + 1
    L.append("Events           " + ", ".join("%s %d" % kv for kv in sorted(counts.items())))
    return "\n".join(L)
