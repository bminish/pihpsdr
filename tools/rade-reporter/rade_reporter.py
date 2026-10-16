#!/usr/bin/env python3
"""RADE Reporter - a FreeDV Reporter window for piHPSDR's native RADE.

A separate program: piHPSDR starts it (RADE window, Reporter button) and does
not own it, so it stays up when piHPSDR is closed. It shows who is on the
FreeDV Reporter, like freedv-gui's Reporter window, and, only if you turn it
on, reports that you are present: your callsign, grid square, dial frequency,
the RADE mode piHPSDR has selected (RADEV1 or RADEV2) and whether you are
transmitting. It does not send receive reports.

  * It always starts with reporting OFF; the setting is not remembered.
  * piHPSDR tells it the mode, frequency, transmit state and callsign through
    ~/.local/state/pihpsdr-rade/state.json (rewritten every second or so).
    RADE off, or piHPSDR gone, ends the report at once.
  * RADEV1 is reported to qso.freedv.org, RADEV2 to reporter-radev2.k6aq.net
    (Settings can change either). Only the reporter for the mode piHPSDR has
    selected is ever sent anything.
  * What the list shows is separate: "Show" V1, V2 or both. The reporters shown
    are watched (view role); if you are present, the one for your mode is joined
    as a reporter whether or not it is shown.
"""
import functools
import fcntl
import os
import signal
import subprocess
import sys
import time

import gi
gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import core  # noqa: E402

HEAD = ["Callsign", "Locator", "km", "Hdg", "Version", "MHz", "Mode", "Status", "Msg", "Last TX",
        "RX call", "SNR", "Last update"]
(C_CALL, C_GRID, C_KM, C_HDG, C_VER, C_MHZ, C_MODE, C_STAT, C_MSG, C_LTX, C_RXC, C_SNR, C_UPD,
 C_SID, C_BG, C_KMV, C_HDGV, C_MHZV, C_SNRV, C_UPDV) = range(20)


def snr_text(v):
    return "%.1f dB" % v if isinstance(v, (int, float)) else "- (no lock)"


def hhmm(iso):
    return iso[11:19] if len(iso) >= 19 else ""


def age_text(secs):
    if secs < 90:
        return "%ds" % secs
    if secs < 5400:
        return "%dm" % (secs // 60)
    return "%dh%02dm" % (secs // 3600, (secs % 3600) // 60)


def iso_to_epoch(iso):
    try:
        import datetime
        return datetime.datetime.fromisoformat(iso.replace("Z", "+00:00")).timestamp()
    except Exception:
        return None


def open_below(combo):
    """Open a ComboBoxText's list below it, every row showing. GTK's own popup is centred on the chosen row, and
    on Wayland, where GTK does not know where the window is on the screen, it is cut to a row or two with scroll
    arrows even with the whole screen free. Keyboard use is left as it was."""
    def press(w, ev):
        if ev.button != 1 or not w.get_sensitive():
            return False
        menu = Gtk.Menu()
        for i, row in enumerate(w.get_model()):
            it = Gtk.MenuItem(label=row[0])
            it.connect("activate", lambda _i, i=i: w.set_active(i))
            menu.append(it)
        menu.set_size_request(w.get_allocated_width(), -1)
        menu.show_all()
        menu.attach_to_widget(w, None)
        menu.connect("deactivate", lambda m: GLib.idle_add(m.destroy))
        menu.popup_at_widget(w, Gdk.Gravity.SOUTH_WEST, Gdk.Gravity.NORTH_WEST, ev)
        return True
    combo.connect("button-press-event", press)


class App(Gtk.Window):
    def __init__(self, show=True):
        super().__init__(title="RADE Reporter  [%s]" % CODE_ID)
        self._show = show                 # tests build the window without putting it on the screen
        self.cfg = core.load_config()
        self.dirty = True
        # one connection to each reporter (V1, V2), each with its own roster; which are used is decided in _tick
        self.rosters = {k: core.Roster() for k in self.cfg["servers"]}
        self.states = {k: "idle" for k in self.cfg["servers"]}
        self.links = {k: core.Link(self.rosters[k], functools.partial(self._on_event, k),
                                   functools.partial(self._on_state, k), "pihpsdr-rade")
                      for k in self.cfg["servers"]}
        self.report_key = None
        self.view_keys = []
        self.reporting = False            # never remembered
        # these are remembered: the message goes to every connection, the view stays as it was left
        self.message = self.cfg.get("message", "")
        self.periodic = core.PeriodicSnr(5.0)      # always on: sent whenever reporting and in sync with an SNR
        self._loading = True
        self.last_qsy = None
        # list filters set from the headings (right click km: distance, MHz: frequency range); 0 = no limit on that side
        self._dist = (float(self.cfg.get("dist_min", 0) or 0), float(self.cfg.get("dist_max", 0) or 0))
        self._frng = (float(self.cfg.get("f_min", 0) or 0), float(self.cfg.get("f_max", 0) or 0))
        self._applying_cols = False
        self.set_default_size(*self.cfg.get("size", [1100, 520]))
        self.connect("delete-event", self._quit)
        self._build()
        GLib.timeout_add(500, self._tick)
        GLib.idle_add(lambda: (self._tick(), False)[1])      # once, now; a callback returning True here would run forever

    # ------------------------------------------------------------------ build
    def _build(self):
        v = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        v.set_border_width(8)
        self.add(v)

        r1 = Gtk.Box(spacing=8)
        v.pack_start(r1, False, False, 0)
        self.rep_chk = Gtk.CheckButton(label="Reporting")
        self.rep_chk.set_tooltip_text("Lists you on the FreeDV Reporter, in the RADE mode piHPSDR has selected, with the callsign "
                                      "from piHPSDR's RADE window,\nand sends the SNR of a signal in sync every 5 s.\n"
                                      "Always off when this program starts.")
        self.rep_chk.connect("toggled", self._on_report_toggled)
        r1.pack_start(self.rep_chk, False, False, 0)
        self.call_l = Gtk.Label(label="")
        self.call_l.set_tooltip_text("The callsign set in piHPSDR's RADE window")
        r1.pack_start(self.call_l, False, False, 0)
        r1.pack_start(Gtk.Label(label="Grid"), False, False, 0)
        self.grid_e = Gtk.Entry(width_chars=8, max_length=8, text=self.cfg.get("grid", ""))
        self.grid_e.connect("changed", self._on_ident_changed)
        r1.pack_start(self.grid_e, False, False, 0)
        self.why = Gtk.Label(xalign=0)
        r1.pack_start(self.why, True, True, 6)

        r2 = Gtk.Box(spacing=8)
        v.pack_start(r2, False, False, 0)
        r2.pack_start(Gtk.Label(label="Band"), False, False, 0)
        self.band = Gtk.ComboBoxText()
        for b in ["All"] + [n for n, _, _ in core.BANDS] + ["Other"]:
            self.band.append_text(b)
        names = ["All"] + [n for n, _, _ in core.BANDS] + ["Other"]
        self.band.set_active(names.index(self.cfg.get("band", "All")) if self.cfg.get("band", "All") in names else 0)
        self.band.connect("changed", self._on_view_filter)
        open_below(self.band)
        r2.pack_start(self.band, False, False, 0)
        self.track = Gtk.CheckButton(label="Track")
        self.track.set_tooltip_text("Show only stations on piHPSDR's band or frequency")
        self.track.set_active(bool(self.cfg.get("track", False)))
        self.track.connect("toggled", self._on_view_filter)
        r2.pack_start(self.track, False, False, 0)
        self.t_band = Gtk.RadioButton.new_with_label(None, "band")
        self.t_freq = Gtk.RadioButton.new_with_label_from_widget(self.t_band, "freq.")
        self.t_freq.set_active(self.cfg.get("track_mode", "band") == "freq")
        self.t_band.connect("toggled", self._on_view_filter)
        r2.pack_start(self.t_band, False, False, 0)
        r2.pack_start(self.t_freq, False, False, 0)
        r2.pack_start(Gtk.Label(label="Idle <"), False, False, 0)
        self.idle = Gtk.ComboBoxText()
        for t in ["off", "5 min", "15 min", "60 min", "180 min"]:
            self.idle.append_text(t)
        idles = ["off", "5 min", "15 min", "60 min", "180 min"]
        self.idle.set_active(idles.index(self.cfg.get("idle", "off")) if self.cfg.get("idle", "off") in idles else 0)
        self.idle.connect("changed", self._on_view_filter)
        open_below(self.idle)
        r2.pack_start(self.idle, False, False, 0)
        r2.pack_start(Gtk.Label(label="Show"), False, False, 12)
        self.view = Gtk.ComboBoxText()
        for k, label in (("follow", "Follow radio mode"), ("v1", "RADE V1 only"), ("v2", "RADE V2 only"), ("all", "All (V1 and V2)")):
            self.view.append(k, label)
        self.view.set_active_id(self.cfg.get("view", "follow"))
        self.view.set_tooltip_text("Which reporter's stations the list shows: Follow radio mode shows the reporter of the RADE mode "
                                   "piHPSDR has selected (both while RADE is off). Reports are sent only to the reporter "
                                   "for the RADE mode piHPSDR has selected, whatever is shown.")
        self.view.connect("changed", self._on_view_changed)
        open_below(self.view)
        r2.pack_start(self.view, False, False, 0)
        self.qsy_b = Gtk.Button(label="Request QSY")
        self.qsy_b.set_tooltip_text("Asks the selected station to move to your frequency")
        self.qsy_b.connect("clicked", self._on_qsy)
        r2.pack_end(self.qsy_b, False, False, 0)
        web = Gtk.Button(label="Website")
        web.connect("clicked", self._on_web)
        r2.pack_end(web, False, False, 0)
        vw = Gtk.Button(label="Columns / filters")
        vw.set_tooltip_text("The same menu as a right click on a column heading: which columns to show, and the distance (km) and frequency (MHz) filters.\nDrag a heading to move a column.")
        vw.connect("clicked", lambda b: self._header_menu(None).popup_at_widget(b, Gdk.Gravity.SOUTH_WEST, Gdk.Gravity.NORTH_WEST, None))
        r2.pack_end(vw, False, False, 0)
        st = Gtk.Button(label="Settings")
        st.connect("clicked", self._on_settings)
        r2.pack_end(st, False, False, 0)

        r3 = Gtk.Box(spacing=8)
        v.pack_start(r3, False, False, 0)
        r3.pack_start(Gtk.Label(label="Message"), False, False, 0)
        self.msg_e = Gtk.Entry(max_length=60, width_chars=40)
        self.msg_e.set_text(self.message)
        self.msg_e.set_tooltip_text("Sent to the reporter every time this program connects, and kept for next time, "
                                    "until you Clear it")
        self.msg_e.connect("activate", self._on_send)
        r3.pack_start(self.msg_e, False, False, 0)
        b = Gtk.Button(label="Send")
        b.connect("clicked", self._on_send)
        r3.pack_start(b, False, False, 0)
        b = Gtk.Button(label="Clear")
        b.connect("clicked", self._on_clear)
        r3.pack_start(b, False, False, 0)

        r4 = Gtk.Box(spacing=8)
        v.pack_start(r4, False, False, 0)
        self.snr_ind = Gtk.Label(xalign=0, use_markup=True)
        r4.pack_start(self.snr_ind, True, True, 6)

        self.rxinfo = Gtk.Label(xalign=0)
        v.pack_start(self.rxinfo, False, False, 0)

        self.store = Gtk.ListStore(str, str, str, str, str, str, str, str, str, str, str, str, str,
                                   str, str, float, float, float, float, float)
        self._fp = (0, "All", False, True, 0)
        self.filt = self.store.filter_new()
        self.filt.set_visible_func(self._visible, None)
        self.sort = Gtk.TreeModelSort(model=self.filt)
        self.tree = Gtk.TreeView(model=self.sort)
        self.cols = {}
        self.labels = {}
        numeric = {C_KM: C_KMV, C_HDG: C_HDGV, C_MHZ: C_MHZV, C_SNR: C_SNRV, C_UPD: C_UPDV}
        for i, name in enumerate(HEAD):
            cell = Gtk.CellRendererText()
            cell.set_property("xalign", 0.0)              # left justified, all columns
            col = Gtk.TreeViewColumn(name, cell, text=i, cell_background=C_BG)
            col.set_resizable(True)
            col.set_alignment(0.0)                         # and the headings
            col.set_sort_column_id(numeric.get(i, i))
            col.set_reorderable(True)                      # drag a heading to move the column
            self.tree.append_column(col)
            self.cols[name] = col
            # the heading is a label we own, so that its button can be found (the column's own button is not reliably the one
            # that receives the click) and its text changed when a filter is on
            lab = Gtk.Label(label=name, xalign=0.0)
            lab.show()
            col.set_widget(lab)
            self.labels[name] = lab
            btn = lab.get_ancestor(Gtk.Button) or col.get_button()
            btn.add_events(Gdk.EventMask.BUTTON_PRESS_MASK)
            btn.connect("button-press-event", self._on_header_click, name)      # right click: columns, filters
        self.tree.connect("button-press-event", self._on_tree_click)
        self.tree.connect("columns-changed", self._on_cols_changed)
        # the sort is remembered; the first time it is newest update first (C_UPDV is minus the update time)
        sc, so = self.cfg.get("sort", [C_UPDV, 0]) if isinstance(self.cfg.get("sort"), list) and len(self.cfg.get("sort")) == 2 else [C_UPDV, 0]
        if sc not in set(numeric.values()) | set(range(len(HEAD))) or so not in (0, 1):
            sc, so = C_UPDV, 0
        self.sort.set_sort_column_id(sc, Gtk.SortType.DESCENDING if so else Gtk.SortType.ASCENDING)
        self.sort.connect("sort-column-changed", self._on_sort_changed)
        sw = Gtk.ScrolledWindow()
        sw.add(self.tree)
        v.pack_start(sw, True, True, 0)

        self.status = Gtk.Label(xalign=0)
        v.pack_start(self.status, False, False, 0)
        self._apply_columns()
        self._titles()
        if self._show:
            self.show_all()
        else:
            for child in self.get_children():
                child.show_all()          # the widgets are made, the window is not mapped
        self._loading = False

    # ------------------------------------------------------------- callbacks
    def _on_state(self, key, st, msg):
        GLib.idle_add(self._set_state, key, st, msg)

    def _set_state(self, key, st, msg):
        self.states[key] = st + (": " + msg if msg else "")
        self.dirty = True

    def _on_event(self, key, name, data):
        if name == "qsy_request" and isinstance(data, dict):
            GLib.idle_add(self._qsy_request, data)
        self.dirty = True

    def _qsy_request(self, d):
        dlg = Gtk.MessageDialog(transient_for=self, modal=False, message_type=Gtk.MessageType.INFO,
                                buttons=Gtk.ButtonsType.CLOSE,
                                text="QSY request from %s" % d.get("callsign", "?"))
        dlg.format_secondary_text("%.4f MHz\n%s" % (d.get("frequency", 0) / 1e6, d.get("message", "")))
        dlg.connect("response", lambda w, r: w.destroy())
        dlg.show()

    def _on_report_toggled(self, w):
        self.reporting = w.get_active()
        self._tick()

    def _on_ident_changed(self, w):
        self.grid_e.set_text(self.grid_e.get_text().upper())
        self.cfg["grid"] = self.grid_e.get_text().strip()
        core.save_config(self.cfg)
        self._tick()

    def _on_sort_changed(self, model):
        if self._loading:
            return
        col, order = model.get_sort_column_id()
        if col is not None and col >= 0:
            self._remember(sort=[col, 1 if order == Gtk.SortType.DESCENDING else 0])

    def _on_view_changed(self, w):
        self.cfg["view"] = w.get_active_id() or "follow"
        core.save_config(self.cfg)
        self._tick()

    def _remember(self, **kw):
        self.cfg.update(kw)
        core.save_config(self.cfg)

    def _on_send(self, *_):
        self.message = self.msg_e.get_text().strip()
        self._remember(message=self.message)
        self._tick()

    def _on_clear(self, *_):
        self.msg_e.set_text("")
        self.message = ""
        self._remember(message="")
        self._tick()

    def _on_view_filter(self, *_):
        """Band, Track and Idle are kept for next time."""
        if self._loading:
            return
        self._remember(band=self.band.get_active_text() or "All", track=self.track.get_active(),
                       track_mode="band" if self.t_band.get_active() else "freq",
                       idle=self.idle.get_active_text() or "off")
        self._refilter()

    def _on_web(self, *_):
        v = core.resolve_view(self.cfg.get("view", "follow"), self.pihpsdr)
        k = v if v in self.cfg["servers"] else (self.report_key or "v1")
        url = self.cfg["servers"][k].get("web") or ("https://%s/" % self.cfg["servers"][k]["host"]
                                                    if self.cfg["servers"][k]["host"] else "")
        if url:
            subprocess.Popen(["xdg-open", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def _selected(self):
        model, it = self.tree.get_selection().get_selected()
        return model[it] if it else None

    def _on_qsy(self, *_):
        row = self._selected()
        if row is None or not self.pihpsdr or not self.pihpsdr.get("linked"):
            return
        key, _, sid = row[C_SID].partition(":")
        if key != self.report_key:             # a QSY request is made as a reporter, on the reporter we are on
            return
        self.links[key].emit("qsy_request", {"dest_sid": sid, "message": "",
                                             "frequency": int(self.pihpsdr.get("dial_hz", 0))})

    def _on_tree_click(self, tree, ev):
        if ev.button != 3:
            return False
        hit = tree.get_path_at_pos(int(ev.x), int(ev.y))
        if not hit:
            return False
        tree.get_selection().select_path(hit[0])
        row = self._selected()
        if row is None:
            return True
        menu = Gtk.Menu()
        call = row[C_CALL].split("/")[0]
        for label, url in (("Lookup via QRZ", "https://www.qrz.com/db/" + call),
                           ("Lookup via HamQTH", "https://www.hamqth.com/" + call)):
            it = Gtk.MenuItem(label=label)
            it.connect("activate", lambda _w, u=url: subprocess.Popen(["xdg-open", u],
                                                                        stdout=subprocess.DEVNULL,
                                                                        stderr=subprocess.DEVNULL))
            menu.append(it)
        it = Gtk.MenuItem(label="Copy message")
        it.connect("activate", lambda _w, m=row[C_MSG]: Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD).set_text(m, -1))
        menu.append(it)
        menu.show_all()
        menu.popup_at_pointer(ev)
        return True

    def _on_settings(self, *_):
        dlg = Gtk.Dialog(title="Reporter servers", transient_for=self, modal=True)
        dlg.add_button("Cancel", Gtk.ResponseType.CANCEL)
        dlg.add_button("OK", Gtk.ResponseType.OK)
        g = Gtk.Grid(column_spacing=8, row_spacing=8, margin=12)
        dlg.get_content_area().add(g)
        entries = {}
        for r, k in enumerate(("v1", "v2")):
            s = self.cfg["servers"][k]
            g.attach(Gtk.Label(label=s["label"] + "  (" + s["mode"] + ")", xalign=1), 0, r, 1, 1)
            e = Gtk.Entry(width_chars=34)
            e.set_text(("https://" if s["secure"] else "http://") + s["host"] + (":%d" % s["port"] if s["host"] else "")
                       if s["host"] else "")
            e.set_placeholder_text("host, host:port or https://host/  (empty: nothing is sent)")
            g.attach(e, 1, r, 1, 1)
            entries[k] = e
        g.attach(Gtk.Label(label="RADEV1 is reported to qso.freedv.org, RADEV2 to reporter-radev2.k6aq.net "
                                 "(what freedv-gui uses).", xalign=0), 0, 2, 2, 1)
        dlg.show_all()
        if dlg.run() == Gtk.ResponseType.OK:
            for k, e in entries.items():
                host, port, secure = core.parse_server(e.get_text())
                self.cfg["servers"][k].update(host=host, port=port, secure=secure)
            core.save_config(self.cfg)
            for lk in self.links.values():
                lk.close()                  # reconnect to what is now wanted
        dlg.destroy()
        self._tick()

    def _quit(self, *_):
        self.hide()                       # gone from the screen at once; the rest is tidying up
        self.cfg["size"] = list(self.get_size())
        core.save_config(self.cfg)
        for lk in self.links.values():
            lk.close()
        Gtk.main_quit()
        return False

    # --------------------------------------------------------------- the loop
    current_key = None
    pihpsdr = None
    _last_fill = 0.0

    def _tick(self):
        self.pihpsdr = core.read_pihpsdr_state()
        p = self.pihpsdr
        mycall = ((p.get("call") or "") if p and p.get("linked") else "").strip().upper()
        if mycall != getattr(self, "_shown_call", None):
            self._shown_call = mycall
            self.call_l.set_markup("Call <b>%s</b>" % GLib.markup_escape_text(mycall) if mycall
                                   else "Call <i>(none set in the RADE window)</i>")
        # An identity that is still being typed is not announced: it has to stand still for 1.5 s,
        # or the server would see a new station for every key press.
        ident = (mycall, self.grid_e.get_text().strip().upper())
        now = now_t = time.time()
        if ident != getattr(self, "_ident", None):
            self._ident, self._ident_since = ident, now
        if now - self._ident_since >= 1.5:
            self._stable = ident
        call, grid = getattr(self, "_stable", ("", ""))
        # Reporting needs piHPSDR connected and a valid frequency. Without them it is switched OFF (the tick comes out and cannot be put
        # back until they are there), never merely paused: nothing may be reported with a bad frequency or a state that is not known.
        ok_gate, why_gate = core.report_gate(p)
        if not ok_gate and self.reporting:
            self.rep_chk.set_active(False)                  # -> _on_report_toggled: reporting False
        self.rep_chk.set_sensitive(ok_gate)
        self.rep_chk.set_tooltip_text("Lists you on the FreeDV Reporter, in the RADE mode piHPSDR has selected.\nAlways off when this program starts."
                                      + ("" if ok_gate else "\nDisabled: " + why_gate + "."))
        targets, why, rkey, vkeys = core.plan(self.cfg, p, self.reporting, call, grid, self.cfg.get("view", "follow"))
        self.report_key, self.view_keys = rkey, vkeys
        software = "pihpsdr-rade %s" % ((p or {}).get("version") or "").strip()
        for k, lk in self.links.items():
            lk.software = software
            if k in targets:
                srv, role, mode, freq, tx = targets[k]
                lk.set_target(srv, role, call, grid, freq, mode, tx, self.message)
            else:
                lk.set_target(None, "view")
        role = "report" if rkey else "view"
        mode = targets[rkey][2] if rkey else ""
        srv = self.cfg["servers"][rkey] if rkey else None
        self.why.set_text(why)
        self._rx_note(p, rkey, role, mode, call)
        self.qsy_b.set_sensitive(role == "report")
        if self.dirty or now_t - self._last_fill >= 1.0:     # once a second: the blue "receiving" marks time out
            self.dirty = False
            self._last_fill = now_t
            self._fill()
        link = ("piHPSDR: RADE %s%s, %.4f MHz%s" % (
            {0: "off", 1: "V1", 2: "V2"}.get(int(p.get("waveform", 0)), "?"),
            "", p.get("dial_hz", 0) / 1e6, ", transmitting" if p.get("tx") else "")
            if p and p.get("linked") else "piHPSDR: not running")
        parts = []
        for k in self.links:
            if k in targets:
                parts.append("%s %s%s" % (self.cfg["servers"][k]["label"], self.states[k],
                                          " (reporting)" if k == rkey else ""))
        self.status.set_text("%s    |    %s    |    %d stations" % (
            link, "; ".join(parts) or "no server", len(self.store)))
        return True

    # ------------------------------------------------------------- receiving
    _heard_seq = None
    _tx_seen = None
    _heard_note = ""

    def _rx_note(self, p, rkey, role, mode, mycall):
        """The live SNR, and the calls piHPSDR hears: shown, and reported while we are present."""
        if not p or not p.get("linked"):
            self.rxinfo.set_text("Receiving: piHPSDR is not running")
            self._heard_seq = None
            self.periodic.reset()
            self.snr_ind.set_markup('<span foreground="#999999">\u25cf</span> waiting: piHPSDR is not running')
            return
        if int(p.get("waveform", 0)) == 0:
            live = "RADE is off"
        elif p.get("tx"):
            live = "transmitting"
        elif p.get("rx_sync"):
            sn = p.get("rx_snr")
            live = ("signal, SNR %.1f dB, offset %+.1f Hz" % (sn, p.get("rx_foff", 0.0))) if isinstance(sn, (int, float)) \
                else "signal (no SNR yet)"
        else:
            live = "no signal"
        due = core.rx_reports_due(p, self._heard_seq, mycall)
        if self._heard_seq is None or role != "report":
            # not present: whatever is heard now is old news if we do become present
            seq = int(p.get("heard_seq", 0))
            if self._heard_seq is not None and seq != self._heard_seq and p.get("heard_call"):
                self._heard_note = "last heard %s %s, SNR %s: not reported (%s)" % (
                    time.strftime("%H:%M:%S"), p["heard_call"], snr_text(p.get("heard_snr")), self.why.get_text())
            self._heard_seq = seq
            due = []
        for call, snr, seq in due:                      # every call decoded, in order
            self._heard_seq = max(self._heard_seq, seq)
            self.periodic.heard(call, time.time())
            ok = self.links[rkey].report_rx(call, mode, snr) if rkey else False
            self._heard_note = "last heard %s %s, SNR %.1f dB: %s" % (
                time.strftime("%H:%M:%S"), call, snr, "reported as %s" % mode if ok else "not reported (not connected)")
        if not due and p.get("heard_call") and not self._heard_note:
            self._heard_note = "last heard %s, SNR %s (not reported)" % (p["heard_call"], snr_text(p.get("heard_snr")))
        # transmissions too short to have been seen as "transmitting": the server still wants them
        txc = int(p.get("tx_count", 0))
        if self._tx_seen is None or role != "report":
            self._tx_seen = txc
        elif txc > self._tx_seen:
            if not p.get("tx"):
                self.links[rkey].pulse_tx(mode)         # began and ended between two looks
            self._tx_seen = txc                         # (if it is still on, tx_report transmitting goes as usual)
        self.rxinfo.set_text("Receiving: %s    |    %s" % (live, self._heard_note or "no call heard yet"))
        # the regular SNR report
        if role != "report":
            self.periodic.reset()
            status = "waiting: not reporting (%s)" % self.why.get_text()
        else:
            send, pc, psnr, status = self.periodic.step(p, time.time())
            if send and not self.links[rkey].report_rx(pc, mode, psnr):
                status = "not connected yet"
        lastp = self.periodic.last
        lasttxt = "" if not lastp else "   last: %s%+d dB %ds ago" % (
            lastp[0] + " " if lastp[0] else "(no call) ", int(lastp[1] + 0.5), int(time.time() - lastp[2]))
        sending = bool(role == "report" and lastp and time.time() - lastp[2] < 2 * self.periodic.interval + 1
                       and self.periodic.enabled)
        self.snr_ind.set_markup('<span foreground="%s">\u25cf</span> %s%s' % (
            "#2e9e4a" if sending else "#999999", GLib.markup_escape_text(status), GLib.markup_escape_text(lasttxt)))

    def _fill(self):
        """Bring the list up to date, changing only what changed (rebuilding it, as before, re-sorted
        and re-filtered the whole list for every row, several times a second)."""
        now = time.time()
        mygrid = self.grid_e.get_text().strip()
        self._dial = (self.pihpsdr or {}).get("dial_hz", 0)
        rows = {r[C_SID]: r.iter for r in self.store}
        seen = set()
        structural = False
        filter_cols = False
        stations = [(k, s) for k in self.view_keys for s in self.rosters[k].snapshot()]
        for key, s in stations:
            sid = "%s:%s" % (key, s.sid)               # the same id can come from both reporters
            dbg = core.distance_bearing(mygrid, s.grid)
            ts = iso_to_epoch(s.last_update)
            # as freedv-gui: orange while transmitting; blue while receiving, i.e. an rx_report with an SNR arrived lately
            bg = "#ffe7c2" if s.transmitting else ("#d6ecff" if s.receiving_now(now) else None)
            snr = "" if s.rx_snr is None else ("%+d" % round(s.rx_snr))
            vals = [s.callsign, s.grid, "%d" % dbg[0] if dbg else "", "%d" % dbg[1] if dbg else "",
                    s.version, "%.4f" % (s.freq / 1e6) if s.freq else "", s.mode, s.status,
                    s.message, hhmm(s.last_tx), s.rx_call, snr, age_text(now - ts) if ts else "",
                    sid, bg, dbg[0] if dbg else -1.0, dbg[1] if dbg else -1.0,
                    s.freq / 1e6 if s.freq else 0.0, s.rx_snr if s.rx_snr is not None else -99.0,
                    -ts if ts else 0.0]            # newest update sorts first; a time, not an age, so it keeps still
            seen.add(sid)
            it = rows.get(sid)
            if it is None:
                self.store.append(vals)
                structural = True
                continue
            cur = self.store[it]
            for i, v in enumerate(vals):
                if cur[i] != v:
                    self.store.set_value(it, i, v)
                    if i in (C_MHZV, C_UPDV, C_KMV):
                        filter_cols = True
        for sid, it in rows.items():
            if sid not in seen:
                self.store.remove(it)
                structural = True
        self._refilter(structural or filter_cols)

    # ------------------------------------------------------- columns and heading filters
    def _col_names(self):
        """The columns in the order they are shown."""
        by_col = {id(c): n for n, c in self.cols.items()}
        return [by_col[id(c)] for c in self.tree.get_columns()]

    def _apply_columns(self):
        """Hidden and ordered columns from the config (names the config does not know are ignored; new ones keep their place)."""
        c = self.cfg.get("columns") or {}
        self._applying_cols = True
        try:
            hidden = set(c.get("hidden") or [])
            for n, col in self.cols.items():
                col.set_visible(n not in hidden or n == "Callsign")
            order = [n for n in (c.get("order") or []) if n in self.cols]
            order += [n for n in HEAD if n not in order]
            prev = None
            for n in order:
                self.tree.move_column_after(self.cols[n], prev)
                prev = self.cols[n]
        finally:
            self._applying_cols = False

    def _save_columns(self):
        self._remember(columns={"order": self._col_names(), "hidden": [n for n, c in self.cols.items() if not c.get_visible()]})

    def _on_cols_changed(self, *_):
        if self._loading or self._applying_cols:
            return
        self._save_columns()

    def _set_column_visible(self, name, on):
        if not on and sum(1 for c in self.cols.values() if c.get_visible()) <= 1:
            return                                   # at least one column stays
        self.cols[name].set_visible(on)
        self._save_columns()

    def _titles(self):
        """A heading carries a star while its filter is on."""
        self.labels["km"].set_text("km *" if any(self._dist) else "km")
        self.labels["MHz"].set_text("MHz *" if any(self._frng) else "MHz")

    def _set_dist(self, lo, hi):
        self._dist = (max(0.0, float(lo)), max(0.0, float(hi)))
        self._remember(dist_min=self._dist[0], dist_max=self._dist[1])
        self._titles()
        self._refilter(True)

    def _set_freq(self, lo, hi):
        self._frng = (max(0.0, float(lo)), max(0.0, float(hi)))
        self._remember(f_min=self._frng[0], f_max=self._frng[1])
        self._titles()
        self._refilter(True)

    def _header_menu(self, name):
        """The menu of a column heading: its filter (km, MHz), and which columns are shown."""
        m = Gtk.Menu()
        def add(label, fn, check=None):
            it = Gtk.CheckMenuItem(label=label) if check is not None else Gtk.MenuItem(label=label)
            if check is not None:
                it.set_active(check)
                it.connect("toggled", lambda w: fn(w.get_active()))
            else:
                it.connect("activate", lambda *_: fn())
            m.append(it)
            return it
        if name in ("km", None):
            add("Distance filter...", self._dist_dialog)
            if any(self._dist):
                add("Clear the distance filter", lambda: self._set_dist(0, 0))
            m.append(Gtk.SeparatorMenuItem())
        if name in ("MHz", None):
            add("Frequency range filter...", self._freq_dialog)
            if any(self._frng):
                add("Clear the frequency filter", lambda: self._set_freq(0, 0))
            m.append(Gtk.SeparatorMenuItem())
        for n in HEAD:
            add(n, lambda on, n=n: self._set_column_visible(n, on), check=self.cols[n].get_visible())
        m.show_all()
        return m

    def _on_header_click(self, button, ev, name):
        if ev.button != 3:
            return False
        self._header_menu(name).popup_at_pointer(ev)
        return True

    def _range_dialog(self, title, text, unit, lo, hi, maxv, digits, step):
        dlg = Gtk.Dialog(title=title, transient_for=self, modal=True)
        dlg.add_button("Cancel", Gtk.ResponseType.CANCEL)
        dlg.add_button("OK", Gtk.ResponseType.OK)
        g = Gtk.Grid(column_spacing=8, row_spacing=8, margin=12)
        dlg.get_content_area().add(g)
        g.attach(Gtk.Label(label=text, xalign=0), 0, 0, 3, 1)
        spins = []
        for r, (label, v) in enumerate((("from", lo), ("to", hi))):
            g.attach(Gtk.Label(label=label, xalign=1), 0, r + 1, 1, 1)
            sp = Gtk.SpinButton.new_with_range(0, maxv, step)
            sp.set_digits(digits)
            sp.set_value(v)
            g.attach(sp, 1, r + 1, 1, 1)
            g.attach(Gtk.Label(label=unit + ("  (0: no limit)"), xalign=0), 2, r + 1, 1, 1)
            spins.append(sp)
        dlg.show_all()
        ok = dlg.run() == Gtk.ResponseType.OK
        out = (spins[0].get_value(), spins[1].get_value())
        dlg.destroy()
        return out if ok else None

    def _dist_dialog(self, *_):
        note = "Stations between these distances from you (from your Grid; stations with no locator are hidden while this is on)"
        r = self._range_dialog("Distance filter", note, "km", self._dist[0], self._dist[1], 20000, 0, 50)
        if r is not None:
            self._set_dist(*r)

    def _freq_dialog(self, *_):
        note = "Stations on frequencies in this range (stations that have sent no frequency are hidden while this is on)"
        r = self._range_dialog("Frequency range filter", note, "MHz", self._frng[0], self._frng[1], 60000, 4, 0.001)
        if r is not None:
            self._set_freq(*r)

    def _visible(self, model, it, _d):
        dial, band, tracking, by_band, idle = self._fp
        f = model[it][C_MHZV] * 1e6
        if band not in (None, "All") and core.band_of(f) != band:
            return False
        if tracking and dial:
            if by_band and core.band_of(f) != core.band_of(dial):
                return False
            if not by_band and abs(f - dial) > 1000:
                return False
        if idle and model[it][C_UPDV] and time.time() + model[it][C_UPDV] > idle:
            return False
        dlo, dhi = self._dist
        if dlo or dhi:
            km = model[it][C_KMV]                 # -1: no distance (no locator, or none for the station)
            if km < 0 or (dlo and km < dlo) or (dhi and km > dhi):
                return False
        flo, fhi = self._frng
        if flo or fhi:
            mhz = model[it][C_MHZV]               # 0: the station has told us no frequency
            if mhz <= 0 or (flo and mhz < flo) or (fhi and mhz > fhi):
                return False
        return True

    def _refilter(self, force=True):
        idle_t = self.idle.get_active_text()
        fp = (getattr(self, "_dial", 0), self.band.get_active_text(), self.track.get_active(),
              self.t_band.get_active(), 0 if idle_t in (None, "off") else int(idle_t.split()[0]) * 60)
        if force or fp != self._fp or fp[4]:
            self._fp = fp
            self.filt.refilter()


def code_id():
    """Identifies the code this process runs (the program files' contents): a Reporter left running from before an update is replaced."""
    import hashlib
    h = hashlib.sha1()
    for n in ("rade_reporter.py", "core.py", "sio.py"):
        try:
            h.update(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), n), "rb").read())
        except OSError:
            pass
    return h.hexdigest()[:8]


CODE_ID = code_id()


def single_instance():
    """Only one reporter window. A second start asks the first to show itself - unless the first is running older code (it outlives
    piHPSDR, so it can be hours old): then it is stopped and this one takes over."""
    d = os.path.dirname(core.state_path())
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "reporter.lock")
    f = open(path, "a+")
    for attempt in range(2):
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except OSError:
            f.seek(0)
            txt = f.read().split()
            try:
                pid = int(txt[0]) if txt else 0
            except ValueError:
                pid = 0
            if attempt == 0 and pid and (len(txt) < 2 or txt[1] != CODE_ID):
                try:
                    os.kill(pid, signal.SIGTERM)             # older code: replace it
                except OSError:
                    pass
                t = time.time()
                while time.time() - t < 5.0:                 # until it lets go of the lock
                    try:
                        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        break
                    except OSError:
                        time.sleep(0.1)
                else:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except OSError:
                        pass
                    time.sleep(0.3)
                continue
            try:
                os.kill(pid, signal.SIGUSR1)                 # the same code: show it
            except (ValueError, OSError):
                pass
            return None
    f.seek(0)
    f.truncate()
    f.write("%d %s" % (os.getpid(), CODE_ID))
    f.flush()
    return f


def main():
    signal.signal(signal.SIGHUP, signal.SIG_IGN)      # outlives the terminal / piHPSDR that started it
    lock = single_instance()
    if lock is None:
        return 0
    app = App()
    try:                                # newer PyGObject moved this
        gi.require_version("GLibUnix", "2.0")
        from gi.repository import GLibUnix
        sigadd = GLibUnix.signal_add
    except (ValueError, ImportError):
        sigadd = GLib.unix_signal_add
    sigadd(GLib.PRIORITY_DEFAULT, signal.SIGUSR1, lambda: (app.present(), True)[1])
    sigadd(GLib.PRIORITY_DEFAULT, signal.SIGTERM, lambda: (app._quit(), False)[1])
    Gtk.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
