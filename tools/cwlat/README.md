# cwlat - external CW sidetone latency / jitter measurement

Measures, from outside piHPSDR, the time from a MIDI paddle event to the sidetone entering the
PipeWire sink, with jitter, element lengths, dropped events and double elements.

* sender: ALSA sequencer note events (CWR=20, CWL=21, PTT=22, channel 0) into a VirMIDI port
* receiver: PipeWire capture of the sink monitor, each capture callback time-stamped
  with CLOCK_MONOTONIC (the same clock as the MIDI send time-stamps)
* nothing is printed during a run, so the instrument does not disturb piHPSDR

## Setup
1. `sudo modprobe snd-virmidi`; `aconnect -l` shows `Virtual Raw MIDI 4-0` etc.
2. In piHPSDR's MIDI menu enable the matching device (`hw:4,0` is seq port `32:0`, the default
   destination). `cat /proc/asound/card4/midi0` shows the owner PID when it is open.
3. `tools/cwlat/build.sh`
4. PA off, drive zero, CW mode, operator present.

## Check the analysis without a radio
    ./cwlat --selftest --test dit --trials 60

## One run
    ./cwlat --confirm-safe --sink alsa_output.usb-Arylic_Up2Stream_USB_Audio_20190808-00.analog-stereo \
            --test dit --label "synth iambicB breakin Q64" --csv dit.csv

Tests: `straight` (CWR held, tone length vs hold), `dit` / `dah` (tap sweep 20-140 ms, shows where the
keyer produces a second element), `squeeze` (both paddles). `hang` and `blip` are described below. `--ptt` holds PTT for the run (no break-in
transitions); without it, about 20 % of gaps are long pauses so break-in has to go to TX again ("cold").

## Matrix
    ./run_matrix.sh --sink <sink> [--paths synth,old] [--keys straight,iambicA,iambicB]
                    [--modes breakin,ptt] [--quanta 64,128,256]

piHPSDR settings (Synth sidetone, Paddle Mode, break-in) are set by the operator when prompted; the
quantum is set by the script with `clock.force-quantum` and restored at the end.

## Reading the numbers
* "onset latency" is MIDI write -> tone at the sink monitor. The 10 % / 50 % points lie 1.0 / 2.5 ms
  into the 5 ms ramp. Add the sink's own latency (one quantum plus headroom, from `pw-dump`)
  for the ALSA input. USB transfer and DAC are not covered, and the virtual MIDI port skips the
  USB-MIDI hop of the real controller.
* "warm" = first tone came within `--cold-ms` of the previous tone; "cold" = first element after a pause.
* Run the selftest after any change to the analysis: it must show lat10 about 6.9 ms and lat50 about 8.5 ms.

## Hang test (`--test hang`)
Pairs of dit taps: the first follows a long pause (cold), the second comes D ms after the keyer has finished the first
(press + dot + gap), D = 100 ... 1400 ms in steps of 50. The report lists, per D, the second tap's sink latency, whether
it is cold-like (the radio had dropped out of TX) and whether it produced no element at all. It needs Iambic, break-in on,
radio in RX, no PTT mode, and the hang time set in pihpsdr. Taps are lost in two places (see `docs/cw_keyer_first_tap.md`):
cold starts, and a window of about 100 ms after the hang time has expired.

    ./capture_run.sh --kiwi kiwi.minish.org:8074 --freq 28187 --dir results/<group> --name <path>_<key>_<mode>_Q<q>_hang \
        --label "<text>" -- --confirm-safe --sink <node> --dest 32:0 --wpm 22 --test hang --trials 120 --label "<text>"

`cwlat_analyze.py` takes path, key, mode, quantum and test from the capture name (`synth_iambicB_breakin_Q128_hang`);
other names make it stop with an IndexError. RF and sink are both missing for a lost tap, so "RF only" / "sink only"
in the analysis separate keyer losses from sidetone losses.

## Short straight-key taps (`--test short`)
Straight key (CWR) holds of 5, 10, 15, 20, 25, 30, 35, 40 and 50 ms, each after a pause of 1.2 to 1.8 s, so that every tap
arrives while the keyer is switching from RX to TX. Without LC-066 the taps that are over before TX is up give no element. With it
each gives an element that is late by the switch, as long as the hold (never shorter than one RF ramp), with the usual 5 ms edges.
The analysis reports tone length against hold. Use 90 trials (10 per hold length).
`analysis/cwlat_short_bursts.py LABEL=CAPTURE_DIR ...` classifies each tap from the carrier envelope as full amplitude, a burst that never reaches the plateau, or nothing above the noise floor (the standard analysis counts a burst as missing).

## Blip test (`--test blip`)
Checks that the keyer does not start a transmission for a contact-bounce blip (LC-065 records short taps in paddle modes only).
Pairs of trials: a first tap, then a 60 ms probe tap 150 ms after the press of the first, then a 2.5 s pause. The first tap is a
real 60 ms tap (reference), or a blip of 1, 2 or 3 ms. A blip is shorter than the RX to TX transition and gives no element; if
the keyer started TX for it anyway, the probe is warm (about 20 ms earlier than the cold reference). Straight key (CWR) with
break-in; compare an unchanged build with the one under test. The sink report lists, per blip length, how many probes were
cold-like; the RF side is in `cwlat_analyze.py` output (`blip test, RF`) and the trials CSV. Blips are not counted as missing taps.

## Recording of the sink monitor (`--sink-wav FILE`)
`capture_run.sh` passes it: the recorded sink monitor is saved as mono float32 WAV at 48 kHz with `FILE.cb.csv` (first sample and
CLOCK_MONOTONIC time of each capture callback), so a tap that is present at RF but absent at the sink can be looked at afterwards
instead of only counted.

## Known issue: PTT-mode runs leave pihpsdr's PTT stuck
Runs with `--ptt` (PTT held by MIDI note 22 while paddle notes are sent) leave pihpsdr's PTT asserted
afterwards (seen 2026-11-06). A PTT-only press/release does not, and break-in runs never do. Operators use
MIDI PTT with CW daily without it sticking, so the fault is in this tool's MIDI output (assumptions about
the message stream, event spacing, release order, or a race), not in pihpsdr. **Fix this before further
PTT-mode testing.** Until then clear PTT by hand after each PTT-mode run. Suggested first step: record the
real controller's bytes (`amidi -p hw:3,0,0 -d`) for a PTT + paddle sequence and compare with what this
tool sends.

## Analysis of the KiwiSDR captures
    python3 -I analysis/cwlat_analyze.py run results/<group>/captures/<name> --plot     # one capture
    python3 -I analysis/cwlat_analyze.py all results/iambicB-subset results/ptt-and-straight --out results/analysis
    python3 -I analysis/cwlat_report.py results/analysis                                  # repeat curve, cold start, clipping, plots
`cwlat_analyze.py` puts the Kiwi IQ on UTC from the GNSS stamps, finds the keyed carrier by correlating power with the times keyed,
and matches RF elements to the MIDI events: latency, jitter, element length against commanded or nominal (truncation / clipping),
rise / fall time, missing and extra elements, and the RF-versus-sidetone offset. Quarantined runs (`_invalid*`) are skipped.
Results are summarised in `docs/cw_sidetone-findings.md`.
