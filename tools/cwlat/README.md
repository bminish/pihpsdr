# cwlat - external CW sidetone latency / jitter measurement (LT-038)

Measures, from outside piHPSDR, the time from a MIDI paddle event to the sidetone entering the
PipeWire sink, with jitter, element lengths, dropped events and double elements. With a KiwiSDR
(`capture_run.sh`) it also records the RF. Index of the CW work: `docs/cw.md`.

* sender: ALSA sequencer note events (CWR=20, CWL=21, PTT=22, channel 0; for `--test ext` also the
  key 23 and the keyer's PTT 24) into a VirMIDI port
* receiver: PipeWire capture of the sink monitor, each capture callback time-stamped
  with CLOCK_MONOTONIC (the same clock as the MIDI send time-stamps)
* nothing is printed during a run, so the instrument does not disturb piHPSDR

## Setup
1. `sudo modprobe snd-virmidi`; `aconnect -l` shows `Virtual Raw MIDI 4-0` etc.
2. In piHPSDR's MIDI menu enable the matching device (`hw:4,0` is seq port `32:0`, the default
   destination). `cat /proc/asound/card4/midi0` shows the owner PID when it is open.
3. `tools/cwlat/build.sh`
4. Radio into a dummy load at low drive (the measured runs: 1 %), CW mode, operator present.

## Check the analysis without a radio
    ./cwlat --selftest --test dit --trials 60

## One run
    ./cwlat --confirm-safe --sink alsa_output.usb-Arylic_Up2Stream_USB_Audio_20190808-00.analog-stereo \
            --test dit --label "synth iambicB breakin Q64" --csv dit.csv

Tests (`--test`): `straight` (CWR held, tone length vs hold), `dit` / `dah` (tap sweep 20-140 ms, shows where the
keyer produces a second element), `squeeze` (both paddles), `short`, `hang`, `blip` and `ext` (below). `--ptt` holds PTT (note 22) for the run (no
break-in transitions, the keyer must have break-in off); without it, about 20 % of gaps are long pauses so break-in has to go to TX again ("cold").

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

The grid of D can be changed with `--hang-from MS --hang-to MS --hang-step MS` (default 100 1400 50, at most 64 values; the
smallest D values are the warm reference, so start where the keyer is still warm), e.g. `--hang-from 250 --hang-to 500 --hang-step 10
--trials 104` to resolve the expiry at hang 600.

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

## The sink must be silent between taps
cwlat finds the side tone in the sink monitor with a 550 Hz envelope and a threshold at half of the recording's 99.5 % level. Receiver audio on the same sink raises the envelope between taps: on 2026-11-09, with the AF gain at its minimum (which is not a mute: about 4e-3 rms on the sink, against a side tone peak of 1e-2), 10 of 36 cold-start tones were reported as "dropped" although they were in the recording (`--sink-wav` showed them 22 to 38 ms after the press), and the 10 % points are unreliable. A run with a noisy sink shows as "sink only" losses in the analysis (RF present, sink missing) concentrated on the first tap after a pause, because receiver audio is off while TX is up. Mute the receiver audio (not the side tone), or check the sink first: its monitor should be at about -80 dBFS between taps. Since 2026-11-09 the scan resumes 3 ms after the end of a tone instead of where the level falls below 10 %, which removes the worst of it but not all.

## External keyer test (`--test ext`)
Plays what an external keyer does: PTT on (note 24), the key down on note 23 after a lead of 5, 30 or 100 ms (5: before TX is up), a hold of 40, 60 or 80 ms, key up, PTT off 60 ms later; the next trial comes 120, 250 or 380 ms after the key-up, so the first key-down carries a wait of 120 to 480 ms, below the 500 ms after which `CW_KEYER_KEYDOWN` sends no wait. In piHPSDR map note 23 to "CW Key (Keyer)" (`CWKy`) and note 24 to "PTT (CW Keyer)" (`CWKyPTT`) in the MIDI menu (note 22 stays the footswitch `PTT`). It exercises the path the side tone's idle state (`sidetone_idle()`) is for: the first event after TX start must not wait for its gap again (verified on air, `docs/cw_sidetone-findings.md`). The tone length is compared with the hold (as for the straight key). The PTT events belong to no trial. Not for PTT-held runs (`--ptt`): its PTT is per trial.

## `midisend`: teaching a note, releasing a stuck key
`gcc -O2 -Wall -o midisend midisend.c -lasound`; `midisend --note 23 --hold-ms 1000 --gap-ms 500` sends the note repeatedly (until interrupted or `--stop-file` appears) for piHPSDR's MIDI menu to learn; `midisend --note 22 --off` sends only a Note Off. It sends the way cwlat does (see the next section).

## Running status: what the old PTT-mode and squeeze results were (found and fixed 2026-11-09)
The virtual MIDI port turns the sequencer events into a raw byte stream **with running status**: a message with the same status as the one before it is sent without its status byte (`90 18 64` then `17 64`, not `90 17 64`). piHPSDR's parser (`parse_midi_byte()` in `midi2.c`) does not support running status and drops such a message. Real controllers are not affected: USB-MIDI sends a full status with every message.
Consequences for cwlat before 2026-11-09:
* two note-ons in a row lost the second: PTT on then a paddle (**the first tap after PTT was lost**, 24 of 24 in the PTT runs), or CWL then CWR in the **squeeze** test (the dash paddle was never pressed: every squeeze result so far is a single-paddle result);
* two note-offs in a row lost the second: a PTT release after a paddle release, and the three note-offs of `release_all()` at the end of a run (**PTT left stuck**).
dit, dah, straight, short, hang and blip alternate on and off and were not affected (the status byte of a note-on and of a note-off differ).
`midi_send()` and `midisend` now send a Program Change in front of every note, which changes the status, so every note carries its own status byte (checked on a spare virtual port: `C0 00 90 18 64 C0 00 90 17 64 C0 00 80 17 00 ...`); piHPSDR ignores Program Change. Squeeze and PTT mode were re-run on 2026-11-09 and are valid; results made before that date are not.

## Analysis of the KiwiSDR captures
    python3 -I analysis/cwlat_analyze.py run results/<group>/captures/<name> --plot     # one capture
    python3 -I analysis/cwlat_analyze.py all results/iambicB-subset results/ptt-and-straight --out results/analysis
    python3 -I analysis/cwlat_report.py results/analysis                                  # repeat curve, cold start, clipping, plots
`cwlat_analyze.py` puts the Kiwi IQ on UTC from the GNSS stamps, finds the keyed carrier by correlating power with the times keyed,
and matches RF elements to the MIDI events: latency, jitter, element length against commanded or nominal (truncation / clipping),
rise / fall time, missing and extra elements, and the RF-versus-sidetone offset. Quarantined runs (`_invalid*`) are skipped.
Results are summarised in `docs/cw_sidetone-findings.md`.
