# RADE V1: filtering and mode changes, from our own captures

2026-10-16. Branch `feature/rade-native`. Scripts are in `test/rade_native/`; every number below can be
reproduced with them (commands at the end).

The principle this is judged against: **the centre of the transmitted energy stays put** through every
change of mode, so that the signal is always in the same general passband. For this purpose V1 is taken to
sit 1.5 kHz off the carrier, like V2 (the modem's carriers are 750..2200 Hz, centre 1475; V2 1062..1938,
centre 1500).

## Summary

1. **The transmitted spectrum was reshaped, and the centre of energy was 190 Hz too high.** In all three
   TX captures the modem's energy centre is +1650 Hz, not +1457 Hz, and the bottom third of the carriers is
   about 5 dB (the lowest ones 6 to 10 dB) below the top third. Cause: the station's SSB profile (leveler,
   CFC with its post-equaliser) was switched back on during RADE transmit. Reproduced offline to within
   2 dB. **Fixed in the code** (below); the effect on decoding is small (see 1b).
2. **The 1.6 kHz filters are the right size.** The TX filter passes the outermost carriers at 0 dB and is
   98 dB down at the edges of a 300..2700 Hz SSB channel. On the RX side a 1.4 kHz filter and a 1.6 kHz filter
   give the same decode; even 1.0 kHz costs only 0.006 in vocoder-feature error. Widening gains nothing.
3. **What limits tuning tolerance is librade, not the filter**: V1 decodes fully up to about +/-55..65 Hz off
   and not beyond, for any filter width from 0.9 to 2.4 kHz.
4. **Changing mode at a fixed dial moved the transmitted energy by 3 kHz** in four of the common cases
   (invert on/off, and V1 -> V2 below 10 MHz), the opposite of the principle. Now the dial moves instead, and
   RADE off hands back to USB or LSB on the same carrier; see "Mode changes".

## What was scored

13 receive captures (`_rx48.wav`, 55 to 628 s each; 14.236 and 18.118 MHz normal, 1.987 MHz inverted) and
the 3 transmit captures (`_tx.wav` at 192 kHz, 3 s, 144 s and 195 s of transmitting, all 1.987 MHz LSB,
inverted). Receive, from the capture's own stats: sync 37 to 100 % of the capture, median SNR -2.3 to 11.7 dB,
callsigns decoded from an end-of-over frame in 3 of the 13 (that needs about 10 dB). The off-air Kiwi sessions
so far contain no transmission, so they say nothing about filtering.

## 1. The transmit spectrum

`tx_shape.py` on the three captures (all three the same):

| | measured | what librade makes for the same speech |
|---|---|---|
| energy centre | **+1649 .. +1651 Hz** | +1454 .. +1457 Hz |
| top third - bottom third of the carriers | **+4.7 .. +4.9 dB** | -0.1 dB |
| lowest carriers vs middle | **-10 dB** | -1.5 dB |
| 100..500 Hz outside the carriers, vs the carriers | -30 to -38 dB | |
| beyond 1 kHz out | -127 to -131 dB | |

Per 100 Hz, relative to the 750..2100 mean (dB):

```
                 600  700  800  900 1000 1100 1200 1300 1400 1500 1600 1700 1800 1900 2000 2100 2200
librade, same speech  -10   -1    0    0    0    0    0    0    0    0    0    0    0    0    0    0   -5
real capture          -24  -10   -6   -3   -2   -1    0    1    1    1    2    2    2    2    2    2   -4
speech stages on      -24   -6   -3   -2   -1   -1    0   -1   -1    0    0    1    2    3    2    2   -4
(offline, same filter)
```

The last row is our own TXA run with the leveler and the CFC *with its post-equaliser* set as this station's
SSB profile has them (post-EQ -6, -12, -11, -11, 0, 0, +2, +1 dB at 50, 100, 200, 500, 1000, 1500, 2000,
2500 Hz): it has the same shape as the real capture, and the same -24 dB at 600 Hz. With the stages neutral
(as designed) the offline output is flat (+/-1 dB). The shape is constant over the whole 195 s, so it is a
fixed filter in the chain, not level-dependent.

**Why the stages were on**: `tx_xmit_captured_data_start()` switches them off once, when RADE transmit is
turned on, but piHPSDR re-applies the mode profile (`profiles_load_tx_profile()` -> `tx_set_compressor()`,
`tx_set_equalizer()`, `tx_set_phrot()`, `tx_set_dexp()`) whenever the mode changes and when TUNE ends, and
that put the profile's CFC, leveler and phase rotator back.

**Fixed**: while RADE transmit is on, those four functions keep the profile values for later but leave the
stages off (`transmitter.c`), and the phase rotator is switched off with the rest. **Not run on a radio**: it
compiles and the logic is a guard at the top of each function; the check is the next capture, with
`tx_shape.py` (PASS needs the centre within 75 Hz of +1457 and the tilt within 3 dB; the three captures
above fail it, a clean offline run passes at +1468 Hz and -0.1 dB).

**1b. How much it mattered**: little. The same 40 s of speech, flat vs tilted, noise added, decoded by librade:
feature error 0.038 vs 0.041 at 0 dB SNR, 0.027 vs 0.028 at 6 dB, 0.025 vs 0.026 at 9 dB (in 3 kHz); sync 97 %
either way. White noise only; a fading channel could be less forgiving. It still breaks the principle (190 Hz
of centre) and should not be there.

## 2. Is 1.6 kHz right?

**Transmit** (WDSP TXA, white noise in; dB re the middle of the passband):

| filter | 750 Hz carrier | 2200 Hz carrier | at 300 Hz | at 2700 Hz | at +/-3 kHz |
|---|---|---|---|---|---|
| 775..2175 (1.4 kHz) | -12.0 | -10.9 | -103 | -105 | -115 |
| 725..2225 (1.5 kHz) | -2.0 | -1.9 | -101 | -103 | -113 |
| **675..2275 (1.6 kHz)** | **+0.2** | **0.0** | **-98** | **-100** | **-112** |
| 575..2375 (1.8 kHz) | +0.3 | +0.1 | -91 | -95 | -109 |
| 475..2475 (2.0 kHz) | +0.3 | +0.1 | -82 | -87 | -105 |
| 275..2675 (2.4 kHz) | +0.3 | +0.1 | -2 | -12 | -94 |

1.6 kHz is the narrowest that leaves the outermost carriers alone; wider only moves the skirts towards the
channel edge. The captured skirts (-30..-38 dB at 100..500 Hz outside the carriers) are the filter's own.

**Receive**: `filter_study.py` shifts a captured signal and passes it through a brickwall of the width under
test, then decodes. Columns are filter widths; cells are sync % / SNR dB / vocoder-feature error against the
unshifted 1.6 kHz decode of the same stretch (0 = identical). Two captures, 14.236 MHz (normal) and 1.987 MHz
(inverted), 60 s each, shift 0:

| width (Hz) | 900 | 1000 | 1200 | 1400 | 1600 | 2000 |
|---|---|---|---|---|---|---|
| 14.236 MHz | 99 / 8.1 / 0.008 | 99 / 8.8 / 0.006 | 99 / 8.3 / 0.003 | 99 / 8.2 / 0.001 | 99 / 8.5 / 0.000 | 99 / 8.5 / 0.000 |
| 1.987 MHz | 99 / 6.3 / 0.011 | 99 / 6.3 / 0.008 | 99 / 6.6 / 0.004 | 99 / 5.8 / 0.001 | 99 / 6.5 / 0.000 | 99 / 6.5 / 0.000 |

Narrower than the carriers (they span 1450 Hz) costs very little, because the code carries redundancy;
wider buys nothing on a clean signal. The cost of a wide filter is outside what the capture can show (an
adjacent strong signal reaching the AGC), which is the reason not to go wider than needed.

## 3. Tuning tolerance

Same method, shifting the station by a whole number of Hz and keeping the 1.6 kHz filter (both captures):

| shift | -90 | -80 | -70 | -65 | -60 | -55 | +55 | +60 | +65 | +70 | +80 | +90 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 14.236 MHz sync % | 3 | 3 | 30 | | 62 | 65 | **99** | **99** | **99** | 66 | 56 | 13 |
| 1.987 MHz sync % | | 5 | 48 | 59 | **99** | **99** | **99** | 59 | 53 | 34 | 5 | |

(+/-50 Hz and the middle: 99 % with feature error 0.000 in both.) Full decode runs from about -50 to +65 Hz on
the 14.236 MHz capture and -60 to +55 Hz on the 1.987 MHz one; beyond that the decode is partial and wrong
(feature error 1.0 to 1.9, i.e. garbage). **The same cliff at 900, 1000, 1200, 1400, 1600 and 2000 Hz**, so it is the modem's
acquisition (`RADE_ACQ_FRANGE` is 100 Hz, in practice 60), not our filter. A station more than about 60 Hz off
will not decode at all; the tuning indication matters, and a search over frequency while out of sync
(parallel offsets of +/-100 Hz) would extend the reach; not built.

## 4. Mode changes

Where the energy sits relative to the dial, from the code (`modes_table`):

| state | WDSP passband | centre |
|---|---|---|
| V1 normal / inverted | 675..2275 / -2275..-675 | +1475 / -1475 |
| V2 normal / inverted | 1000..2000 / -2000..-1000 | +1500 / -1500 |
| SSB USB (DIGU) / LSB (DIGL) | 150..2850 / -2850..-150 | +1500 / -1500 |

V1 is inverted by default below 10 MHz, V2 never. What a change does at a **fixed dial**:

| change | energy moves | the dial change that keeps it |
|---|---|---|
| 7 MHz: V1 (default) -> V2 | +2975 Hz | -2975 |
| 7 MHz: V1 -> Invert on | +2950 | -2950 |
| 7 MHz or 14 MHz: V2 -> Invert on | -3000 | +3000 |
| 14 MHz: V1 -> Invert on | -2950 | +2950 |
| 14 MHz: V1 -> V2 | +25 | -25 |
| RADE off -> SSB | 0 if the VFO mode's sideband matches the invert state, +/-3000 if not | |

So four of the six common changes throw the signal 3 kHz out of where the passband was. On receive that
means: toggle Invert because the station looks mirrored and it is now 3 kHz outside the passband until the
operator retunes by hand. Nothing couples the VFO's USB/LSB to the RADE invert state today (the 1.987 MHz
captures happen to be LSB + inverted and the 14 MHz ones USB + normal because that is how they were set).

**Built (2026-10-16, on your decisions).** The centre of the energy, `Fe = dial + sign * 1500 Hz`
(sign +1 upper, -1 lower; V1 uses 1500 for this arithmetic and keeps its 1475 in the filter edges), does not
move. On any change of waveform or Invert the dial moves so that it does not, and the VFO is put in DIGU or
DIGL for the sideband in use (those profiles have no speech processing). Switching RADE off puts the radio
in USB or LSB on the same dial, which is the carrier of the last RADE sideband. What it does
(`modes_table`, from the same rule the code uses; the reporter is told the dial, which is the carrier):

```
160 m, energy centre 1985.5 kHz:
  RADE V1                    LSB      1987.000 -> V1 lower  1987.000 kHz   dial    +0
  V1 -> V2                   V1 lower 1987.000 -> V2 upper  1984.000 kHz   dial -3000
  V2 -> V1                   V2 upper 1984.000 -> V1 lower  1987.000 kHz   dial +3000
  Invert on / off            V1 lower 1987.000 -> V1 upper  1984.000 kHz   dial -3000 / +3000
  RADE off                   V2 lower 1987.000 -> LSB       1987.000 kHz   dial    +0
40 m, from USB at 7.053:     RADE V1 -> lower sideband, dial 7056.000 (+3000); V1 -> V2 7053.000 (-3000);
                             RADE off: USB at 7053.000
20 m, from USB at 14.236:    RADE V1 and V2 upper, dial unchanged; Invert +3000 (14239.000), off -3000
```

Details: the sideband is stored when it is chosen, not recomputed from the dial, so the 3 kHz moves can
never flip the 10 MHz band default; tuning across 10 MHz by more than 100 kHz with V1 on re-applies the band
default (sideband and VFO mode only, the dial stays where the operator put it). Invert is the user's
exclusive-or on the default and is kept when the waveform changes (V2 with Invert on is lower sideband).
The dial is the CTUN frequency when CTUN is on. From a mode with no sideband (AM, CW) the dial is taken as the
centre. Entering a digital mode lowers the drive to the digital-mode maximum, as piHPSDR always does, and
leaving RADE does not put it back. `test_geometry` checks the rule (the examples above, and that the centre is
unchanged over a six-step sequence); the menu code that applies it, the VFO changes and the Reporter's
frequency are **built but not run on a radio**.

## Reproduce

```
test/rade_native/tx_shape.py 20261015-232319            # transmit spectrum; PASS/FAIL
make -C test/rade_native modes_table && test/rade_native/modes_table      # the mode-change rule
make -C test/rade_native check                                              # and its unit test
test/rade_native/filter_study.py 20261015-223028 --start 10 --dur 60 --widths 900,1200,1600,2000 --deltas -100,-50,0,50,100
```
The offline reproduction of the tilt is `wdsp_probe tx --speech-stages-eq` (leveler, CFC with post-EQ and
phase rotator as this station's SSB profile sets them) against `--neutral`.
