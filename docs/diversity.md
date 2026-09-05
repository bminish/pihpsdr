# Diversity in piHPSDR

How two-antenna diversity works, and how the automatic phasing added on top
of it functions.

This is the reference document. Four companions cover narrower ground:
[`diversity-rade.md`](diversity-rade.md) for the FreeDV RADE V1 correlator,
[`diversity-digital-iq-proposal.md`](diversity-digital-iq-proposal.md)
for the FSK/Digital reference,
[`diversity-auto-phasing.md`](diversity-auto-phasing.md) for the design
rationale and the ideas that were tried and discarded, and
[`diversity-measurements.md`](diversity-measurements.md) for what all of
it measurably does on recorded on-air signals.

---

## 1. What the radio provides

Diversity needs two receive chains that are coherent — same clock, same
local oscillator, no relative drift — and configured identically. Both
HPSDR protocols provide that, and the phasing itself is done entirely in
the host, not in the FPGA.

### Protocol 2

`src/new_protocol.c` reconfigures the DDC map when diversity is enabled:

| | DDC0 | DDC1 | DDC2 | DDC3 |
|---|---|---|---|---|
| Normal RX | off | off | RX1 | RX2 |
| Diversity RX | ADC0, synced | ADC1, synced | off | off |

Byte 1363 of the receive-specific packet is DDC0's sync map, set to `0x02`,
which tells the firmware to lock DDC1 to DDC0 and merge them into a single
UDP stream: **119 interleaved sample pairs** per packet (I0 Q0 I1 Q1, 24
bits each) rather than 238 consecutive samples.

Both DDCs are given the same NCO frequency — the high-priority packet
copies bytes 9-12 into 13-16 — the same sample rate, the same band-pass
filter and the same dither/random setting. The step attenuator is shared
too, unless the operator has asked for separate ones; see "Separate
attenuators" below.

The consequence is the one everything else rests on: **the two streams
have no relative sample delay and no relative drift**, so the channel
between them is a memoryless complex gain rather than a filter.

### Protocol 1

Equivalent: `how_many_receivers()` forces two HPSDR receivers, ADC0 is
wired to RX1 and ADC1 to RX2, the attenuators are tied together by
default, and the sample pairs arrive interleaved in the same frame.

### Front-end asymmetry

Worth knowing when interpreting results. On pre-Orion2 boards only ADC0's
path is under software control — ALEX high-pass, the TX low-pass when
using ANT1-3, and the ALEX attenuator. ADC1 is a bare rear-panel input.
So the relative gain and phase between the two antennas are stable within
a band and **jump** when the band, antenna or attenuator changes.

The analysis discards its statistics and starts again on a change of
frequency, sample rate, mode, filter edges, any window setting (§4) or
either ADC's step attenuator — but **not** on an antenna change, which it
does not watch. There the estimate simply re-converges over a few time
constants, which is slower than a restart but arrives at the same place.
**Restart averaging** is the button for it if the wait is unwelcome.

### Separate attenuators

The two step attenuators are tied together while diversity runs, both
protocols sending ADC0's value to ADC1 as well. That is the safe default,
because an attenuator change moves the relative gain between the arms and
so invalidates whatever weight is in force — including a manual one the
operator set by hand.

**ATT**, the tick box beside **Div** at the top of the
menu, unties them, and puts an **Attenuator (dB)** row with a value for
each ADC underneath — a row that is there only while they are split. The
reason to want it is headroom on one antenna alone: a local source strong
enough to overload ADC0 that the second antenna cannot hear at all
(measured at 10.5 dB above the floor on ADC0 only — Finding 5 of
[`diversity-measurements.md`](diversity-measurements.md)) can then be
attenuated where it is, instead of costing the quiet antenna the same
10 dB of sensitivity it did not need to lose.

Untied, the step is not simply allowed through. The weight is a ratio, so
a known change of *d* dB on one arm has a known effect on it: ADC1 moving
by *d* raises the correct weight by *d*, ADC0 moving by *d* lowers it by
the same. That correction is applied to `div_cos`/`div_sin` at the instant
the attenuator moves, so the combined audio does not step, and a manual
gain and phase stay valid across the change. The measurement itself
restarts, since both attenuations are part of the analysis context.

This applies to any path that moves an attenuator — the ATT slider, an
encoder, CAT, or the two spin buttons in the Diversity menu, which are
the only way to reach ADC1 while the loop is running and has made RX1 the
active receiver.

---

## 2. The combiner

Three lines, in `rx_add_div_iq_samples()` (`src/receiver.c`):

```c
double i_sample = i0 + (div_cos * i1 - div_sin * q1);
double q_sample = q0 + (div_sin * i1 + div_cos * q1);
```

That is `y = z0 + w·z1` with one complex weight
`w = 10^(div_gain/20)·e^{jφ}`, computed in `radio_calc_div_params()`
(`src/radio.c`).

Four properties follow, and they shape everything else:

- **The reference weight is fixed at 1.0.** Only the ratio is adjustable.
  SINR is invariant to a common scale so nothing is lost, but the output
  level moves as the weight adapts.
- **The weight is flat across the whole DDC passband.** It can align the
  two antennas exactly at one frequency only.
- **There is no output normalisation by default.** Two equal in-phase
  signals give +6 dB of signal and +3 dB of noise. The **Level output**
  control (§6) divides that common scale back out when it is ticked, on
  every reference; everything below describes the unnormalised case
  unless it says otherwise.
- **It is applied per raw IQ sample, ahead of WDSP** — before the noise
  blanker, before `fexchange0`.

RX1's panadapter shows the combined stream; RX2, if running, shows raw
ADC1.

---

## 3. Manual control

Unchanged from before this work. The Diversity menu has coarse and fine
gain (±25 dB / ±2 dB) and phase (±180° / ±5°) sliders, and there are
encoder actions (`DIV_GAIN`, `DIV_PHASE`) in `src/actions.c`. `div_gain`
is clamped to ±27 dB and `div_phase` wrapped to ±180°.

The sliders are live whenever the automatic loop is not driving the
weight, and grey out when it is.

---

## 4. The automatic loop

`src/diversity_auto.c`. The shape is:

```
protocol RX thread                    analysis thread
------------------                    ---------------
rx_add_div_iq_samples()
  |
  +-- diversity_auto_sample()  -->  queue (4 blocks)  -->  div_process_block()
  |     4 stores per sample                                  |
  |                                          window + 2 FFTs, or
  +-- z0 + w*z1  ------------------------->  RADE pilot correlation
        (applied immediately,                                |
         weight read from                              solve for w
         div_cos/div_sin)                                    |
                                              slew into div_cos/div_sin
```

The analysis never sits in the audio path. The weight is applied to every
sample exactly as it was before, and the loop just changes what that
weight is, roughly twelve times a second.

### The queue

Four block buffers, in the style of the DDC packet rings in
`src/new_protocol.c`: a single-producer/single-consumer ring with no
mutex anywhere on it.

The protocol RX thread owns `q_head` and the worker owns `q_tail`, both
`volatile atomic_int`. The producer fills the slot at `q_head`, writes
the block's `q_gap[]` entry, issues a `MEMORY_BARRIER` (`src/atomic.h`)
and only then publishes the new `q_head` — so the worker cannot see a
head without also seeing the payload that goes with it. The worker
barriers again after its last read from a slot and before handing it back
through `q_tail`. One slot is always the one being filled, so the head
can never catch the tail and `q_head == q_tail` unambiguously means
empty; at most three blocks are ever waiting, which is what the old
`q_count < DIV_QUEUE - 1` test allowed too.

A semaphore is posted once per block enqueued, and once by
`diversity_auto_stop()` to wake the worker so it can see `worker_run` go
to zero. It is created on the first start and never destroyed: the sample
path tests `div_auto_running` without a lock and can still be inside
`sem_post()` after `diversity_auto_stop()` has returned, so destroying it
there would be the same use-after-free that the never-freed sample
buffers avoid. `new_protocol_menu_stop()` can destroy its semaphores
because it has joined every thread that posts them; we cannot, because
the protocol thread keeps running. Instead the count is drained at start
and the worker checks the ring pointers before it acts on a wake — the
count is a hint, not an invariant.

Two things reach the queue from other threads, and both arrive as
generation counters rather than flags:

| Word | Bumped by | Acted on by |
|---|---|---|
| `gap_seq` | `rxtx()` (`diversity_auto_gap()`) | the sample path, on its next sample |
| `reset_seq` | the menu and the remote settings path (`diversity_auto_reset()`) | the worker, between blocks |

The thread that acts on one only ever writes its own private `*_seen`
copy. A test-and-clear flag has a lost-update window — a request raised
between the read and the clear is erased — and a counter does not, since
any change of value is what the reader responds to.

Everything else is single-owner. `fillptr`, `q_pending_drop`, `gap_seen`
and `fill0`/`fill1` belong to the sample path; `work0`/`work1` and
`reset_seen` belong to the worker. That is only true because
`diversity_auto_gap()` no longer does the discard itself — see §4's
transmit-gap note below.

### Block cadence

`div_choose_nfft()` picks the transform length to land near the requested
bin width, so at the default **Resolution** of 12 Hz the block is 85.3 ms
at every sample rate:

| Sample rate | nfft | bin | block |
|---|---|---|---|
| 48 kHz | 4096 | 11.7 Hz | 85.3 ms |
| 96 kHz | 8192 | 11.7 Hz | 85.3 ms |
| 192 kHz | 16384 | 11.7 Hz | 85.3 ms |
| 384 kHz | 32768 | 11.7 Hz | 85.3 ms |

Asking for finer bins doubles nfft and therefore the block period; asking
for coarser bins halves it. `DIV_MIN_NFFT` is 2048 and `DIV_MAX_NFFT` is
65536, and between them every setting on the menu is reachable at every
sample rate up to 384 kHz. The status line always shows the bin width
actually achieved.

| Resolution | Block | 48 kHz | 96 kHz | 192 kHz | 384 kHz | 768 kHz |
|---|---|---|---|---|---|---|
| 24 Hz | 43 ms | yes | yes | yes | yes | yes |
| 12 Hz | 85 ms | yes | yes | yes | yes | yes |
| 6 Hz | 171 ms | yes | yes | yes | yes | capped at 12 Hz |

**This is a block-length control read in hertz, and that is the way round
that matters.** The loop measures the channel over one block and applies
the weight over the next, so the block period is the lag between the two -
and on a fast path the channel has moved by then. Swept over twenty rows
on fourteen captures, both objectives want the short block on seventeen of
them, the largest being 6.4 dB of extra null depth on `122632` (Findings 40,
42, 43).

Finer bins do the opposite thing and are worth having in one case: they
lift a weak signal further out of the per-bin noise floor, which is not
what turning Averaging up does - averaging reduces the variance of an
estimate, resolution improves the SNR the estimate is made from. One
capture in the set wants that, `011225`, whose fading is essentially
uncorrelated so that a short block costs more estimate variance than it
saves lag (Finding 39). The cost is responsiveness in both directions.

The 3 Hz setting was retired in favour of 24 Hz: measured, it was behind
12 Hz on both objectives on five captures of six, and above 192 kHz it was
not a distinct setting from 6 Hz. A props file carrying it comes back as
6 Hz.

### The queue

The sample path fills one buffer and hands it to a four-deep queue. If the
worker falls far enough behind that the queue fills, the block is dropped
and the worker is told, because a gap in the stream invalidates RADE V1's
pilot timing and it has to re-acquire rather than track a pilot that has
silently moved. For the transform-based modes a dropped block costs
nothing but one block's contribution.

### Three objectives

Over the bins selected by the reference (§5), the loop accumulates the
cross spectrum and both auto spectra with exponential forgetting, then:

| Objective | Weight | Behaviour |
|---|---|---|
| **Null** | `w = −Sxy/Syy` | minimises `E\|z0 + w·z1\|²` — cancels whatever the two antennas have in common. Noise cancelling. |
| **Sum** | `w = +(N0/N1)·Sxy/Sxx` | `Sxy/Sxx` equals `conj(h)` for `z1 = h·z0`; the branch noise ratio `N0/N1` is what makes it maximum ratio combining rather than MRC under the assumption that the two branches are equally noisy. On a pair 12 dB apart that assumption cost 3.6 dB and put the audio 14.8 dB louder — see Finding 22 in [`diversity-measurements.md`](diversity-measurements.md). Where `N0/N1` comes from is below. |
| **Best** | `w = 0` or `w` at the clamp, co-phased | gives the output to whichever antenna is measuring better, rather than combining them. |

Null and Sum use **different denominators**, so they are not simply
sign-flipped — though since `Sxx` and `Syy` are positive reals the two
answers are exactly 180° apart, differing only in magnitude by `Sxx/Syy`.
Both come from the same accumulators, so switching between them takes
effect on the next block and is applied without slewing.

**Best** is a selection rather than a third formula. It acts on the
per-antenna SNR estimate `div_auto_arm_db` — each arm's signal measured
against its *own* noise floor, so an antenna that is 12 dB down because it
is deaf is distinguished from one that is 12 dB down because it is quiet.
Every reference computes it, from the same pair of noise floors the Sum
weight uses. Whichever arm is ahead is used alone, with
1 dB of hysteresis so a marginal difference does not chatter.

Selecting arm 1 is not directly expressible: the combiner forms
`z0 + w·z1` with arm 0 pinned at unity gain, so "arm 1 only" exists only
as the limit `w → ∞`. The nearest reachable point is `w` at the loop's own
`DIV_MAX_WEIGHT` clamp with the co-phasing angle — +20 dB, inside the
sliders' ±27 dB, so arm 1 is dominant with arm 0 co-phased in underneath
it 20 dB down. That residue is not a compromise: measured
against a decoder it beat the full MVDR solve by 0.6 dB on the capture
where the two antennas disagreed about which was better, because arm 0 is
still doing useful combining. Selecting arm 0 needs no such trick —
`w = 0` is exact.

If the estimate is unavailable the loop **holds** rather than falling back
to arm 0, which would silently turn the mode into "diversity off" whenever
the floor could not be measured. Because Best selects rather than steers,
**Invert** does not apply to it and the button is greyed out.

Fit quality is the magnitude-squared coherence
`γ² = |Sxy|²/(Sxx·Syy)`. Below that reference's **Min coherence** the loop
holds rather than chasing noise, and the status line says `HOLD`.

That control has a floor, and the floor is not zero. Over *N* independent
samples the `γ²` of two *uncorrelated* noises averages `1/N` and is
distributed as `Beta(1, N-1)`, so a gate set under that stops being a gate:
noise-only blocks pass it and the loop fits a weight to whatever the two
antennas agreed on by accident — near-unity magnitude and random phase,
about 3 dB of added noise on a matched pair of arms. `diversity_auto_coh_floor()`
returns the value pure noise reaches one block in a hundred,

    floor = 1 − 0.01^(1/(N−1))  ≈  4.6/N,

with *N* the accumulated bin count times the effective length of the
exponential average, `(2−α)/α`. Both terms matter. A 3 kHz window at 12 Hz
bins and 2 s of averaging has thousands of samples and a floor of **0.1 %**
— which is why switching the gate off costs nothing there (Finding 38) —
while the Carrier reference accumulates five bins and reaches **1.6 %** at
the same averaging and **15 %** at 0.2 s. The slider's lower bound is that
number, recomputed whenever the reference, the window, the resolution or
the averaging time moves, and a stored setting below it is raised to it so
that what is displayed is what the gate compares against.

### Noticing that the signal has stopped

Coherence alone does not notice. The accumulators forget exponentially,
so when a transmission ends `Sxy`, `Sxx` and `Syy` all decay *together*
and `γ²` stays near 1 the whole way down — a 30 dB signal at the default
2 s averaging kept the loop reporting `track` for 5.8 time constants,
about **twelve seconds**, adjusting the weight on noise the entire time.
Once per gap, on every mode that has gaps.

So each block also compares this block's power, over the same bins and
with the same weights the estimate uses, against the smoothed power
accumulated over them. More than 10 dB apart and the statistics no longer
describe what is on the air: the loop holds, and the weight stays at the
last value measured on a real signal — which is what is wanted across a
gap. Measured: `track` to holding in **one block**, with the weight
unmoved.

The test scales itself. It fires on a signal well out of the noise, which
is exactly where stale statistics do the most harm, and stays quiet on a
weak one, where they hold little signal to be stale about. 10 dB is
comfortably past ordinary fading and far short of a signal stopping, and
holding through a deep fade is wanted anyway. It is one-sided, so a
signal *starting* never trips it.

It holds rather than flushing. The accumulators go on decaying at the
operator's averaging time either way, but flushing would put the loop one
block from the start, where the single-block cross spectrum is perfectly
coherent by construction and any bin at all looks like a signal.

**This matters most on CW**, where the signal is absent for most of a
transmission rather than only between them. The loop previously spent
every key-up period walking the weight around on noise; it now measures
only while there is something to measure, so the estimate is built from
key-down periods alone.

### The output slew

The solve's answer is not written to `div_cos`/`div_sin` directly. It is
slewed towards, so a change in the mix is not heard as a step and a single
bad block leans the weight rather than putting itself into the audio.

**A quarter of the Averaging setting, up to 0.5 s.** Both halves of that
were wrong until Finding 48 and in ways nothing said.

It was a *fraction per block* — 0.15 of the remaining distance — and the
block period is the reciprocal of the bin width, so the Resolution control
moved it: 0.26 s at 24 Hz bins against 1.05 s at 6 Hz. Resolution is
documented as trading estimate variance against measurement lag, and it
was quietly moving the *output* lag the same way, which made it worth more
than it looked for a reason nobody had written down.

And it was **fixed**, in series with the Averaging control, which is the
lag the operator can see. At 2.0 s of averaging a half-second slew is a
quarter of it and hardly shows. At the bottom of the slider — 0.2 s, which
is where a fading path wants it — it is two and a half times the
averaging and is then the only thing the operator's control is fighting.
The bottom two thirds of that slider's travel could not reach the applied
weight at all.

Tying it to the slider fixes both, and the old half-second is kept as the
ceiling so that nothing moves for anyone running the 2.0 s that used to be
the default. Measured on two 41 m AM captures, at 0.2 s of averaging,
shortening the slew from 0.5 s to 0.05 s is worth 0.36 and 0.30 dB of mean
signal-to-noise and 0.47 and 1.09 dB at the first percentile — the fades,
which is what the slider is being shortened for.

### The two branch noises

Two of the three things above want them: Sum multiplies `conj(h)` by
`N0/N1`, and Best compares each arm's signal against its own floor. The
FSK/Digital and RADE V1 references have them already — their MVDR
covariance is a measurement of the noise taken off the signal — and the
two wideband references have to find them somewhere else.

They are measured **across frequency, not across time**. Every block, the
bin powers outside the operator's filter are sampled — 1024 of them per
arm, strided down from the central 80 % of the transform, with the filter
and the analysis window and a 1 kHz skirt taken out — and the floor is the
tenth percentile, averaged over the two percentiles either side. A
percentile because a mean is dragged up by anything transmitting in the
sampled span; a band of order statistics because a single one is a noisy
sample and its scatter reaches the weight.

The obvious alternative is a minimum over time: the quietest the in-window
power has recently been. That is what shipped until Finding 47, and it has
a premise — that the band goes quiet often enough for the quietest recent
moment to be noise. `DIV_ARM_MIN_DB` was the guard on it, and **a fading
carrier supplies that clearance itself**: the minima land in the fades and
what gets published is the ratio of two independent fades. Measured on a
41 m broadcast it read +10.5 dB where the truth was −0.35, put 19 dB of
surplus arm 1 into the Sum weight, and inverted Best's choice of antenna
for a whole minute. Measured across frequency instead, the same captures
come out inside 0.3 dB.

The old minimum is still in the file. `div_window_quiet()` takes the
stand-down's decision from it — that test asks a different question, "is
either arm carrying anything", for which a temporal minimum is the right
shape — and it is the fallback where a hand-placed window leaves fewer
than 128 bins outside the filter to sample.

### Standing the combiner down

Holding is right when the signal is momentarily not measurable. It is
wrong when there is nothing there at all: the weight then in force was
fitted to something else, possibly on another band, and it goes on adding
the second antenna's noise to the output for as long as the band stays
empty. Measured at up to **12 dB** of extra noise between overs on a pair
whose chains are 15 dB apart, and at 3.5 dB over two thirds of a minute
where the weight had been carried in from a previous band.

So the two wideband references ask a second question when they decline a
block, and `div_window_quiet()` answers it: is *either* arm carrying
anything? It is the complement of the clearance test the branch noise
ratio used to apply, asked of both arms, against a bounded five-second
minimum and a half-second smoother — deliberately not the operator's
averaging time, which on a 30 dB signal takes twenty seconds to fall far
enough to read empty, and not the long-memory floor tracker, which starts
about 8 dB low and needs the better part of a minute to climb into place.
The noise ratio itself no longer uses either of them (above); this test
is the minimum's only remaining consumer, and the question it asks —
whether anything is on the air — is one a temporal minimum is the right
shape for.

If the window has been quiet for `DIV_QUIET_DWELL` — two seconds — the
weight is slewed to zero, which is arm 0 alone, the antenna the operator
would hear with the feature switched off. The estimate is not discarded
and the accumulators decay exactly as they did; only what is applied
changes. When the window fills again the weight is put back **in one
step**, by whichever of two doors applies: if the band fills while the
loop is still holding it puts back the weight it was standing on, because
it has no measurement for that block; if the over arrives strongly enough
to open the coherence gate on its own first block, that block's own answer
goes in instead. Only the first door existed until Finding 47 — the second
case was left climbing off zero at the slew rate, which is half a second
at the default settings and the whole of the 0.58 dB the restore is
worth.

Both conditions are required, and the dwell is what makes the test safe.
A signal that never stops eventually leaves the bounded minimum sitting
on the signal itself, so "quiet" is measured against the signal and reads
true on every deep fade; without the dwell that costs about half a
decibel on two captures with a station in the window throughout. Two
seconds is well past a fade and two orders of magnitude past the 60 to
300 ms gap between CW characters, which stays held.

The gate *opening* does not step the weight, only the presence path does.
The shipped threshold passes about one no-signal block in twenty, so a
step there would put a weight fitted to noise straight into the audio
where a slew only leans towards it. How far it leans is now the
operator's Averaging setting - see "The output slew" below - and at the
bottom of that slider it is half the way in one block. Measured on the
five bare-band captures that costs nothing: the silence penalty improves
on three, is unchanged on one and costs 0.16 dB on the fifth (Finding
48).

Scored on nineteen wideband captures the noise between overs improves on
seven and is never worse — by 7.3 to 8.3 dB on the three the change was
written for — while in speech it gains 3.5 dB on a CW capture and 1.7 dB
on a voice one against a single cost of 0.6 dB. See Findings 36 and 42 in
[`diversity-measurements.md`](diversity-measurements.md).

### Applying the weight

`div_apply_weight()` clamps `|w|` to +20 dB — inside the sliders' ±27 dB,
because a large weight means the aux antenna's own noise dominates the sum
and it costs headroom downstream. It then moves 15 % of the remaining
distance per block (about a 0.5 s time constant) and back-computes
`div_gain`/`div_phase` so the menu, the props file and remote clients stay
consistent with what is actually applied.

### Seeing where it is looking

The status line is held to a fixed 44 characters in four columns — what
is being measured, what the loop is doing, one detail belonging to the
mode, and the weight — in a monospace face, with a small margin at each
end. It is the widest thing in the dialog and so sets the minimum window
width, and building every line to the same length out of fields that
truncate as well as pad means nothing arriving at run time can widen it.
The predecessor was a printf per mode, the longest around a hundred
characters.

```
Win 12Hz  track  coh 100%     -2.1 dB   +32°
Win 12Hz  quiet  coh   4%    -27.0 dB    +0°
Car  3Hz* HOLD   +400000 Hz  -12.3 dB  +179°
RADE V1   LOCK   LSB 100%     -2.1 dB   +32°
Dig 12Hz  track  occ  293Hz   -2.1 dB   +38°
Dig 12Hz  search no signal    +0.0 dB    +0°
```

`quiet` is a third state and not a flavour of `wait`. Holding says the
loop stopped updating and is still applying what it had; quiet says it
stopped updating *and* stood the combiner down. The operator's question
between overs is which of those happened, and until the stand-down
existed the second one was the invisible case where the output got
noisier.

Below it sits a second, single-field line reporting which antenna is
measuring better and by how much, on the same fixed width:

```
Antennas  measuring
Antennas  ADC1 better by  3.4 dB
Antennas  ADC0 better by 12.1 dB  using ADC0
```

It is shown whatever objective is running, because nothing else an
operator can see distinguishes a deaf antenna from a quiet one and the two
want opposite weights. `measuring` means the estimate is not yet available
— it needs a signal standing clear of the noise floor on *both* arms. The
trailing `using ADCn` appears only under **Best**, and is what the
selection has actually settled on. A remote client reads the same three
lines as the radio: `div_auto_arm_db`, `_arm_valid` and `_arm_pick` all
arrive in `INFO_DIVERSITY`, so `div_arm_status_set()` needs no
remote-aware case of its own.

In FSK/Digital the third field is the width of what was found occupied,
which is checkable against the darker band on the panadapter, and
`search` means the region is in the right place but empty - as against
`wait`, which means something was found and then rejected as incoherent.

In **Carrier** the third field is where the tracker has settled, measured
from the **zero beat** - so a correctly tuned signal reads near zero in
every mode. In CW that is not the shifted frame's own zero:
`rx_set_filter()` folds the sidetone into the passband, so
`div_auto_carrier` sits one pitch away from the note being listened to and
`div_window_zero()` takes it back out before the number is shown. The
panadapter's carrier line has always used the same correction, so the
reading and the mark agree.

A `*` on the first field means the window ran past the Nyquist limit for
this sample rate and was clamped. Under **Hold** the weight shown is the
one the loop has *tracked to*, not the one being applied — the sliders
show what is applied, and seeing the two apart is the point of the
control.

The analysis window is drawn on the RX panadapter as a translucent green
band, using the theme's "ok" accent at low alpha. It is drawn under the
spectrum trace, in the same place in the draw order as the notch shading.

This matters most for a window placed outside the passband, which is
otherwise completely invisible. In follow-filter mode the band lands
exactly on the filter shading, which is a convenient check that the
frequency reference is right.

In Carrier mode the band is the search region and a brighter vertical line
marks where the tracker has settled within it. In FSK/Digital it is again
the search region, with the bins found occupied shaded more strongly over
the top, so the operator's setting and the measurement are both visible -
seeing them disagree is how a region placed on the wrong thing shows
itself. In RADE V1 it is the whole modem band, because the pilot
correlator taps the raw stream ahead of WDSP and is not affected by the
filter.

The frequency reference deserves a note, because both halves of it were
wrong here at different times. The window and the filter edges live in
WDSP's shifted frame, where the tuned signal sits at zero; the analysis
works on the tapped DDC buffer. The conversion is

    bin frequency = −(s + frame_off)

with `frame_off = vfo[0].offset`, less the CW sidetone in CWU and plus it
in CWL.

The **displacement** is stated by two independent places in the code: the
panadapter's own filter overlay, and WDSP's notch database, which compares
absolute RF notch frequencies against `flow + tunefreq + shift`. It is
also the only arrangement that puts the CW passband on the dial frequency.

The **inversion** is not derived, it is measured. The tapped buffer runs
backwards with respect to RF: a signal above the dial appears at a
negative complex frequency in it. Three on-air observations say so — the
wideband RADE mode finding an LSB modem's energy at positive bin
frequencies, the V1 correlator locking the un-mirrored pilot bank on an
LSB signal twice by a wide margin, and the plain operator's expectation
that LSB, having been inverted once by the transmitter, arrives here the
right way up. Reading the code does not give this answer; `wdsp/shift.c`,
`wdsp/analyzer.c` and the panadapter's pixel mapping cannot all three be
read consistently with one another, and the measurement does not care.

Neither error showed with a symmetric window, CTUN off and a phone mode,
which is most bench testing. What they broke was everything asymmetric: a
hand-placed window at +5 kHz measured −5 kHz, the RADE window sat on the
wrong sideband, and with CTUN on the analysis measured a window `2 ×
offset` away from the one drawn on the screen.

If this is ever revisited, revisit it with a signal: put a known carrier a
few kHz off the dial, run the Carrier reference, and see which way
`div_auto_carrier` moves.

### Starting again

`div_context_changed()` watches the tuned and CTUN frequencies, the CTUN
offset, the CW sidetone, the sample rate, the mode, both filter edges, and
every window setting — the reference, the follow tick, centre, width and
weighting. Any change throws the accumulated statistics away, rather than
relying on call sites to notify it.

The three frequencies carry a tolerance. Below `DIV_RETUNE_HZ` (20 Hz,
cumulative since the last reset, not per block) nothing is discarded: what
the estimate describes is the pair of antennas and the path, and that does
not change because the dial moved a few hertz, while a retune small enough
to leave the same signal in the window is not a reason to start again. 20 Hz
is under a tenth of the narrowest CW filter and inside the ±60 Hz the RADE
correlator tracks, so a lock survives it; tuning across a band to another
station moves kilohertz and still resets. See Finding 15's neighbourhood in
[`diversity-measurements.md`](diversity-measurements.md).

**A transmit gap is not a retune, and is handled separately.** Both
protocols stop feeding `rx_add_div_iq_samples()` for the whole over — P2
only sets `RXACTION_DIV` when not transmitting, duplex included
(`update_action_table()`), and P1 guards the mixer on
`!radio_is_transmitting()`.

That stop is required, not incidental. On P2 the two receive chains the
diversity pair uses are DDC0 and DDC1, and those are exactly the
synchronised pair the radio needs for the PURESIGNAL feedback while
transmitting; with PureSignal on they are additionally retuned to the
transmit frequency
(`new_protocol.c`, the `xmit && transmitter->puresignal` block in the
high-priority packet). P1 reserves the same two chains the same way.
There is no arrangement in which two antennas keep arriving during an
over, and none is wanted — the diversity weight is not being measured
while the operator is talking.

So the analysis stream has a hole in it, and nothing in the context
comparison can see it: `div_context_changed()` does not watch PTT, and
`q_pending_drop` counts only blocks lost to a full queue. Reporting it is
all `diversity_auto_gap()` does. It is called from `rxtx()`, the one
funnel every TX/RX transition goes through, so MOX, VOX and Tune are all
covered, and it is called on **both** edges. It bumps a generation
counter; the sample path picks that up on its next sample, discards the
partly filled block and marks the next complete one as following a gap,
which is the flag the worker already uses to call `rade_corr_reset()`.

The discard belongs to the sample path rather than to `rxtx()` for two
reasons. It keeps `fillptr` and `q_pending_drop` private to one thread,
which is what lets the queue run without a mutex at all (see "The queue"
in §4); and it is more accurate, because a store from the GTK thread
could land in the middle of a block the protocol thread was still
writing. It also puts the discard at the right instant: samples still in
flight from the last DDC packet when `rxtx()` runs are pre-TX samples,
and they are thrown away with the block they belong to instead of being
spliced onto post-TX ones. The TX→RX edge is the one that does that work,
and it is guaranteed to arrive first — every caller of `rxtx(0)` clears
`mox`/`vox`/`tune` only after it returns, so `radio_is_transmitting()` is
still true for the whole call and both protocol gates are still shut.

That matters only to RADE V1, and it matters a lot: without it the first
block after an over spliced pre-TX and post-TX samples into one transform
while `lock_a` went on advancing by one modem frame against a ring that
had skipped an arbitrary number of samples. The correlator tracked
straight through into a dead lock, holding a frozen weight for the whole
hang time — 10 s — before it started searching
again. It now re-acquires deliberately after every over, which costs the
searching load in §7 for a second or two.

**The weight and the transform accumulators are deliberately kept across
the gap.** `div_cos`/`div_sin` are written only by `div_apply_weight()`,
and every path with no answer to give sets `div_auto_holding` and returns
without calling it, so the gain and phase from before the over stay
applied until a new lock produces a better fit. `div_reset_stats()` is not
called either: a cross spectrum is a time average rather than something
locked to the sample clock, so once no single transform spans the hole it
is unharmed. Window, Carrier and FSK/Digital therefore lose nothing at all
across an over.

---

## 5. The four references

What part of the spectrum the decision is taken from. Selected by
**Measure on** in the Diversity menu, which lists them as

    Window (wideband)
    FSK/Digital (occupancy MVDR)
    Carrier (AM/SAM)
    RADE V1 pilot (MVDR)

— the two general-purpose references first and the two that need a
particular signal to be present after them. That order is a display order
only: the `DIV_REF_*` values are what land in the props file and go over
the wire, so they are fixed and new ones go on the end. `ref_rows[]` in
`src/diversity_menu.c` is the only place the two orders meet. The sections
below are in the order the references were built, which is neither.

### Window (wideband)

Every bin in the analysis window. The window either follows the RX filter
or is placed by hand with **Window centre** and **Window width**, in Hz
relative to the tuned signal — the same reference the filter edges use.

"The tuned signal", not the dial frequency: `rx_set_filter()` folds the CW
sidetone into `filter_low`/`filter_high`, so a CW passband sits at +pitch
in CWU and -pitch in CWL, and the shifted frame's own zero is one pitch
away from the only signal there is. `div_window_zero()` supplies that
offset to the hand-placed window, so a centre of 0 is the zero-beat note
in every mode and the hand-placed and filter-following windows agree.
Following the filter never had the problem, because it takes the folded
edges. The panadapter overlay places the drawn window with the same
function, so what is drawn stays what is measured.

**The window may be placed outside the passband.** That is often the better
way to cancel noise: measuring the noise on its own, clear of the wanted
signal, gives a cleaner estimate of the noise channel than measuring it
through the signal. It is also how you size a window to take in just the
mark and space tones of an FSK signal.

This works because the channel between the two antennas is flat over any
realistic frequency gap. What curves it is the differential delay between
the feedlines, and 20 dB of cancellation needs the phase right to about
5.7°:

| Δ delay | 1 kHz | 5 kHz | 10 kHz | 30 kHz |
|---|---|---|---|---|
| 10 m coax (50 ns) | 0.02° | 0.09° | 0.18° | 0.54° |
| 30 m coax (152 ns) | 0.05° | 0.27° | 0.55° | 1.64° |
| 100 m coax (505 ns) | 0.18° | 0.91° | 1.82° | 5.45° |

So measuring a few kHz away costs nothing. The limit is the Nyquist
frequency, ±half the sample rate: a window beyond it is pulled back to the
edge and the status line marks the first field with a `*`. Before that guard existed
a window at +30 kHz on a 48 kHz stream was silently measured at −18 kHz
instead.

#### Weighting: retired

The window is summed **flat** — the spectra are accumulated over the
window and divided, making the answer a power-weighted average of `h(f)`.
There is no control, and the alternative it used to offer has been
withdrawn.

That alternative was **coherence weighting**, which weighted each bin by
its own magnitude-squared coherence so that bins both antennas heard
dominated and noise-only bins fell out. On synthetic speech it roughly
halves the gain error, and this section used to recommend it on that
basis. On recorded signals it does not survive:

- it is **no better as an estimator** — flat is as good or better on four
  of five captures scored with the gate out of the way, and 0.66 dB better
  on one (Finding 27);
- it is **worse at the gate at any matched false-alarm rate**, by 1.4 to
  5.6 points of signal kept, because selecting the most coherent bins and
  then reporting the coherence of what you selected is a biased estimator
  that does not know whether there is a signal underneath it. It raises
  the reported `γ²` on all thirty-two captures, the ones holding nothing
  included (Findings 27 and 40);
- on the six SSB captures whose windows are mostly noise — the case it
  existed for — flat is ahead or level on **twenty of twenty-four** cells
  and ahead by up to 0.93 dB on the two weak ones (Finding 42).

The synthetic result and the on-air one differ because the synthetic
signal has no gate: it measures the estimator alone, on a window where
the signal is always present somewhere. What coherence weighting bought
in practice was a lower effective threshold, and the threshold control
does that on its own, transparently.

**The threshold moved with it.** Coherence weighting inflates `γ²`, so the
same number was a laxer test with it than without — flat wants a threshold
about 0.10 lower for the same false-alarm rate. The Window default is
therefore **0.20**, not the 0.30 that shipped alongside coherence
weighting: measured over thirty-two captures that is 5.2 % false alarms
against 5.7 %, with 0.30 dB more signal. Retiring the control without
moving the threshold would have made every operator's radio stricter than
it was. See Findings 27, 29, 40 and 42 in
[`diversity-measurements.md`](diversity-measurements.md).

The setting still travels on the wire and is still written to the props
file, so neither had to change shape; `div_settings_validate()` pins it to
flat rather than range-checking it, which is what happens to a value left
by an older build or sent by an older client. `DIV_WEIGHT_COHERENCE`
stays in the enum because the offline harness still has to be able to
sweep the retired path.

### Carrier (AM/SAM)

The carrier bin only. The carrier is found from the spectrum: the peak bin
**inside the analysis window**, refined by parabolic interpolation on log
power across the three bins about the peak, then smoothed with the
Averaging control.

Because the search is confined to the window, a carrier other than the
primary can be tracked — and therefore nulled. Park a 1 kHz window on
+5 kHz and the primary carrier is outside the search entirely. The
panadapter shows the search region as a green band with a brighter line
where the tracker has settled.

**Window centre and width are modal twice over.** The Window, Carrier and
FSK/Digital references each keep their own pair, so aiming the carrier
tracker at a station 5 kHz away does not destroy the window set up for
wideband work; switching back restores it. All three pairs persist, and
all three are part of the per-mode block described in §6, so the pairs
built up for AM are still there after an evening on SSB.

Measured at 384 kHz, where a bin is 11.7 Hz wide: **0.03 Hz of error and
0.002 Hz rms of jitter** at 11 s averaging, holding at −6 dB carrier SNR.

**The estimate is good and the gate is not.** `Min coherence` has no
discriminating power in this mode: the tracker accumulates
`2·DIV_CARRIER_BINS+1` = five bins, where `γ̂²`'s own `1/N` bias sits about
where the threshold has to, and over thirty-two captures it clears 0.30 on
36.0 % of blocks holding a signal and 36.1 % of blocks holding none. **No
threshold separates them, and accumulating more bins does not either** —
swept from five bins to sixty-five the two rates fall together and never
part, because a carrier occupies the same few bins however wide the sum,
so every bin added is noise on both sides of the comparison. What does
separate them is the averaging time: 0.5 to 30 s takes the no-signal rate
from 37.0 % to 32.2 %, with the best signal-kept figure at 10 s. The
constant is therefore left alone, the tooltip says what the control is,
and the mode remains usable because what it measures is a real carrier.
See Finding 26 and "What was changed" in
[`diversity-measurements.md`](diversity-measurements.md).

It does not use WDSP's SAM PLL, which is deliberately set for fast
acquisition — 39.8 Hz natural frequency, ~25 Hz loop noise bandwidth,
around 7 Hz of jitter on a weak carrier. That is right for demodulating
SAM and about a hundred times wider than suits measuring a carrier that is
not going anywhere. Working from the spectrum also means this mode works
in plain AM, where the SAM PLL does not run at all.

### RADE V1 pilot (MVDR)

Correlates against RADE V1's known pilot — on the side of the tuned
frequency the operator's passband names, and only that side — to separate
the wanted signal from everything else, which allows the interference covariance to be
estimated separately and a null steered onto QRM rather than onto the
signal. The covariance comes from the off-carrier bins of the pilot span,
on the modem's own side of the tuned frequency, so the rejected sideband
cannot get into it; the channel is accumulated as a cross-spectrum, so a
residual frequency error cannot decohere it. Both of those replaced
simpler versions that measurably did not work — see
[`diversity-measurements.md`](diversity-measurements.md).
Uses no transform at all. See [`diversity-rade.md`](diversity-rade.md).

Selecting the RADE V1 reference sets the objective to **Sum**, since the
signal the pilot correlator is pointing at is the wanted one. **Null**
turns that answer through 180 degrees to cancel the RADE station instead,
which is the quickest way to check the array is pointed at it. The
correlator has only one answer to compute - MVDR against the interference
covariance already maximises the pilot's SINR - so unlike the transform
references the two objectives here really are a sign flip.

Unlike every other reference, this one holds a *lock* - a timing, a
frequency and a pilot bank it keeps returning to - so it is the only one
with something to give up. `DIV_HANG_DEFAULT` decides when: how long the
lock outlives the pilot before the correlator searches again, fixed at
10 s.

**It used to be a slider and is not one any more.** Swept from 1 to 10 s
on the two captures that can be scored on decode it moves lock uptime from
38 % to 94 % and synced frames by +10, +11, +10, +10 — inside the scatter
of the measurement either way. The correlator's uptime is a true statement
about the pilot lock and very nearly uncoupled from what the modem does
with the audio, so there was nothing here for an operator to tune. The long
end is what is kept, because it re-acquires least often; the one case that
might have argued for a short hang — several stations taking turns on one
frequency, each wanting its own weight — is not in the capture set, so it
is not evidence. See Findings 33, 35 and 41 in
[`diversity-measurements.md`](diversity-measurements.md) and
[`diversity-rade.md`](diversity-rade.md).

The wideband **RADE passband** reference that used to sit alongside it has
been retired. It placed a window on the 750-2200 Hz modem band, on the
side of the tuned frequency the operator's passband implied, and clipped
it to the filter. FSK/Digital does the same thing from the same passband
and does it better: it finds where the modem's energy actually is rather
than assuming the nominal band, and it measures the noise separately
instead of assuming both branches carry the same amount. Its slot in the
props file's `diversity_auto_ref` is migrated to FSK/Digital on restore.

**There is no threshold row in this mode.** It used to gate
`rade_corr_quality`, and three gates on the pilot already stand in front
of that: the acquisition ladder's sigmas, the confirm and probation
ladder, and `RADE_USE_RATIO`'s per-frame freeze. Measured over the whole
capture set, the five recordings with no signal in them produce **no
weight at all** through those — 3515 blocks of it — so the false-alarm job
a threshold exists for was already finished before this one was reached.

What is left it cannot do either. `rade_corr_quality` is a display
quantity by construction, and it does not separate a good lock from a poor
one: a strong capture producing a weight on three blocks in four reads a
median 0.217 with 31 % of its blocks under 0.05, where one that
re-acquires eight times a minute reads 0.193. And no reachable setting was
safe — on `165826`, where one antenna decodes 176 frames, the other 323
and the combiner 329, **every** block that produced a weight sits under
0.25; setting the old slider to 0.15 takes the loop from producing a
weight on 32.6 % of blocks to 1.7 %.

So the value is pinned to zero, which is what shipped, and the row is
gone. See Findings 26, 33, 35 and 41 in
[`diversity-measurements.md`](diversity-measurements.md).

### FSK/Digital (occupancy MVDR)

For a narrow digital signal - FT8, RTTY, PSK31, VARA, JS8 - in a passband
that is mostly empty. It is the only reference that measures the *noise*
separately from the signal, and everything it does differently follows
from that.

**"Mostly empty" is a real precondition, not a figure of speech**, and the
name of the mode hides how much rests on it. The split works by calling a
bin occupied when it stands 6 dB over the region's own median, which
requires the wanted signal to occupy a minority of the region. That
describes FSK and it describes FT8. It does not describe **OFDM** - DRM,
and the RADE waveform itself - or a **wideband QAM or PSK modem**, both of
which fill the region edge to edge so that the signal *is* the median:
measured on a DRM broadcast 41 dB over its noise floor, the number of bins
standing 6 dB over the median is **five, where chance alone gives four**.
Those shapes are served, if at all, by the "region is full" fallback
below rather than by the split. Both are in everyday use on these bands
and both are wider than the region this reference works best in. See
Finding 46 in [`diversity-measurements.md`](diversity-measurements.md),
which also records that the mode has no false-alarm control and that three
ways of giving it one have been tried and rejected.

The other references have no picture of the noise on its own, so **Sum**
has to assume the two branches carry equal, uncorrelated noise. That is
what makes `w = +Sxy/Sxx` maximum ratio combining. On a real station the
assumption is usually false in two ways at once: ADC1 is often a small
loop or an active whip on a bare rear-panel input, several dB noisier
than the main antenna (§1), and much of what both antennas hear is
common-mode hash picked up on the feedlines, which is *correlated*
between them. Sum is blind to both.

A digital signal is narrow, so the empty part of the passband can simply
be looked at:

1. **The region.** The analysis region is the RX filter with **Window
   follows RX filter** ticked, or a hand-placed centre and width without
   it - the same two controls as Window mode, kept as their own modal
   pair. Following the filter needs no sideband table and no ±1500 Hz
   constant: the passband is already on the correct side of the tuned
   frequency in USB, LSB, DIGU, DIGL and CW, and under CTUN.
2. **Occupancy.** The noise floor is the **median** of the bin powers
   over the region - a median, not a mean, so a signal filling part of
   the region cannot drag the floor up and hide itself. Bins more than
   6 dB above it *and* coherent between the two antennas are signal.

   This test has no false-alarm control that scales with the region: three
   bins clearing the threshold is the requirement whether the region holds
   thirty of them or two hundred. On a wide region full of nothing but
   noise, three will. Measured on a no-signal capture the mode produces a
   weight on 30 % of blocks, and it is why the reference is the wrong one
   for a narrow CW passband. See
   [`diversity-measurements.md`](diversity-measurements.md).
3. **The covariance.** Everything at least four bins clear of an occupied
   bin is noise, and `R` is accumulated over it. Correlation is not a
   disqualification here - correlated noise is precisely what `R` exists
   to describe.
4. **The solve.** `w = R⁻¹h`, with `h0 = ΣSxx` and `h1 = conj(ΣSxy)` over
   the signal bins. Diagonally loaded at 1 %, the same 2×2 solve the RADE
   V1 correlator uses (`div_mvdr2()`), which reaches `R` and `h` from
   pilot correlations instead.

With `R` diagonal and equal the solve reduces to `conj(h1/h0)`, which is
exactly `+Sxy/Sxx` - so **the mode degenerates to Sum when the noise
really is equal and uncorrelated**, which is both the right behaviour and
the property the tests pin down.

**The guard band is not optional.** A signal 40 dB above the noise puts
more into its neighbouring bins, through the analysis window's skirts,
than the noise floor holds - and those bins are correlated with the
signal's own channel. Feeding them to `R` tells MVDR that the direction
the signal arrives from is interference, and it steers the null straight
onto it. This is the standard failure of MVDR trained on data containing
the wanted signal, and it was observed here before the guard existed: on
a synthetic test the weight moved 8° off the correct answer. Four bins is
where the Blackman-Harris skirts have gone.

**A transmission ending is noticed by the shared staleness test** (§4),
which matters more here than anywhere else: the occupancy test is a ratio
against the median floor, so it is scale invariant and would not see the
level collapse at all. When it fires, the occupied span is withdrawn from
the status line and the panadapter as well, so a signal that has gone
stops being drawn as one.

**What it cannot do is separate a wanted signal from co-channel QRM.**
Both are occupied and both are correlated between the arms, so occupancy
has nothing to tell them apart by - that is what the RADE V1 pilot is
for. Here the operator separates them by placing the region, and **Null**
cancels what the region is sitting on, exactly as in Window mode. Both
objectives are meaningful, so unlike the RADE V1 reference this one does
not force Sum on selection.

**A full region is not an empty one.** If the signal covers the whole
region the median *is* the signal and occupancy finds nothing - which is
what a filter set snugly around the signal looks like, with the follow
tick on, which is what a careful operator does. Holding there would be a
trap: the better the filter, the more certainly the mode would do
nothing. Coherence tells the two apart. A full region is coherent, so it
is accumulated whole and falls through to plain maximum ratio combining;
an empty one is not, and holds.

Measured on synthetic 2-FSK against a two-path channel: identical to the
Window reference within 0.2 dB when both branches carry the same noise,
**13 dB better output SINR** when the aux branch is 20 dB noisier, and
one block from `track` to `search` when the signal stops.

---

## 6. Operator controls

| Control | Effect | Shown for |
|---|---|---|
| **Div** | The whole feature, including the DDC re-plumbing | always |
| **ATT** | Split ADC0's and ADC1's step attenuators (§1). The split pair is remembered and put back the next time you tick this — how much hotter one antenna is than the other is found by ear once and wanted again every time | two ADCs with a step attenuator |
| **Level output** | Keeps the combined output at the level of arm 0 alone instead of letting it rise with the array gain. Off by default; Sum and Best only. See Finding 32 in [`diversity-measurements.md`](diversity-measurements.md) | Sum, Best |
| **Attenuator (dB)** | ADC0 and ADC1, 0-31 dB each | only while split |
| **Gain / Phase** (coarse, fine) | Manual weight; live when Auto is not driving, and under **Hold** | always |
| **Auto** | Off / Null / Sum / Best — the objective | always |
| **Measure on** | Which reference (§5) | always |
| **Window follows RX filter** | — | Window, FSK/Digital |
| **Window centre / width** | The analysis window, the carrier search region in Carrier mode, or the occupancy search region in FSK/Digital. Measured from the tuned signal, which in CW is the zero-beat note. Kept separately per reference | Window (unticked), Carrier, FSK/Digital (unticked) |
| **Resolution** | 24 / 12 / 6 Hz bins — really a block-length control, 43 / 85 / 171 ms, since the block period is the reciprocal of the bin width. Coarser measures the channel more often, which is what a null is limited by; finer lifts weak signals out of the per-bin noise floor. Both objectives want the coarse end on seventeen rows of twenty (§4) | all but RADE V1 |
| **Averaging** | 0.2-30 s, on a geometric scale so that 64 % of the travel is below 5 s. Time constant for the estimate | always |
| **Min coherence** | Below this the loop holds rather than adapts. **Stored per reference**, because the three do not compare the same quantity: `γ²` over the window in Window and Carrier, `γ²` over the occupied bins in FSK/Digital — see Finding 26 in [`diversity-measurements.md`](diversity-measurements.md). **The range is not fixed**: each goes from its own noise floor (§4) to 95 %. It has no discriminating power in Carrier (§5) and there is no such row in RADE V1 (below) | Window, Carrier, FSK/Digital |
| **Restart averaging** | Discards the accumulated statistics | always |
| **Hold** | Stops applying the loop's answer without stopping the loop | always |
| **Invert** | Swaps Null and Sum | always; inactive under Best |

Rows that the selected reference cannot use are **hidden, not greyed
out**. The RADE V1 reference places its own window, so four rows never
apply to it; it uses no transform at all, so two more do not either; and
it no longer has a threshold, so that row is gone from it as well.
Greying them left a tall dialog of mostly dead controls.

There is no Weighting row any more, in any reference. It only ever reached
the Window reference — the carrier tracker accumulates a handful of bins
either side of one peak and FSK/Digital's occupancy split had already
decided which bins carry signal — and on four independent measurements it
was behind or level with the flat sum it replaced (see above).

### One set of settings per group of modes

The right reference, window and objective are a property of what is being
received, and the mode is the operator's own statement of that: a carrier
to track in AM and SAM, an FSK occupancy to find in DIGU and DIGL, a
filter-wide window in SSB, and in CW a window narrow enough to sit on one
note. A single set carried across a mode change therefore hands the loop
settings chosen for a signal that is no longer there — the carrier tracker
hunting a carrier SSB does not have, or the 100 Hz window left over from
CW swallowing an SSB passband whole — and the operator has to notice and
undo it every time.

So the whole settings block is modal. The groups are

| Group | Modes |
|---|---|
| SSB | LSB, USB |
| CW | CWL, CWU |
| FM | FMN |
| AM | AM, SAM, DSB |
| Digital | DIGU, DIGL |
| Other | everything else, presently SPEC |

DSB sits with AM and SAM because its passband is symmetric about the
carrier, so a window and a carrier search mean the same thing there.
Anything unnamed shares one block, which costs nothing and means a mode
added later still lands somewhere sensible.

`rx_mode_changed()` announces the change to `diversity_auto_mode_changed()`
for RX0, which covers every route a mode can change by — the menu, CAT, a
bandstack recall, a VFO swap, and a client asking for one. That files what
is in force under the outgoing group, adopts the incoming one, and draws
the same restart and reset conclusions `diversity_auto_apply_settings()`
does. It deliberately does *not* invert the weight when the objective
crosses between Null and Sum: there the operator asked for the weight in
force to be turned over, here two unrelated blocks merely happen to
differ.

Hold is not modal. It is an operating state rather than a setting, and it
is not persisted either — see below.

The blocks live on the radio, with the analysis. A client is sent the
outcome of a switch the same way it is sent any other settings change, so
a panel running remotely follows a mode change on the radio with no
remote-aware code of its own.

### Hold

Stops the loop *applying* its answer. It keeps measuring, and the status
line keeps showing where it has got to, but the gain and phase controls
become the operator's meanwhile. Releasing puts the tracked answer in
place in one step rather than slewing to it.

That makes two things easy that were not: comparing the loop's answer
against a hand-set one on the same signal, and holding a good weight
through a period when the band is doing something the loop should not
follow, without losing the loop's progress.

It is an operating state rather than a setting, so it is not persisted,
and it is released when the dialog closes — there is no indicator for it
anywhere else, and a loop that had silently stopped applying anything
would be a mystery.

### Invert

Swaps Null and Sum, and **turns the weight in force through 180 degrees at
the same time**, whether or not the loop is currently applying anything
and whether or not Hold is set. It is the quickest way to tell whether the
array is pointed at the wanted signal or at the interference.

Both halves are needed. Changing the objective alone only takes effect
when the loop next produces a weight, and it may not be producing one: the
coherence gate can be holding, the RADE correlator can be frozen on a
fade, and under Hold nothing is applied at all. The control then changed
what was being computed while leaving the audio exactly as it was.

Under Hold it acts on the operator's own manual weight, which is the only
thing being applied then.

It applies to Null and Sum only. **Best** is not one of that pair — it
selects an antenna rather than steering a null, so there is no opposite
answer to swap to — and the button is greyed out there. Left live, it
would have moved the combo to Null and performed no inversion at all.

The objective combo takes the same path, so the button and the combo
cannot behave differently.

For a while this control was inert in **RADE V1** and looked broken rather
than unimplemented. The correlator's answer was applied whatever the
objective said, so Invert turned `div_cos`/`div_sin` over immediately -
the audio changed - and then the next block wrote the un-inverted answer
back and slewed straight to it. Every reference now applies the sign the
objective asks for.

Settings persist in the props file as `diversity_auto_*`, with the
per-reference window pairs as `diversity_band_*`, `diversity_carrier_*`
and `diversity_digital_*`, and every group's block as
`diversity_group[n].*`. All of them are range checked on restore, by one
`div_settings_validate()` rather than a clamp per global. New reference
modes go on the end of the enum: the value is what is written to the
file, so inserting one in the middle silently changes what an existing
file means.

The flat `diversity_auto_*` keys stay, and are the current group's values.
They are also what seeds every group whose own keys are absent, so a file
written before the blocks existed gives each group what the radio was last
set to — the old single-block behaviour, until the operator moves a
control in one mode and not another. The mode is not restored until after
`diversity_auto_restore_state()` runs, so which group the flat keys belong
to is not knowable there; the first mode change announced adopts that
group's block, which for a file this version wrote is the same thing.

**The DSP runs on the radio; the UI runs wherever the operator is.** The
sample pair only exists on the radio side, so the analysis thread, the
correlator and the weight all live there and nothing about that changes
for a remote operator. What travels is the control surface and what it
displays, so a client drives every part of the feature — objective,
reference, window, resolution, averaging, min coherence, Hold, Invert,
Restart and the manual weight — exactly as the radio's own panel does. The
retired fields, weighting and hang, still travel so that the message keeps
its shape; the radio pins both on arrival.

Three messages carry it:

| Message | Direction | Contents |
|---|---|---|
| `CMD_DIVERSITY` | client → server | Enable, manual gain and phase (unchanged, predates this) |
| `CMD_DIV_SETTINGS` | both | The whole auto-loop control block, plus an action byte |
| `INFO_DIVERSITY` | server → client | What the loop is measuring, on the server's 150 ms timer |

`CMD_DIV_SETTINGS` sends the **whole** control block whenever any one
control moves, rather than one message per control. It is small, it is
idempotent, and it lets the radio work out what a change means by
comparing the block against what it has in force. That is why
`diversity_auto_apply_settings()` exists: the rules for what moving a
control implies — restart when the transform length changes or the
objective crosses Off, rebuild when a RADE reference is selected or left,
reset when the accumulated bins stop being the right bins, turn the weight
over on Null ↔ Sum — used to live in the menu callbacks, which was fine
while the only operator sat at the radio. With the UI able to run
elsewhere the server has to draw the same conclusions from a settings
block that the menu drew from a widget, and two copies of those rules
would drift. There is now one copy, and a client never has to reason about
restarts at all. **Restart averaging** is the single control that changes
no setting, so it cannot be seen as a difference between two blocks and
travels as the action byte instead.

The settings block also carries the three per-reference window pairs. They
are modal state the operator built up rather than derived values, so a
client that sent only the live pair would silently flatten the other two
on the radio.

**The radio owns the settings.** A connecting client receives
`CMD_DIV_SETTINGS` and adopts what it finds rather than imposing what it
saved, so the radio behaves the same however it is being driven and a
second client sees what the first one set. `diversity_auto_save_state()`
returns early on a client for the same reason — a client is a radio in its
own right when it is not connected, and must not carry the last radio's
settings into its own next session. The block travels the other way too,
so a control moved on the radio's own panel reaches a watching client, and
`diversity_menu_refresh()` repaints whichever dialog did not originate the
change.

`INFO_DIVERSITY` is written straight into the `div_auto_*` globals by
`diversity_auto_apply_status()`. Every consumer — the status line, the
antenna line, the panadapter overlay — already reads those, so none of
them needed remote-aware code, and `update_manual_sensitivity()` in
particular needs no remote special case: `div_auto_running`,
`div_auto_mode` and `div_auto_hold` are all current on both sides, so the
same three terms grey the manual sliders in the same places. That is the
check that the split is in the right place.

Because `div_auto_running` is now true on a client as well,
`diversity_auto_start()`, `_stop()`, `_reset()`, `_invert()` and
`_gap()` all return early when `radio_is_remote`. The flag no longer means
"there is an engine here" — there never is one on a client — and without
those guards a client would try to tear down an engine it never built.
`radio_is_remote` is only ever set true and never cleared, so the guard is
a property of the process rather than something that can go stale.

The one control that does not travel is the `DIVERSITY_CAPTURE`
development button: it writes a file from inside the analysis thread, so
it belongs where that thread is.

`CLIENT_SERVER_VERSION` is `0x01300008`. Client and server check it on
connect and refuse a mismatch, so both ends must be built from the same
tree.


---

## 7. CPU cost

Measured by `test/diversity/bench_cpu.c`, on a 12th Gen Intel i7-12700K.
The last column is the fraction of one core needed to keep up with the
85.3 ms block period. **A Raspberry Pi is several times slower at this
kind of scalar double-precision work, so scale accordingly.**

| Mode | 48 kHz | 96 kHz | 192 kHz | 384 kHz |
|---|---|---|---|---|
| Window | 0.2 % | 0.4 % | 0.9 % | 1.7 % |
| Carrier | 0.2 % | 0.4 % | 0.9 % | 1.3 % |
| FSK/Digital | 0.3 % | 0.7 % | 1.0 % | 2.4 % |
| RADE V1, **searching** | 4.7 % | 5.4 % | 4.9 % | 7.1 % |
| RADE V1, searching, AM passband | 6.2 % | 6.5 % | 7.8 % | 7.2 % |
| RADE V1, locked | 0.5 % | 1.0 % | 1.8 % | 3.6 % |

The two searching rows are the same work over one pilot bank and over
two. An SSB passband names the bank, so only one is searched; AM, SAM and
FM say nothing and cost both. The saving is less than half because the
decimator is a fixed cost that grows with the sample rate — by 384 kHz it
dominates and the difference nearly vanishes.

Across the Resolution settings the per-block cost scales with nfft, but so
does the block period, so the cost per *second* is close to unchanged in
both directions.

Reading these:

- The transform modes scale with `nfft` and are cheap. FSK/Digital adds a
  partial sort for the median noise floor, capped at 4096 samples however
  wide the region is, which is why it stays with the rest of them. Window
  and Carrier add two sorts of 1024 for the branch noise floors, which is
  0.11 to 0.23 ms a block — 0.13 to 0.27 % of a core — and is flat in the
  sample rate, so it is proportionally largest at 48 kHz. FSK/Digital does
  not pay it: its own region already has unoccupied bins to measure from.
- **RADE V1 while searching is by far the peak load** and is nearly
  rate-independent, because acquisition works on the fixed 8 kHz decimated
  stream. It costs 3-4 points more than tracking does. On a Pi this is the
  number to watch: it is the state the engine sits in whenever there is no
  RADE signal to lock to.
- RADE V1 once locked scales with the sample rate, because what remains is
  the decimator.

On the protocol receive thread the added cost is four float stores and
one atomic load per sample pair, plus one `sem_post()` per block — well
under 1 % of a core at 384 kHz, and **no added audio latency**. There is
no mutex on that path at all; see "The queue" in §4.

Run it yourself with `make -C test/diversity bench`.

---

## 8. Timings

| Event | Time |
|---|---|
| Weight slew | a quarter of the Averaging setting, up to 0.5 s |
| Estimate settling | the Averaging control, 0.2-30 s (geometric) |
| **RADE V1 acquisition** | **1-5 s** of continuous signal |
| RADE V1 confirmation ("probation") | ~1 s of that |
| RADE V1 freeze when the pilot goes | ~1 s |
| RADE V1 lock drop | `DIV_HANG_DEFAULT`, 10 s, plus the ~1 s the freeze gate takes |

RADE V1 acquisition is in two parts. The search scores its grid at 8, 16
and 32 passes of a 120 ms modem frame, with the threshold coming down as
the integration lengthens, so a strong signal is found in about a second
and a weak one in under four. What it finds is a *candidate*: the tracker
then follows that one timing and frequency for eight frames, applying its
ordinary per-frame test, and **produces no weight until it confirms**. A
false alarm therefore costs a second and never moves the combiner.

This replaced a scheme that re-ran the entire blind search three times
before declaring lock — `3 x 32 x 120 ms`, 11.5 s for every signal however
strong. Confirming the one cell we care about answers the same question
for a fraction of the cost. Measured: 2.2 s to lock on synthetic signals,
USB and LSB, with and without CTUN.

Once locked, the frequency is tracked from the pilot-to-pilot phase
advance with a ~2 s time constant, which removes the 5 Hz search-grid
quantisation and follows the few Hz per minute a station drifts.

---

## 9. Files

| File | Role |
|---|---|
| `src/diversity_auto.c`, `.h` | The engine: tap, queue, worker, transform modes, occupancy split, weight |
| `src/rade_correlator.c`, `.h` | RADE V1 pilot correlation; the MVDR solve itself is `div_mvdr2()`, shared with FSK/Digital |
| `src/diversity_menu.c` | Controls and status |
| `src/rx_panadapter.c` | The analysis-window overlay, and the RADE modem passband |
| `src/receiver.c` | The combiner, the tap into it, and `rx_mode_changed()`, where a mode change reaches the modal settings |
| `src/radio.c` | Start/stop, props, shutdown, and `rxtx()`, where a transmit gap is reported |
| `src/new_protocol.c` | P2 DDC pairing and ADC configuration |
| `src/client_server.c`, `.h` | `CMD_DIV_SETTINGS` and `INFO_DIVERSITY` on the wire |
| `src/client_thread.c`, `src/server_thread.c` | Where those are sent and received |
| `test/diversity/` | Mode coverage, window placement including the CW zero, weighting and keying, RADE acquisition, FSK/Digital occupancy and MVDR, the modal per-mode blocks, props migration, CPU benchmark |

---

## 10. Related

- [`diversity-guide.md`](diversity-guide.md) — **start here**: what the
  feature does and how to use it, with worked examples
- [`diversity-rade.md`](diversity-rade.md) — the RADE V1 pilot correlator in detail
- [`diversity-measurements.md`](diversity-measurements.md) — what the
  combiners measurably do on recorded on-air captures, band by band.
  Ongoing; it is the record that decides what the constants should be
- [`diversity-digital-iq-proposal.md`](diversity-digital-iq-proposal.md) —
  the FSK/Digital reference in detail, and what the proposal it grew out
  of got wrong
- [`diversity-dither-fix.md`](diversity-dither-fix.md) — a P2 bug found
  along the way, where ADC1 never received the dither/random setting
- [`diversity-auto-phasing.md`](diversity-auto-phasing.md) — design
  history, including the approaches that were tried and abandoned. Not a
  description of current behaviour
