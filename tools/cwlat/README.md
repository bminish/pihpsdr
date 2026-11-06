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
keyer produces a second element), `squeeze` (both paddles). `--ptt` holds PTT for the run (no break-in
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

## Known issue: PTT-mode runs leave pihpsdr's PTT stuck
Runs with `--ptt` (PTT held by MIDI note 22 while paddle notes are sent) leave pihpsdr's PTT asserted
afterwards (seen 2026-11-06). A PTT-only press/release does not, and break-in runs never do. Operators use
MIDI PTT with CW daily without it sticking, so the fault is in this tool's MIDI output (assumptions about
the message stream, event spacing, release order, or a race), not in pihpsdr. **Fix this before further
PTT-mode testing.** Until then clear PTT by hand after each PTT-mode run. Suggested first step: record the
real controller's bytes (`amidi -p hw:3,0,0 -d`) for a PTT + paddle sequence and compare with what this
tool sends.
