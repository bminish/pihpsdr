#!/usr/bin/env python3
"""Tests of the RADE Reporter's parts that are not the window (tools/rade-reporter/core.py, sio.py)
against a mock reporter server, and - with --live - a read-only look at the real one.

  test_reporter.py [--live]
"""
import json
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "rade-reporter"))
sys.path.insert(0, HERE)
import core  # noqa: E402
from mock_server import MockServer  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    print("  %-4s %s" % ("ok" if cond else "FAIL", what))
    if not cond:
        fails += 1


def wait(pred, secs=5.0):
    t = time.time()
    while time.time() - t < secs:
        if pred():
            return True
        time.sleep(0.05)
    return False


def places():
    print("places and bands")
    d = core.distance_bearing("IO53HU", "JN58")
    check(d is not None and 1400 < d[0] < 1700, "Castlebar to Munich about 1500 km (%.0f)" % d[0])
    check(abs(core.distance_bearing("IO53", "IO53")[0]) < 1, "same square is 0 km")
    check(core.grid_valid("io53hu") and core.grid_valid("IO53") and not core.grid_valid("ZZ99")
          and not core.grid_valid("IO5"), "grid validation")
    check(core.band_of(7053000) == "40 m" and core.band_of(14236000) == "20 m" and core.band_of(1000) == "Other",
          "band_of")
    check(core.parse_server("https://qso.freedv.org/") == ("qso.freedv.org", 443, True), "parse https")
    check(core.parse_server("http://h:8080") == ("h", 8080, False), "parse http:port")
    check(core.parse_server("example.org") == ("example.org", 443, True), "parse bare host")


def decisions():
    print("what gets reported")
    cfg = core.load_config(os.path.join(tempfile.mkdtemp(), "none.json"))
    live = {"linked": True, "waveform": 1, "dial_hz": 7053000, "tx": False, "call": "EI6IZ"}
    d = core.decide(cfg, live, False, "EI6IZ", "IO53hu", "v1")
    check(d[1] == "view" and d[2] == "", "reporting off: only watching")
    d = core.decide(cfg, live, True, "EI6IZ", "IO53hu", "v1")
    check(d[1] == "report" and d[2] == "RADEV1" and d[3] == 7053000 and d[0]["host"] == "qso.freedv.org",
          "V1 on: reported as RADEV1 on qso.freedv.org at the dial frequency")
    d = core.decide(cfg, dict(live, tx=True), True, "EI6IZ", "IO53hu", "v1")
    check(d[4] is True, "transmitting is passed on")
    cfg0 = json.loads(json.dumps(cfg))
    cfg0["servers"]["v2"]["host"] = ""
    d = core.decide(cfg0, dict(live, waveform=2), True, "EI6IZ", "IO53hu", "v1")
    check(d[1] == "view" and "no server set" in d[5], "V2 with its server blanked in Settings: not reported, says why")
    d = core.decide(cfg, dict(live, waveform=2), True, "EI6IZ", "IO53hu", "v1")
    check(d[1] == "report" and d[2] == "RADEV2" and d[0]["host"] == "reporter-radev2.k6aq.net",
          "V2 on: reported as RADEV2 on reporter-radev2.k6aq.net (the freedv-gui V2 host)")
    # plan(): what is shown is separate from where reports go
    t, why, rk, vk = core.plan(cfg, dict(live, waveform=2), False, "EI6IZ", "IO53hu", "v1")
    check(list(t) == ["v1"] and t["v1"][1] == "view" and rk is None, "show V1 only, not reporting: V1 watched, V2 not connected")
    t, why, rk, vk = core.plan(cfg, dict(live, waveform=2), False, "EI6IZ", "IO53hu", "v2")
    check(list(t) == ["v2"], "show V2 only: V2 watched only")
    t, why, rk, vk = core.plan(cfg, dict(live, waveform=2), False, "EI6IZ", "IO53hu", "all")
    check(sorted(t) == ["v1", "v2"] and all(x[1] == "view" for x in t.values()) and vk == ["v1", "v2"], "show all: both watched")
    t, why, rk, vk = core.plan(cfg, dict(live, waveform=2), True, "EI6IZ", "IO53hu", "all")
    check(rk == "v2" and t["v2"][1] == "report" and t["v2"][2] == "RADEV2" and t["v1"][1] == "view",
          "all, radio on V2, present: report to V2 only, V1 stays a viewer")
    t, why, rk, vk = core.plan(cfg, dict(live, waveform=1), True, "EI6IZ", "IO53hu", "all")
    check(rk == "v1" and t["v1"][1] == "report" and t["v1"][2] == "RADEV1" and t["v2"][1] == "view",
          "all, radio on V1, present: report to V1 only, V2 stays a viewer")
    t, why, rk, vk = core.plan(cfg, dict(live, waveform=2), True, "EI6IZ", "IO53hu", "v1")
    check(rk == "v2" and t["v2"][1] == "report" and t["v1"][1] == "view" and vk == ["v1"],
          "show V1 only, radio on V2, present: still reports to V2 (joined though not shown), V1 shown")
    t, why, rk, vk = core.plan(cfg, dict(live, waveform=0), True, "EI6IZ", "IO53hu", "all")
    check(rk is None and all(x[1] == "view" for x in t.values()), "RADE off: reports to neither")
    c2 = core.load_config(os.path.join(tempfile.mkdtemp(), "none.json"))
    st = dict(live, waveform=2)
    check(core.resolve_view("follow", st) == "v2" and core.resolve_view("follow", dict(live, waveform=1)) == "v1"
          and core.resolve_view("follow", dict(live, waveform=0)) == "all" and core.resolve_view("follow", None) == "all"
          and core.resolve_view("v1", st) == "v1", "Follow shows the radio's mode (all while RADE is off or piHPSDR is gone)")
    t, why, rk, vk = core.plan(cfg, st, False, "EI6IZ", "IO53hu", "follow")
    check(vk == ["v2"], "plan watches only V2 when following a V2 radio")
    S = core.Station("a"); S.rx_snr = 5.0; S.rx_call = "DL1ABC"; S.rx_time = 1000.0
    check(S.receiving_now(1015.0) and not S.receiving_now(1021.0), "receiving (blue): 20 s after a report with a call")
    S.rx_call = ""
    check(S.receiving_now(1004.0) and not S.receiving_now(1006.0), "... 5 s after one without a call")
    S.rx_call = "DL1ABC"; S.rx_snr = None
    check(not S.receiving_now(1001.0), "no SNR: not blue")
    S.rx_snr = 5.0; S.rx_only = True
    check(not S.receiving_now(1001.0), "receive-only status: not blue")
    S.rx_only = False; S.transmitting = True
    check(not S.receiving_now(1001.0), "transmitting: not blue")
    check(c2["view"] == "follow" and c2["servers"]["v2"]["host"] == "reporter-radev2.k6aq.net", "defaults: follow the radio mode; V2 host set")
    d = core.decide(cfg, dict(live, waveform=0), True, "EI6IZ", "IO53hu", "v1")
    check(d[1] == "view" and "RADE is off" in d[5], "RADE off: not present")
    d = core.decide(cfg, dict(live, linked=False), True, "EI6IZ", "IO53hu", "v1")
    check(d[1] == "view" and "not running" in d[5], "piHPSDR gone: not present")
    d = core.decide(cfg, live, True, "", "IO53hu", "v1")
    check(d[1] == "view", "no callsign: not present")
    d = core.decide(cfg, live, True, "EI6IZ", "nonsense", "v1")
    check(d[1] == "view", "bad grid: not present")
    d = core.decide(cfg0, None, False, "", "", "v2")
    check(d[1] == "view" and d[0] is None, "watching a server with no address: nothing")
    p = os.path.join(tempfile.mkdtemp(), "state.json")
    json.dump({"running": 1, "updated": 1000.0, "waveform": 1}, open(p, "w"))
    check(core.read_pihpsdr_state(p, now=1003.0)["linked"] is True, "state 3 s old: linked")
    check(core.read_pihpsdr_state(p, now=1020.0)["linked"] is False, "state 20 s old: piHPSDR gone")
    json.dump({"running": 0, "updated": 1000.0}, open(p, "w"))
    check(core.read_pihpsdr_state(p, now=1001.0)["linked"] is False, "running=0: gone at once")
    cp = os.path.join(tempfile.mkdtemp(), "c.json")
    core.save_config(dict(cfg, reporting=True, callsign="X"), cp)
    check("reporting" not in json.load(open(cp)) and "reporting" not in core.load_config(cp),
          "reporting is never saved: always starts off")


def heard():
    print("calls heard (rx_report): every one, every time")
    now = 1000.0
    h = lambda seq, call, t=990.0, snr=7.4: {"seq": seq, "call": call, "snr": snr, "t": t}
    p = {"linked": True, "heard": [h(3, "vk5dgr")]}
    check(core.rx_reports_due(p, None, "EI6IZ", now) == [], "first look: what is there is old news")
    check(core.rx_reports_due(p, 3, "EI6IZ", now) == [], "already dealt with: not again")
    check(core.rx_reports_due(p, 2, "EI6IZ", now) == [("VK5DGR", 7.4, 3)], "a new call is due, upper case")
    p2 = {"linked": True, "heard": [h(4, "VK5DGR"), h(5, "OH2BCD", snr=-3.0), h(6, "VK5DGR")]}
    check(core.rx_reports_due(p2, 3, "EI6IZ", now) == [("VK5DGR", 7.4, 4), ("OH2BCD", -3.0, 5), ("VK5DGR", 7.4, 6)],
          "three calls since the last look, the same station twice: all three, in order")
    check(core.rx_reports_due(p2, 5, "EI6IZ", now) == [("VK5DGR", 7.4, 6)], "only those after the last one dealt with")
    check(core.rx_reports_due({"linked": True, "heard": [h(7, "VK5DGR", t=900.0)]}, 6, "EI6IZ", now) == [], "a stale call is not reported")
    check(core.rx_reports_due({"linked": True, "heard": [h(7, "EI6IZ")]}, 6, "EI6IZ", now) == [], "our own call is not reported")
    check(core.rx_reports_due({"linked": True, "heard": [h(7, "")]}, 6, "EI6IZ", now) == [], "no call, nothing to report")
    check(core.rx_reports_due(dict(p2, linked=False), 3, "EI6IZ", now) == [], "piHPSDR gone: nothing")
    old = {"linked": True, "heard_seq": 9, "heard_call": "g4abc", "heard_snr": 5.0, "heard_t": 990.0}
    check(core.rx_reports_due(old, 8, "EI6IZ", now) == [("G4ABC", 5.0, 9)], "an older piHPSDR (latest call only) still works")


def periodic():
    print("regular SNR reports (as freedv-gui sends them)")
    pr = core.PeriodicSnr(interval=5.0, reset_s=10.0)
    p = {"linked": True, "tx": 0, "rx_sync": 1, "rx_snr": 7.4, "rx_foff": 1.0}
    r = pr.step(p, 100.0)
    check(r[0] is True and r[1] == "" and r[2] == 7.4, "in sync with no call decoded yet: sent at once, with no callsign")
    check("no call decoded yet" in r[3], "  and the window says that")
    check(pr.step(p, 103.0)[0] is False and pr.step(p, 105.1)[0] is True, "then every 5 s")
    pr.heard("VK5DGR", 106.0)
    check(pr.step(p, 108.0)[0] is False, "the end-of-over report has just gone: a full interval to the next")
    r = pr.step(dict(p, rx_snr=-2.0), 111.5)
    check(r[0] is True and r[1:3] == ("VK5DGR", -2.0), "afterwards the live SNR goes with the call last decoded")
    check(pr.step(dict(p, rx_sync=0), 120.0)[0] is False and "waiting for a signal" in pr.step(dict(p, rx_sync=0), 120.0)[3],
          "no sync: nothing")
    check(pr.step(p, 125.0)[1] == "VK5DGR" or pr.step(p, 126.0)[1] == "VK5DGR",
          "sync back after under 10 s: still the same call")
    check(pr.step(dict(p, rx_sync=0), 140.0)[0] is False, "sync lost again")
    r = pr.step(p, 151.0)
    check(r[0] is True and r[1] == "", "sync lost for over 10 s: the call is forgotten, reports go blank")
    check(pr.step(dict(p, tx=1), 160.0)[0] is False and "paused while transmitting" in pr.step(dict(p, tx=1), 160.0)[3],
          "transmitting: nothing")
    pr.enabled = False
    check(pr.step(p, 170.0)[0] is False, "switched off: nothing")
    pr.enabled = True
    pr.reset()
    check(pr.call == "" and pr.last is None, "reset (not reporting): forgets the call")
    # no lock, no SNR: a missing SNR is never turned into 0, a real 0 or a negative SNR is reported
    pz = core.PeriodicSnr(interval=5.0)
    r = pz.step(dict(p, rx_sync=0, rx_snr=None), 200.0)
    check(r[0] is False, "no sync and no SNR (null from piHPSDR): nothing sent")
    r = pz.step(dict(p, rx_sync=0, rx_snr=0.0), 201.0)
    check(r[0] is False, "no sync (an SNR of 0.0 is only what librade says when it has no lock): nothing sent")
    r = pz.step(dict(p, rx_sync=1, rx_snr=None), 202.0)
    check(r[0] is False and "no SNR" in r[3], "in sync but no SNR yet: nothing sent, and it says so")
    r = pz.step(dict(p, rx_sync=1, rx_snr=0.0), 203.0)
    check(r[0] is True and r[2] == 0.0, "locked at an SNR of exactly 0 dB: reported as 0")
    r = pz.step(dict(p, rx_sync=1, rx_snr=-4.0), 209.0)
    check(r[0] is True and r[2] == -4.0, "locked at a negative SNR: reported")
    now = 1000.0
    hh = lambda seq, call, snr, t=990.0: {"seq": seq, "call": call, "snr": snr, "t": t}
    check(core.rx_reports_due({"linked": True, "heard": [hh(10, "VK5DGR", None)]}, 9, "EI6IZ", now) == [],
          "a call decoded with no SNR behind it (null) is not reported")
    check(core.rx_reports_due({"linked": True, "heard": [hh(10, "VK5DGR", 0.0), hh(11, "G4ABC", -3.5)]}, 9, "EI6IZ", now)
          == [("VK5DGR", 0.0, 10), ("G4ABC", -3.5, 11)], "calls heard at 0 and at a negative SNR are reported")
    check(core.rx_reports_due({"linked": True, "heard_seq": 4, "heard_call": "G4ABC", "heard_snr": None, "heard_t": 990.0},
                              3, "EI6IZ", now) == [], "an older piHPSDR's latest call with no SNR: not reported")
    check(core.Link(core.Roster(), lambda n, d: None, lambda st, m: None, "x").report_rx("G4ABC", "RADEV1", None) is False,
          "report_rx refuses an SNR of None")
    # what the server sends about a station: a blank report (no call, no mode, snr 0) is "nothing heard", as freedv-gui treats it
    ro = core.Roster()
    ro.event("new_connection", {"sid": "x", "callsign": "EI6IZ", "grid_square": "IO53", "version": "v", "last_update": "", "connect_time": ""})
    ro.event("rx_report", {"sid": "x", "callsign": "", "mode": "", "snr": 0, "receiver_callsign": "EI6IZ"})
    st = ro.snapshot()[0]
    check(st.rx_snr is None and st.rx_call == "" and st.rx_mode == "", "a blank rx_report (no call, no mode, snr 0) clears the SNR: it is not an SNR of 0")
    ro.event("rx_report", {"sid": "x", "callsign": "", "mode": "RADEV1", "snr": 12})
    check(ro.snapshot()[0].rx_snr == 12, "an SNR-only report (blank call, a mode) is an SNR: 12")
    ro.event("rx_report", {"sid": "x", "callsign": "", "mode": "RADEV1", "snr": 0})
    check(ro.snapshot()[0].rx_snr == 0, "an SNR-only report of 0 is an SNR of 0 (the receiver is locked at 0 dB)")
    ro.event("rx_report", {"sid": "x", "callsign": "", "mode": "", "snr": 0})
    check(ro.snapshot()[0].rx_snr is None, "and a blank one clears it again")
    ro.event("bulk_update", [["rx_report", {"sid": "x", "callsign": "G4ABC", "mode": "RADEV1", "snr": -3}], ["rx_report", {"sid": "x", "callsign": "", "mode": "", "snr": 0}]])
    check(ro.snapshot()[0].rx_snr is None, "the same inside a bulk_update")
    # reporting is only possible with piHPSDR connected and a valid frequency
    ok, why = core.report_gate({"linked": True, "dial_hz": 28645000})
    check(ok, "connected, 28.645 MHz: may report")
    for bad in (0, -1, None, "28645000", float("nan"), True, 50, 1e12):
        ok, why = core.report_gate({"linked": True, "dial_hz": bad})
        check(not ok and "frequency" in why, "dial %r: no reporting (%s)" % (bad, why))
    check(not core.report_gate(None)[0] and not core.report_gate({"linked": False, "dial_hz": 14236000})[0], "piHPSDR not connected (no state, or stale): no reporting")
    cfgg = core.load_config(os.path.join(tempfile.mkdtemp(), "none.json"))
    d = core.decide(cfgg, {"linked": True, "waveform": 1, "dial_hz": 0, "tx": False}, True, "EI6IZ", "IO53hu", "v1")
    check(d[1] == "view" and "frequency" in d[5], "decide() reports nothing for a dial of 0: %s" % d[5])
    check(core.PeriodicSnr(1.0).interval == 1.0, "interval is settable (freedv-gui itself sends about once a second)")


def link_flow():
    print("link to a (mock) reporter")
    s1, s2 = MockServer().start(), MockServer().start()
    cfg = core.load_config(os.path.join(tempfile.mkdtemp(), "none.json"))
    cfg["servers"]["v1"].update(host="127.0.0.1", port=s1.port, secure=False)
    cfg["servers"]["v2"].update(host="127.0.0.1", port=s2.port, secure=False)
    roster = core.Roster()
    states = []
    link = core.Link(roster, lambda n, d: None, lambda st, m: states.append(st), "pihpsdr-rade test")
    live = {"linked": True, "waveform": 1, "dial_hz": 7053000, "tx": False}

    def go(p, reporting=True, view="v1"):
        srv, role, mode, freq, tx, why = core.decide(cfg, p, reporting, "EI6IZ", "IO53hu", view)
        link.set_target(srv, role, "EI6IZ", "IO53hu", freq, mode, tx, "QRV for a CQ")
        return why

    go(live, reporting=False)
    check(wait(lambda: s1.clients and s1.clients[0]["auth"] is not None), "watching: connects")
    check(s1.clients[0]["auth"].get("role") == "view" and "callsign" not in s1.clients[0]["auth"],
          "watching sends no identity")
    check(wait(lambda: len(roster.snapshot()) == 1), "stations from the server appear")
    st = roster.snapshot()[0]
    check((st.callsign, st.freq, st.mode, st.transmitting, st.status) == ("VK5DGR", 7053000, "RADEV1", True, "Transmitting"),
          "station data and status")
    time.sleep(0.4)
    check(s1.clients[0]["events"] == [], "watching emits nothing")

    go(live, reporting=True)
    check(wait(lambda: len(s1.clients) == 2 and s1.clients[1]["auth"] is not None), "reporting on: reconnects as reporter")
    a = s1.clients[1]["auth"]
    check(a.get("role") == "report" and a.get("callsign") == "EI6IZ" and a.get("grid_square") == "IO53hu"
          and a.get("rx_only") is False and a.get("protocol_version") == 2 and a.get("version") == "pihpsdr-rade test",
          "reporter identity: " + json.dumps(a))
    ev = s1.clients[1]["events"]
    check(wait(lambda: len(ev) >= 2), "sends its state once the server says connection_successful")
    check(["freq_change", {"freq": 7053000}] in ev and ["tx_report", {"mode": "RADEV1", "transmitting": False}] in ev,
          "freq_change 7053000 and tx_report RADEV1/not transmitting")
    check(not any(e[0] == "rx_report" for e in ev), "no receive reports until a call is heard")
    check(["message_update", {"message": "QRV for a CQ"}] in ev, "the stored message goes out with the connection")
    go(dict(live, tx=True))
    check(wait(lambda: ["tx_report", {"mode": "RADEV1", "transmitting": True}] in ev), "transmit start is reported")
    go(dict(live, tx=True, dial_hz=7054000))
    check(wait(lambda: ["freq_change", {"freq": 7054000}] in ev), "frequency change is reported")
    n = len(ev)
    go(dict(live, tx=True, dial_hz=7054000))
    time.sleep(0.3)
    check(len(ev) == n, "unchanged state is not sent again")
    go(dict(live, tx=False, dial_hz=7054000))
    check(wait(lambda: ev.count(["tx_report", {"mode": "RADEV1", "transmitting": False}]) == 2), "transmit end is reported")

    check(link.report_rx("VK5DGR", "RADEV1", 7.4) is True, "a heard call is reported while present")
    check(wait(lambda: ["rx_report", {"callsign": "VK5DGR", "mode": "RADEV1", "snr": 7}] in ev),
          "as rx_report with callsign, mode and whole-dB SNR")
    go(dict(live, waveform=2, dial_hz=14236000))
    check(wait(lambda: s1.clients[1]["closed"] or s1.clients[1]["left"]), "V2 selected: left the V1 reporter")
    check(wait(lambda: s2.clients and s2.clients[0]["auth"] is not None), "joined the v3 reporter")
    check(s2.clients[0]["auth"].get("role") == "report", "as a reporter")
    check(wait(lambda: ["tx_report", {"mode": "RADEV2", "transmitting": False}] in s2.clients[0]["events"]),
          "reported as RADEV2")
    check(["message_update", {"message": "QRV for a CQ"}] in s2.clients[0]["events"], "the message goes to the second reporter too")
    check(link.pulse_tx("RADEV2") and wait(
        lambda: s2.clients[0]["events"].count(["tx_report", {"mode": "RADEV2", "transmitting": True}]) == 1
        and s2.clients[0]["events"][-1] == ["tx_report", {"mode": "RADEV2", "transmitting": False}]),
        "a transmission that came and went between two looks is told as on, then off")
    check(link.report_rx("VK5DGR", "RADEV2", -3.6) and wait(
        lambda: ["rx_report", {"callsign": "VK5DGR", "mode": "RADEV2", "snr": -3}] in s2.clients[0]["events"]),
        "V2: reported on the v3 reporter; SNR as freedv-gui rounds it, int(snr + 0.5)")
    check(link.report_rx("", "RADEV2", 4.2) and wait(
        lambda: ["rx_report", {"callsign": "", "mode": "RADEV2", "snr": 4}] in s2.clients[0]["events"]),
        "a report with no callsign goes out as it is")
    go(dict(live, waveform=0))
    check(wait(lambda: s2.clients[0]["closed"] or s2.clients[0]["left"]), "RADE off: no longer present on v3")
    check(link.report_rx("VK5DGR", "RADEV1", 5.0) is False, "watching only: a heard call is not reported")
    check(wait(lambda: len(s1.clients) == 3 and s1.clients[2]["auth"] is not None
               and s1.clients[2]["auth"].get("role") == "view"), "watching again, no identity")
    go(dict(live, waveform=1))
    check(wait(lambda: len(s1.clients) == 4), "V1 again with reporting still on: present again")
    check(wait(lambda: ["message_update", {"message": "QRV for a CQ"}] in s1.clients[3]["events"]),
          "the message is sent again on the new connection")
    go(None)
    check(wait(lambda: s1.clients[3]["closed"] or s1.clients[3]["left"]), "piHPSDR gone: no longer present")
    link.close()
    s1.stop(); s2.stop()


def live():
    print("real reporter, watching only")
    roster = core.Roster()
    st = []
    link = core.Link(roster, lambda n, d: None, lambda s, m: st.append(s), "pihpsdr-rade test")
    link.set_target(core.DEFAULT_SERVERS["v1"], "view")
    ok = wait(lambda: len(roster.snapshot()) > 0, 15)
    check(ok, "qso.freedv.org lists stations (%d)" % len(roster.snapshot()))
    if ok:
        s = roster.snapshot()[0]
        print("       e.g. %s %s %.4f MHz %s %s" % (s.callsign, s.grid, s.freq / 1e6, s.mode, s.status))
    link.close()


if __name__ == "__main__":
    places()
    decisions()
    heard()
    periodic()
    link_flow()
    if "--live" in sys.argv:
        live()
    print("FAILED %d" % fails if fails else "all passed")
    sys.exit(1 if fails else 0)
