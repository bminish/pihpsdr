/* Copyright (C)
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

#include <gtk/gtk.h>
#include <math.h>
#include <semaphore.h>
#include <string.h>
#include <fftw3.h>

#include "atomic.h"
#include "diversity_auto.h"
#include "diversity_menu.h"
#ifdef __APPLE__
  #include "MacOS.h"        // for apple_sem()
#endif
#include "message.h"
#include "mode.h"
#include "property.h"
#include "radio.h"
#include "rade_correlator.h"
#include "receiver.h"
#include "vfo.h"

#ifdef DIVERSITY_CAPTURE
  //
  // DEVELOPMENT TOOL - remove with the rest of the capture instrument.
  // See src/diversity_capture.h and test/diversity/devtools/README.md.
  //
  #include "diversity_capture.h"
  //
  // Blocks the sample path lost immediately ahead of the one about to be
  // processed. The worker knows it; the capture hook, further down, needs
  // it to mark the discontinuity in the file.
  //
  static int divcap_dropped = 0;
#endif

//
// ----------------------------------------------------------------------
// Theory of operation
// ----------------------------------------------------------------------
//
// rx_add_div_iq_samples() forms  z = z0 + w*z1  with a single complex
// weight w that is flat across the whole DDC passband. This module works
// out a value for w.
//
// Three of the four reference modes do that from the cross spectrum of
// the two raw streams, as described below. The fourth, DIV_REF_RADE_V1,
// uses no transform at all: it hands the block to rade_correlator.c,
// which correlates against the known FreeDV RADE pilot and solves for an
// MVDR weight. See that file.
//
// Every block of nfft sample pairs is windowed and transformed. Writing
// X0 and X1 for the two spectra, we accumulate, over the bins k that fall
// inside the analysis window,
//
//     Sxy = sum X0(k) * conj(X1(k))     Sxx = sum |X0(k)|^2
//                                       Syy = sum |X1(k)|^2
//
// with an exponential forgetting factor across blocks, and then take
//
//   DIV_AUTO_NULL:  w = -Sxy/Syy    minimises E|z0 + w*z1|^2, i.e. it
//                                   subtracts whatever is common to both
//                                   antennas. This is the noise-cancelling
//                                   case and the default.
//
//   DIV_AUTO_SUM:   w = +Sxy/Sxx    equals conj(h) for z1 = h*z0, which is
//                                   maximum ratio combining when the two
//                                   channels carry equal noise power: the
//                                   antennas are co-phased and each is
//                                   weighted by its own signal strength.
//
// Note the two cases use *different* denominators. -Sxy/Syy and +Sxy/Sxx
// are not simply sign-flipped versions of one another.
//
// The quality of the fit is the magnitude squared coherence
//
//     gamma^2 = |Sxy|^2 / (Sxx*Syy)
//
// which is 1 when a single complex weight describes the relationship
// perfectly and 0 when the two antennas are unrelated. The loop holds
// (stops updating) below div_auto_coherence_min, which keeps it from
// wandering off when there is nothing worth combining.
//
// ----------------------------------------------------------------------
// Frequency bookkeeping
// ----------------------------------------------------------------------
//
// We tap the *raw* DDC streams, ahead of WDSP. The operator's passband
// (filter_low/filter_high) and the window controls are expressed in
// WDSP's shifted frame, where the tuned signal sits at zero. Converting
// between the two takes an offset and a sign, and both have been wrong
// here at different times.
//
// The offset. WDSP's frame is displaced from the dial by
//
//     frame_off = vfo[0].offset, less the CW sidetone frequency in CWU
//                 and plus it in CWL
//
// because rx_set_filter() folds the sidetone into filter_low/filter_high
// and rx_set_offset() takes it back out before handing the shift to WDSP.
// The panadapter draws the filter edges at cAp*filter_low + cAp*offset
// with the same sidetone terms, and WDSP's notch database compares
// absolute RF notch frequencies against flow + tunefreq + shift
// (wdsp/nbp.c). It is also the only arrangement that puts the CW passband
// on the dial frequency. So in *RF* terms, a shifted-frame frequency s is
// at dial + frame_off + s.
//
// The sign. The tapped buffer is spectrally inverted with respect to RF:
// a signal above the dial appears at a *negative* complex frequency in
// it, and one below the dial at a positive one. So
//
//     bin frequency = -(s + frame_off)
//
// which is what div_shift_to_bin() computes.
//
// That inversion is not derived, it is measured, and it has now been
// measured three times on air:
//
//   - the wideband RADE mode compares the energy in the modem band on
//     each side of the carrier. On an LSB RADE signal it found the energy
//     at positive bin frequencies;
//   - the V1 pilot correlator searches a normal and a mirrored pilot
//     bank. On an LSB signal it locks the *normal* bank - carriers at
//     +750..+2200 - twice, on separate occasions, by a wide margin
//     (7.97 against 4.75 on a weak signal);
//   - which is also what an operator expects: LSB inverts the audio on
//     transmit, and if the path to this tap inverts it again the two
//     cancel and the modem arrives the right way up.
//
// Reading the code does not give this answer, and three attempts to
// derive it produced two different wrong ones. The chain that ought to
// settle it - wdsp/shift.c, wdsp/analyzer.c and the panadapter's pixel
// mapping - cannot all three be read consistently with each other, and
// the measurement does not care. If this is ever revisited, revisit it
// with a signal, not with a text editor: put a known carrier a few kHz
// off the dial, run the Carrier reference, and see which way
// div_auto_carrier moves.
//
// ----------------------------------------------------------------------
//

//
// Tunables. Target bin width, in Hz. The FFT length is chosen per sample
// rate to land near this, so the frequency resolution and the block
// duration are the same whatever the radio is running at.
//
// The default target; the operator can ask for finer bins - see
// div_auto_resolution.
//
#define DIV_TARGET_BIN_HZ   12.0
//
// 2048, not 4096. The floor decides which bin widths a given sample rate
// can actually reach, and at 4096 the coarsest reachable width at 48 kHz
// was 11.72 Hz - so the 24 Hz setting the measurements ask for existed at
// 96 kHz and above and silently did nothing at 48. At 2048 every rate from
// 48 kHz up reaches 23.44 Hz in a 42.7 ms block, which is what makes the
// setting mean the same thing everywhere.
//
// Nothing else depends on the floor. The per-bin buffers are allocated at
// DIV_MAX_NFFT whatever is running, and the occupancy split's limits count
// bins in a region rather than bins in a transform.
//
// See Finding 43 in docs/diversity-measurements.md for why a coarser bin
// is wanted at all: both objectives want the short block, by up to 6.4 dB.
//
#define DIV_MIN_NFFT        2048
#define DIV_MAX_NFFT        65536

//
// Never let the automatic loop ask for more than this. The manual sliders
// go to +/-27 dB, but a large |w| means the aux antenna's own noise
// dominates the sum, and it costs headroom in everything downstream.
//
#define DIV_MAX_WEIGHT      10.0    // +20 dB

//
// How far div_mvdr2()'s denominator may cancel before the solve is called
// degenerate. A relative test, not an absolute one - see the note there.
// 1e-9 is seven orders of magnitude clear of double precision and still
// rejects a genuinely singular pair.
//
#define DIV_MVDR_EPS        1e-9

//
// DIV_AUTO_BEST: how much better one antenna must measure before the
// selection moves to it, and how fast the per-arm noise floor is allowed
// to creep up when the reference has no noise bins of its own and has to
// track a minimum over time instead.
//
// The hysteresis matters most where it matters least: two antennas within
// a decibel of each other are the case where the choice does not matter
// and the case where an ungated comparison would chatter between them.
//
// The floor rise is slow deliberately. It only has to outrun a change of
// band conditions, and anything faster starts following the signal it is
// supposed to be measuring underneath.
//
#define DIV_BEST_HYST_DB    1.0
#define DIV_FLOOR_RISE_DB   0.2     // dB per second

//
// The floor is tracked on power smoothed over this, not over the
// operator's averaging time.
//
// That distinction is the whole mechanism. Averaging is seconds to tens
// of seconds, longer than the gap between two overs and far longer than
// the gap between two syllables, so a minimum taken over the averaged
// power never sees a gap at all - it lands on a moment that still holds
// signal, on both arms, in the same ratio as the signal itself, and the
// estimate cancels to exactly 0.0 dB. Measured doing precisely that on
// the voice captures before this was separated out.
//
#define DIV_FLOOR_TAU       0.5     // seconds
//
// The window the branch noise ratio's own minimum is taken over, in
// seconds. Long enough to contain a gap between overs or between CW
// characters, short enough that a signal which never stops cannot hide
// behind a stale minimum from a minute ago. The effective window is
// between this and twice it - see div_arm_nratio_update().
//
#define DIV_NRATIO_WIN      5.0

//
// The output-level normaliser.
//
// receiver.c forms z0 + w*z1 with arm 0 pinned at unity, so the combined
// output is louder than one antenna by whatever the array does to it -
// measured at +1.5 to +8.0 dB per block across the capture set, of which
// +2.9 to +9.4 dB *more* than the SNR it bought. That is a level rise
// with nothing behind it: an operator switching diversity on hears the
// band get louder and cannot tell whether anything improved.
//
// The correction is one number: divide the output by the square root of
// its own power ratio against arm 0 alone, so the level stays where it
// was and an SNR gain arrives as the noise floor dropping instead. The
// ratio is
//
//   P_out / P_arm0 = 1 + |w|^2 * (Syy/Sxx) + 2 Re(conj(w) * Sxy) / Sxx
//
// over the analysis window, which is the band the operator listens to -
// not the whole DDC, where an out-of-band signal would drag it about.
//
// Smoothed over DIV_NORM_TAU because the raw ratio steps by up to 7 dB
// between blocks and would pump; smoothed, its block-to-block movement is
// under 0.31 dB at the ninetieth percentile on every capture measured.
// Clamped because nothing downstream should ever see a large gain from
// here. Off by default: it changes what every operator hears, and the
// part that decides whether it is an improvement - what the AGC does with
// it - is downstream of the capture tap and cannot be measured from a
// recording. See Finding 32 in docs/diversity-measurements.md.
//
#define DIV_NORM_TAU        1.0     // seconds
#define DIV_NORM_MAX        4.0     // +/- 12 dB, a guard rail not a setting

//
// How far the window power must stand above the tracked floor, on both
// arms, before that floor is taken to be noise.
//
// Without this the tracker answers confidently and wrongly. Its minimum
// is only a noise floor if the capture contained a moment with no signal
// in it; where the signal never stops, the minimum is signal too, and
// since both arms carry the same signal scaled by the same path the two
// minima are in the same ratio as the two powers. Everything then
// cancels and the estimate comes out at exactly 0.0 dB - not "the arms
// are equal" but "this method has told you nothing". Simulated and then
// confirmed on the 60 m RADE captures, which have no gaps in them at all
// and where it read +0.0 dB against a truth of +2.5.
//
// Six decibels is enough to say the floor was set under conditions
// genuinely different from now.
//
#define DIV_ARM_MIN_DB      6.0

//
// Fraction of the remaining distance to the target that w moves in one
// block. With ~85 ms blocks this settles in a little over a second from
// any starting point, which is fast enough to be useful and slow enough
// that the change in the mix is not heard as a step. A fixed absolute
// step was tried first and is wrong: the time to converge then depends on
// how far away the answer is, and a large |w| took the best part of a
// minute to reach.
//
#define DIV_SLEW_FRAC       0.15

//
// Number of bins either side of the carrier bin used in DIV_REF_CARRIER.
// The window spreads a pure tone over a few bins.
//
#define DIV_CARRIER_BINS    2

//
// FSK/Digital occupancy detection.
//
// How far a bin has to stand above the noise floor of the search region
// to count as occupied. 6 dB is deliberately low: the split only has to
// be good enough to keep the signal out of the noise covariance, and a
// bin wrongly called noise costs far more than one wrongly called
// signal - it puts the wanted signal into R and steers the null onto it.
//
#define DIV_OCC_DB          6.0

//
// The floor is the median of the bin powers over the region, which is
// robust to a signal filling a good part of it. Sorting is O(n log n) per
// block, so the number of bins that go into the estimate is capped and a
// wider region is sampled by striding rather than by sorting more. 4096
// bins is far more than the estimate needs and costs well under a
// millisecond.
//
#define DIV_OCC_MAX_SAMPLES 4096

//
// ------------------------------------------------------------------
// The per-arm noise floor, taken across frequency rather than time
// ------------------------------------------------------------------
//
// What a maximum-ratio weight is missing is N0/N1, the ratio of the two
// branch noises, and what the per-arm SNR readout needs is the same pair.
// Both used to come from a minimum over *time*: the quietest the window
// has recently been on each arm - see div_arm_floor_update() and
// div_arm_nratio_update(), which are still here and still feed the
// stand-down.
//
// A temporal minimum has one premise: that the band goes quiet often
// enough for the quietest recent moment to be noise. DIV_ARM_MIN_DB is
// the guard on it, and it refuses to answer for a signal that never
// stops - which is the case the comment beside it describes.
//
// **A fading carrier defeats that guard**, and Finding 47 in
// docs/diversity-measurements.md is the measurement. On a 41 m broadcast
// whose carrier fades 17 dB peak to peak the fades supply the clearance
// the guard asks for: the minima land in them, the smoothed power stands
// well clear the rest of the time, and what gets published on 78 % of
// blocks is a ratio of two independent fades. It read +10.5 dB where the
// truth was -0.35, put 19 dB of surplus arm 1 into the Sum weight, and
// cost 2.05 of the 2.60 dB that capture had to give. The same corruption
// reaches the per-arm SNR through div_arm_from_floor(): 8.7 dB out with
// its sign inverted, so Best chose the wrong antenna on 95.6 % of blocks.
//
// The estimate here has no such premise, because it never waits for a
// gap. The noise floor is what the *quietest bins of this block* sit at,
// and there are thousands of them in the DDC passband every block,
// outside the operator's filter where no wanted signal can be. A fade
// takes the signal down and leaves those bins exactly where they were, so
// a fade cannot be mistaken for silence; a carrier that never stops is
// not a difficulty either, because nothing is being waited for.
//
// Measured against the whole capture set - 26 files, 48 and 192 kHz, four
// transform sizes, signals from FT8 to DRM to bare band noise - the
// median error is inside 0.3 dB on twenty of them and 1.3 dB on all but
// two, against 1.4 to 10.6 dB for the temporal minimum, which also
// produces no answer at all on six. The two apparent outliers are the
// estimate being right: on `122632` it follows the operator's ADC1
// attenuator one for one from 0 to 16 dB while arm 0's floor holds to
// 0.7 dB, and on `002710` it finds the two undocumented attenuator steps
// that capture's format-version-1 header could not record.
//
// A low percentile rather than a minimum or a mean: a minimum over
// thousands of bins is the low tail of the noise distribution and moves
// with the bin count, a mean is dragged up by anything transmitting in
// the sampled span, and a tenth percentile tolerates the span being up to
// ninety per cent occupied before it starts to read the occupants.
//
#define DIV_NF_PCT          10
//
// ...averaged over a band of order statistics either side of it, rather
// than read off as a single one. A lone percentile is one sample of a
// noisy distribution and its scatter goes straight into the weight; two
// percentiles either side is about forty sorted values at
// DIV_NF_SAMPLES, the array is already sorted so they cost nothing, and
// the ratio's block-to-block scatter drops from about 0.7 dB to 0.3.
// Still a percentile, so it needs no distribution assumption - which a
// trimmed mean scaled back to the true mean would.
//
#define DIV_NF_BAND         2
//
// Bins sampled per arm per block. Two qsorts of this length is the whole
// cost, and at 1024 the percentile's own scatter is a few tenths of a
// decibel before DIV_NF_TAU smooths it. Same striding idea as
// DIV_OCC_MAX_SAMPLES: a wider span is sampled, not sorted in full.
//
#define DIV_NF_SAMPLES      1024
//
// Below this many candidate bins there is not enough spectrum outside the
// filter to say anything, and the temporal floor is used instead. Reached
// only by a hand-placed window far wider than the passband it sits in.
//
#define DIV_NF_MIN_BINS     128
//
// Keep this clear of the filter edge. The window is Blackman-Harris, so
// leakage is not what this is for; it is the filter skirt itself, and
// signals the operator has tuned close enough to hear the edge of.
//
#define DIV_NF_SKIRT_HZ     1000.0
//
// Sample the central fraction of the transform only. The DDC's own
// response is not flat at the edge of its passband, and a floor measured
// in the roll-off is a measurement of the roll-off.
//
#define DIV_NF_SPAN         0.80
//
// Smoothing, seconds. Long enough to take the scatter out of a
// percentile, short enough that the step attenuators - which reset the
// statistics anyway - are followed rather than averaged through.
//
#define DIV_NF_TAU          2.0

//
// Fewer occupied bins than this and there is nothing worth calling a
// signal, whatever the coherence says.
//
#define DIV_OCC_MIN_BINS    3

//
// How coherent a single bin has to be to count as occupied.
//
// This used to be div_auto_coherence_min, which put two unrelated jobs on
// one control: moving the slider changed *which bins the estimate is made
// from* as well as whether the estimate was acted on. They want different
// numbers and they answer different questions - this one is a false-alarm
// test on a single bin, where gamma^2-hat is biased upward by about 1/N
// for N averages and a noise-only bin is not silent but merely small.
//
// 0.30 is what the shared control shipped at, so nothing moves until it
// is swept. What it should be is measured against the no-signal captures:
// `231532` has no signal anywhere and still produced a weight on 30 % of
// blocks. See Finding 26 in docs/diversity-measurements.md.
//
#define DIV_OCC_COH         0.30

//
// How far this block's power in the bins being measured may fall below
// the smoothed power accumulated over them before the statistics are
// declared stale and the loop holds.
//
// This is what notices a transmission ending, and without it nothing
// does. The accumulators forget exponentially, so Sxy, Sxx and Syy decay
// together and the coherence gate sees gamma^2 stay near 1 the whole way
// down; the FSK/Digital occupancy test is a ratio against the median
// floor and is scale invariant, so it does not see the level collapse at
// all. Left to the forgetting factor alone a 30 dB signal at tau = 2 s
// keeps the loop "tracking" for 5.8 tau - about twelve seconds of walking
// the weight around on noise, once per gap.
//
// Comparing the two answers "is the thing these statistics describe still
// there", which is the question that actually matters, and it scales
// itself: it fires on a signal well out of the noise, which is exactly
// the case where stale statistics do the most harm, and stays quiet on a
// weak one, where they contain little signal to be stale about.
//
// It matters most on a keyed mode. CW is the extreme case - the signal is
// absent for most of a transmission, not just between them - and the loop
// previously spent every key-up period adjusting the weight on noise. It
// now measures only while there is something to measure.
//
// 10 dB is comfortably past ordinary fading and far short of a signal
// stopping. Holding through a deep fade is wanted anyway.
//
#define DIV_STALE_DB        10.0

//
// How far over its own tracked floor an arm has to sit before the window
// is credited with containing anything.
//
// The same six decibels as DIV_ARM_MIN_DB, and deliberately the same
// number: "this arm carries something" is one question and it should not
// have two answers in one file. div_arm_from_floor() asks it of both arms
// to decide whether the per-arm comparison means anything;
// div_window_quiet() asks the complement of it, of both arms, to decide
// whether there is anything here at all.
//
#define DIV_QUIET_DB        DIV_ARM_MIN_DB

//
// How long the window has to stay both held and quiet before the
// combiner is stood down, in seconds.
//
// Without it the test fires on any block that is momentarily both, which
// includes the deep fades on a signal that never stops - there the
// bounded minimum has climbed onto the signal itself, so "quiet" is
// measured against the signal rather than against the noise and reads
// true whenever the signal dips. Measured: no dwell costs 0.47 dB in
// speech on `000537` and 0.42 dB on `122211`, both of which have a
// station in the window throughout.
//
// Two seconds is longer than the 0.5 to 4.1 s coherence time's dips and
// far shorter than a gap between overs, which is what this exists to
// catch. It also covers the other end of the same problem: standing down
// and slewing back up at every syllable would cost the start of every
// over.
//
#define DIV_QUIET_DWELL     2.0

//
// Bins either side of an occupied one that are excluded from the noise
// covariance as well as from the signal.
//
// Without this the mode cancels the signal it is trying to receive. A
// strong signal spreads past its own bins - the analysis window's skirts
// are 92 dB down but a signal 40 dB above the noise still puts more into
// its neighbours than the noise floor holds - and those bins are
// correlated between the arms with the signal's own channel. Feeding them
// to R tells MVDR that the direction the signal arrives from is
// interference, and it dutifully steers the null onto it.
//
// This is the standard failure of MVDR trained on data containing the
// desired signal, and a guard band is the standard answer to it. Four
// bins is where the Blackman-Harris skirts have gone; the cost is four
// bins of noise estimate either side of the signal, which a mostly empty
// passband has plenty of.
//
#define DIV_OCC_GUARD       4

//
// How far the receiver may be retuned before the accumulated statistics
// are thrown away.
//
// This was an exact comparison, so every click of the tuning knob - one
// hertz on a fine step - discarded the channel estimate, the covariance
// and the correlator's lock. Measured on a recorded capture of an
// operator tuning around: 23 resets over 30 analysis blocks, a median of
// one block between them, against the 31 blocks the operator's averaging
// time asks for. The mode spends the whole of a tune permanently in the
// first block or two of an estimate that needs thirty-one, and stays
// there for an averaging time after the knob stops.
//
// The antenna-to-antenna transfer h1/h0 is a property of the two antennas
// and the path. It does not change because the dial moved a hertz. What a
// retune changes is *which* signal is in the window, and 20 Hz is far too
// little to change that: it is under a tenth of the narrowest CW filter,
// and well inside the +/-60 Hz the RADE correlator tracks, so a lock
// survives it. Tuning across a band to a different station moves
// kilohertz and still resets.
//
// Cumulative, not per block: the comparison below is against the context
// as it was at the last reset, so twenty single-hertz steps count as
// twenty hertz. Comparing against the previous block would never fire at
// all, which would be worse than resetting too often.
//
#define DIV_RETUNE_HZ       20


//
// Bin-weighting for the wideband window.
//
// Flat sums the cross and auto spectra over the window and divides, which
// makes the answer a power-weighted average of h(f): dominated by the
// loudest bins whether or not the two antennas actually agree there, and
// diluted by noise-only bins that add to the denominator but not the
// numerator.
//
// Coherence weights each bin by how well the antennas agree in it, so
// bins carrying signal dominate and noise-only bins fall out. That is
// what makes a wide window usable on SSB voice, where the energy moves
// around constantly and there is no carrier to sit on.
//
// (the enum itself is in diversity_auto.h)

//
// How long a RADE lock is held after the pilot stops being detectable,
// before the correlator gives up and searches again.
//
// It no longer has a control. Swept from 1 to 10 s on the two captures in
// the set that can be scored on decode it moves lock uptime from 38 % to
// 94 % and synced frames by +10, +11, +10, +10 - which is well inside the
// scatter of the measurement - so the correlator's uptime is telling the
// truth about the pilot lock and nothing at all about what the modem does
// with the audio. There is nothing here for an operator to tune. See
// Findings 33, 35 and 41 in docs/diversity-measurements.md.
//
// The long end is the value kept: it re-acquires least often, and the one
// case that might argue for a short hang - several stations taking turns
// on one frequency, each wanting its own weight - is not in the capture
// set and so is not evidence.
//
#define DIV_HANG_DEFAULT 10.0

int    div_auto_mode           = DIV_AUTO_OFF;
int    div_auto_ref            = DIV_REF_BAND;
int    div_auto_follow_filter  = 1;
double div_auto_centre         = 0.0;
double div_auto_width          = 1000.0;
double div_auto_tau            = 2.0;
double div_auto_hang           = DIV_HANG_DEFAULT;
double div_auto_coherence_min  = 0.20;
int    div_auto_weighting      = DIV_WEIGHT_FLAT;
int    div_auto_normalise      = 0;
double div_auto_resolution     = DIV_TARGET_BIN_HZ;

//
// The window controls are modal: DIV_REF_BAND, DIV_REF_CARRIER and
// DIV_REF_DIGITAL_IQ each keep their own centre and width, so moving
// between them does not destroy the others' settings. div_auto_centre and
// div_auto_width always hold the pair for whichever reference is
// selected; these hold the pairs for the rest.
//
double div_band_centre         = 0.0;
double div_band_width          = 1000.0;
double div_carrier_centre      = 0.0;
double div_carrier_width       = 1000.0;
//
// The digital default is the whole SSB audio passband rather than a
// narrow slice: occupancy narrows it from there, so the operator does not
// have to know how wide the signal is. It is only reachable at all with
// the follow tick cleared.
//
double div_digital_centre      = 0.0;
double div_digital_width       = 2600.0;

//
// So is the coherence threshold, and for a stronger reason than the
// windows: the four references do not compare the same quantity, so one
// number cannot mean one thing.
//
//   Window, Carrier    gamma^2 over the analysis window
//   FSK/Digital        gamma^2 over the *occupied* bins only
//   RADE V1            rade_corr_quality, which is acc_sig/(acc_sig+r00) -
//                      a signal fraction, not a coherence at all
//
// For equal arms and uncorrelated noise a gamma^2 gate at g demands a
// per-arm SNR of sqrt(g)/(1-sqrt(g)), and a quality gate at q demands
// q/(1-q). At 30 % that is +0.8 dB per arm against -3.7 dB on the pilot:
// the same slider position asking for four and a half decibels more
// signal in one mode than in the other. And gamma^2-hat is biased - a
// window of pure noise still reports about 1/N for N averages - so the
// same threshold is a different false-alarm risk in a five-bin carrier
// window and a two-hundred-bin passband, and moves again with Averaging
// and Resolution.
//
// The parameter was already modal, but on the mode group, which is the
// wrong axis: what fixes the meaning is the reference. See Finding 26 in
// docs/diversity-measurements.md.
//
// Defaults reproduce the single 0.30 that shipped before, except on RADE
// V1, where the gate was never applied at all and 0.0 is what "as it was"
// means - and on Window, which is 0.20 because the weighting it gates
// changed underneath it.
//
// The two are one decision. Coherence weighting inflates gamma^2, so the
// same number was a laxer test with it than without: measured over
// thirty-two captures, flat wants a threshold about 0.10 lower for the
// same false-alarm rate. Flat at 0.20 gives 5.2 % false alarms against
// coherence at 0.30's 5.7 %, and 0.30 dB more signal with it. Moving one
// without the other would have moved every operator's operating point.
// See Findings 27, 29 and 40.
//
double div_band_cohmin         = 0.20;
double div_carrier_cohmin      = 0.30;
double div_digital_cohmin      = 0.30;
double div_rade_cohmin         = 0.0;

//
// Set when the requested window had to be pulled inside the Nyquist
// limit, so the UI can say so rather than quietly measuring elsewhere.
//
int    div_auto_clamped        = 0;

//
// The bin width actually achieved, which is not always the one asked for:
// nfft is capped at DIV_MAX_NFFT.
//
double div_auto_binhz          = 0.0;

double div_auto_coherence      = 0.0;
int    div_auto_holding        = 1;
double div_auto_carrier        = 0.0;
int    div_auto_carrier_valid  = 0;

//
// Set when the loop has stood the combiner down because there is nothing
// in the window - as distinct from holding, which leaves the last weight
// applied. See div_hold_or_stand_down().
//
int    div_auto_standdown      = 0;
double div_auto_arm_db         = 0.0;
int    div_auto_arm_valid      = 0;
int    div_auto_arm_pick       = 0;

//
// Per-arm noise floors for the references that have no noise bins to
// measure one in. See div_arm_floor_update().
//
static double arm_floor0 = 0.0, arm_floor1 = 0.0;
static int    arm_floor_valid = 0;
static double arm_pw0 = 0.0, arm_pw1 = 0.0;
//
// The branch noise ratio N0/N1. The Sum weight needs it and the two
// wideband references have no other source for it - see
// div_arm_nratio_update().
//
//
// The output-level normaliser. div_norm scales the combined output in
// receiver.c; the three smoothed window statistics behind it are here.
//
static double nrm_xx = 0.0, nrm_yy = 0.0, nrm_xy_re = 0.0, nrm_xy_im = 0.0;
static int    nrm_valid = 0;
static double nr_f0 = 0.0, nr_f1 = 0.0;        // its own smoother, seeded
static int    nr_f_valid = 0;
static double nr_cur0 = 0.0, nr_cur1 = 0.0;    // minimum over the slot in progress
static double nr_prev0 = 0.0, nr_prev1 = 0.0;  // over the slot before it
static int    nr_slot_left = 0, nr_have_prev = 0;
//
// The same pair of minima, published for div_window_quiet(). Written
// where they are computed rather than recomputed there, so the presence
// test and the branch noise ratio can never be looking at different
// instants.
//
static double nr_min0 = 0.0, nr_min1 = 0.0;
static int    nr_min_valid = 0;
//
// Consecutive blocks the loop has been both holding and looking at an
// empty window, and the weight that was in force when the count ran out.
// See DIV_QUIET_DWELL and div_hold_or_stand_down().
//
static int    quiet_run = 0;
static double quiet_wr = 0.0, quiet_wi = 0.0;
static double arm_nratio = 1.0;
static int    arm_nratio_valid = 0;

static double arm_fast0 = 0.0, arm_fast1 = 0.0;

double div_auto_occ_lo         = 0.0;
double div_auto_occ_hi         = 0.0;
int    div_auto_occ_valid      = 0;

int    div_auto_running        = 0;

//
// FFT state, owned by the analysis thread once it is started
//
static int             nfft = 0;
static double          binhz = 0.0;
static double          blocktime = 0.0;
static float          *window = NULL;
static fftwf_complex  *fftin0 = NULL, *fftin1 = NULL;
static fftwf_complex  *fftout0 = NULL, *fftout1 = NULL;
static fftwf_plan      plan0, plan1;
static int             have_plans = 0;

//
// Sample collection. fill[] is written by the RX sample path, work[] is
// read by the analysis thread; the two are swapped when a block is ready.
//
//
// A short queue rather than a single slot.
//
// The original design handed over one block at a time and dropped any
// block that arrived while the worker was busy, on the grounds that the
// estimate moves far more slowly than one block. That is true for the
// three transform-based reference modes and quite wrong for RADE V1,
// which tracks the pilot by *absolute* decimated sample index and carries
// the NCO phase and the decimator delay line across blocks. A dropped
// block slides the real pilot by a non-multiple of the modem frame -
// 682 samples at 192 kHz against a 960-sample frame - which the one
// sample of timing nudge in the tracker cannot recover, so the lock is
// lost a few seconds later.
//
// It was also self-inflicted: acquisition is by far the most expensive
// thing the worker does, so drops were most likely precisely while
// searching, and were then repeated for up to RADE_ACQ_PASSES passes.
//
// The queue holds DIV_QUEUE buffers, one of which is always the one being
// filled, so at most DIV_QUEUE-1 are ever waiting.
//
#define DIV_QUEUE 4

static float          *qbuf0[DIV_QUEUE], *qbuf1[DIV_QUEUE];

//
// Ring pointers, in the style of the protocol ring buffers - see the
// DDC packet queue in new_protocol.c. q_head is written only by the
// sample path and q_tail only by the worker, so with a memory barrier
// on each side this is a single-producer/single-consumer ring and needs
// no mutex. One slot is always the block being filled, so the head can
// never catch the tail and q_head == q_tail is unambiguously "empty".
//
static volatile atomic_int q_head = 0;  // slot being filled
static volatile atomic_int q_tail = 0;  // slot being processed

//
// Owned by the sample path. diversity_auto_start() initialises these
// before it sets div_auto_running, and nothing else writes them while
// the engine is running.
//
static float          *fill0 = NULL, *fill1 = NULL;
static int             fillptr = 0;

//
// Owned by the worker.
//
static float          *work0 = NULL, *work1 = NULL;

//
// Discontinuities in the sample stream: blocks the sample path had to
// throw away because the queue was full, and partial blocks it threw
// away at a transmit gap. Read by the worker, because a discontinuity
// invalidates RADE V1's pilot timing and it has to re-acquire rather
// than carry on against a pilot that has silently moved.
//
// q_gap[] is written by the sample path immediately before it publishes
// q_head, and read by the worker after it has seen that q_head, so the
// publication orders it and it needs no atomics of its own. It stays a
// plain int[] so that the memset in diversity_auto_start() remains
// legal, which it would not be on an array of atomics.
//
//
// A gap in the sample stream is recorded against the slot that follows
// it, not in one counter read at dequeue time.
//
// A drop can only happen when the queue is full, which means DIV_QUEUE-1
// blocks from *before* the gap are still waiting. Reading a global
// counter at dequeue therefore reset the correlator three blocks early:
// it re-acquired on pre-gap data and then tracked straight through the
// discontinuity with nothing to tell it, which is exactly the failure -
// the pilot slides by a non-multiple of the modem frame and the lock dies
// a few seconds later - that this mechanism exists to prevent.
//
static int             q_pending_drop = 0;   // discontinuities since the last enqueue
static int             q_gap[DIV_QUEUE];     // discontinuities ahead of each queued slot

//
// Requests from other threads, carried as generations rather than as
// flags. The thread that acts on a request only ever writes its own
// *_seen copy, so acknowledging one request cannot erase another that
// arrived while it was being acknowledged - which a test-and-clear flag
// can do, and did.
//
// Two threads bumping the same generation may lose an increment, but
// never a request: any change of value is what the reader acts on.
//
static volatile atomic_int gap_seq   = 0;    // rxtx()  -> sample path
static int             gap_seen = 0;         // sample path private
static volatile atomic_int reset_seq = 0;    // GTK/net -> worker
static int             reset_seen = 0;       // worker private

//
// Raised once per block enqueued, and once by diversity_auto_stop() to
// wake the worker so that it can see worker_run go to zero. Created on
// the first start and never destroyed - see the note in
// diversity_auto_start().
//
static int             div_sem_created = 0;
#ifdef __APPLE__
  static sem_t        *div_sem = NULL;
  #define DIV_SEM        div_sem
#else
  static sem_t         div_sem;
  #define DIV_SEM        (&div_sem)
#endif

static volatile atomic_int worker_run = 0;
static GThread        *worker = NULL;

//
// Accumulated statistics
//
static double          acc_xy_re, acc_xy_im, acc_xx, acc_yy;

//
// Per-bin running cross and auto spectra, allocated at DIV_MAX_NFFT with
// the rest of the buffers. Indexed by wrapped bin, so only the bins
// inside the current window are ever touched.
//
static double         *bin_xy_re = NULL, *bin_xy_im = NULL;
static double         *bin_xx = NULL, *bin_yy = NULL;
static int             acc_valid = 0;

//
// Scratch for the FSK/Digital noise-floor median. Sized at
// DIV_OCC_MAX_SAMPLES rather than DIV_MAX_NFFT because the estimate is
// strided down to that many bins however wide the region is.
//
static double         *occ_scratch = NULL;

//
// Scratch for the per-arm noise floor, one buffer per arm. See
// DIV_NF_SAMPLES.
//
static double         *nf_scratch0 = NULL, *nf_scratch1 = NULL;

//
// The per-arm noise floor itself, in power per bin, and whether it has
// been established. Smoothed at DIV_NF_TAU; seeded from the first block
// rather than started at zero, for the reason div_arm_nratio_update()
// gives - a smoother that starts at zero reads its own startup transient
// as the quietest the band has been.
//
static double          div_nf0 = 0.0, div_nf1 = 0.0;
static int             div_nf_valid = 0;

//
// Which bins were found occupied, by wrapped index, so the noise pass can
// keep its distance from them. See DIV_OCC_GUARD.
//
static unsigned char  *occ_mask = NULL;

//
// Everything the bin mask depends on. When any of it changes the
// accumulated statistics describe a different measurement and have to be
// thrown away, so we watch it here rather than hooking every call site
// that could move the radio.
//
struct div_context {
  long long frequency;
  long long ctun_frequency;
  long long offset;
  int       sidetone;
  int       sample_rate;
  int       mode;
  int       filter_low;
  int       filter_high;
  int       ref;
  int       follow;
  double    centre;
  double    width;
  int       weighting;
  int       att0;
  int       att1;
};

static struct div_context lastctx;

//
// +1 when the RADE modem is above the tuned carrier in this frame, -1
// when below. Written by DIV_REF_RADE_V1 on every block, from the
// operator's passband, and read by the menu and the panadapter overlay.
// The other references leave it alone.
//
static int div_rade_side = 1;

//
// Set when the next weight update should be applied without slewing.
//
static int div_jump = 0;

//
// Operator hold. The analysis carries on; only the application of its
// answer is suspended. Not persisted - it is an operating state, not a
// setting, and coming back up held would be baffling.
//
int    div_auto_hold = 0;
double div_track_gain = 0.0;
double div_track_phase = 0.0;

//
// Smoothed carrier frequency, shifted frame, for DIV_REF_CARRIER.
//
static double div_carrier_hz = 0.0;


//
// Swap Null for Sum, or the other way about.
//
// The two are the same measurement with the sign of the answer and the
// normalising power exchanged, so they land essentially 180 degrees
// apart. Setting div_jump alone was not enough: it only takes effect when
// the loop next produces a weight, and it may not be producing one - the
// coherence gate can be holding, the RADE correlator can be frozen on a
// fade, and under operator Hold nothing is applied at all. The control
// then changed which answer was being computed while leaving the audio
// exactly as it was, which is not what "invert" means to anyone.
//
// So the weight in force is turned through 180 degrees here and now,
// whatever the loop is doing, and div_jump is set so that when the loop
// does have something it goes straight there rather than slewing.
//
// Under Hold this acts on the operator's own manual weight, which is the
// only thing being applied then, and is exactly what is wanted.
//
void diversity_auto_invert(void) {
  if (radio_is_remote) { return; }

  div_cos = -div_cos;
  div_sin = -div_sin;
  div_phase += 180.0;

  while (div_phase >  180.0) { div_phase -= 360.0; }

  while (div_phase < -180.0) { div_phase += 360.0; }

  //
  // The magnitude is unchanged, so div_gain is left alone.
  //
  div_jump = 1;
}

void diversity_auto_set_hold(int on) {
  on = on ? 1 : 0;

  if (on == div_auto_hold) { return; }

  div_auto_hold = on;

  if (!on) {
    //
    // Apply what the loop has tracked to meanwhile, in one step. Done by
    // the analysis thread on its next block rather than here, so the
    // weight is only ever written from one place.
    //
    div_jump = 1;
  }

  //
  // Hold hands the weight back to the operator, so it changes whether the
  // loop owns it - a remote client's sliders go live and dead with this.
  //
}

int div_rade_side_get(void) {
  return div_rade_side;
}

static void div_reset_stats(void) {
  acc_xy_re = acc_xy_im = acc_xx = acc_yy = 0.0;

  if (bin_xy_re != NULL) {
    //
    // The bins in use, not the whole allocation. Every index into these
    // is taken modulo nfft, so nfft entries is all that can be reached,
    // and nfft is set before this runs on every path that changes it -
    // diversity_auto_start() computes it before the reset below it, and
    // it cannot change while the worker is running. At the default
    // 12 Hz on a 48 kHz DDC that is 128 KB rather than 2 MB, and this
    // runs from div_process_block() on every context change - tuning
    // across a band raises one about once a block.
    //
    const size_t n = (size_t)((nfft > 0 && nfft <= DIV_MAX_NFFT) ? nfft : DIV_MAX_NFFT);
    memset(bin_xy_re, 0, n * sizeof(double));
    memset(bin_xy_im, 0, n * sizeof(double));
    memset(bin_xx,    0, n * sizeof(double));
    memset(bin_yy,    0, n * sizeof(double));
  }

  acc_valid = 0;
  arm_floor_valid = 0;
  arm_floor0 = arm_floor1 = 0.0;
  div_nf0 = div_nf1 = 0.0;
  div_nf_valid = 0;
  arm_pw0 = arm_pw1 = 0.0;
  nrm_xx = nrm_yy = nrm_xy_re = nrm_xy_im = 0.0;
  nrm_valid = 0;
  div_norm = 1.0;
  nr_f0 = nr_f1 = 0.0;
  nr_f_valid = 0;
  nr_cur0 = nr_cur1 = nr_prev0 = nr_prev1 = 0.0;
  nr_slot_left = 0;
  nr_have_prev = 0;
  //
  // The published minimum goes with the slots it was taken from. Left
  // standing, it is a minimum of the *previous* band measured against a
  // smoother that has just been re-seeded on this one - so a retune from
  // a noisy band to a quiet one, or a step attenuator moved, makes
  // div_window_quiet() true on the first block and stands the combiner
  // down on a band nothing has been measured on yet. Two DIV_NRATIO_WIN
  // slots pass before div_arm_nratio_update() writes them again.
  //
  nr_min0 = nr_min1 = 0.0;
  nr_min_valid = 0;
  arm_nratio = 1.0;
  arm_nratio_valid = 0;
  arm_fast0 = arm_fast1 = 0.0;
  div_auto_arm_valid = 0;
  div_auto_arm_db = 0.0;
  div_auto_coherence = 0.0;
  div_auto_holding = 1;
  //
  // Not stood down, merely not started: the floor tracker has nothing yet
  // and div_window_quiet() is false until it has, so this only has to
  // survive a reference change - the flag is set in the wideband path and
  // no other reference clears it.
  //
  div_auto_standdown = 0;
  quiet_run = 0;
  div_carrier_hz = 0.0;
  div_auto_carrier_valid = 0;
  div_auto_occ_valid = 0;
  div_auto_occ_lo = 0.0;
  div_auto_occ_hi = 0.0;
  //
  // Start the tracked readout from what is actually applied, so it does
  // not claim 0 dB / 0 degrees before the loop has produced anything.
  //
  div_track_gain = div_gain;
  div_track_phase = div_phase;
}

//
// One of the two step attenuators moved by delta_db, and the new value is
// not in place yet. Everything the loop has accumulated was measured with
// the old front ends, so it is discarded by div_context_changed() on the
// next block; what cannot wait for the next block is the weight in force,
// because the operator would hear the step.
//
// div_gain is the arm-1 gain in dB relative to arm 0. Attenuating arm 0
// by delta makes arm 0 smaller, so the correct ratio falls by delta;
// attenuating arm 1 raises it by delta. Applying that here keeps the
// combined audio continuous across the change - and, under Hold or with
// the loop off, keeps the operator's own manual weight valid, which is
// the thing tying the two attenuators together used to protect.
//
void diversity_auto_att_changed(int a, int delta_db) {
  if (radio_is_remote) { return; }

  const double shift = (a == 1) ? (double)delta_db : -(double)delta_db;
  //
  // Scale what is being applied, then back-compute the readout, which is
  // the same order div_apply_weight() uses. Doing it the other way about
  // would step the audio: div_cos/div_sin are the slewed values actually
  // in force and div_gain/div_phase merely describe them.
  //
  const double k = pow(10.0, 0.05 * shift);
  div_cos *= k;
  div_sin *= k;
  double mag = sqrt(div_cos * div_cos + div_sin * div_sin);

  if (mag > DIV_MAX_WEIGHT) {
    div_cos *= DIV_MAX_WEIGHT / mag;
    div_sin *= DIV_MAX_WEIGHT / mag;
    mag = DIV_MAX_WEIGHT;
  }

  if (mag > 1.0e-9) {
    div_gain = 20.0 * log10(mag);
  } else {
    div_gain = -27.0;
  }

  if (div_gain >  27.0) { div_gain =  27.0; }

  if (div_gain < -27.0) { div_gain = -27.0; }

  //
  // The phase is untouched - k is real and positive - so div_phase needs
  // no recomputing. The tracked readout moves with the applied one, since
  // the loop's answer for the old front ends is now the wrong one by
  // exactly this much.
  //
  div_track_gain += shift;

  if (div_track_gain >  27.0) { div_track_gain =  27.0; }

  if (div_track_gain < -27.0) { div_track_gain = -27.0; }
}

void diversity_auto_reset(void) {
  if (radio_is_remote) { return; }

  //
  // Called from a UI thread or from rxtx(). Zeroing the transform accumulators from
  // here is harmless - the worker only ever adds to them, so the worst
  // case is one block's contribution lost.
  //
  // rade_corr_reset() is a different matter: it clears the correlator's
  // lock state and memsets an 80 KB accumulation grid that the worker may
  // be part way through reading. So it is requested here and performed by
  // the worker between blocks instead.
  //
  div_reset_stats();
  reset_seq++;
}

//
// Pick an FFT length for this sample rate. Powers of two only, so that
// fftw takes its fast path.
//
//
// Pick the transform length for a requested bin width.
//
// Finer bins raise a weak carrier further out of the per-bin noise floor,
// which is the real sensitivity control - averaging only reduces the
// variance of an estimate, it does not lift the signal. The cost is
// responsiveness: the block period is nfft/rate, so every halving of the
// bin width doubles it.
//
// nfft is capped at DIV_MAX_NFFT rather than growing to meet the request,
// because the buffers are allocated at the cap whatever rate is running.
// The achieved bin width is published in div_auto_binhz so the UI can
// show what was actually obtained.
//
static int div_choose_nfft(int sample_rate, double target_hz) {
  int n = DIV_MIN_NFFT;

  if (target_hz < 0.5) { target_hz = 0.5; }

  while (n < DIV_MAX_NFFT && (double)sample_rate / (double)n > target_hz) {
    n <<= 1;
  }

  return n;
}

//
// 4-term Blackman-Harris. The whole point of the analysis window is to
// look at one narrow slice of spectrum and ignore everything else, so the
// -92 dB sidelobes are worth having over the -31 dB of a Hann.
//
static void div_make_window(void) {
  const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;

  for (int i = 0; i < nfft; i++) {
    double x = 2.0 * M_PI * (double)i / (double)nfft;
    window[i] = (float)(a0 - a1 * cos(x) + a2 * cos(2.0 * x) - a3 * cos(3.0 * x));
  }
}

//
// Offset of WDSP's shifted frame from our raw one: raw = shifted +
// div_frame_off(). See the frequency bookkeeping note at the top.
//
static double div_frame_off(const struct div_context *ctx) {
  double off = (double)ctx->offset;

  if (ctx->mode == modeCWU) {
    off -= (double)ctx->sidetone;
  } else if (ctx->mode == modeCWL) {
    off += (double)ctx->sidetone;
  }

  return off;
}

//
// Shifted frame -> the frequency our FFT bins are indexed by. Both the
// displacement and the inversion; see the note at the top.
//
static double div_shift_to_bin(const struct div_context *ctx, double s) {
  return -(s + div_frame_off(ctx));
}

//
// Where zero is for a hand-placed window, in the shifted frame.
//
// Everywhere but CW that is the shifted frame's own zero: the operator
// tunes the signal to the dial frequency and the window controls read as
// an offset from it. In CW it is not. rx_set_filter() folds the sidetone
// into filter_low/filter_high, so a CW passband sits at +pitch in CWU and
// -pitch in CWL, and the shifted-frame zero is one pitch away from the
// only signal there is - a window centred on 0 measured a patch of empty
// spectrum a pitch off the tone the operator was listening to.
//
// Following the RX filter never had the problem, because it takes the
// folded edges; this puts the hand-placed window on the same reference,
// so that centre 0 is the zero-beat note in every mode and the two agree
// when the width matches the filter.
//
double div_window_zero(int mode, int sidetone) {
  if (mode == modeCWU) { return  (double)sidetone; }

  if (mode == modeCWL) { return -(double)sidetone; }

  return 0.0;
}

//
// The edges of a hand-placed window, in the shifted frame.
//
static void div_manual_window(const struct div_context *ctx, double *lo, double *hi) {
  const double z = div_window_zero(ctx->mode, ctx->sidetone);
  *lo = z + ctx->centre - 0.5 * ctx->width;
  *hi = z + ctx->centre + 0.5 * ctx->width;
}

//
// Which side of the tuned frequency the RADE modem is on, from the
// operator's own passband: the midpoint of filter_low..filter_high in the
// shifted frame. Returns 0 when the passband straddles zero (AM, SAM, FM,
// DSB), where it says nothing.
//
// The passband is used rather than vfo[].mode because it is what the
// operator actually set and it covers the digital modes without a table:
// an LSB-side passband is negative in this frame whatever the mode is
// called.
//
static int div_rade_side_expected(const struct div_context *ctx) {
  const double mid = 0.5 * ((double)ctx->filter_low + (double)ctx->filter_high);

  if (mid >  0.5 * RADE_CORR_FLO) { return  1; }

  if (mid < -0.5 * RADE_CORR_FLO) { return -1; }

  return 0;
}

//
// Snapshot everything the bin mask depends on.
//
static void div_get_context(struct div_context *ctx) {
  const RECEIVER *rx = receiver[0];
  ctx->frequency      = vfo[0].frequency;
  ctx->ctun_frequency = vfo[0].ctun_frequency;
  ctx->offset         = vfo[0].offset;
  ctx->sidetone       = cw_keyer_sidetone_frequency;
  ctx->sample_rate    = rx->sample_rate;
  ctx->mode           = vfo[0].mode;
  ctx->filter_low     = rx->filter_low;
  ctx->filter_high    = rx->filter_high;
  ctx->ref            = div_auto_ref;
  ctx->follow         = div_auto_follow_filter;
  ctx->centre         = div_auto_centre;
  ctx->width          = div_auto_width;
  ctx->weighting      = div_auto_weighting;
  ctx->att0           = adc[0].attenuation;
  ctx->att1           = adc[1].attenuation;
}

//
// b is the context as it stood at the last reset, not the previous
// block - div_process_block() only writes lastctx when it resets - so the
// three frequency comparisons below are against where the estimate was
// actually made. See DIV_RETUNE_HZ.
//
static int div_context_changed(const struct div_context *a, const struct div_context *b) {
  return llabs(a->frequency      - b->frequency)      > DIV_RETUNE_HZ ||
         llabs(a->ctun_frequency - b->ctun_frequency) > DIV_RETUNE_HZ ||
         llabs(a->offset         - b->offset)         > DIV_RETUNE_HZ ||
         a->sidetone       != b->sidetone       ||
         a->sample_rate    != b->sample_rate    ||
         a->mode           != b->mode           ||
         a->filter_low     != b->filter_low     ||
         a->filter_high    != b->filter_high    ||
         a->ref            != b->ref            ||
         a->follow         != b->follow         ||
         a->centre         != b->centre         ||
         a->width          != b->width          ||
         a->weighting      != b->weighting      ||
         a->att0           != b->att0           ||
         a->att1           != b->att1;
}

//
// Work out which bins to accumulate over, as an inclusive range of
// unwrapped indices (they may be negative; the caller wraps them).
// Returns 0 if there is nothing usable to measure.
//
static int div_bin_range(const struct div_context *ctx, int *klo, int *khi) {
  double flo, fhi;

  if (ctx->ref == DIV_REF_CARRIER) {
    //
    // The carrier bin only. The frequency comes from our own tracker,
    // which runs on the spectrum further down, so this works in any mode
    // with a carrier and its smoothing is under the operator's control.
    //
    // div_carrier_hz always holds a usable value - it starts at zero,
    // the tuned frequency, which is where an AM carrier sits to within
    // the tuning error. It deliberately has no "not valid yet" state:
    // an earlier version returned failure here until the tracker had run
    // once, and since the bin range is computed before the transform and
    // the tracker runs after it, that could never happen. The mode sat on
    // "searching" for ever on a strong, perfectly tuned signal.
    //
    flo = div_carrier_hz - DIV_CARRIER_BINS * binhz;
    fhi = div_carrier_hz + DIV_CARRIER_BINS * binhz;
  } else if (ctx->ref == DIV_REF_DIGITAL_IQ) {
    //
    // The *search region*, not the bins finally accumulated. Occupancy
    // narrows it after the transform - see div_digital_solve().
    //
    // Nothing computed from the spectrum may appear here: this runs
    // before the transform, and making the bin range depend on something
    // only the transform can supply is exactly what left the carrier
    // reference sitting on "searching" for ever. The region therefore
    // starts from the filter or from the operator's own numbers, both of
    // which are always available.
    //
    // Following the filter is the default and wants no sideband table:
    // the passband is already on the right side of the tuned frequency in
    // every mode, DIGU/DIGL and CW included, and under CTUN too. That is
    // why there is no +/-1500 Hz constant anywhere in this mode.
    //
    if (ctx->follow) {
      flo = ctx->filter_low;
      fhi = ctx->filter_high;
    } else {
      div_manual_window(ctx, &flo, &fhi);
    }
  } else if (ctx->follow) {
    //
    // Method A following the operator's filter.
    //
    flo = ctx->filter_low;
    fhi = ctx->filter_high;
  } else {
    //
    // Method A with a hand-placed window: park it on a known noise, or
    // size it to take in just the mark and space tones of an FSK signal.
    //
    div_manual_window(ctx, &flo, &fhi);
  }

  if (fhi <= flo) { return 0; }

  //
  // Shifted frame -> bin frequency. This inverts as well as displaces, so
  // the two edges swap. See the note at the top of this file.
  //
  {
    const double a = div_shift_to_bin(ctx, flo);
    const double b = div_shift_to_bin(ctx, fhi);
    flo = (a < b) ? a : b;
    fhi = (a < b) ? b : a;
  }

  //
  // Hold the window inside the first Nyquist zone.
  //
  // The accumulation loops index bins as k % nfft, so a bin outside
  // [-nfft/2, nfft/2) is not an error - it silently becomes a *different*
  // frequency. Before this guard a window edge at +30 kHz on a 48 kHz
  // stream was measured at -18 kHz instead, with nothing to say so, and
  // the spin ranges allowed exactly that.
  //
  // Clamping rather than rejecting keeps a partly-reachable window
  // usable; div_auto_clamped tells the UI it happened.
  //
  const double nyq = 0.5 * (double)ctx->sample_rate - binhz;
  div_auto_clamped = 0;

  if (flo < -nyq) {
    flo = -nyq;
    div_auto_clamped = 1;
  }

  if (fhi > nyq) {
    fhi = nyq;
    div_auto_clamped = 1;
  }

  if (fhi <= flo) {
    //
    // Entirely outside the usable spectrum.
    //
    return 0;
  }

  *klo = (int)floor(flo / binhz);
  *khi = (int)ceil (fhi / binhz);

  //
  // A window wider than the DDC passband is meaningless, and one that has
  // collapsed to nothing gives us no statistics at all.
  //
  if (*khi - *klo + 1 > nfft) { return 0; }

  if (*khi < *klo) { return 0; }

  return 1;
}

//
// MVDR for a two-element array.
//
// For R = [[r00, r01], [conj(r01), r11]] and h = [h0, h1], the weight
// vector g = R^-1 h is
//
//   g0 = (r11*h0 - r01*h1)       / det
//   g1 = (r00*h1 - conj(r01)*h0) / det
//
// The combiner forms y = z0 + w*z1, which is g^H z with arm 0 normalised
// to unity, so the weight it wants is conj(g1/g0) - and det cancels.
//
// With R diagonal and equal - two arms carrying the same, uncorrelated
// noise - this reduces to conj(h1/h0), which is exactly the maximum ratio
// combining answer the wideband "Sum" mode computes as +Sxy/Sxx. So the
// mode degenerates to the older one whenever there is no correlated
// interference to null, which is both the right behaviour and the easiest
// property to test.
//
// Shared with the RADE V1 correlator, which arrives at the same two
// matrices from pilot correlations rather than from spectral occupancy.
//
void div_mvdr2(double r00, double r11, double r01re, double r01im,
               double h0re, double h0im, double h1re, double h1im,
               double *wr, double *wi) {
  //
  // Diagonal loading. Without it a nearly singular covariance - two arms
  // seeing almost identical noise - produces an enormous weight out of
  // what is mostly estimation error.
  //
  const double load = 0.01 * (r00 + r11) + 1e-20;
  r00 += load;
  r11 += load;
  //
  // num = r00*h1 - conj(r01)*h0,  den = r11*h0 - r01*h1
  //
  const double numre = r00 * h1re - (r01re * h0re + r01im * h0im);
  const double numim = r00 * h1im - (r01re * h0im - r01im * h0re);
  const double denre = r11 * h0re - (r01re * h1re - r01im * h1im);
  const double denim = r11 * h0im - (r01re * h1im + r01im * h1re);
  const double d2 = denre * denre + denim * denim;
  //
  // Reject a degenerate solve, and only that.
  //
  // This used to read "d2 > 1e-30", which is an absolute magnitude test
  // on a quantity that has no fixed magnitude. den is a difference of two
  // products of energies, so on the RADE path it goes as the eighth power
  // of the sample level: measured across the recorded captures it lands
  // anywhere between 1e-28 and 1e-34 with nothing wrong with any of them.
  // The test fired on between half and all of the frames of every capture
  // but the loudest, returned a weight of exactly zero - which mutes the
  // second antenna and shows in the menu as the -27 dB floor with phase 0,
  // indistinguishable from a real answer - and cost up to 2.0 dB against
  // simply using the better antenna. See Finding 11 in
  // docs/diversity-measurements.md.
  //
  // What makes the answer meaningless is not that den is small but that
  // it is small *compared with the two terms it is the difference of*,
  // which is the catastrophic-cancellation condition and is scale-free.
  // DIV_MVDR_EPS is far above the point where double precision runs out,
  // so this now fires only on a covariance that really is singular
  // against the channel - and the diagonal loading above has already made
  // that very difficult to arrange.
  //
  const double h0m  = sqrt(h0re * h0re + h0im * h0im);
  const double h1m  = sqrt(h1re * h1re + h1im * h1im);
  const double r01m = sqrt(r01re * r01re + r01im * r01im);
  const double scale = r11 * h0m + r01m * h1m;

  if (!(scale > 0.0) || !(d2 > DIV_MVDR_EPS * DIV_MVDR_EPS * scale * scale)) {
    *wr = 0.0;
    *wi = 0.0;
    return;
  }

  //
  // num/den, then conjugated for the g^H combining sense.
  //
  const double qre = (numre * denre + numim * denim) / d2;
  const double qim = (numim * denre - numre * denim) / d2;
  *wr =  qre;
  *wi = -qim;
}

//
// ------------------------------------------------------------------
// Which antenna is better
// ------------------------------------------------------------------
//
// Every reference can say something about the two arms separately, and
// what it needs to say it is the same in each case: the signal power on
// each arm, and the noise power on each arm. The advantage of arm 1 is
// then (S1/N1)/(S0/N0), and where the reference measures the channel
// ratio rather than the two signal powers - which all of them do - that
// is |h1/h0|^2 * (N0/N1).
//
// The RADE V1 and FSK/Digital references already have both halves: their
// MVDR covariance is a measurement of N0 and N1 taken off the signal. The
// wideband Window and Carrier references have no such thing, so they get
// a noise floor tracked over time instead - see div_arm_floor_update().
//
// This is worth publishing whatever objective is running. Nothing an
// operator can otherwise see separates an antenna that reads 12 dB down
// because it is deaf from one that reads 12 dB down because it is quiet,
// and the two want opposite weights - which is exactly the case the 60 m
// captures turned up. See Finding 13 in docs/diversity-measurements.md.
//
//
// Sort order for the noise-floor percentile. Same shape as div_occ_cmp(),
// which sorts the FSK/Digital region for its median.
//
static int div_nf_cmp(const void *a, const void *b) {
  const double x = *(const double *)a;
  const double y = *(const double *)b;
  return (x > y) - (x < y);
}

//
// The two branch noise floors, in power per bin, from this block's
// spectrum outside the operator's filter. See DIV_NF_PCT.
//
// What is excluded is the *filter*, not the analysis window. The Carrier
// reference accumulates five bins and an AM signal's sidebands fill the
// passband either side of them; sampling those as noise would credit
// whichever arm hears the station better with the higher noise floor,
// which is the error this function exists to remove. Where the operator
// has placed a window wider than the filter - parking one on a known
// noise, which Finding 4 recommends - the union is excluded instead.
//
// Returns 1 once div_nf0/div_nf1 hold something.
//
static int div_noise_floor_update(const struct div_context *ctx, int klo, int khi) {
  if (fftout0 == NULL || nf_scratch0 == NULL || nfft <= 0 || binhz <= 0.0) { return 0; }

  //
  // The filter, through the same shift-and-invert div_bin_range() uses,
  // widened to take in the analysis window and then by the skirt.
  //
  int elo, ehi;
  {
    const double a = div_shift_to_bin(ctx, (double)ctx->filter_low);
    const double b = div_shift_to_bin(ctx, (double)ctx->filter_high);
    const double flo = (a < b) ? a : b;
    const double fhi = (a < b) ? b : a;
    elo = (int)floor(flo / binhz);
    ehi = (int)ceil (fhi / binhz);

    if (klo < elo) { elo = klo; }

    if (khi > ehi) { ehi = khi; }

    const int skirt = (int)ceil(DIV_NF_SKIRT_HZ / binhz);
    elo -= skirt;
    ehi += skirt;
  }
  const int span = (int)(0.5 * DIV_NF_SPAN * (double)nfft);

  if (span < 1) { return 0; }

  //
  // How many bins are left to choose from, in closed form, so the stride
  // can be set before anything is touched.
  //
  const int xlo = (elo > -span) ? elo :  -span;
  const int xhi = (ehi <  span) ? ehi :   span;
  const int excl = (xhi >= xlo) ? (xhi - xlo + 1) : 0;
  const int cand = (2 * span + 1) - excl;

  if (cand < DIV_NF_MIN_BINS) { return 0; }

  const int stride = (cand > DIV_NF_SAMPLES) ? (cand / DIV_NF_SAMPLES + 1) : 1;
  int ns = 0;

  for (int k = -span; k <= span && ns < DIV_NF_SAMPLES; k += stride) {
    if (k >= elo && k <= ehi) {
      //
      // Step over the excluded band in one go rather than striding
      // through it, or a wide filter would eat most of the sample count.
      //
      k = ehi;
      continue;
    }

    int idx = k % nfft;

    if (idx < 0) { idx += nfft; }

    nf_scratch0[ns] = (double)fftout0[idx][0] * fftout0[idx][0]
                      + (double)fftout0[idx][1] * fftout0[idx][1];
    nf_scratch1[ns] = (double)fftout1[idx][0] * fftout1[idx][0]
                      + (double)fftout1[idx][1] * fftout1[idx][1];
    ns++;
  }

  if (ns < DIV_NF_MIN_BINS) { return 0; }

  qsort(nf_scratch0, (size_t)ns, sizeof(double), div_nf_cmp);
  qsort(nf_scratch1, (size_t)ns, sizeof(double), div_nf_cmp);
  int ilo = (ns * (DIV_NF_PCT - DIV_NF_BAND)) / 100;
  int ihi = (ns * (DIV_NF_PCT + DIV_NF_BAND)) / 100;

  if (ilo < 0)   { ilo = 0; }

  if (ihi >= ns) { ihi = ns - 1; }

  if (ihi < ilo) { ihi = ilo; }

  double f0 = 0.0, f1 = 0.0;

  for (int i = ilo; i <= ihi; i++) {
    f0 += nf_scratch0[i];
    f1 += nf_scratch1[i];
  }

  f0 /= (double)(ihi - ilo + 1);
  f1 /= (double)(ihi - ilo + 1);

  if (!(f0 > 0.0) || !(f1 > 0.0)) { return 0; }

  if (!div_nf_valid) {
    div_nf0 = f0;
    div_nf1 = f1;
    div_nf_valid = 1;
  } else {
    const double a = 1.0 - exp(-blocktime / DIV_NF_TAU);
    div_nf0 += a * (f0 - div_nf0);
    div_nf1 += a * (f1 - div_nf1);
  }

  return 1;
}

static void div_arm_publish(int valid, double db) {
  div_auto_arm_valid = valid;

  if (valid) { div_auto_arm_db = db; }
}

//
// Minimum statistics: the noise floor of a channel is the quietest it has
// recently been. Track the smoothed in-window power down instantly and
// let it creep back up slowly, so a gap between overs sets it and a long
// transmission does not drag it along.
//
// Crude next to a covariance measured off the carriers, and the only
// thing available to a reference whose window is the whole passband: the
// bins outside it are the rejected sideband, and Finding 1 is the record
// of what happens when that is used as a noise reference.
//
static void div_arm_floor_update(double p0, double p1) {
  if (!(p0 > 0.0) || !(p1 > 0.0)) { return; }

  if (!arm_floor_valid) {
    arm_floor0 = p0;
    arm_floor1 = p1;
    arm_floor_valid = 1;
    return;
  }

  const double rise = pow(10.0, 0.1 * DIV_FLOOR_RISE_DB * blocktime);
  arm_floor0 = (p0 < arm_floor0) ? p0 : arm_floor0 * rise;
  arm_floor1 = (p1 < arm_floor1) ? p1 : arm_floor1 * rise;
}

//
// The advantage of arm 1, in dB. Fails while there is no noise reference,
// and while either arm is sitting on its own floor - there is no signal
// to compare then, and the ratio of two noises is not an answer to the
// question.
//
// nbins is how many bins p0 and p1 were summed over, because the spectral
// floor is per bin and the window powers are not. The temporal floor is
// the fallback and is already in window units, so it needs no scaling -
// which is also why it cannot be mixed with the other: the two are the
// same quantity in different units and only nbins relates them.
//
static int div_arm_from_floor(double p0, double p1, int nbins, double *db) {
  double n0, n1;

  if (div_nf_valid && div_nf0 > 0.0 && div_nf1 > 0.0 && nbins > 0) {
    n0 = (double)nbins * div_nf0;
    n1 = (double)nbins * div_nf1;
  } else if (arm_floor_valid && arm_floor0 > 0.0 && arm_floor1 > 0.0) {
    n0 = arm_floor0;
    n1 = arm_floor1;
  } else {
    return 0;
  }

  const double s0 = p0 - n0;
  const double s1 = p1 - n1;

  if (!(s0 > 0.0) || !(s1 > 0.0)) { return 0; }

  //
  // Both arms have to stand clear of their own floor, or there is no
  // signal to compare. See DIV_ARM_MIN_DB.
  //
  const double need = pow(10.0, 0.1 * DIV_ARM_MIN_DB) - 1.0;

  if (s0 < need * n0 || s1 < need * n1) { return 0; }

  *db = 10.0 * log10((s1 / n1) / (s0 / n0));
  return 1;
}

//
// Is there anything in the window at all?
//
// The complement of the clearance test div_arm_nratio_update() already
// applies, asked of both arms: neither antenna is carrying anything that
// stands out of the quietest it has recently been. Not the same question
// as "is the per-arm estimate available", which is also false when one
// arm is loud and the other is not, and which div_apply_best() rightly
// refuses to fall back to arm 0 on.
//
// The minima come from that function rather than from arm_floor0/1, and
// which minimum is used decides whether this works at all. The floor
// tracker is an all-time minimum with a slow rise, taken from a smoother
// that starts at zero, so its first value sits about 8 dB under the band
// and DIV_FLOOR_RISE_DB needs the better part of a minute to walk it into
// place. Nothing reads quiet for that minute, which on a one-minute
// capture is the whole of it: pointed at the floor tracker this test
// recovered 1.8 dB of the 12 available on `122843` and pointed at the
// bounded window it recovers all of it. The bounded window is seeded from
// its first block and needs one slot - five seconds - to become usable.
//
// Both sides of the comparison are nr_f0/nr_f1, the same DIV_FLOOR_TAU
// smoother the minimum is taken from, and not the operator's averaging
// time. Like against like, and fast: arm_pw0/arm_pw1 have to fall the
// whole way from the signal to the noise before they read empty, which at
// a two-second time constant and 30 dB of signal is twenty seconds -
// measured, on the synthetic gap in test_window, against one second here.
// Responsiveness is safe to take because DIV_QUIET_DWELL is what decides
// whether a dip is a gap.
//
// False until then, so this can never fire on the first blocks after a
// reset, when everything is quiet because nothing has been measured yet.
//
// Note what it cannot see: a signal that never stops leaves the minimum
// sitting on the signal, and reads quiet. That is why the caller must
// already be holding - a signal the two antennas agree on keeps the
// coherence gate open and never reaches this test at all.
//
static int div_window_quiet(void) {
  if (!nr_min_valid || nr_min0 <= 0.0 || nr_min1 <= 0.0) { return 0; }

  const double need = pow(10.0, 0.1 * DIV_QUIET_DB);
  return (nr_f0 < need * nr_min0) && (nr_f1 < need * nr_min1);
}

static void div_apply_weight(double wr, double wi);
static void div_write_weight(double wr, double wi, int track);

//
// ------------------------------------------------------------------
// Holding, and standing down
// ------------------------------------------------------------------
//
// The loop declines a block for one of two reasons, and until now it did
// the same thing for both: stop updating and leave the last weight
// applied.
//
// That is right when the signal is momentarily not measurable - a fade, a
// gap between syllables, a key-up - because the weight in force was
// measured on the real signal and is the best guess at what it will be
// when it comes back.
//
// It is wrong when there is nothing there at all. The weight then in
// force was fitted to something else, possibly on another band, and it
// goes on adding the second antenna's noise to the output for as long as
// the band stays empty. Measured: on `122843` - 17 m, one side of a QSO,
// two thirds of the minute bare band noise, ADC1 15 dB the noisier chain
// - the output is 12.18 dB noisier than the better antenna through the
// gaps, and freezing the weight through them brings that to -0.02 dB with
// the in-speech figure unchanged to a hundredth. On `235906` a weight
// carried in from a previous band cost 3.5 dB over two thirds of a
// minute, and substituting w = 0 on the held blocks would have turned
// -3.54 dB into +3.85. See Findings 36 and 42.
//
// So: hold as before, but if the window is quiet as well, slew the weight
// to zero - arm 0 alone, the antenna the operator would have been
// listening to with the feature switched off. The estimate is not
// discarded and the accumulators go on decaying exactly as they did; only
// what is applied changes, and it changes back the moment anything
// arrives, because div_apply_weight() slews rather than steps.
//
// Both conditions are required. A signal both antennas hear keeps the
// coherence gate open and never reaches here, which is what stops the
// floor tracker's slow rise from standing the combiner down on a signal
// that never stops.
//
//
// Coming out of a stand-down, by whichever door.
//
// The weight the stand-down left is zero, and DIV_SLEW_FRAC would take
// half a second to climb off it - which is the whole of the in-speech
// cost the restore was measured to avoid. div_hold_or_stand_down() does
// this for the case where the band fills while the loop is still
// holding; the other case is an over that arrives strongly enough to
// open the coherence gate on its first block, which goes straight to the
// solve and never reaches that function at all. It was slewing up from
// zero, and the synthetic gap in test_window measured it doing so:
// twelve blocks after the signal came back the weight had reached
// -2.85 dB of the -2.11 it had been standing on and was still climbing.
// It steps back to it at once now, and what is left in that test is the
// loop's own re-convergence on the returning signal.
//
// div_jump rather than a weight: this door has a measured answer for
// this block, so the answer goes in - the door in
// div_hold_or_stand_down() has none, which is why that one puts back
// what it was standing on instead.
//
static void div_leave_standdown(void) {
  if (div_auto_standdown) {
    div_auto_standdown = 0;
    div_jump = 1;
  }
}

static void div_hold_or_stand_down(void) {
  div_auto_holding = 1;

  if (!div_window_quiet()) {
    //
    // Something has arrived. If the combiner was stood down, put back the
    // weight it was standing on rather than slewing up from zero: the
    // estimate was never discarded, and the loop is still holding, so
    // holding is what should resume. div_jump makes it one step, which is
    // what the objective switch uses it for.
    //
    // Measured: without this the first second of every over runs with a
    // weight on its way back up, which costs 0.58 dB in speech on
    // `122843` - the whole of the in-speech cost of this change.
    //
    if (div_auto_standdown) {
      div_leave_standdown();
      //
      // Written without tracking: the loop did not measure this weight
      // this block, it is the one being put back. See div_write_weight().
      //
      div_write_weight(quiet_wr, quiet_wi, 0);
    }

    quiet_run = 0;
    return;
  }

  if (blocktime > 0.0 && quiet_run < (int)(DIV_QUIET_DWELL / blocktime) + 1) {
    quiet_run++;
    return;
  }

  //
  // Remember what was being applied before the first zero goes out, not
  // after: div_cos/div_sin are on their way to zero from the second block
  // of the stand-down onwards.
  //
  if (!div_auto_standdown) {
    quiet_wr = div_cos;
    quiet_wi = div_sin;
    div_auto_standdown = 1;
  }

  //
  // Zero is what gets applied, not what the loop has: the estimate is
  // still there and div_track_gain/div_track_phase have to go on showing
  // it, which under Hold - where the readout is the only sight of the
  // loop's answer - is the whole point of them.
  //
  div_write_weight(0.0, 0.0, 0);
}

//
// The branch noise ratio, by minimum statistics over a bounded window.
//
// Deliberately not arm_floor0/arm_floor1, though those are minima too and
// were tried first. That pair is an all-time minimum with a slow
// exponential rise, and what it holds depends on how the run started: the
// fast smoother it watches begins at zero, so the first blocks after a
// reset are quieter than the band will ever be again, and the rise -
// DIV_FLOOR_RISE_DB, a fifth of a decibel a second - takes a minute to
// climb out of that. On a signal with gaps the gaps pull it back to the
// truth and nobody notices. On a signal with none it simply sits far
// below, the clearance test passes anyway, and the ratio of two signals
// gets used as a ratio of two noises - which on the synthetic continuous
// carrier in test_window inverts the Sum weight outright, +2.09 dB where
// -2.11 dB is right.
//
// A minimum over a bounded window has no such memory. Two slots are kept,
// the one in progress and the one before it, and the estimate is the
// lesser: an effective window of between DIV_NRATIO_WIN and twice it,
// with no ring buffer. A gap anywhere in it sets the pair; a signal that
// never stops leaves the minimum sitting on the signal, the
// DIV_ARM_MIN_DB clearance below then fails, and no ratio is published -
// which is the right answer, because a carrier that never stops carries
// no evidence about the noise underneath it. The weight is then exactly
// what it was before any of this existed.
//
// Both arms are taken from the same slot, so the pair is contemporaneous:
// two independent minima drifting apart on a fading path would give a
// ratio of two different instants rather than of two front ends.
//
static void div_arm_nratio_update(double x0, double x1, double p0, double p1) {
  if (!(x0 > 0.0) || !(x1 > 0.0)) { return; }

  //
  // Its own smoother, seeded from the first block rather than started at
  // zero. arm_fast0/arm_fast1 cannot be used here: they start at zero, so
  // their first blocks are lower than anything that follows and a minimum
  // taken over them is the startup transient rather than the band. The
  // floor tracker downstream of them survives that because a gap pulls it
  // back; a bounded minimum does not, because the transient sits inside
  // its window. Seeding costs one branch and removes the whole class of
  // fault - which cost three wrong answers before it was found.
  //
  if (!nr_f_valid) {
    nr_f0 = x0;
    nr_f1 = x1;
    nr_f_valid = 1;
  } else {
    const double fa = 1.0 - exp(-blocktime / DIV_FLOOR_TAU);
    nr_f0 += fa * (x0 - nr_f0);
    nr_f1 += fa * (x1 - nr_f1);
  }

  const double f0 = nr_f0, f1 = nr_f1;

  if (nr_slot_left <= 0) {
    if (nr_cur0 > 0.0) {
      nr_prev0 = nr_cur0;
      nr_prev1 = nr_cur1;
      nr_have_prev = 1;
    }

    nr_cur0 = nr_cur1 = 0.0;
    nr_slot_left = (int)(DIV_NRATIO_WIN / blocktime) + 1;
  }

  nr_slot_left--;

  if (nr_cur0 <= 0.0 || f0 + f1 < nr_cur0 + nr_cur1) {
    nr_cur0 = f0;
    nr_cur1 = f1;
  }

  if (!nr_have_prev) { return; }

  if (!(nr_cur0 > 0.0) || !(nr_cur1 > 0.0)
      || !(nr_prev0 > 0.0) || !(nr_prev1 > 0.0)) { return; }

  //
  // The quieter of the two, taken as a pair so that both arms come from
  // the same moment.
  //
  const double m0 = (nr_cur0 + nr_cur1 < nr_prev0 + nr_prev1) ? nr_cur0 : nr_prev0;
  const double m1 = (nr_cur0 + nr_cur1 < nr_prev0 + nr_prev1) ? nr_cur1 : nr_prev1;
  nr_min0 = m0;
  nr_min1 = m1;
  nr_min_valid = 1;
  const double need = pow(10.0, 0.1 * DIV_ARM_MIN_DB);

  if (p0 >= need * m0 && p1 >= need * m1) {
    arm_nratio = m0 / m1;
    arm_nratio_valid = 1;
  }
}

//
// Hold the combined output at the level of arm 0 alone. See DIV_NORM_TAU.
//
// Uses the weight actually in force - div_cos, div_sin, after slewing and
// after any hold - rather than the one the solve just produced, because
// what has to be normalised is what the samples will actually be
// multiplied by. Runs whether or not the loop is holding: the weight may
// be frozen but the powers behind the ratio are not.
//
// Null is excluded by construction. That objective exists to make the
// output quieter, and dividing the drop back out would undo the only
// thing it does.
//
static void div_norm_update(double xx, double yy, double xyre, double xyim) {
  if (!(xx > 0.0)) { return; }

  if (!nrm_valid) {
    nrm_xx = xx;
    nrm_yy = yy;
    nrm_xy_re = xyre;
    nrm_xy_im = xyim;
    nrm_valid = 1;
  } else {
    const double a = 1.0 - exp(-blocktime / DIV_NORM_TAU);
    nrm_xx    += a * (xx   - nrm_xx);
    nrm_yy    += a * (yy   - nrm_yy);
    nrm_xy_re += a * (xyre - nrm_xy_re);
    nrm_xy_im += a * (xyim - nrm_xy_im);
  }

  if (!div_auto_normalise
      || (div_auto_mode != DIV_AUTO_SUM && div_auto_mode != DIV_AUTO_BEST)
      || !(nrm_xx > 0.0)) {
    div_norm = 1.0;
    return;
  }

  //
  // P_out / P_arm0 with the weight in force. conj(w) because nrm_xy is
  // accumulated as X0 * conj(X1), the same convention as bin_xy.
  //
  const double w2 = div_cos * div_cos + div_sin * div_sin;
  const double ratio = 1.0 + w2 * (nrm_yy / nrm_xx)
                       + 2.0 * (div_cos * nrm_xy_re + div_sin * nrm_xy_im) / nrm_xx;

  if (!(ratio > 1.0e-12)) { div_norm = 1.0; return; }

  double g = 1.0 / sqrt(ratio);

  if (g > DIV_NORM_MAX) { g = DIV_NORM_MAX; }

  if (g < 1.0 / DIV_NORM_MAX) { g = 1.0 / DIV_NORM_MAX; }

  div_norm = g;
}

//
// The factor the wideband Sum weight is missing.
//
// The Window and Carrier references form Sum as acc_xy/acc_xx, which is
// conj(h1/h0) and nothing else: maximum ratio combining under the
// assumption that the two branches carry equal noise. Maximum ratio
// combining actually wants conj(h1/h0) * (N0/N1), and on a pair of
// antennas whose front ends are far apart that missing factor is the
// whole answer. Measured on `002534` - ADC1 12.3 dB hotter and 5.1 dB
// worse - the loop applied |w| = 1.17 where 0.072 was right, made the
// audio 14.8 dB louder with a noise floor 18.3 dB higher, and landed
// 3.6 dB *below* simply listening to ADC0 where +1.4 dB was available.
// The two forms differ by exactly the noise ratio and the measurement
// says so: 16.2 against a measured 16.8. See Findings 20 and 22 in
// docs/diversity-measurements.md.
//
// Null is deliberately not scaled. Its weight minimises output power,
// which is the right answer whatever the branch noises are; only the
// SNR-maximising objective needs to know them. Best is not scaled either
// because it uses the co-phasing direction and throws the magnitude away,
// and a positive real factor does not move a direction.
//
// It comes from div_noise_floor_update() - this block's own spectrum,
// outside the filter - which is available on essentially every block and
// cannot be fooled by a fading carrier. See DIV_NF_PCT for what that
// replaced and what it cost.
//
// The temporal minimum is the fallback, for the one case the spectral
// floor cannot serve: a hand-placed window so wide that fewer than
// DIV_NF_MIN_BINS are left outside it. It is latched, because it is a
// property of the two receive chains rather than of the path and because
// switching formula every time its clearance test toggled - which on a
// continuous carrier is constantly, 4 to 32 % of blocks by Finding 16 -
// moved the weight for no reason. The spectral floor needs no latch: it
// is smoothed at DIV_NF_TAU and does not toggle.
//
// Until either has been measured once the behaviour is exactly what it
// was before this term existed.
//
static double div_wideband_sum_scale(void) {
  if (div_nf_valid && div_nf1 > 0.0) { return div_nf0 / div_nf1; }

  return arm_nratio_valid ? arm_nratio : 1.0;
}

//
// Write a new weight, rate limited. Called from the analysis thread.
//
static void div_apply_weight(double wr, double wi);

//
// DIV_AUTO_BEST: give the output to whichever antenna is measuring
// better.
//
// Not a switch, because the combiner cannot express one. It forms
// z0 + w*z1 with arm 0 pinned at unity gain (see receiver.c), so "use
// arm 1 only" exists only as the limit w -> infinity, and the nearest
// reachable point is w at the clamp with the co-phasing angle - arm 1
// dominant with arm 0 co-phased in underneath it, 20 dB down. That is not
// a compromise forced on us: measured against a decoder it beat the full
// MVDR solve by 0.6 dB on the one capture where the two antennas
// disagreed about which was better, because the residue of arm 0 is still
// doing useful combining. Selecting arm 0 needs no such trick - w = 0 is
// exact.
//
// cophase_re/im only has to point the right way; its magnitude is thrown
// away. Every reference already computes it, as the Sum weight.
//
static void div_apply_best(double cophase_re, double cophase_im) {
  if (!div_auto_arm_valid) {
    //
    // Nothing to choose on. Hold rather than guess - and in particular do
    // not fall back to arm 0, which would silently turn the mode into
    // "diversity off" whenever the estimate was unavailable.
    //
    div_auto_holding = 1;
    return;
  }

  if (div_auto_arm_pick == 0) {
    if (div_auto_arm_db >  DIV_BEST_HYST_DB) { div_auto_arm_pick = 1; }
  } else {
    if (div_auto_arm_db < -DIV_BEST_HYST_DB) { div_auto_arm_pick = 0; }
  }

  if (div_auto_arm_pick == 0) {
    div_auto_holding = 0;
    div_apply_weight(0.0, 0.0);
    return;
  }

  const double m = sqrt(cophase_re * cophase_re + cophase_im * cophase_im);

  if (!(m > 0.0)) {
    div_auto_holding = 1;
    return;
  }

  const double k = DIV_MAX_WEIGHT / m;
  div_auto_holding = 0;
  div_apply_weight(cophase_re * k, cophase_im * k);
}

//
// The ordinary way in: a weight the loop has just measured, so the
// tracked readout follows it.
//
static void div_apply_weight(double wr, double wi) {
  div_write_weight(wr, wi, 1);
}

//
// Write a new weight, rate limited. Called from the analysis thread.
//
// track == 0 writes the weight and leaves the readout alone. Only
// div_hold_or_stand_down() asks for that, and it is the one caller
// applying a weight the loop did not produce: zero while the band is
// empty, and the weight it was standing on when something comes back.
// div_track_gain/div_track_phase say where the *loop* is, which under
// Hold is the only sight of it there is - and the weight a stand-down
// would otherwise push into the readout is the operator's own manual
// one, so the readout would end up agreeing with the sliders it exists
// to be compared against.
//
static void div_write_weight(double wr, double wi, int track) {
  double mag = sqrt(wr * wr + wi * wi);

  if (!isfinite(wr) || !isfinite(wi)) { return; }

  if (mag > DIV_MAX_WEIGHT) {
    wr *= DIV_MAX_WEIGHT / mag;
    wi *= DIV_MAX_WEIGHT / mag;
  }

  if (track) {
    //
    // Where the loop has got to, in the units the operator reads. Kept
    // separately from div_gain/div_phase, which describe what is actually
    // being applied to the samples: under Hold the two diverge, and being
    // able to see the tracked answer while the manual controls hold a
    // different one is the whole point of the control.
    //
    div_track_gain = (mag > 1.0e-9) ? 20.0 * log10(mag) : -27.0;

    if (div_track_gain >  27.0) { div_track_gain =  27.0; }

    if (div_track_gain < -27.0) { div_track_gain = -27.0; }

    div_track_phase = atan2(wi, wr) * (180.0 / M_PI);
  }

  if (div_auto_hold) {
    //
    // Hold: keep measuring, stop applying. The operator has the gain and
    // phase controls meanwhile, and releasing sets div_jump so the next
    // block puts the tracked answer in place in one step rather than
    // slewing to it from wherever they left it.
    //
    return;
  }

  //
  // The sample path reads div_cos and div_sin one after the other without
  // a lock, so a read can catch the old value of one and the new value of
  // the other. That costs a single sample computed with a mismatched pair
  // - inaudible - and the alternative, locking per sample at up to 384 kHz,
  // is not worth it.
  //
  if (div_jump) {
    //
    // The operator asked for a different objective; go straight there so
    // the two can be compared without waiting out the slew.
    //
    div_jump = 0;
    div_cos = wr;
    div_sin = wi;
  } else {
    div_cos += DIV_SLEW_FRAC * (wr - div_cos);
    div_sin += DIV_SLEW_FRAC * (wi - div_sin);
  }
  //
  // Back-compute the values the menu, the props file and remote clients
  // work in, so everything stays consistent with what is actually being
  // applied to the samples.
  //
  mag = sqrt(div_cos * div_cos + div_sin * div_sin);

  if (mag > 1.0e-9) {
    div_gain = 20.0 * log10(mag);
  } else {
    div_gain = -27.0;
  }

  if (div_gain >  27.0) { div_gain =  27.0; }

  if (div_gain < -27.0) { div_gain = -27.0; }

  div_phase = atan2(div_sin, div_cos) * (180.0 / M_PI);
}

static int div_occ_cmp(const void *a, const void *b) {
  const double x = *(const double *)a;
  const double y = *(const double *)b;
  return (x > y) - (x < y);
}

//
// FSK/Digital: split the search region into signal and noise by spectral
// occupancy, then solve.
//
// The wideband references treat every bin in the window the same way, or
// weight it by its own coherence. Neither of them ever forms a picture of
// the *noise* on its own, so "Sum" has to assume the two branches carry
// equal, uncorrelated noise - which is what makes w = +Sxy/Sxx maximum
// ratio combining. On a real station that assumption is usually false:
// ADC1 is often a small loop or an active whip on a bare rear-panel
// input, and much of what both antennas hear is common-mode noise picked
// up on the feedlines, which is correlated between them.
//
// A digital signal is narrow and sits in a passband that is mostly empty,
// so here the noise can simply be looked at directly: the bins that carry
// no signal are the noise, and the covariance measured over them is what
// MVDR needs. w = R^-1 h then whitens against both an unequal branch
// noise level and a correlated one, and degenerates exactly to +Sxy/Sxx
// when the noise really is equal and uncorrelated.
//
// What this does *not* do is separate a wanted signal from co-channel QRM
// inside the same region: both are occupied and both are correlated
// between the arms, so occupancy cannot tell them apart. That is what the
// RADE V1 pilot is for. Here the operator separates them by placing the
// region, and nulls with the Null objective, exactly as in Window mode.
//
static void div_digital_solve(const struct div_context *ctx, int klo, int khi) {
  const int n = khi - klo + 1;

  //
  // The noise floor, as the median of the bin powers over the region.
  //
  // A median rather than a mean because a signal filling a good part of
  // the region would drag a mean up with it and hide itself. Sorting is
  // the only per-block cost this mode adds over the wideband ones, so the
  // sample count is capped and a wider region is strided down to it
  // rather than sorted in full.
  //
  const int stride = (n > DIV_OCC_MAX_SAMPLES) ? (n / DIV_OCC_MAX_SAMPLES + 1) : 1;
  int ns = 0;

  for (int k = klo; k <= khi && ns < DIV_OCC_MAX_SAMPLES; k += stride) {
    int idx = k % nfft;

    if (idx < 0) { idx += nfft; }

    occ_scratch[ns++] = bin_xx[idx] + bin_yy[idx];
  }

  if (ns < DIV_OCC_MIN_BINS) {
    div_auto_occ_valid = 0;
    div_auto_coherence = 0.0;
    div_auto_holding = 1;
    return;
  }

  qsort(occ_scratch, ns, sizeof(double), div_occ_cmp);
  const double floorp = occ_scratch[ns / 2];
  const double thresh = floorp * pow(10.0, DIV_OCC_DB / 10.0);
  //
  // Signal: above the floor *and* coherent between the arms.
  // Noise:  below the floor, whether or not it is coherent - correlated
  //         noise is precisely what R exists to describe, so it must not
  //         be excluded for being correlated.
  //
  // A bin that is loud but incoherent - a burst on one antenna only -
  // belongs to neither. Putting it in R would describe noise the arms do
  // not actually share; calling it signal would aim the array at it.
  //
  double sig_xy_re = 0.0, sig_xy_im = 0.0, sig_xx = 0.0, sig_yy = 0.0;
  double r01re = 0.0, r01im = 0.0, r00 = 0.0, r11 = 0.0;
  int nsig = 0, nnoise = 0;
  int kmin = 0, kmax = 0;
  //
  // This block's power in the signal bins against the smoothed power
  // that selected them. See DIV_STALE_DB.
  //
  double cur_sig = 0.0, acc_sig = 0.0;
  memset(occ_mask, 0, (size_t)nfft);

  //
  // First pass: which bins carry signal, and the channel over them.
  //
  for (int k = klo; k <= khi; k++) {
    int idx = k % nfft;

    if (idx < 0) { idx += nfft; }

    const double xx = bin_xx[idx], yy = bin_yy[idx];
    const double xyre = bin_xy_re[idx], xyim = bin_xy_im[idx];

    if (xx + yy > thresh) {
      const double den = xx * yy;

      if (den <= 0.0) { continue; }

      double g2 = (xyre * xyre + xyim * xyim) / den;

      if (g2 > 1.0) { g2 = 1.0; }

      if (g2 < DIV_OCC_COH) { continue; }

      if (nsig == 0) { kmin = kmax = k; }

      if (k < kmin) { kmin = k; }

      if (k > kmax) { kmax = k; }

      occ_mask[idx] = 1;

      //
      // Weighted by the bin's own coherence, for the reason the wideband
      // window offers the same choice: summing flat makes h a
      // power-weighted average of h(f), and the marginal bins that only
      // just cleared the occupancy threshold then add their noise to the
      // denominator while adding little signal to the numerator.
      //
      // Occupancy has already thrown out the bins that are pure noise,
      // so this is a smaller correction here than it is over a whole
      // passband - on a strong signal every occupied bin has g2 near 1
      // and it does nothing at all. It earns its place on a weak one,
      // where the threshold sits just above the floor and most of the
      // occupied bins are marginal.
      //
      // Not an operator control: the threshold has already decided which
      // bins count as signal, and this only stops the weakest of those
      // dominating by weight of numbers.
      //
      sig_xy_re += g2 * xyre;
      sig_xy_im += g2 * xyim;
      sig_xx    += g2 * xx;
      sig_yy    += g2 * yy;
      cur_sig   += (double)fftout0[idx][0] * fftout0[idx][0]
                   + (double)fftout0[idx][1] * fftout0[idx][1]
                   + (double)fftout1[idx][0] * fftout1[idx][0]
                   + (double)fftout1[idx][1] * fftout1[idx][1];
      acc_sig   += xx + yy;
      nsig++;
    }
  }

  //
  // Second pass: the noise covariance, from the bins that are neither
  // occupied nor next to an occupied one.
  //
  // Correlation is deliberately not a disqualification here. Common-mode
  // noise picked up on both feedlines is correlated, and describing it is
  // the whole reason R is measured separately - excluding coherent bins
  // would throw away the one thing this mode can do that Sum cannot.
  // Distance from the signal is what keeps the signal out of R instead.
  //
  for (int k = klo; k <= khi; k++) {
    int idx = k % nfft;

    if (idx < 0) { idx += nfft; }

    int near = 0;

    for (int d = -DIV_OCC_GUARD; d <= DIV_OCC_GUARD && !near; d++) {
      int j = (k + d) % nfft;

      if (j < 0) { j += nfft; }

      if (occ_mask[j]) { near = 1; }
    }

    if (near) { continue; }

    r01re += bin_xy_re[idx];
    r01im += bin_xy_im[idx];
    r00   += bin_xx[idx];
    r11   += bin_yy[idx];
    nnoise++;
  }

  if (nsig < DIV_OCC_MIN_BINS) {
    //
    // Nothing stands out. Two very different situations look like this
    // and the difference matters, because one of them wants a weight and
    // the other must not get one.
    //
    // The region is *empty*: it is all noise, the median is the noise and
    // nothing clears it. Hold. A weight invented from noise would be
    // applied across the whole passband.
    //
    // The region is *full*: the signal covers all of it, so the median is
    // the signal and nothing clears that either. This is not a corner
    // case - it is what a filter set snugly around the signal looks like,
    // with the follow tick on, which is what a careful operator does.
    // Holding there would be a trap: the better the filter, the more
    // certainly the mode would do nothing.
    //
    // Coherence tells them apart. Accumulate the region as a whole and
    // let the ordinary gate below decide: a full region is coherent and
    // gets a weight, an empty one is not and holds. With every bin called
    // signal there are no noise bins left, so the solve falls through to
    // plain maximum ratio combining further down - which is the right
    // answer when nothing is known about the noise, and is exactly what
    // the wideband Window reference would have produced.
    //
    sig_xy_re = sig_xy_im = sig_xx = sig_yy = 0.0;
    r01re = r01im = r00 = r11 = 0.0;
    cur_sig = acc_sig = 0.0;
    nsig = nnoise = 0;

    for (int k = klo; k <= khi; k++) {
      int idx = k % nfft;

      if (idx < 0) { idx += nfft; }

      const double xx = bin_xx[idx], yy = bin_yy[idx];
      const double den = xx * yy;

      if (den <= 0.0) { continue; }

      double g2 = (bin_xy_re[idx] * bin_xy_re[idx]
                   + bin_xy_im[idx] * bin_xy_im[idx]) / den;

      if (g2 > 1.0) { g2 = 1.0; }

      sig_xy_re += g2 * bin_xy_re[idx];
      sig_xy_im += g2 * bin_xy_im[idx];
      sig_xx    += g2 * xx;
      sig_yy    += g2 * yy;
      cur_sig   += (double)fftout0[idx][0] * fftout0[idx][0]
                   + (double)fftout0[idx][1] * fftout0[idx][1]
                   + (double)fftout1[idx][0] * fftout1[idx][0]
                   + (double)fftout1[idx][1] * fftout1[idx][1];
      acc_sig   += xx + yy;
      nsig++;
    }

    kmin = klo;
    kmax = khi;
  }

  if (nsig < DIV_OCC_MIN_BINS || sig_xx <= 0.0 || sig_yy <= 0.0) {
    div_auto_occ_valid = 0;
    div_auto_coherence = 0.0;
    div_auto_holding = 1;
    return;
  }

  //
  // Have the bins we chose actually still got a signal in them?
  //
  // Deliberately before the span is published and before any solve, so
  // that a transmission ending clears the overlay and the status line
  // rather than leaving both asserting a signal that has gone.
  //
  // Holding rather than flushing. The accumulators keep decaying at the
  // operator's averaging time either way, but the weight in force is the
  // last one measured on a real signal, which is what is wanted across a
  // gap. Flushing would put the loop one block from the start, where the
  // single-block cross spectrum is perfectly coherent by construction and
  // any bin at all looks like a signal.
  //
  if (acc_sig > 0.0 && cur_sig * pow(10.0, DIV_STALE_DB / 10.0) < acc_sig) {
    div_auto_occ_valid = 0;
    div_auto_holding = 1;
    return;
  }

  //
  // Publish the occupied span for the overlay and the status line, back
  // in the shifted frame the operator's controls use. Half a bin either
  // side so a single occupied bin still has a width to draw. The mapping
  // inverts, so the edges swap - see div_shift_to_bin().
  //
  {
    const double fo = div_frame_off(ctx);
    const double sa = -((double)kmin - 0.5) * binhz - fo;
    const double sb = -((double)kmax + 0.5) * binhz - fo;
    div_auto_occ_lo = (sa < sb) ? sa : sb;
    div_auto_occ_hi = (sa < sb) ? sb : sa;
    div_auto_occ_valid = 1;
  }
  const double xy2 = sig_xy_re * sig_xy_re + sig_xy_im * sig_xy_im;
  div_auto_coherence = xy2 / (sig_xx * sig_yy);

  if (div_auto_coherence > 1.0) { div_auto_coherence = 1.0; }

  if (div_auto_coherence < div_auto_coherence_min) {
    div_auto_holding = 1;
    return;
  }

  div_auto_holding = 0;
  //
  // Per-arm SNR, from the same two sets of bins the solve uses: the
  // channel ratio over the occupied ones, the two noise powers over the
  // rest. Where occupancy found no noise bins there is nothing to divide
  // by and the estimate is simply unavailable.
  //
  {
    double db = 0.0;
    int ok = 0;

    if (nnoise >= DIV_OCC_MIN_BINS && r00 > 0.0 && r11 > 0.0 && sig_xx > 0.0) {
      const double hr = (sig_xy_re * sig_xy_re + sig_xy_im * sig_xy_im)
                        / (sig_xx * sig_xx);

      if (hr > 0.0) {
        db = 10.0 * log10(hr * r00 / r11);
        ok = 1;
      }
    }

    div_arm_publish(ok, db);
  }

  if (div_auto_mode == DIV_AUTO_BEST) {
    div_apply_best(sig_xy_re / sig_xx, sig_xy_im / sig_xx);
    return;
  }

  if (div_auto_mode == DIV_AUTO_NULL) {
    //
    // Null cancels what the region is sitting on, which is the occupied
    // part of it - the same objective as everywhere else, restricted to
    // the bins that carry something. MVDR has no part in it: it maximises
    // the signal-to-interference ratio of the thing it is pointed at, and
    // Null exists to do the opposite.
    //
    div_apply_weight(-sig_xy_re / sig_yy, -sig_xy_im / sig_yy);
    return;
  }

  if (nnoise < DIV_OCC_MIN_BINS || r00 <= 0.0 || r11 <= 0.0) {
    //
    // The signal fills the region, so there are no noise bins to build R
    // from. Diagonal loading would not rescue an empty covariance - it
    // would just return the unweighted answer through a longer route - so
    // take the maximum ratio combining weight directly and say nothing:
    // it is the correct answer when nothing is known about the noise.
    //
    div_apply_weight(sig_xy_re / sig_xx, sig_xy_im / sig_xx);
    return;
  }

  //
  // h with arm 0 as the reference. Writing z_m = a_m s + n_m and
  // accumulating over the signal bins,
  //
  //   Sxx = |a0|^2 S,   Sxy = a0 conj(a1) S
  //
  // so h0 = Sxx = a0 * (conj(a0) S) and h1 = conj(Sxy) = a1 * (conj(a0) S)
  // are the two channels scaled by one common complex factor, which is
  // all MVDR needs since the solve normalises arm 0 to unity.
  //
  {
    double wr, wi;
    div_mvdr2(r00, r11, r01re, r01im,
              sig_xx, 0.0, sig_xy_re, -sig_xy_im,
              &wr, &wi);
    div_apply_weight(wr, wi);
  }
}

#ifdef DIVERSITY_CAPTURE
//
// DEVELOPMENT TOOL - remove with the rest of the capture instrument.
//
// The context the previous captured block was taken with, so that the
// record can say whether this one differs. lastctx is no use for that:
// div_process_block() only writes it when it resets, so a change inside
// DIV_RETUNE_HZ - or an attenuator step on a block that also reset for
// another reason - would never show.
//
// The comparison is exact, deliberately. div_context_changed() is the
// engine's question ("is the estimate still valid?") and it tolerates a
// 20 Hz dial move; this is the reader's question ("did anything move in
// this file?"), and an analyst looking for the block where an attenuator
// or a filter changed wants every one of them.
//
static struct div_context divcap_prevctx;
static int divcap_haveprev = 0;

static int divcap_ctx_differs(const struct div_context *a,
                              const struct div_context *b) {
  return a->frequency      != b->frequency      ||
         a->ctun_frequency != b->ctun_frequency ||
         a->offset         != b->offset         ||
         a->sidetone       != b->sidetone       ||
         a->sample_rate    != b->sample_rate    ||
         a->mode           != b->mode           ||
         a->filter_low     != b->filter_low     ||
         a->filter_high    != b->filter_high    ||
         a->ref            != b->ref            ||
         a->follow         != b->follow         ||
         a->weighting      != b->weighting      ||
         a->att0           != b->att0           ||
         a->att1           != b->att1           ||
         a->centre         != b->centre         ||
         a->width          != b->width;
}
#endif

//
// Process one block. Runs on the analysis thread.
//
static void div_process_block(void) {
  struct div_context ctx;
  int klo, khi;

  if (receivers < 1 || receiver[0] == NULL) {
    div_auto_holding = 1;
    return;
  }

  div_get_context(&ctx);

#ifdef DIVERSITY_CAPTURE
  int divcap_reset = 0;
#endif

  if (div_context_changed(&ctx, &lastctx)) {
    //
    // The radio moved under us: anything we accumulated describes a
    // different measurement.
    //
    div_reset_stats();
    rade_corr_reset();
    lastctx = ctx;
#ifdef DIVERSITY_CAPTURE
    divcap_reset = 1;
#endif
  }

#ifdef DIVERSITY_CAPTURE

  //
  // DEVELOPMENT TOOL - remove with the rest of the capture instrument.
  //
  // The tap. This block is what the correlator is about to be given, and
  // the correlator globals still hold what the previous block left, so a
  // record written here is an (input, state) pair the replay can be
  // checked against. See src/diversity_capture.h.
  //
  if (div_capture_active) {
    struct divcap_block m;
    memset(&m, 0, sizeof(m));
    m.dropped         = (guint32)divcap_dropped;
    //
    // Bit 0: this block's context differs from the previous captured
    // block's. The first block of a file never sets it - there is nothing
    // before it to differ from.
    //
    m.rec_flags       = 0u;

    if (divcap_haveprev && divcap_ctx_differs(&ctx, &divcap_prevctx)) {
      m.rec_flags |= DIVCAP_FLAG_CTX_CHANGED;
    }

    if (divcap_reset) { m.rec_flags |= DIVCAP_FLAG_ENGINE_RESET; }

    divcap_prevctx    = ctx;
    divcap_haveprev   = 1;
    m.frequency       = (gint64)ctx.frequency;
    m.ctun_frequency  = (gint64)ctx.ctun_frequency;
    m.offset          = (gint64)ctx.offset;
    m.sidetone        = ctx.sidetone;
    m.ctx_sample_rate = ctx.sample_rate;
    m.mode            = ctx.mode;
    m.filter_low      = ctx.filter_low;
    m.filter_high     = ctx.filter_high;
    m.ref             = ctx.ref;
    m.follow          = ctx.follow;
    m.weighting       = ctx.weighting;
    m.att0            = ctx.att0;
    m.att1            = ctx.att1;
    m.centre          = ctx.centre;
    m.width           = ctx.width;
    //
    // Recorded whatever the reference is, so a capture taken while
    // watching one mode can still be replayed into another. The RADE
    // branch below derives these two the same way.
    //
    {
      const int expect = div_rade_side_expected(&ctx);
      m.expect_bank = (expect == 0) ? -1 : (expect < 0 ? 0 : 1);
    }
    m.auto_mode        = div_auto_mode;
    m.frame_off        = div_frame_off(&ctx);
    m.tau              = div_auto_tau;
    m.hang             = div_auto_hang;
    m.live_locked      = rade_corr_locked;
    m.live_confirming  = rade_corr_confirming;
    m.live_mirrored    = rade_corr_mirrored;
    m.live_holding     = div_auto_holding;
    m.live_quality     = rade_corr_quality;
    m.live_freq_off    = rade_corr_freq_off;
    m.live_snr         = rade_corr_snr;
    m.live_coherence   = div_auto_coherence;
    m.live_track_gain  = div_track_gain;
    m.live_track_phase = div_track_phase;
    m.live_cos         = div_cos;
    m.live_sin         = div_sin;
    diversity_capture_block(work0, work1, &m);
  }

  divcap_dropped = 0;
#endif

  if (ctx.ref == DIV_REF_RADE_V1) {
    //
    // Pilot-correlating path. This one does not use the FFT at all: it
    // downconverts to the 8 kHz modem rate and correlates against the
    // known RADE V1 pilot, which separates the wanted signal from noise
    // and QRM well enough to estimate the two separately.
    //
    //
    // Which is why the output-level normaliser has to be fed by hand
    // here: the powers it wants come from the bins everywhere else, and
    // this path has none. Summed over the whole tapped buffer instead,
    // which is what the operator hears anyway. Unconditional, like the
    // windowed path's: div_norm_update() is also what puts div_norm back
    // to 1.0 when the control is unticked, so skipping the call while it
    // is off would leave the last correction in force.
    //
    {
      double nxx = 0.0, nyy = 0.0, nxy_re = 0.0, nxy_im = 0.0;

      for (int i = 0; i < nfft; i++) {
        const double i0 = work0[2 * i], q0 = work0[2 * i + 1];
        const double i1 = work1[2 * i], q1 = work1[2 * i + 1];
        nxx    += i0 * i0 + q0 * q0;
        nyy    += i1 * i1 + q1 * q1;
        nxy_re += i0 * i1 + q0 * q1;
        nxy_im += q0 * i1 - i0 * q1;
      }

      div_norm_update(nxx, nyy, nxy_re, nxy_im);
    }

    double wr, wi;
    //
    // The operator's sideband, as the pilot bank to search.
    //
    // Bank 0 is the pilot as transmitted, carriers at +750..+2200 Hz in
    // the tapped buffer. The buffer is inverted with respect to RF, so
    // those positive bin frequencies are *below* the dial: bank 0 is the
    // LSB bank and bank 1 the USB one. See the frequency bookkeeping note
    // at the top - this is the mapping the on-air logs give, and it is
    // the opposite of the one reading the code suggests.
    //
    // -1 means the passband straddles the carrier and does not say.
    //
    const int expect = div_rade_side_expected(&ctx);
    const int bank = (expect == 0) ? -1 : (expect < 0 ? 0 : 1);
    int ok = rade_corr_process(work0, work1, nfft, bank,
                               div_frame_off(&ctx), div_auto_tau, div_auto_hang,
                               &wr, &wi);
    //
    // The overlay follows the passband, locked or not. It used to switch
    // to the bank the correlator reported once it locked, which is how a
    // lock on the wrong side of an LSB passband announced itself: the
    // green box jumped across the carrier at the moment of locking. There
    // is nothing to report any more - the correlator only searches the
    // bank the passband names - and the only case where it still chooses
    // is AM/SAM/FM, where the passband says nothing and the correlator's
    // answer is the only one there is.
    //
    div_rade_side = (expect != 0) ? expect
                    : (rade_corr_locked ? (rade_corr_mirrored ? 1 : -1) : div_rade_side);

    //
    // The correlator measures both arms whenever it is locked, whether or
    // not it produced a weight this block.
    //
    div_arm_publish(rade_corr_arm_valid, rade_corr_arm_db);

    //
    // The coherence gate reaches this mode too.
    //
    // It did not before: div_auto_coherence was set to rade_corr_quality
    // and then never compared with anything, so the Min coherence control
    // was inert in RADE V1 - the one reference where the operator is most
    // likely to be watching a marginal signal and wondering why the loop
    // is acting on it. The quantity is not a coherence but a signal
    // fraction, acc_sig/(acc_sig + acc_r00), which is why it has its own
    // threshold: at the same slider position a gamma^2 gate and a quality
    // gate ask for per-arm SNRs four and a half decibels apart. See
    // div_band_cohmin.
    //
    // Default zero, which is the behaviour this replaces exactly.
    //
    if (ok && rade_corr_quality < div_auto_coherence_min) {
      div_auto_coherence = rade_corr_quality;
      div_auto_holding = 1;
      return;
    }

    if (ok) {
      div_auto_coherence = rade_corr_quality;
      div_auto_holding = 0;

      if (div_auto_mode == DIV_AUTO_BEST) {
        //
        // rade_corr_arm_cos/sin is the unit weight that brings arm 1 onto
        // arm 0 in phase, which is all div_apply_best() wants. The MVDR
        // weight in wr/wi would do at a pinch but its phase is not the
        // co-phasing one - measured 18 degrees off on the capture where
        // arm 1 won - and the whole point of this mode is not to depend
        // on that solve.
        //
        div_apply_best(rade_corr_arm_cos, rade_corr_arm_sin);
        return;
      }

      //
      // Respect the objective, as every other reference does.
      //
      // The correlator always solves for the weight that maximises the
      // pilot's SINR - that is what MVDR against the interference
      // covariance means, and there is no second answer to compute.
      // Turning it through 180 degrees is what Null asks for here:
      // cancel the signal the pilot is pointing at rather than combine
      // for it, which is how an operator checks that the array really is
      // pointed at the RADE station and not at something else.
      //
      // Without this the objective and the Invert button were inert in
      // this mode. diversity_auto_invert() turns div_cos/div_sin over
      // immediately, so the audio changed - and then the next block
      // applied the un-inverted answer again and slewed straight back,
      // which looks like a control that does not work rather than one
      // that is not implemented.
      //
      const double sign = (div_auto_mode == DIV_AUTO_SUM) ? 1.0 : -1.0;
      div_apply_weight(sign * wr, sign * wi);
    } else {
      div_auto_coherence = rade_corr_quality;
      div_auto_holding = 1;
    }

    return;
  }

  if (!div_bin_range(&ctx, &klo, &khi)) {
    //
    // Nothing worth transforming: an empty or nonsensical window.
    //
    // Note this runs *before* the transform, so nothing computed from the
    // spectrum may be required to make it succeed - see the note in
    // div_bin_range() about the carrier tracker.
    //
    div_auto_holding = 1;
    return;
  }

  for (int i = 0; i < nfft; i++) {
    fftin0[i][0] = work0[2 * i    ] * window[i];
    fftin0[i][1] = work0[2 * i + 1] * window[i];
    fftin1[i][0] = work1[2 * i    ] * window[i];
    fftin1[i][1] = work1[2 * i + 1] * window[i];
  }

  fftwf_execute(plan0);
  fftwf_execute(plan1);

  if (ctx.ref == DIV_REF_CARRIER) {
    //
    // Find the carrier ourselves rather than asking the SAM PLL.
    //
    // WDSP's SAM PLL is set up for fast acquisition and drift following:
    // omegaN 250 rad/s with unity damping is a 39.8 Hz natural frequency
    // and about 25 Hz of loop noise bandwidth, which on a weak carrier
    // gives several Hz of frequency jitter. That is the right choice for
    // demodulating SAM and the wrong one for measuring a stable carrier,
    // and it cannot be narrowed without spoiling the audio it exists to
    // produce.
    //
    // The spectrum is already in front of us, so the peak bin plus a
    // parabolic interpolation over its neighbours gives a sub-bin
    // estimate, and it can then be smoothed as slowly as the operator
    // likes. It also works in plain AM, where the SAM PLL does not run at
    // all.
    //
    //
    // Search where the operator pointed us, not blindly around the tuned
    // frequency. Parking a 1 kHz window on +5 kHz is what lets a carrier
    // other than the primary be tracked - and nulled - since the primary
    // is then outside the search entirely. The selection has no memory
    // between blocks, so restricting the region is the whole mechanism.
    //
    double wlo, whi;
    div_manual_window(&ctx, &wlo, &whi);
    const double a = div_shift_to_bin(&ctx, wlo);
    const double b = div_shift_to_bin(&ctx, whi);
    double slo = (a < b) ? a : b;
    double shi = (a < b) ? b : a;
    const double snyq = 0.5 * (double)ctx.sample_rate - binhz;

    if (slo < -snyq) { slo = -snyq; }

    if (shi >  snyq) { shi =  snyq; }

    int klo_s = (int)floor(slo / binhz);
    int khi_s = (int)ceil (shi / binhz);
    int peak = klo_s;
    double peakval = -1.0;

    for (int k = klo_s; k <= khi_s; k++) {
      int idx = k % nfft;

      if (idx < 0) { idx += nfft; }

      double p = (double)fftout0[idx][0] * fftout0[idx][0]
                 + (double)fftout0[idx][1] * fftout0[idx][1];

      if (p > peakval) {
        peakval = p;
        peak = k;
      }
    }

    double delta = 0.0;

    if (peakval > 0.0) {
      //
      // Parabolic interpolation on log power over the three bins about
      // the peak. Good to a small fraction of a bin for a windowed tone.
      //
      double m[3];

      for (int j = 0; j < 3; j++) {
        int idx = (peak - 1 + j) % nfft;

        if (idx < 0) { idx += nfft; }

        double p = (double)fftout0[idx][0] * fftout0[idx][0]
                   + (double)fftout0[idx][1] * fftout0[idx][1];
        m[j] = log(p > 1e-30 ? p : 1e-30);
      }

      double den2 = m[0] - 2.0 * m[1] + m[2];

      if (fabs(den2) > 1e-12) {
        delta = 0.5 * (m[0] - m[2]) / den2;
      }

      if (delta > 0.5) { delta = 0.5; }

      if (delta < -0.5) { delta = -0.5; }
    }

    //
    // Bin frequency back to the shifted frame, which is what the menu,
    // the overlay and div_bin_range() all work in: the inverse of
    // div_shift_to_bin(), which is its own inverse up to the sign.
    //
    double hz = -((double)peak + delta) * binhz - div_frame_off(&ctx);

    if (!div_auto_carrier_valid) {
      //
      // First look after a reset: take it, rather than crawling towards
      // it from the tuned frequency over one averaging time.
      //
      div_carrier_hz = hz;
      div_auto_carrier_valid = 1;
    } else {
      div_carrier_hz += (1.0 - exp(-blocktime / div_auto_tau)) * (hz - div_carrier_hz);
    }

    div_auto_carrier = div_carrier_hz;

    //
    // Re-aim the window now the carrier is known.
    //
    // The second call can fail where the first succeeded - the carrier can
    // be near enough the Nyquist limit that its few bins clamp away to
    // nothing - and klo/khi would then still hold the whole search window.
    // Accumulating that as if it were the carrier bin is worse than not
    // measuring at all.
    //
    if (!div_bin_range(&ctx, &klo, &khi)) {
      div_auto_holding = 1;
      return;
    }
  }

  //
  // See below: weighting applies to the wideband window only.
  //
  const int coherence_weighted = (ctx.weighting == DIV_WEIGHT_COHERENCE)
                                 && (ctx.ref == DIV_REF_BAND);
  //
  // Exponential forgetting across blocks, applied per bin.
  //
  double alpha = 1.0 - exp(-blocktime / div_auto_tau);

  if (!acc_valid) {
    alpha = 1.0;
    acc_valid = 1;
  }

  double cur_xx = 0.0, cur_yy = 0.0, cur_xy_re = 0.0, cur_xy_im = 0.0;

  //
  // Per-bin running spectra. Keeping these per bin rather than as four
  // scalars is what allows the bins to be weighted by how well the two
  // antennas agree in each - see below.
  //
  for (int k = klo; k <= khi; k++) {
    int idx = k % nfft;

    if (idx < 0) { idx += nfft; }

    double i0 = fftout0[idx][0], q0 = fftout0[idx][1];
    double i1 = fftout1[idx][0], q1 = fftout1[idx][1];
    //
    // X0 * conj(X1)
    //
    bin_xy_re[idx] += alpha * ((i0 * i1 + q0 * q1) - bin_xy_re[idx]);
    bin_xy_im[idx] += alpha * ((q0 * i1 - i0 * q1) - bin_xy_im[idx]);
    bin_xx[idx]    += alpha * ((i0 * i0 + q0 * q0) - bin_xx[idx]);
    bin_yy[idx]    += alpha * ((i1 * i1 + q1 * q1) - bin_yy[idx]);
    //
    // Unweighted window power per arm, for the noise floor tracker. It
    // has to be unweighted and it has to be per arm: a coherence-weighted
    // sum follows the signal, which is the one thing a noise floor must
    // not do.
    //
    cur_xx += i0 * i0 + q0 * q0;
    cur_yy += i1 * i1 + q1 * q1;
    //
    // ...and the cross term over the same bins, which is the third thing
    // the output-level normaliser needs. See div_norm_update().
    //
    cur_xy_re += i0 * i1 + q0 * q1;
    cur_xy_im += q0 * i1 - i0 * q1;
  }

  //
  // The output-level normaliser, here rather than after the solve below.
  //
  // The three powers it wants are the ones just accumulated, and this is
  // the last point every windowed reference still passes through: it used
  // to sit past the FSK/Digital return further down, which left Level
  // output visible, ticked and doing nothing in that mode.
  //
  // The weight it reads is div_cos/div_sin - what the sample path is
  // multiplying by now, this block's solve not having run yet - which is
  // the weight the level actually has to be corrected for. Its own
  // DIV_NORM_TAU smoothing makes a block either way invisible in any
  // case; the wideband path called it from the same side of the solve.
  //
  div_norm_update(cur_xx, cur_yy, cur_xy_re, cur_xy_im);

  //
  // FSK/Digital takes it from here. The region has been accumulated;
  // which of its bins are signal is decided from the spectrum, which is
  // why this cannot happen in div_bin_range() with the rest.
  //
  if (ctx.ref == DIV_REF_DIGITAL_IQ) {
    div_digital_solve(&ctx, klo, khi);
    return;
  }

  //
  // Combine the bins.
  //
  // Flat reproduces the original behaviour: sum everything and divide,
  // which is a power-weighted average of h(f). It is dominated by the
  // loudest bins whether or not the antennas agree there, and noise-only
  // bins dilute it by adding to the denominator but not the numerator.
  //
  // Coherence weights each bin by its own magnitude-squared coherence, so
  // bins carrying a signal both antennas hear dominate and noise-only
  // bins fall out. That is what makes a wide window work on SSB voice,
  // where the energy moves about constantly and there is no carrier to
  // sit on: the window can span the whole passband and the estimator
  // picks the bins worth using, following the voice as it moves.
  //
  acc_xy_re = acc_xy_im = acc_xx = acc_yy = 0.0;
  double wsum = 0.0;
  //
  // This block's power against the smoothed power, over the same bins and
  // with the same weights, so the staleness test below asks about exactly
  // what the estimate is being made from. See DIV_STALE_DB.
  //
  double cur_p = 0.0, acc_p = 0.0;

  for (int k = klo; k <= khi; k++) {
    int idx = k % nfft;

    if (idx < 0) { idx += nfft; }

    double xx = bin_xx[idx], yy = bin_yy[idx];
    double w = 1.0;

    //
    // Only where there is a window of bins to choose between. The carrier
    // reference accumulates a handful either side of one peak, all of them
    // the same signal, so weighting them against each other does nothing -
    // and the menu hides the control there, which would otherwise leave
    // whichever setting was last chosen in Window mode silently in force.
    //
    if (coherence_weighted) {
      double den = xx * yy;

      if (den <= 0.0) { continue; }

      double g2 = (bin_xy_re[idx] * bin_xy_re[idx]
                   + bin_xy_im[idx] * bin_xy_im[idx]) / den;

      if (g2 > 1.0) { g2 = 1.0; }

      w = g2;

      if (w <= 0.0) { continue; }
    }

    acc_xy_re += w * bin_xy_re[idx];
    acc_xy_im += w * bin_xy_im[idx];
    acc_xx    += w * xx;
    acc_yy    += w * yy;
    cur_p     += w * ((double)fftout0[idx][0] * fftout0[idx][0]
                      + (double)fftout0[idx][1] * fftout0[idx][1]
                      + (double)fftout1[idx][0] * fftout1[idx][0]
                      + (double)fftout1[idx][1] * fftout1[idx][1]);
    acc_p     += w * (xx + yy);
    wsum      += w;
  }

  //
  // Per-arm signal and noise, before any of the gates below: the floor
  // has to go on learning while the loop is holding, because holding is
  // mostly what it does between overs and between overs is when the floor
  // is measurable.
  //
  arm_pw0 += alpha * (cur_xx - arm_pw0);
  arm_pw1 += alpha * (cur_yy - arm_pw1);
  {
    //
    // A second, much shorter smoothing, for the floor only. See
    // DIV_FLOOR_TAU.
    //
    // Both smoothers deliberately start at zero rather than at their
    // first sample. Seeding them looks tidier and is worse: the floor
    // tracker takes its minimum from the fast smoother, so a seeded start
    // sets the floor to the first block's power - signal included - and
    // on a signal with no gaps nothing ever pulls it back down. Starting
    // low and letting DIV_FLOOR_RISE_DB carry it up into place is what
    // makes the floor a floor. Measured: seeding costs 0.3 to 2.0 dB on
    // every capture in Finding 22's table.
    //
    const double fa = 1.0 - exp(-blocktime / DIV_FLOOR_TAU);
    arm_fast0 += fa * (cur_xx - arm_fast0);
    arm_fast1 += fa * (cur_yy - arm_fast1);
  }
  //
  // The noise floor, from the bins outside the filter. Here rather than
  // beside the transform because this is where it is consumed and because
  // the Carrier reference recomputes klo/khi after the tracker has run -
  // the exclusion has to take in the window that was actually
  // accumulated, not the search region it was found in.
  //
  div_noise_floor_update(&ctx, klo, khi);
  div_arm_floor_update(arm_fast0, arm_fast1);
  {
    //
    // Evaluated into a local first: the order in which a call's arguments
    // are evaluated is unspecified, so passing the estimate and the
    // function that produces it in one expression reads the estimate
    // before it has been written.
    //
    double db = 0.0;
    const int ok = div_arm_from_floor(arm_pw0, arm_pw1, khi - klo + 1, &db);
    div_arm_publish(ok, db);
  }
  div_arm_nratio_update(cur_xx, cur_yy, arm_pw0, arm_pw1);

  if (acc_xx <= 0.0 || acc_yy <= 0.0 || wsum <= 0.0) {
    //
    // Nothing accumulated yet. Not a statement about the band, so not a
    // reason to stand anything down.
    //
    div_auto_coherence = 0.0;
    div_auto_holding = 1;
    div_leave_standdown();
    return;
  }

  //
  // Is what these statistics describe still on the air?
  //
  // The window is summed flat, so this compares the whole of it. Under
  // the coherence weighting that used to be selectable the comparison was
  // weighted too, which made it sensitive to a narrow signal - a CW
  // carrier inside a wide filter - stopping while the rest of the window
  // stayed as noisy as ever. Flat gives that up; the stale test is now
  // the blunter of the two presence tests here and the quiet test below
  // is the one that catches an empty band.
  //
  if (acc_p > 0.0 && cur_p * pow(10.0, DIV_STALE_DB / 10.0) < acc_p) {
    div_hold_or_stand_down();
    return;
  }

  double xy2 = acc_xy_re * acc_xy_re + acc_xy_im * acc_xy_im;
  div_auto_coherence = xy2 / (acc_xx * acc_yy);

  if (div_auto_coherence > 1.0) { div_auto_coherence = 1.0; }

  if (div_auto_coherence < div_auto_coherence_min) {
    //
    // Nothing the two antennas agree on. Hold what we have rather than
    // chase noise - and if there is nothing in the window either, stand
    // the combiner down rather than go on applying a weight fitted to
    // something that is no longer there.
    //
    div_hold_or_stand_down();
    return;
  }

  div_auto_holding = 0;
  quiet_run = 0;

  //
  // The gate has opened, so the stand-down ends - but the weight is
  // slewed to like any other, deliberately. Stepping to it would be
  // right if the gate opening always meant a signal, and it does not:
  // the shipped threshold passes about one no-signal block in twenty
  // (Findings 26 and 29), and a step would put a weight fitted to noise
  // straight into the audio where the slew merely leans towards it for
  // one block. The restore that does step is in
  // div_hold_or_stand_down(), where the evidence is the window filling
  // up rather than a coherence reading, and it fires first in practice:
  // the presence test runs off a half-second smoother and the gate has
  // to rebuild the accumulators.
  //
  div_leave_standdown();

  if (div_auto_mode == DIV_AUTO_BEST) {
    div_apply_best(acc_xy_re / acc_xx, acc_xy_im / acc_xx);
    return;
  }

  double den = (div_auto_mode == DIV_AUTO_SUM) ? acc_xx : acc_yy;
  double sign = (div_auto_mode == DIV_AUTO_SUM) ? 1.0 : -1.0;

  //
  // Sum is maximum ratio combining and wants the branch noise ratio in
  // it; Null minimises power and does not. See div_wideband_sum_scale().
  //
  if (div_auto_mode == DIV_AUTO_SUM) { sign *= div_wideband_sum_scale(); }

  div_apply_weight(sign * acc_xy_re / den, sign * acc_xy_im / den);
}

static gpointer div_worker_thread(gpointer data) {
  (void) data;
  t_print("%s: diversity auto-phasing analysis thread running\n", __func__);

  while (worker_run) {
    sem_wait(DIV_SEM);

    //
    // Woken to quit rather than woken by data.
    //
    if (!worker_run) { break; }

    //
    // The semaphore count says there may be work; the ring pointers say
    // whether there is. They can disagree - a post left over from a
    // previous run that the drain in diversity_auto_start() did not
    // catch, or an interrupted wait - so swallow the difference, the way
    // the keyer thread swallows a stale cw_event.
    //
    if (q_tail == q_head) { continue; }

    //
    // The sample path published q_head after a barrier, so having seen
    // it we are guaranteed the block and its q_gap[] entry are complete.
    //
    MEMORY_BARRIER;
    work0 = qbuf0[q_tail];
    work1 = qbuf1[q_tail];
    int dropped = q_gap[q_tail];
    int seq = reset_seq;

    if (seq != reset_seen) {
      reset_seen = seq;
      rade_corr_reset();
    }

#ifdef DIVERSITY_CAPTURE
    //
    // DEVELOPMENT TOOL - the capture hook in div_process_block() marks
    // the discontinuity in the file. Remove with the rest.
    //
    divcap_dropped = dropped;
#endif

    if (dropped > 0) {
      //
      // The block about to be processed is the first after a hole in the
      // sample stream. Everything the correlator knows about where the
      // pilot is refers to a clock that has just skipped, so start again
      // rather than track something that has moved.
      //
      // The count is the number of discontinuities charged to this
      // block, not the number of holes: a transmit gap contributes one
      // for each edge of the over, so a clean over reads 2.
      //
      t_print("%s: sample stream discontinuity, re-acquiring (%d)\n",
              __func__, dropped);
      rade_corr_reset();
    }

    div_process_block();
    //
    // The slot is not handed back until everything has been read out of
    // it, which is what the barrier says.
    //
    MEMORY_BARRIER;
    q_tail = (q_tail + 1) % DIV_QUEUE;
  }

  t_print("%s: diversity auto-phasing analysis thread stopped\n", __func__);
  return NULL;
}

void diversity_auto_sample(double i0, double q0, double i1, double q1) {
  //
  // Called once per sample pair from rx_add_div_iq_samples(), on the
  // protocol receive thread. Nothing but stores happens here.
  //
  //
  // A transmit gap is consumed here rather than performed by rxtx().
  // Two reasons. It keeps fillptr and q_pending_drop private to this
  // thread, which is what lets the queue below be a plain SPSC ring with
  // no mutex at all; and it is the more accurate placement, because the
  // partial block is discarded by the first sample that arrives after
  // the transition rather than by a store from another thread into a
  // block this one may be part way through writing.
  //
  // It has to be per sample and not at the block boundary. Checking only
  // when fillptr reaches nfft would let post-TX samples splice onto the
  // pre-TX partial block, which is the failure the whole mechanism
  // exists to prevent.
  //
  int seq = gap_seq;

  if (seq != gap_seen) {
    gap_seen = seq;
    fillptr = 0;
    q_pending_drop++;
  }

  fill0[2 * fillptr    ] = (float)i0;
  fill0[2 * fillptr + 1] = (float)q0;
  fill1[2 * fillptr    ] = (float)i1;
  fill1[2 * fillptr + 1] = (float)q1;
  fillptr++;

  if (fillptr < nfft) { return; }

  fillptr = 0;
  //
  // One slot is always reserved for filling, so the most that can be
  // waiting is DIV_QUEUE-1 and the head never collides with the tail.
  //
  int nhead = (q_head + 1) % DIV_QUEUE;

  if (nhead != q_tail) {
    //
    // This block is the first one after any discontinuity, so it carries
    // the count. Stored before the barrier: the worker must not be able
    // to see the new head without also seeing the flag that goes with
    // it. The slot is still ours until q_head moves, so the worker
    // cannot have looked at it.
    //
    q_gap[q_head] = q_pending_drop;
    q_pending_drop = 0;
    MEMORY_BARRIER;
    q_head = nhead;
    sem_post(DIV_SEM);
  } else {
    //
    // The worker is DIV_QUEUE-1 blocks behind. Throw this one away and
    // keep filling the same slot; the count is carried into the next
    // block that does get through.
    //
    q_pending_drop++;
  }

  //
  // Re-read q_head rather than reuse nhead. If diversity_auto_start()
  // has reset the ring under a call still in flight from the previous
  // run, this picks up the new head and the stream re-synchronises after
  // at most one block of stale data.
  //
  fill0 = qbuf0[q_head];
  fill1 = qbuf1[q_head];
}

//
// Called from rxtx() on every transmit/receive transition, in both
// directions.
//
// Both protocols stop feeding rx_add_div_iq_samples() for the whole
// over, duplex included: new_protocol only sets RXACTION_DIV when !xmit
// - see update_action_table() - and old_protocol guards the diversity
// mixer on !radio_is_transmitting().
//
// That is not a defect to be worked around. For P2 it is necessary:
// DDC0 and DDC1 are used as a synchronised pair for the PURESIGNAL
// feedback during TX, and with PureSignal on they are retuned to the
// transmit frequency as well, so they are simply not available to carry
// two antennas while the radio is transmitting. P1 reserves the same two
// receive chains the same way. So the analysis stream acquires a hole,
// and all this function has to do is report it.
//
// It has to, because nothing else does. q_pending_drop counts only
// blocks lost to a full queue, and div_context_changed() does not watch
// PTT, so without this the correlator would track straight through: the
// first block after the over would splice pre-TX and post-TX samples
// into one transform, and lock_a would keep advancing by RADE_CORR_NMF
// against a ringtotal that has skipped an arbitrary number of samples.
// That is exactly the failure the gap mechanism exists to prevent, so
// route the transmit gap into it and let the worker re-acquire off the
// first clean block.
//
// Both edges are signalled. The TX->RX edge is the one that does the
// necessary work: the sample path consumes the generation on its next
// sample, so the RX->TX bump is taken by whatever pre-TX samples were
// still in flight from the last DDC packet, and without a second bump
// that pre-TX remnant would still be sitting in the fill buffer when the
// first post-TX sample arrived. The signal is guaranteed to get there
// first - every caller of rxtx(0) clears mox/vox/tune only after it
// returns, so radio_is_transmitting() is still true for the whole call
// and both protocol gates are still shut.
//
// Nothing here touches the applied weight. div_cos/div_sin are written
// only by div_apply_weight(), and every path that has no answer to give
// sets div_auto_holding and returns without calling it - so the gain and
// phase in force when the over started stay in force until a new lock
// produces a better fit. That is deliberate: they may not be ideal for
// the returning signal, but they are a great deal better than nothing.
//
// Only a generation counter is bumped here. Discarding the partial block
// and counting the discontinuity are done by the sample path itself, on
// its next sample, which keeps fillptr and q_pending_drop private to
// that one thread - and that is what lets the queue be a plain
// single-producer/single-consumer ring with no mutex at all.
//
void diversity_auto_gap(void) {
  //
  // div_auto_running is now also set on a remote client, from the status
  // the radio sends, so that its menu reads the same as the radio's. The
  // engine is not running here though, and there is no sample path to
  // have a gap in.
  //
  if (!div_auto_running || radio_is_remote) { return; }

  gap_seq++;
}

void diversity_auto_start(void) {
  if (div_auto_running) { return; }

  if (div_auto_mode == DIV_AUTO_OFF) { return; }

  if (!diversity_enabled || receivers < 1 || receiver[0] == NULL) { return; }

  //
  // On a remote client the samples are combined on the server side and
  // rx_add_div_iq_samples() never runs here, so there would be nothing to
  // analyse.
  //
  if (radio_is_remote) { return; }

  nfft = div_choose_nfft(receiver[0]->sample_rate, div_auto_resolution);
  binhz = (double)receiver[0]->sample_rate / (double)nfft;
  div_auto_binhz = binhz;
  blocktime = (double)nfft / (double)receiver[0]->sample_rate;
  //
  // The buffers are allocated once, at the largest size we will ever use,
  // and then kept for the lifetime of the program. The RX sample path
  // checks div_auto_running without any lock and can already be inside
  // diversity_auto_sample() when diversity_auto_stop() runs, so freeing
  // these on stop would be a use-after-free. Holding on to them costs a
  // few MB and makes start/stop trivially safe: a write that arrives late
  // lands in a buffer that is still valid, and the worst that happens is
  // one block of stale data.
  //
  if (window == NULL) {
    window  = g_new(float, DIV_MAX_NFFT);
    bin_xy_re = g_new0(double, DIV_MAX_NFFT);
    bin_xy_im = g_new0(double, DIV_MAX_NFFT);
    bin_xx    = g_new0(double, DIV_MAX_NFFT);
    bin_yy    = g_new0(double, DIV_MAX_NFFT);
    occ_scratch = g_new0(double, DIV_OCC_MAX_SAMPLES);
    nf_scratch0 = g_new0(double, DIV_NF_SAMPLES);
    nf_scratch1 = g_new0(double, DIV_NF_SAMPLES);
    occ_mask    = g_new0(unsigned char, DIV_MAX_NFFT);
    for (int i = 0; i < DIV_QUEUE; i++) {
      qbuf0[i] = g_new0(float, 2 * DIV_MAX_NFFT);
      qbuf1[i] = g_new0(float, 2 * DIV_MAX_NFFT);
    }
    fftin0  = fftwf_malloc(sizeof(fftwf_complex) * DIV_MAX_NFFT);
    fftin1  = fftwf_malloc(sizeof(fftwf_complex) * DIV_MAX_NFFT);
    fftout0 = fftwf_malloc(sizeof(fftwf_complex) * DIV_MAX_NFFT);
    fftout1 = fftwf_malloc(sizeof(fftwf_complex) * DIV_MAX_NFFT);
  }

  //
  // The semaphore is created once and never destroyed, for the same
  // reason the buffers above are never freed. The RX sample path tests
  // div_auto_running without any lock and can still be inside
  // diversity_auto_sample() - and so inside sem_post() - after
  // diversity_auto_stop() has returned, so destroying it on stop would
  // be a use-after-free. new_protocol_menu_stop() can destroy its
  // semaphores because it has joined every thread that posts them; we
  // have not, and cannot, because the protocol thread keeps running.
  //
  if (!div_sem_created) {
#ifdef __APPLE__
    div_sem = apple_sem(0);
#else
    (void)sem_init(&div_sem, 0, 0);
#endif
    div_sem_created = 1;
  }

  //
  // Discard any count left over from the previous run. That is the
  // normal case, not an edge case: a worker that was inside
  // div_process_block() when the stop came returns to its loop test,
  // sees worker_run clear and exits without ever consuming the post that
  // woke it. The worker's empty-ring test would swallow these anyway;
  // draining here keeps the count meaning what it says.
  //
  while (sem_trywait(DIV_SEM) == 0) { }

  div_make_window();
  //
  // Plan creation is not thread safe, so it happens here, before the
  // analysis thread exists. ESTIMATE rather than MEASURE: planning a
  // 65536-point transform with MEASURE can stall for seconds, which is
  // not acceptable when the operator has just flipped a switch.
  //
  plan0 = fftwf_plan_dft_1d(nfft, fftin0, fftout0, FFTW_FORWARD, FFTW_ESTIMATE);
  plan1 = fftwf_plan_dft_1d(nfft, fftin1, fftout1, FFTW_FORWARD, FFTW_ESTIMATE);
  have_plans = 1;
  //
  // No lock. The worker does not exist yet, and the only words it will
  // ever share with the sample path are q_head and q_tail. A sample-path
  // call still in flight from the previous run can still publish into
  // the ring after this, exactly as it can still store into a buffer
  // that is deliberately never freed - and the cost is the same one
  // block of stale data, because the sample path re-reads q_head rather
  // than caching it and so re-synchronises on the very next block.
  //
  // Taking the mutex here never prevented that either: it made this
  // reset atomic against the producer's enqueue section, not against its
  // per-sample stores.
  //
  // Adopting the current generations rather than zeroing them means a
  // gap or reset raised while the engine was stopped is not acted on as
  // a spurious re-acquire on the first block of the new run.
  //
  fillptr = 0;
  q_head = 0;
  q_tail = 0;
  q_pending_drop = 0;
  memset(q_gap, 0, sizeof(q_gap));
  gap_seen   = gap_seq;
  reset_seen = reset_seq;
  fill0 = qbuf0[0];
  fill1 = qbuf1[0];
  div_reset_stats();
  div_get_context(&lastctx);
  t_print("%s: nfft=%d bin=%0.2f Hz block=%0.1f ms rate=%d\n", __func__,
          nfft, binhz, 1000.0 * blocktime, receiver[0]->sample_rate);
  if (div_auto_ref == DIV_REF_RADE_V1) {
    if (!rade_corr_start(receiver[0]->sample_rate)) {
      //
      // The correlator needs a DDC rate that is a whole multiple of the
      // 8 kHz modem rate. Every rate piHPSDR offers satisfies that, but
      // fall back to FSK/Digital rather than silently doing nothing if
      // that ever stops being true - it places itself on the operator's
      // passband and finds the modem's occupied bins there, which is the
      // job the retired RADE passband reference used to do.
      //
      // Stored and recalled rather than assigned, because the window
      // pair and the coherence threshold are per reference: dropping the
      // reference on its own leaves RADE V1's window in force and its
      // pinned zero as FSK/Digital's threshold, which is that reference's
      // coherence gate switched off. The menu is told as well, or its
      // combo goes on naming a reference the engine is not using.
      //
      t_print("%s: falling back to DIV_REF_DIGITAL_IQ\n", __func__);
      diversity_auto_ref_store(DIV_REF_RADE_V1);
      div_auto_ref = DIV_REF_DIGITAL_IQ;
      diversity_auto_ref_recall(DIV_REF_DIGITAL_IQ);
      g_idle_add(diversity_menu_settings_changed, NULL);
    }
  }

  //
  // Before g_thread_new(), or the new thread can lose the race and exit
  // on its very first loop test. div_auto_running cannot double as the
  // loop condition the way P2running does in new_protocol.c, because it
  // is set after the thread is spawned and is also written from the
  // client status path.
  //
  worker_run = 1;
  worker = g_thread_new("div_auto", div_worker_thread, NULL);
  //
  // Set last: the sample path tests this without any lock.
  //
  div_auto_running = 1;
}

#ifdef DIVERSITY_CAPTURE
//
// DEVELOPMENT TOOL - remove with the rest of the capture instrument.
//
// The menu arms the capture through here because the block geometry it
// has to be sized for - nfft - is private to this file.
//
int diversity_auto_capture_start(void) {
  if (!div_auto_running || receivers < 1 || receiver[0] == NULL) { return 0; }

  //
  // A new file starts with nothing before it, so the first block must not
  // be marked as differing from the last block of the previous one.
  //
  divcap_haveprev = 0;
  return diversity_capture_start(receiver[0]->sample_rate, nfft);
}

#endif

void diversity_auto_stop(void) {
  //
  // div_auto_running is kept current on a client too, from the radio's
  // status, so it can no longer stand alone as "there is an engine here".
  // There never is one on a client - diversity_auto_start() refuses - so
  // there is nothing to tear down and a worker of NULL to not join.
  //
  if (radio_is_remote) { return; }

  if (!div_auto_running) { return; }

#ifdef DIVERSITY_CAPTURE
  //
  // The blocks stop here, so the file has to be closed here: a capture
  // left armed across a sample-rate change would otherwise be waiting for
  // a thread that is never going to feed it again.
  //
  diversity_capture_stop();
#endif

  //
  // Stop the sample path feeding us first, then wake the thread so it can
  // see the quit flag.
  //
  div_auto_running = 0;
  //
  // The scale goes back to unity with the engine. Left where it was, a
  // stopped loop would go on quietening the audio by whatever the last
  // block asked for, which is a control that appears to do nothing until
  // it is turned off.
  //
  div_norm = 1.0;
  worker_run = 0;

  if (div_sem_created) {
    sem_post(DIV_SEM);
  }

  if (worker != NULL) {
    g_thread_join(worker);
    worker = NULL;
  }

  //
  // No sem_destroy()/sem_close() here, and no wait to make one safe: see
  // the note in diversity_auto_start(). Everything below is ordered by
  // the join, which is stronger than the mutex this used to hold.
  //

  if (have_plans) {
    fftwf_destroy_plan(plan0);
    fftwf_destroy_plan(plan1);
    have_plans = 0;
  }

  rade_corr_stop();
  //
  // The sample and FFT buffers are deliberately not freed here; see the
  // note in diversity_auto_start().
  //
  div_auto_coherence = 0.0;
  div_auto_holding = 1;
}

void diversity_auto_restart(void) {
  diversity_auto_stop();

  if (diversity_enabled && div_auto_mode != DIV_AUTO_OFF) {
    diversity_auto_start();
  }
}

//
// Numbering scheme for diversity_auto_ref.
//
// Scheme 1 was BAND, CARRIER, RADE_BAND, RADE_V1, DIGITAL_IQ. The RADE
// passband reference has since been retired - FSK/Digital does the same
// job from the operator's passband and does it better - so scheme 2 is
// BAND, CARRIER, RADE_V1, DIGITAL_IQ, and every value from 2 upwards
// means something different from what it used to.
//
// A file written before this key existed carries no scheme, and the two
// numberings cannot be told apart by inspection: a stored 2 is either the
// old RADE passband or the new RADE V1. Writing the scheme is what makes
// the migration below unambiguous rather than a guess.
//
#define DIV_REF_SCHEME 2

//
// Move the per-reference settings between their own slots and the live
// pair the engine reads. The window centre/width has worked this way
// since the references became modal; the coherence threshold now joins it,
// for the reason in the note beside div_band_cohmin.
//
// Data only - the menu wraps these and updates its widgets afterwards.
// They are not called from diversity_auto_apply_settings(): a settings
// block carries the live values and the per-reference slots together and
// is self-consistent by construction, so swapping again there would
// overwrite what the sender chose.
//
static double div_cohmin_for_ref(int ref) {
  switch (ref) {
  case DIV_REF_CARRIER:    return div_carrier_cohmin;

  case DIV_REF_DIGITAL_IQ: return div_digital_cohmin;

  case DIV_REF_RADE_V1:    return div_rade_cohmin;

  default:                 return div_band_cohmin;
  }
}

//
// The lowest threshold that is worth setting on the selected reference.
//
// The gate compares a magnitude-squared coherence against a number the
// operator chooses, and that estimate has a floor of its own: over N
// independent samples the coherence of two *uncorrelated* noises is not
// zero but averages 1/N, distributed as Beta(1, N-1). Set the gate below
// that and it stops being a gate - the loop passes noise-only blocks and
// fits a weight to whatever the two antennas happen to agree on by
// accident, which is a weight of random phase and near-unity magnitude.
// That is worth about 3 dB of added noise on a pair of matched arms.
//
// So the slider goes down to no coherence and stops there rather than
// carrying on into it. "No coherence" here is the value pure noise
// reaches 1 % of the time,
//
//     floor = 1 - 0.01^(1/(N-1))  ~  4.6/N for large N,
//
// and N is the number of bins accumulated times the effective number of
// blocks in the exponential average, (2-alpha)/alpha. Both terms are the
// point: a wide window at a short averaging time has thousands of
// samples and a floor of a fraction of a percent - which is why switching
// the gate off costs nothing there (Finding 38) - while the Carrier
// reference accumulates five bins and has a floor two orders of magnitude
// higher. One number could never have served both.
//
// Returns 0 for RADE V1, which does not gate on a coherence at all: its
// control is the pilot signal fraction, whose measured behaviour on a
// working decode says the setting to leave it at is zero (Finding 33).
//
// Computed from the settings rather than from the analysis thread's own
// state, so that a remote client - where no engine runs - reaches the
// same answer from the same numbers.
//
// One thing it does not model. Under Coherence weighting the bins are not
// counted equally, so the effective sample count is lower than the bin
// count and the estimator is biased upward besides (Finding 27) - both of
// which put the true floor above this one. How far above depends on the
// signal and is not knowable from the settings. The figure here is
// therefore a lower bound under that weighting, which is the safe
// direction for a control that stops the operator going too low; Flat is
// what the measurements recommend anyway.
//
#define DIV_COH_FLOOR_PFA 0.01

double diversity_auto_coh_floor(int ref) {
  if (ref == DIV_REF_RADE_V1) { return 0.0; }

  //
  // The achieved bin width, as published by the engine; the requested one
  // before it has ever run.
  //
  double bhz = (div_auto_binhz > 0.0) ? div_auto_binhz : div_auto_resolution;

  if (bhz <= 0.0) { return 0.0; }

  //
  // A block is one transform, so its period is the reciprocal of the bin
  // width. See diversity_auto_start().
  //
  const double bt = 1.0 / bhz;
  const double tau = (div_auto_tau > 0.0) ? div_auto_tau : bt;
  const double alpha = 1.0 - exp(-bt / tau);
  const double nblk = (alpha > 0.0 && alpha <= 1.0) ? (2.0 - alpha) / alpha : 1.0;
  double width;

  if (ref == DIV_REF_CARRIER) {
    //
    // A fixed handful either side of the tracked peak, whatever the
    // window controls say.
    //
    width = (2.0 * DIV_CARRIER_BINS + 1.0) * bhz;
  } else if (ref == DIV_REF_DIGITAL_IQ && div_auto_occ_valid) {
    //
    // Only the occupied bins are accumulated, so those are what the gate
    // is measured over - not the search region they were found in.
    //
    width = div_auto_occ_hi - div_auto_occ_lo;
  } else if (div_auto_follow_filter) {
    width = 0.0;

    if (receivers > 0 && receiver[0] != NULL) {
      width = (double)receiver[0]->filter_high - (double)receiver[0]->filter_low;
    }

    if (width <= 0.0) { width = div_auto_width; }
  } else {
    width = div_auto_width;
  }

  double nbins = floor(width / bhz) + 1.0;

  if (nbins < 1.0) { nbins = 1.0; }

  double n = nbins * nblk;

  if (n < 2.0) { n = 2.0; }

  double floorval = 1.0 - pow(DIV_COH_FLOOR_PFA, 1.0 / (n - 1.0));

  //
  // A floor above the top of the slider would leave nothing to set. It
  // takes a one-bin window and no averaging at all to get there, which
  // the controls do not allow, but the clamp costs nothing.
  //
  if (floorval > 0.5) { floorval = 0.5; }

  if (floorval < 0.0) { floorval = 0.0; }

  //
  // Up onto the tenth of a percent the menu's slider steps in, so that the
  // clamp below and the position the operator can actually reach are the
  // same number. Nothing else depends on the exact value.
  //
  return 0.001 * ceil(1000.0 * floorval);
}

//
// Hold the live threshold at or above that floor, and put the result back
// in the selected reference's own slot, so that what the menu shows, what
// travels to a client and what the gate actually compares against are one
// number. Returns 1 if it had to move.
//
int diversity_auto_clamp_cohmin(void) {
  const double f = diversity_auto_coh_floor(div_auto_ref);

  if (div_auto_coherence_min >= f) { return 0; }

  div_auto_coherence_min = f;
  diversity_auto_ref_store(div_auto_ref);
  return 1;
}

void diversity_auto_ref_store(int ref) {
  if (ref == DIV_REF_CARRIER) {
    div_carrier_centre = div_auto_centre;
    div_carrier_width  = div_auto_width;
    div_carrier_cohmin = div_auto_coherence_min;
  } else if (ref == DIV_REF_BAND) {
    div_band_centre = div_auto_centre;
    div_band_width  = div_auto_width;
    div_band_cohmin = div_auto_coherence_min;
  } else if (ref == DIV_REF_DIGITAL_IQ) {
    div_digital_centre = div_auto_centre;
    div_digital_width  = div_auto_width;
    div_digital_cohmin = div_auto_coherence_min;
  }

  //
  // DIV_REF_RADE_V1 has neither a window of its own - the correlator
  // decides what it looks at - nor a threshold to file any more. Its slot
  // is pinned at zero in div_settings_validate(); storing the live value
  // into it here would let a value that arrived by some other route stick.
  //
}

void diversity_auto_ref_recall(int ref) {
  if (ref == DIV_REF_CARRIER) {
    div_auto_centre = div_carrier_centre;
    div_auto_width  = div_carrier_width;
  } else if (ref == DIV_REF_BAND) {
    div_auto_centre = div_band_centre;
    div_auto_width  = div_band_width;
  } else if (ref == DIV_REF_DIGITAL_IQ) {
    div_auto_centre = div_digital_centre;
    div_auto_width  = div_digital_width;
  }

  div_auto_coherence_min = div_cohmin_for_ref(ref);
}

void diversity_auto_get_settings(DIV_SETTINGS *s) {
  s->mode           = div_auto_mode;
  s->ref            = div_auto_ref;
  s->follow_filter  = div_auto_follow_filter;
  s->weighting      = div_auto_weighting;
  s->normalise      = div_auto_normalise;
  s->hold           = div_auto_hold;
  s->centre         = div_auto_centre;
  s->width          = div_auto_width;
  s->tau            = div_auto_tau;
  s->hang           = div_auto_hang;
  s->coherence_min  = div_auto_coherence_min;
  s->resolution     = div_auto_resolution;
  s->band_cohmin    = div_band_cohmin;
  s->carrier_cohmin = div_carrier_cohmin;
  s->digital_cohmin = div_digital_cohmin;
  s->rade_cohmin    = div_rade_cohmin;
  s->band_centre    = div_band_centre;
  s->band_width     = div_band_width;
  s->carrier_centre = div_carrier_centre;
  s->carrier_width  = div_carrier_width;
  s->digital_centre = div_digital_centre;
  s->digital_width  = div_digital_width;
}

//
// The globals a settings block names, and nothing else: no restarts, no
// resets, no inversion. Hold is deliberately not among them - it is a
// momentary operator control rather than a setting, and the two callers
// below want opposite things done with it.
//
static void div_settings_load(const DIV_SETTINGS *s) {
  div_auto_mode          = s->mode;
  div_auto_ref           = s->ref;
  div_auto_follow_filter = s->follow_filter;
  div_auto_weighting     = s->weighting;
  div_auto_normalise     = s->normalise;
  div_auto_centre        = s->centre;
  div_auto_width         = s->width;
  div_auto_tau           = s->tau;
  div_auto_hang          = s->hang;
  div_auto_coherence_min = s->coherence_min;
  div_auto_resolution    = s->resolution;
  div_band_cohmin        = s->band_cohmin;
  div_carrier_cohmin     = s->carrier_cohmin;
  div_digital_cohmin     = s->digital_cohmin;
  div_rade_cohmin        = s->rade_cohmin;
  //
  // The live threshold always belongs to the selected reference. Taking
  // it from the slot rather than from s->coherence_min is what makes that
  // true on every path into here - a modal block, a client, a properties
  // restore - rather than only on the one the menu takes. Without it a
  // radio starting up in RADE V1 would gate on whatever the *previous*
  // reference was set to, which for a file written before this existed is
  // 0.30 against a mode that had no gate at all.
  //
  div_auto_coherence_min = div_cohmin_for_ref(div_auto_ref);
  div_band_centre        = s->band_centre;
  div_band_width         = s->band_width;
  div_carrier_centre     = s->carrier_centre;
  div_carrier_width      = s->carrier_width;
  div_digital_centre     = s->digital_centre;
  div_digital_width      = s->digital_width;
}

//
// Adopt a settings block, and do whatever the change of state calls for.
//
// This is the one description of what moving a control means. The menu
// callbacks used to hold it, which was fine while the only operator was
// sitting at the radio; with the UI able to run on a client, the server
// has to draw the same conclusions from a settings block that the menu
// used to draw from a widget, and two copies of these rules would drift.
//
// The conclusions are drawn by comparing against what is in force rather
// than being sent, so a client need only ship its control state and never
// has to reason about restarts.
//
static void div_settings_validate(DIV_SETTINGS *s);

void diversity_auto_apply_settings(const DIV_SETTINGS *in, int action) {
  //
  // Validate whatever arrives, wherever it arrived from. This is the one
  // door every settings block comes through - the menu, a client, a
  // properties restore - and a block that has been over the wire has been
  // out of this program's hands. Cheaper than trusting each caller, and it
  // is what stops an out-of-range field becoming a control that behaves
  // strangely rather than one that is refused.
  //
  DIV_SETTINGS vs = *in;
  div_settings_validate(&vs);
  const DIV_SETTINGS *s = &vs;
  const int    old_mode   = div_auto_mode;
  const int    old_ref    = div_auto_ref;
  const int    old_follow = div_auto_follow_filter;
  const int    old_weight = div_auto_weighting;
  const double old_centre = div_auto_centre;
  const double old_width  = div_auto_width;
  const double old_res    = div_auto_resolution;
  div_settings_load(s);

  //
  // A remote client adopts the values and stops there: the analysis, and
  // every consequence of changing it, belongs to the radio. Its own menu
  // reads these globals, so adopting them is the whole job here.
  //
  // Hold is taken directly rather than through diversity_auto_set_hold(),
  // whose job is the weight handover on release - there is no weight to
  // hand over here, and the button still has to show what the radio has.
  //
  if (radio_is_remote) {
    div_auto_hold = s->hold;
    return;
  }

  //
  // Null and Sum are the same measurement 180 degrees apart, so crossing
  // between them turns the weight in force over at once rather than
  // waiting for a loop that may not be applying anything. This is also
  // the path the Invert button takes - it moves the objective, and the
  // objective is what carries the meaning.
  //
  if ((old_mode == DIV_AUTO_NULL && s->mode == DIV_AUTO_SUM) ||
      (old_mode == DIV_AUTO_SUM  && s->mode == DIV_AUTO_NULL)) {
    diversity_auto_invert();
  }

  //
  // The thread has to come up or go down when the objective crosses Off;
  // the transform has to be rebuilt when its length changes; and the
  // pilot correlator's front end has to be built or torn down when a RADE
  // reference is selected or left.
  //
  if ((old_mode == DIV_AUTO_OFF) != (s->mode == DIV_AUTO_OFF) ||
      old_res != s->resolution ||
      (old_ref != s->ref && (old_ref == DIV_REF_RADE_V1 || s->ref == DIV_REF_RADE_V1))) {
    diversity_auto_restart();
  }

  //
  // Anything that changes which bins are accumulated, or how, invalidates
  // what has been accumulated so far.
  //
  if (action == DIV_ACTION_RESET || old_ref != s->ref || old_follow != s->follow_filter ||
      old_weight != s->weighting || old_centre != s->centre || old_width != s->width) {
    diversity_auto_reset();
  }

  diversity_auto_set_hold(s->hold);
}

void diversity_auto_get_status(DIV_STATUS *st) {
  st->enabled         = diversity_enabled;
  st->indep_att       = div_indep_att;
  st->att0            = adc[0].attenuation;
  st->att1            = adc[1].attenuation;
  st->running         = div_auto_running;
  st->holding         = div_auto_holding;
  st->standdown       = div_auto_standdown;
  st->clamped         = div_auto_clamped;
  st->arm_valid       = div_auto_arm_valid;
  st->arm_pick        = div_auto_arm_pick;
  st->carrier_valid   = div_auto_carrier_valid;
  st->occ_valid       = div_auto_occ_valid;
  st->rade_locked     = rade_corr_locked;
  st->rade_confirming = rade_corr_confirming;
  st->rade_side       = div_rade_side;
  st->binhz           = div_auto_binhz;
  st->coherence       = div_auto_coherence;
  st->carrier         = div_auto_carrier;
  st->arm_db          = div_auto_arm_db;
  st->occ_lo          = div_auto_occ_lo;
  st->occ_hi          = div_auto_occ_hi;
  st->gain            = div_gain;
  st->phase           = div_phase;
  st->track_gain      = div_track_gain;
  st->track_phase     = div_track_phase;
  st->rade_quality    = rade_corr_quality;
}

//
// Remote client: write the radio's status into the globals every consumer
// already reads, so the status line, the antenna line and the panadapter
// overlay need no remote-aware code of their own.
//
// div_gain/div_phase are the applied weight and are written here too: on
// a client they are what the sliders show, and while the loop owns them
// the radio is the only thing that knows what they are.
//
void diversity_auto_apply_status(const DIV_STATUS *st) {
  diversity_enabled      = st->enabled;
  //
  // The attenuators travel with the status rather than waiting for the
  // next INFO_ADC, so a client's menu follows one moved at the radio.
  //
  div_indep_att          = st->indep_att;
  adc[0].attenuation     = st->att0;
  adc[1].attenuation     = st->att1;
  div_auto_running       = st->running;
  div_auto_holding       = st->holding;
  div_auto_standdown     = st->standdown;
  div_auto_clamped       = st->clamped;
  div_auto_arm_valid     = st->arm_valid;
  div_auto_arm_pick      = st->arm_pick;
  div_auto_carrier_valid = st->carrier_valid;
  div_auto_occ_valid     = st->occ_valid;
  rade_corr_locked       = st->rade_locked;
  rade_corr_confirming   = st->rade_confirming;
  div_rade_side          = st->rade_side;
  div_auto_binhz         = st->binhz;
  div_auto_coherence     = st->coherence;
  div_auto_carrier       = st->carrier;
  div_auto_arm_db        = st->arm_db;
  div_auto_occ_lo        = st->occ_lo;
  div_auto_occ_hi        = st->occ_hi;
  div_gain               = st->gain;
  div_phase              = st->phase;
  div_track_gain         = st->track_gain;
  div_track_phase        = st->track_phase;
  rade_corr_quality      = st->rade_quality;
}

//
// ----------------------------------------------------------------------
// Modal settings
// ----------------------------------------------------------------------
//
// One block of settings per group of modes, rather than one for the radio.
//
// Which reference, which window and which objective are right is a
// property of what is being received, and the mode is the operator's own
// statement of that: a carrier to track in AM and SAM, an FSK occupancy
// to find in DIGU and DIGL, a filter-wide window in SSB, and in CW a
// window narrow enough to sit on one note. Carrying a single set across a
// mode change therefore hands the loop settings chosen for a signal that
// is no longer there - the carrier tracker hunting a carrier SSB does not
// have, or the 100 Hz window left over from CW swallowing an SSB passband
// whole - and the operator has to notice and undo it every time.
//
// So each group keeps its own block. Changing mode files what is in force
// under the outgoing group and adopts the incoming one, and every block
// is saved, so the settings an operator built up for RTTY are still there
// after an evening on SSB.
//
// The groups are the coarsest division that never mixes two signals
// wanting different answers. DSB sits with AM and SAM because its
// passband is symmetric about the carrier, so a window and a carrier
// search mean the same thing there. Anything not named - presently only
// SPEC - shares one block, which costs nothing and means a mode added
// later still lands somewhere sensible.
//
enum {
  DIV_GROUP_SSB = 0,    // LSB, USB
  DIV_GROUP_CW,         // CWL, CWU
  DIV_GROUP_FM,         // FMN
  DIV_GROUP_AM,         // AM, SAM, DSB
  DIV_GROUP_DIGITAL,    // DIGU, DIGL
  DIV_GROUP_OTHER,      // everything else
  DIV_GROUPS
};

static int div_group_of_mode(int mode) {
  switch (mode) {
  case modeLSB:
  case modeUSB:
    return DIV_GROUP_SSB;

  case modeCWL:
  case modeCWU:
    return DIV_GROUP_CW;

  case modeFMN:
    return DIV_GROUP_FM;

  case modeAM:
  case modeSAM:
  case modeDSB:
    return DIV_GROUP_AM;

  case modeDIGU:
  case modeDIGL:
    return DIV_GROUP_DIGITAL;

  default:
    return DIV_GROUP_OTHER;
  }
}

static DIV_SETTINGS div_group_set[DIV_GROUPS];

//
// The group whose block the div_auto_* globals currently hold. -1 until
// the first mode change is announced, which says "nothing has been filed
// away yet": the globals at that point are what the props file restored,
// and the mode being announced is the mode they were saved under, so the
// group's own block is the one to believe.
//
static int div_group_current = -1;

//
// The operator changed mode.
//
// Called on the radio for RX0 from rx_mode_changed(), which covers every
// route a mode can change by - the menu, CAT, a bandstack recall, a VFO
// swap, and a client asking for one.
//
void diversity_auto_mode_changed(int mode) {
  //
  // The blocks live with the analysis, on the radio. A client is told the
  // outcome the way it is told about any other change to the settings.
  //
  if (radio_is_remote) { return; }

  const int g = div_group_of_mode(mode);

  if (g == div_group_current) { return; }

  if (div_group_current >= 0) {
    diversity_auto_get_settings(&div_group_set[div_group_current]);
  }

  div_group_current = g;
  const int    was_off = (div_auto_mode == DIV_AUTO_OFF);
  const int    old_ref = div_auto_ref;
  const double old_res = div_auto_resolution;
  div_settings_load(&div_group_set[g]);

  //
  // The same three conditions diversity_auto_apply_settings() draws: the
  // thread comes up or goes down when the objective crosses Off, the
  // transform is rebuilt when its length changes, and the pilot
  // correlator's front end is built or torn down when a RADE reference is
  // taken up or left.
  //
  if (was_off != (div_auto_mode == DIV_AUTO_OFF) ||
      old_res != div_auto_resolution ||
      (old_ref != div_auto_ref &&
       (old_ref == DIV_REF_RADE_V1 || div_auto_ref == DIV_REF_RADE_V1))) {
    diversity_auto_restart();
  }

  //
  // Deliberately not diversity_auto_invert(), even when the objective
  // crosses between Null and Sum. There the operator asked for the weight
  // in force to be turned over; here two unrelated blocks merely happen
  // to differ, and the retune has invalidated the statistics behind the
  // old weight anyway.
  //
  diversity_auto_reset();
  //
  // Show it, and tell a client that is running the panel. A mode change
  // can arrive on the server thread as well as on the GTK one, and both
  // halves of that belong to GTK.
  //
  g_idle_add(diversity_menu_settings_changed, NULL);
}

//
// Clamp everything in a settings block to what the controls can express.
//
// A props file can be hand-edited or written by a future version, and an
// out-of-range value is hard to diagnose from the UI: a bad reference
// shows a blank combo, and a coherence threshold above 1.0 wedges the
// loop in permanent HOLD with nothing on screen to say why. Every block
// goes through here, not just the live one.
//
static void div_settings_validate(DIV_SETTINGS *s) {
  if (s->mode < DIV_AUTO_OFF || s->mode > DIV_AUTO_BEST) {
    s->mode = DIV_AUTO_OFF;
  }

  if (s->ref < DIV_REF_BAND || s->ref > DIV_REF_DIGITAL_IQ) {
    s->ref = DIV_REF_BAND;
  }

  s->follow_filter = s->follow_filter ? 1 : 0;
  s->normalise     = s->normalise ? 1 : 0;

  //
  // Pinned, not ranged, for the reason DIV_HANG_DEFAULT is: there is no
  // control for it any more. Coherence weighting was measured four times
  // - on the estimate, on the gate's ROC, in decibels at matched false
  // alarm, and on the six SSB captures whose windows are mostly noise,
  // which is the case it existed for - and it is behind or level every
  // time. The field stays on the wire and in the file so that neither has
  // to change shape, and DIV_WEIGHT_COHERENCE stays in the enum because
  // the offline harness still has to be able to sweep the retired path.
  // See Findings 27, 29, 40 and 42.
  //
  s->weighting = DIV_WEIGHT_FLAT;

  //
  // The live threshold and the three references whose gate has a slider.
  // RADE V1's is not one of them - it is pinned below, for the reasons
  // set out there - so four values, not five.
  //
  {
    double *c[] = { &s->coherence_min, &s->band_cohmin, &s->carrier_cohmin,
                    &s->digital_cohmin
                  };

    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
      if (!(*c[i] >= 0.0)) { *c[i] = 0.0; }

      if (*c[i] > 0.95)    { *c[i] = 0.95; }
    }
  }

  //
  // RADE V1's is pinned, not ranged, for the reason the hang and the
  // weighting are: there is no control for it any more.
  //
  // Three gates already stand in front of it and all of them are on the
  // pilot - the acquisition ladder's sigmas, the confirm and probation
  // ladder, and RADE_USE_RATIO's per-frame freeze - so this one is only
  // reached on blocks where the pilot has already been found. Measured
  // over the whole set: the five no-signal captures produce **no weight
  // at all**, 3515 blocks of it, so the false-alarm job a threshold
  // exists for is already finished. And the quantity it gates does not
  // separate what is left: `234508`, a strong capture producing a weight
  // on three blocks in four, reads a median quality of 0.217 with 31 % of
  // its blocks under 0.05, where a capture that re-acquires eight times a
  // minute reads 0.193.
  //
  // What decides it is that no reachable setting was safe. On `165826`,
  // where one antenna gets 176 frames, the other 323 and the combiner
  // 329, **every** block that produced a weight is under 0.25 and a third
  // are under 0.05 - so the slider's whole travel held the loop through
  // the one decode in the set that best shows the combiner working. A
  // control whose likeliest use is to break a working decode is worse
  // than no control. See Findings 26, 33, 35 and 41.
  //
  // Zero is what shipped as the default, so nothing an operator has today
  // moves. The field stays on the wire and in the props file so neither
  // changes shape, and the comparison stays in the engine so the offline
  // harness can still sweep it.
  //
  s->rade_cohmin = 0.0;

  //
  // 0.2, not 0.1, to match the slider's minimum.
  //
  if (s->tau < 0.2)  { s->tau = 0.2; }

  if (s->tau > 30.0) { s->tau = 30.0; }

  //
  // Pinned, not ranged. There is no control for it any more and it is not
  // a setting an operator can improve on - see DIV_HANG_DEFAULT - so a
  // value left in a props file by an older build, or sent by an older
  // client, is replaced rather than merely clamped. The field stays on
  // the wire and in the file so that neither has to change shape.
  //
  s->hang = DIV_HANG_DEFAULT;

  //
  // Both ends match the menu, which is 24 / 12 / 6 Hz. A props file or an
  // older client carrying the retired 3 Hz setting lands on 6, the nearest
  // one that survives - which is a coarser block than it asked for and, on
  // five captures of six, a better one. See Finding 43.
  //
  if (s->resolution < 6.0)  { s->resolution = 6.0; }

  if (s->resolution > 24.0) { s->resolution = 24.0; }

  //
  // The widths: 20.0, not 10.0, because the spin button's minimum is 20
  // and a restored value below it was silently snapped up the first time
  // the menu was opened.
  //
  // The centres are deliberately generous: the window is allowed outside
  // the passband, and how far is a function of the sample rate, so
  // div_bin_range() does the real limiting against the Nyquist frequency
  // at the rate in use.
  //
  double *widths[]  = { &s->width, &s->band_width, &s->carrier_width, &s->digital_width };
  double *centres[] = { &s->centre, &s->band_centre, &s->carrier_centre, &s->digital_centre };

  for (int i = 0; i < 4; i++) {
    if (*widths[i] < 20.0)    { *widths[i] = 20.0; }

    if (*widths[i] > 40000.0) { *widths[i] = 40000.0; }

    if (*centres[i] < -400000.0) { *centres[i] = -400000.0; }

    if (*centres[i] >  400000.0) { *centres[i] =  400000.0; }
  }
}

//
// One group's block, to and from the props file.
//
// Written under the current reference numbering only - these keys did not
// exist under scheme 1 - so there is nothing here to migrate. See
// DIV_REF_SCHEME.
//
static void div_group_save(int g, const DIV_SETTINGS *s) {
  SetPropI1("diversity_group[%d].mode",           g, s->mode);
  SetPropI1("diversity_group[%d].ref",            g, s->ref);
  SetPropI1("diversity_group[%d].follow_filter",  g, s->follow_filter);
  SetPropI1("diversity_group[%d].weighting",      g, s->weighting);
  SetPropI1("diversity_group[%d].normalise",      g, s->normalise);
  SetPropF1("diversity_group[%d].centre",         g, s->centre);
  SetPropF1("diversity_group[%d].width",          g, s->width);
  SetPropF1("diversity_group[%d].tau",            g, s->tau);
  SetPropF1("diversity_group[%d].hang",           g, s->hang);
  SetPropF1("diversity_group[%d].coherence_min",  g, s->coherence_min);
  SetPropF1("diversity_group[%d].resolution",     g, s->resolution);
  SetPropF1("diversity_group[%d].band_cohmin",    g, s->band_cohmin);
  SetPropF1("diversity_group[%d].carrier_cohmin", g, s->carrier_cohmin);
  SetPropF1("diversity_group[%d].digital_cohmin", g, s->digital_cohmin);
  SetPropF1("diversity_group[%d].rade_cohmin",    g, s->rade_cohmin);
  SetPropF1("diversity_group[%d].band_centre",    g, s->band_centre);
  SetPropF1("diversity_group[%d].band_width",     g, s->band_width);
  SetPropF1("diversity_group[%d].carrier_centre", g, s->carrier_centre);
  SetPropF1("diversity_group[%d].carrier_width",  g, s->carrier_width);
  SetPropF1("diversity_group[%d].digital_centre", g, s->digital_centre);
  SetPropF1("diversity_group[%d].digital_width",  g, s->digital_width);
}

//
// The block arrives seeded with the settings that were in force, and
// GetProp leaves a field alone when its key is absent, so a props file
// written before this existed gives every group what the radio was last
// set to - which is exactly the old single-block behaviour, until the
// operator moves a control in one mode and not another.
//
static void div_group_restore(int g, DIV_SETTINGS *s) {
  GetPropI1("diversity_group[%d].mode",           g, s->mode);
  GetPropI1("diversity_group[%d].ref",            g, s->ref);
  GetPropI1("diversity_group[%d].follow_filter",  g, s->follow_filter);
  GetPropI1("diversity_group[%d].weighting",      g, s->weighting);
  GetPropI1("diversity_group[%d].normalise",      g, s->normalise);
  GetPropF1("diversity_group[%d].centre",         g, s->centre);
  GetPropF1("diversity_group[%d].width",          g, s->width);
  GetPropF1("diversity_group[%d].tau",            g, s->tau);
  GetPropF1("diversity_group[%d].hang",           g, s->hang);
  GetPropF1("diversity_group[%d].coherence_min",  g, s->coherence_min);
  GetPropF1("diversity_group[%d].resolution",     g, s->resolution);
  GetPropF1("diversity_group[%d].band_cohmin",    g, s->band_cohmin);
  GetPropF1("diversity_group[%d].carrier_cohmin", g, s->carrier_cohmin);
  GetPropF1("diversity_group[%d].digital_cohmin", g, s->digital_cohmin);
  GetPropF1("diversity_group[%d].rade_cohmin",    g, s->rade_cohmin);
  GetPropF1("diversity_group[%d].band_centre",    g, s->band_centre);
  GetPropF1("diversity_group[%d].band_width",     g, s->band_width);
  GetPropF1("diversity_group[%d].carrier_centre", g, s->carrier_centre);
  GetPropF1("diversity_group[%d].carrier_width",  g, s->carrier_width);
  GetPropF1("diversity_group[%d].digital_centre", g, s->digital_centre);
  GetPropF1("diversity_group[%d].digital_width",  g, s->digital_width);
}

void diversity_auto_save_state(void) {
  //
  // The radio owns these. A client adopts what it finds on connect, so
  // saving them here would leave it carrying the last radio's settings
  // into its own next session as a standalone radio.
  //
  if (radio_is_remote) { return; }

  //
  // The live values are the current group's, and have not been filed
  // away since the last mode change.
  //
  if (div_group_current >= 0) {
    diversity_auto_get_settings(&div_group_set[div_group_current]);
  }

  SetPropI0("diversity_auto_mode",           div_auto_mode);
  SetPropI0("diversity_auto_ref",            div_auto_ref);
  SetPropI0("diversity_auto_ref_scheme",     DIV_REF_SCHEME);
  SetPropI0("diversity_auto_follow_filter",  div_auto_follow_filter);
  SetPropF0("diversity_auto_centre",         div_auto_centre);
  SetPropF0("diversity_auto_width",          div_auto_width);
  SetPropF0("diversity_auto_tau",            div_auto_tau);
  SetPropF0("diversity_auto_hang",           div_auto_hang);
  SetPropF0("diversity_auto_coherence_min",  div_auto_coherence_min);
  SetPropI0("diversity_auto_weighting",      div_auto_weighting);
  SetPropI0("diversity_auto_normalise",      div_auto_normalise);
  SetPropF0("diversity_auto_resolution",     div_auto_resolution);
  SetPropF0("diversity_band_cohmin",         div_band_cohmin);
  SetPropF0("diversity_carrier_cohmin",      div_carrier_cohmin);
  SetPropF0("diversity_digital_cohmin",      div_digital_cohmin);
  SetPropF0("diversity_rade_cohmin",         div_rade_cohmin);
  SetPropF0("diversity_band_centre",         div_band_centre);
  SetPropF0("diversity_band_width",          div_band_width);
  SetPropF0("diversity_carrier_centre",      div_carrier_centre);
  SetPropF0("diversity_carrier_width",       div_carrier_width);
  SetPropF0("diversity_digital_centre",      div_digital_centre);
  SetPropF0("diversity_digital_width",       div_digital_width);

  for (int g = 0; g < DIV_GROUPS; g++) {
    div_group_save(g, &div_group_set[g]);
  }
}

void diversity_auto_restore_state(void) {
  GetPropI0("diversity_auto_mode",           div_auto_mode);
  GetPropI0("diversity_auto_ref",            div_auto_ref);
  GetPropI0("diversity_auto_follow_filter",  div_auto_follow_filter);
  GetPropF0("diversity_auto_centre",         div_auto_centre);
  GetPropF0("diversity_auto_width",          div_auto_width);
  GetPropF0("diversity_auto_tau",            div_auto_tau);
  GetPropF0("diversity_auto_hang",           div_auto_hang);
  GetPropF0("diversity_auto_coherence_min",  div_auto_coherence_min);
  GetPropI0("diversity_auto_weighting",      div_auto_weighting);
  GetPropI0("diversity_auto_normalise",      div_auto_normalise);
  GetPropF0("diversity_auto_resolution",     div_auto_resolution);
  //
  // A file written before these existed carries none of them, and GetProp
  // leaves a field alone when its key is absent - so each reference
  // inherits the single diversity_auto_coherence_min just read, which is
  // exactly the behaviour that file was written under.
  //
  div_band_cohmin = div_carrier_cohmin = div_digital_cohmin = div_auto_coherence_min;
  GetPropF0("diversity_band_cohmin",         div_band_cohmin);
  GetPropF0("diversity_carrier_cohmin",      div_carrier_cohmin);
  GetPropF0("diversity_digital_cohmin",      div_digital_cohmin);
  GetPropF0("diversity_rade_cohmin",         div_rade_cohmin);
  GetPropF0("diversity_band_centre",         div_band_centre);
  GetPropF0("diversity_band_width",          div_band_width);
  GetPropF0("diversity_carrier_centre",      div_carrier_centre);
  GetPropF0("diversity_carrier_width",       div_carrier_width);
  GetPropF0("diversity_digital_centre",      div_digital_centre);
  GetPropF0("diversity_digital_width",       div_digital_width);

  //
  // Migrate a reference written under the old numbering. Absent key means
  // scheme 1; see DIV_REF_SCHEME. Only the single flat key needs it - the
  // per-group keys below are newer than the renumbering - and it has to
  // happen before the block is seeded from these values, so that every
  // group inherits the migrated reference rather than the raw one.
  //
  {
    //
    // GetPropI0 leaves the variable alone when the key is absent, so the
    // default here has to be the *old* scheme - a file that predates the
    // key is exactly the one that needs migrating.
    //
    int scheme = 1;
    GetPropI0("diversity_auto_ref_scheme", scheme);

    if (scheme < 2) {
      switch (div_auto_ref) {
      case 2:
        //
        // The RADE passband reference. FSK/Digital replaces it: it places
        // itself on the operator's passband in the same way and finds the
        // modem's occupied bins inside it, so an operator who was using
        // that lands on its successor rather than on something unrelated.
        //
        div_auto_ref = DIV_REF_DIGITAL_IQ;
        break;

      case 3: div_auto_ref = DIV_REF_RADE_V1;    break;   // was RADE V1
      case 4: div_auto_ref = DIV_REF_DIGITAL_IQ; break;   // was FSK/Digital

      default: break;                                     // 0 and 1 unmoved
      }
    }
  }

  //
  // Same rule as div_settings_load(): whatever the file said, the live
  // threshold belongs to the selected reference. After the migration
  // above, so that a file carrying the old numbering picks up the
  // threshold for the reference it ends up on rather than the one it was
  // written as.
  //
  div_auto_coherence_min = div_cohmin_for_ref(div_auto_ref);

  //
  // Validate what came out of the file, then use it to seed every group
  // that the file has nothing of its own to say about.
  //
  DIV_SETTINGS base;
  diversity_auto_get_settings(&base);
  div_settings_validate(&base);
  div_settings_load(&base);

  for (int g = 0; g < DIV_GROUPS; g++) {
    div_group_set[g] = base;
    div_group_restore(g, &div_group_set[g]);
    div_settings_validate(&div_group_set[g]);
  }

  //
  // Back to "nothing filed away yet". The mode is not restored until
  // after this runs, so which group the live values belong to is not
  // knowable here; the first diversity_auto_mode_changed() adopts that
  // group's block, which for a file written by this version is the same
  // thing the flat keys just gave us. Without the reset, that first call
  // would file the live values under whichever group was current before
  // the restore and overwrite the block just read for it.
  //
  div_group_current = -1;
}
