/*
 * Two checks on the wideband Window reference.
 *
 * 1. Window placement. Bins are indexed k % nfft, so a window outside the
 *    first Nyquist zone used to be measured at a different frequency
 *    entirely, silently: at 48 kHz a window at +30 kHz landed on -18 kHz,
 *    and the spin ranges allowed exactly that. It must now be clamped and
 *    flagged rather than aliased.
 *
 * 2. Bin weighting on speech. SSB voice has no carrier, the energy moves
 *    about constantly and much of the passband is noise at any instant.
 *    Flat weighting averages h(f) by power and is therefore diluted by
 *    the noise-only bins; coherence weighting should be measurably
 *    better. This is the evidence for which one is the default.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <gtk/gtk.h>

#include "mode.h"
#include "receiver.h"
#include "vfo.h"
#include "adc.h"
#include "diversity_auto.h"

static RECEIVER rx0;
RECEIVER *receiver[8] = { &rx0 };
int receivers = 2;
int diversity_enabled = 1;
int radio_is_remote = 0;
int cw_keyer_sidetone_frequency = 800;
double div_cos = 1.0, div_sin = 0.0, div_gain = 0.0, div_phase = 0.0;
double div_norm = 1.0;
//
// The engine reads the two step attenuators as part of its analysis
// context, so a change of either restarts the statistics.
//
ADC adc[3];
int div_indep_att = 0;
struct _vfo vfo[MAX_VFOS];
static int verbose = 0;
void t_print(const char *fmt, ...) {
  if (!verbose) { return; }

  va_list a;
  va_start(a, fmt);
  vprintf(fmt, a);
  va_end(a);
}
const char *getProperty(const char *n) { (void)n; return NULL; }
void setProperty(const char *n, const char *v) { (void)n; (void)v; }
double myatof(const char *s) { return atof(s); }

/*
 * The engine tells the menu when a mode change swapped one block of modal
 * settings for another. There is no menu here.
 */
gboolean diversity_menu_settings_changed(gpointer data) { (void)data; return G_SOURCE_REMOVE; }

static double frand(void) { return 2.0 * ((double)rand() / RAND_MAX) - 1.0; }

static void settle(void) { g_usleep(15000); }

/* ------------------------------------------------------------------ */
/* 3. a keyed carrier                                                 */
/* ------------------------------------------------------------------ */

/*
 * CW is the hardest case for an exponentially forgetting estimator,
 * because the signal is absent for most of a transmission rather than
 * only between them. While the accumulators decay at the operator's
 * averaging time, Sxy, Sxx and Syy decay together, so the coherence gate
 * sees gamma^2 stay near 1 and the loop goes on reporting "track" with
 * nothing but noise in front of it - for 5.8 time constants on a 30 dB
 * signal, which is about twelve seconds at the default averaging.
 *
 * What is checked here is that it stops promptly instead, and without
 * dragging the weight away from the answer key-down gave it.
 */
static int test_keyed(void) {
  const int rate = 48000, nfft = 4096;
  const double hr = 0.62, hi = -0.48;
  const double nz = 0.03;                 /* a carrier ~30 dB out */
  rx0.sample_rate = rate;
  /*
   * A 500 Hz CW filter. rx_set_filter() folds the sidetone into the
   * filter edges and div_frame_off() takes it back out, so a carrier on
   * the dial lands in the middle of the passband - which is where a
   * correctly tuned CW signal is, and is the arrangement that convention
   * exists to produce.
   */
  rx0.filter_low = -1050;
  rx0.filter_high = -550;
  vfo[0].mode = modeCWL;
  vfo[0].frequency = 7010000;
  vfo[0].ctun_frequency = 7010000;
  vfo[0].offset = 0;
  div_auto_ref = DIV_REF_BAND;
  div_auto_mode = DIV_AUTO_SUM;
  div_auto_follow_filter = 1;
  div_auto_tau = 2.0;
  div_auto_coherence_min = 0.30;
  div_auto_weighting = DIV_WEIGHT_COHERENCE;
  div_auto_resolution = 12.0;
  div_cos = 1.0;
  div_sin = 0.0;
  div_gain = 0.0;
  div_phase = 0.0;
  srand(23);
  diversity_auto_start();
  double ph = 0.0;
  /*
   * Where a correctly tuned CW carrier actually is in the tapped buffer:
   * near the dial, not at the sidetone. rx_set_filter() folds the
   * sidetone into the filter edges and div_frame_off() takes it back out,
   * so the -1050..-550 passband maps to bin frequencies -250..+250 - and
   * that is the whole point of the convention, it puts the CW passband on
   * the dial frequency. 100 Hz off, as a station one might actually be
   * listening to.
   */
  const double tone = 100.0;

  /* key down, long enough to converge */
  for (int b = 0; b < 90; b++) {
    for (int n = 0; n < nfft; n++) {
      ph += 2.0 * M_PI * tone / rate;
      const double s = cos(ph), t = sin(ph);
      diversity_auto_sample(s + nz * frand(), t + nz * frand(),
                            hr * s - hi * t + nz * frand(),
                            hr * t + hi * s + nz * frand());
    }

    settle();
  }

  g_usleep(300000);
  const double g0 = div_gain, p0 = div_phase;
  const double blockms = 1000.0 * (double)nfft / (double)rate;
  int held = -1;

  /*
   * Key up, for one second.
   *
   * It used to be two hundred blocks - seventeen seconds - and that is
   * not a key-up, it is the station having stopped. The loop now stands
   * the combiner down when the window has been empty for DIV_QUIET_DWELL,
   * so over seventeen seconds the weight correctly goes to the floor and
   * this test's drift measurement had nothing left to measure. What it
   * exists to protect is the gap between characters and between words -
   * 60 ms to 300 ms on a real fist, an order of magnitude inside the
   * dwell - and one second covers that with room to spare. The long gap
   * is test_standdown()'s business.
   */
  for (int b = 0; b < 12; b++) {
    for (int n = 0; n < nfft; n++) {
      diversity_auto_sample(nz * frand(), nz * frand(),
                            nz * frand(), nz * frand());
    }

    settle();
    g_usleep(3000);

    if (held < 0 && div_auto_holding) { held = b + 1; }
  }

  g_usleep(300000);
  const double dg = fabs(div_gain - g0);
  double dp = div_phase - p0;

  while (dp >  180.0) { dp -= 360.0; }

  while (dp < -180.0) { dp += 360.0; }

  dp = fabs(dp);
  diversity_auto_stop();
  const double secs = (held < 0) ? -1.0 : held * blockms / 1000.0;
  const int good = (held > 0) && (secs < 2.0) && (dg < 0.5) && (dp < 5.0)
                   && !div_auto_standdown;
  printf("  keyed carrier, key-up: ");

  if (held < 0) {
    printf("still tracking after %.1f s", 12 * blockms / 1000.0);
  } else {
    printf("held after %.2f s", secs);
  }

  printf(", drift %.2f dB %.1f deg, standdown %d  %s\n",
         dg, dp, div_auto_standdown ? 1 : 0, good ? "OK" : "FAIL");
  return good;
}

/* ------------------------------------------------------------------ */
/* 1. window placement                                                */
/* ------------------------------------------------------------------ */

static int test_placement(void) {
  const int rate = 48000, nfft = 4096;
  /* the frequency a +30 kHz request aliases onto at this rate */
  const double alias_hz = 30000.0 - rate;         /* -18000 */
  const double hr = 0.62, hi = -0.48;             /* channel of the alias signal */
  rx0.sample_rate = rate;
  rx0.filter_low = -3000;
  rx0.filter_high = 3000;
  div_auto_ref = DIV_REF_BAND;
  div_auto_mode = DIV_AUTO_SUM;
  div_auto_follow_filter = 0;
  div_auto_centre = 30000.0;     /* beyond Nyquist: must be refused */
  div_auto_width = 2000.0;
  div_auto_tau = 1.0;
  div_auto_coherence_min = 0.1;
  div_auto_weighting = DIV_WEIGHT_FLAT;
  div_cos = 1.0;
  div_sin = 0.0;
  div_gain = 0.0;
  div_phase = 0.0;
  diversity_auto_start();
  double ph = 0.0;
  srand(3);

  for (int b = 0; b < 60; b++) {
    for (int n = 0; n < nfft; n++) {
      /* a strong signal sitting exactly where the bad window would alias */
      ph += 2.0 * M_PI * alias_hz / rate;
      double s = cos(ph), t = sin(ph);
      diversity_auto_sample(s + 0.01 * frand(), t + 0.01 * frand(),
                            hr * s - hi * t + 0.01 * frand(),
                            hr * t + hi * s + 0.01 * frand());
    }

    settle();
  }

  g_usleep(300000);
  int clamped = div_auto_clamped;
  double g = div_gain, p = div_phase;
  diversity_auto_stop();
  /* conj(h) is what it would converge to if it aliased onto the signal */
  double bad_deg = atan2(-hi, hr) * 180.0 / M_PI;
  int moved = (fabs(g) > 0.5) || (fabs(p) > 5.0);
  printf("  placement: window +30 kHz at 48 kHz -> clamped=%d, weight %+0.2f dB %+0.1f deg\n",
         clamped, g, p);
  printf("             (aliasing onto %.0f Hz would give about %+0.1f deg)\n",
         alias_hz, bad_deg);

  if (!clamped) {
    printf("  FAIL: out-of-Nyquist window was not flagged\n");
    return 0;
  }

  if (moved) {
    printf("  FAIL: a weight was produced from an unusable window\n");
    return 0;
  }

  printf("  PASS: refused and flagged, not aliased\n");
  return 1;
}

/* ------------------------------------------------------------------ */
/* 2. flat vs coherence weighting on speech                           */
/* ------------------------------------------------------------------ */

/*
 * Speech-like: a few band-limited tones whose frequencies and amplitudes
 * wander, with pauses. Not a vocoder - just enough non-stationarity that
 * only part of the passband carries signal at any instant, which is the
 * property that separates the two weightings.
 */
static double voice(double t, int *active) {
  double env = 0.5 * (1.0 + sin(2.0 * M_PI * 0.7 * t));
  *active = (env > 0.25);

  if (!*active) { return 0.0; }

  //
  // One narrow formant that wanders across the passband, rather than
  // energy everywhere at once. This is the case the operator described:
  // a wide window of which only a small part carries signal at any
  // instant, which is why hand-placing a 300 Hz window helps.
  //
  double f1 = 1500.0 + 1100.0 * sin(2.0 * M_PI * 0.31 * t);
  return env * sin(2.0 * M_PI * f1 * t);
}

static double run_ssb(int weighting, double noise, double *err_deg) {
  const int rate = 48000, nfft = 4096;
  const double hr = 0.62, hi = -0.48;
  rx0.sample_rate = rate;
  //
  // An LSB passband. voice() below builds its energy at positive
  // frequencies, and the tapped buffer is inverted with respect to RF -
  // see the frequency bookkeeping note in diversity_auto.c - so an LSB
  // passband is what puts the analysis window on top of it. With a USB
  // passband the window would land on the image instead, which still
  // measures the same channel but is not what the mode does on air.
  //
  rx0.filter_low = -2800;
  rx0.filter_high = -200;
  vfo[0].mode = modeLSB;
  div_auto_ref = DIV_REF_BAND;
  div_auto_mode = DIV_AUTO_SUM;
  div_auto_follow_filter = 1;        /* whole passband, as intended */
  div_auto_tau = 3.0;
  div_auto_coherence_min = 0.05;
  div_auto_weighting = weighting;
  div_cos = 1.0;
  div_sin = 0.0;
  div_gain = 0.0;
  div_phase = 0.0;
  diversity_auto_start();
  double t = 0.0;
  srand(11);

  for (int b = 0; b < 120; b++) {
    for (int n = 0; n < nfft; n++) {
      int active;
      double s = voice(t, &active);
      t += 1.0 / rate;
      /* analytic-ish: quadrature partner via a quarter-cycle shift is not
         needed here, the estimator only sees the complex pair we build */
      double q = voice(t + 0.25 / 1200.0, &active);
      diversity_auto_sample(s + noise * frand(), q + noise * frand(),
                            hr * s - hi * q + noise * frand(),
                            hr * q + hi * s + noise * frand());
    }

    settle();
  }

  g_usleep(300000);
  double g = div_gain, p = div_phase;
  diversity_auto_stop();
  /* target for Sum is conj(h) */
  double want_g = 20.0 * log10(hypot(hr, hi));
  double want_p = atan2(-hi, hr) * 180.0 / M_PI;
  double dp = p - want_p;

  while (dp > 180.0) { dp -= 360.0; }

  while (dp < -180.0) { dp += 360.0; }

  *err_deg = fabs(dp);
  return fabs(g - want_g);
}

static int test_weighting(void) {
  int ok = 1;
  //
  // Both the gain and the phase error matter, and they fail differently:
  // noise-only bins in the window add to the denominator but not the
  // numerator, so flat weighting biases the *magnitude* low while leaving
  // the phase roughly right. Reporting only the phase would hide the
  // whole effect.
  //
  printf("  weighting on speech, whole passband, signal in part of it\n");
  printf("  target conj(h) = -2.11 dB, +37.75 deg\n\n");
  printf("    %-7s   %-9s %-9s   %-9s %-9s\n",
         "noise", "flat dB", "flat deg", "coh dB", "coh deg");

  for (int i = 0; i < 4; i++) {
    double noise = (double[]) { 0.05, 0.20, 0.50, 1.00 } [i];
    double fd, cd;
    double fg = run_ssb(DIV_WEIGHT_FLAT, noise, &fd);
    double cg = run_ssb(DIV_WEIGHT_COHERENCE, noise, &cd);
    printf("    %-7.2f   %-9.2f %-9.2f   %-9.2f %-9.2f   %s\n",
           noise, fg, fd, cg, cd,
           (cg <= fg + 0.1 && cd <= fd + 0.5) ? "coherence >= flat" : "flat better here");

    //
    // The claim being tested is that coherence is not worse. A large
    // regression either way is a failure.
    //
    if (cg > fg + 1.0 || cd > fd + 5.0) { ok = 0; }
  }

  return ok;
}

/* ------------------------------------------------------------------ */
/* 4. where a hand-placed window's zero is, in CW                     */
/* ------------------------------------------------------------------ */

/*
 * rx_set_filter() folds the CW sidetone into filter_low/filter_high, so a
 * CW passband sits at +pitch in CWU and -pitch in CWL and the shifted
 * frame's own zero is one pitch away from the only signal there is.
 * Following the filter never had a problem with that, because it takes
 * the folded edges; a hand-placed window measured from the shifted zero
 * and so landed a whole CW pitch - 800 Hz here - off the note the
 * operator was listening to. That is a silent failure: the window is
 * somewhere real, the status line says "track", and what it converged on
 * is noise.
 *
 * The window now starts from the zero-beat note, so one centre works in
 * every mode. The check is the same signal and the same centre in CWL,
 * CWU and USB: all three must converge on the channel, and under the old
 * behaviour the two CW cases would have been looking 800 Hz away in
 * opposite directions.
 */
static int cw_zero_case(int mode, const char *name) {
  const int rate = 48000, nfft = 4096;
  const double hr = 0.62, hi = -0.48;
  /*
   * A carrier 100 Hz above the dial, which is where a CW note the
   * operator has tuned correctly sits: div_frame_off() takes the sidetone
   * back out, so it appears at bin frequency +100 whatever the pitch is.
   * A window centre of -100 is what maps onto it - see div_shift_to_bin().
   */
  const double tone = 100.0;
  rx0.sample_rate = rate;
  /* a 500 Hz CW filter, folded as rx_set_filter() folds it */
  rx0.filter_low  = (mode == modeCWL) ? -1050 : 550;
  rx0.filter_high = (mode == modeCWL) ?  -550 : 1050;

  if (mode == modeUSB) {
    rx0.filter_low  = 200;
    rx0.filter_high = 2800;
  }

  vfo[0].mode = mode;
  vfo[0].frequency = 7010000;
  vfo[0].ctun_frequency = 7010000;
  vfo[0].offset = 0;
  div_auto_ref = DIV_REF_BAND;
  div_auto_mode = DIV_AUTO_SUM;
  div_auto_follow_filter = 0;
  div_auto_centre = -tone;
  div_auto_width = 300.0;
  div_auto_tau = 1.0;
  div_auto_coherence_min = 0.30;
  div_auto_weighting = DIV_WEIGHT_FLAT;
  div_auto_resolution = 12.0;
  div_cos = 1.0;
  div_sin = 0.0;
  div_gain = 0.0;
  div_phase = 0.0;
  srand(29);
  diversity_auto_start();
  double ph = 0.0;

  for (int b = 0; b < 60; b++) {
    for (int n = 0; n < nfft; n++) {
      ph += 2.0 * M_PI * tone / rate;
      const double s = cos(ph), t = sin(ph);
      diversity_auto_sample(s + 0.03 * frand(), t + 0.03 * frand(),
                            hr * s - hi * t + 0.03 * frand(),
                            hr * t + hi * s + 0.03 * frand());
    }

    settle();
  }

  g_usleep(300000);
  const double g = div_gain, p = div_phase;
  const int holding = div_auto_holding;
  diversity_auto_stop();
  const double want_g = 20.0 * log10(hypot(hr, hi));
  const double want_p = atan2(-hi, hr) * 180.0 / M_PI;
  double dp = p - want_p;

  while (dp >  180.0) { dp -= 360.0; }

  while (dp < -180.0) { dp += 360.0; }

  const int ok = !holding && fabs(g - want_g) < 0.5 && fabs(dp) < 5.0;
  printf("  %-4s centre %+.0f Hz -> %+0.2f dB %+0.1f deg  %s\n",
         name, div_auto_centre, g, p, ok ? "OK" : "FAIL");
  return ok;
}

/* ------------------------------------------------------------------ */
/* 5. the CW passband when the window follows the filter              */
/* ------------------------------------------------------------------ */

/*
 * Where a CW filter lands in the tapped frame, which is not the plain
 * inversion of filter_low..filter_high.
 *
 * rx_set_filter() folds the sidetone into the filter, so a CW passband
 * sits at -pitch in CWL and +pitch in CWU. div_shift_to_bin() takes it
 * back out again - it returns -(s + div_frame_off(ctx)) and
 * div_frame_off() adds the pitch in CWL and subtracts it in CWU - so a
 * 500 Hz filter lands symmetrically about zero in both, which is where a
 * correctly tuned CW note actually is. Plain inversion would put the
 * window a whole pitch away from the signal, on the wrong side of it in
 * CWL and the wrong side again in CWU.
 *
 * docs/diversity-measurements.md asserted the plain inversion for two
 * findings' worth of measurements and scored a CW capture in a band that
 * held nothing but noise. The code was right and the note was wrong; this
 * is here so that the next person to reconcile them has something that
 * fails.
 *
 * Two signals are used and they check opposite things. A note 100 Hz off
 * the dial is inside the true window and outside the plain-inversion one:
 * it must be tracked. A note at +900 Hz is inside the plain-inversion
 * window and outside the true one: it must be ignored.
 */
static int cw_follow_case(int mode, const char *name, double tone, int want_track) {
  const int rate = 48000, nfft = 4096;
  const double hr = 0.62, hi = -0.48;
  rx0.sample_rate = rate;
  /* a 500 Hz CW filter at the 800 Hz sidetone, folded as rx_set_filter() folds it */
  rx0.filter_low  = (mode == modeCWL) ? -1050 :  550;
  rx0.filter_high = (mode == modeCWL) ?  -550 : 1050;
  vfo[0].mode = mode;
  vfo[0].frequency = 7010000;
  vfo[0].ctun_frequency = 7010000;
  vfo[0].offset = 0;
  div_auto_ref = DIV_REF_BAND;
  div_auto_mode = DIV_AUTO_SUM;
  div_auto_follow_filter = 1;          /* the case under test */
  div_auto_tau = 1.0;
  div_auto_coherence_min = 0.30;
  div_auto_weighting = DIV_WEIGHT_FLAT;
  div_auto_resolution = 12.0;
  div_cos = 1.0;
  div_sin = 0.0;
  div_gain = 0.0;
  div_phase = 0.0;
  srand(31);
  diversity_auto_start();
  double ph = 0.0;

  for (int b = 0; b < 60; b++) {
    for (int n = 0; n < nfft; n++) {
      ph += 2.0 * M_PI * tone / rate;
      const double s = cos(ph), t = sin(ph);
      diversity_auto_sample(s + 0.03 * frand(), t + 0.03 * frand(),
                            hr * s - hi * t + 0.03 * frand(),
                            hr * t + hi * s + 0.03 * frand());
    }

    settle();
  }

  g_usleep(300000);
  const double g = div_gain, p = div_phase;
  const int holding = div_auto_holding;
  diversity_auto_stop();
  const double want_g = 20.0 * log10(hypot(hr, hi));
  const double want_p = atan2(-hi, hr) * 180.0 / M_PI;
  double dp = p - want_p;

  while (dp >  180.0) { dp -= 360.0; }

  while (dp < -180.0) { dp += 360.0; }

  const int tracked = !holding && fabs(g - want_g) < 0.5 && fabs(dp) < 5.0;
  const int ok = want_track ? tracked : !tracked;
  printf("  %-4s filter %+5d..%+5d, tone %+.0f Hz -> %s (%+0.2f dB %+0.1f deg, holding=%d)  %s\n",
         name, rx0.filter_low, rx0.filter_high, tone,
         tracked ? "tracked" : "ignored", g, p, holding, ok ? "OK" : "FAIL");
  return ok;
}

/* ------------------------------------------------------------------ */
/* 6. the output-level normaliser                                     */
/* ------------------------------------------------------------------ */

/*
 * receiver.c forms z0 + w*z1 with arm 0 at unity gain, so the combined
 * output is louder than one antenna by whatever the array does to it -
 * and that rise is not signal. div_norm is meant to take it back out.
 *
 * Checked three ways, because each is a different way to get it wrong:
 * the scale must actually cancel the level rise; it must be exactly 1
 * when the operator has not asked for it; and it must be exactly 1 in
 * Null, whose whole purpose is to make the output quieter.
 */
static int norm_case(int mode, int on, const char *name, double *level_db) {
  const int rate = 48000, nfft = 4096;
  const double hr = 0.62, hi = -0.48;
  rx0.sample_rate = rate;
  rx0.filter_low  = -3000;
  rx0.filter_high = 3000;
  vfo[0].mode = modeUSB;
  vfo[0].frequency = 7100000;
  vfo[0].ctun_frequency = 7100000;
  vfo[0].offset = 0;
  div_auto_ref = DIV_REF_BAND;
  div_auto_mode = mode;
  div_auto_follow_filter = 1;
  div_auto_tau = 1.0;
  div_auto_coherence_min = 0.30;
  div_auto_weighting = DIV_WEIGHT_FLAT;
  div_auto_resolution = 12.0;
  div_auto_normalise = on;
  div_cos = 1.0;
  div_sin = 0.0;
  div_gain = 0.0;
  div_phase = 0.0;
  div_norm = 1.0;
  srand(37);
  diversity_auto_start();
  double ph = 0.0;
  double p0 = 0.0, pout = 0.0;
  long np = 0;

  for (int b = 0; b < 80; b++) {
    for (int n = 0; n < nfft; n++) {
      ph += 2.0 * M_PI * 700.0 / rate;
      const double s = cos(ph), t2 = sin(ph);
      const double i0 = s + 0.02 * frand(), q0 = t2 + 0.02 * frand();
      const double i1 = hr * s - hi * t2 + 0.02 * frand();
      const double q1 = hr * t2 + hi * s + 0.02 * frand();
      diversity_auto_sample(i0, q0, i1, q1);

      /* the last twenty blocks, once the loop has settled */
      if (b >= 60) {
        const double is = (i0 + (div_cos * i1 - div_sin * q1)) * div_norm;
        const double qs = (q0 + (div_sin * i1 + div_cos * q1)) * div_norm;
        p0   += i0 * i0 + q0 * q0;
        pout += is * is + qs * qs;
        np++;
      }
    }

    settle();
  }

  g_usleep(300000);
  const double norm = div_norm;
  diversity_auto_stop();
  *level_db = 10.0 * log10(pout / p0);
  printf("  %-28s div_norm %.4f -> output %+0.2f dB against arm 0 alone\n",
         name, norm, *level_db);
  (void)np;
  return 1;
}

static int test_normalise(void) {
  double off = 0.0, on = 0.0, nul = 0.0;
  norm_case(DIV_AUTO_SUM,  0, "Sum, normaliser off", &off);
  norm_case(DIV_AUTO_SUM,  1, "Sum, normaliser on",  &on);
  norm_case(DIV_AUTO_NULL, 1, "Null, normaliser on", &nul);
  int ok = 1;

  if (!(off > 1.0)) {
    printf("  FAIL: Sum did not raise the level, so there is nothing to test\n");
    ok = 0;
  }

  if (fabs(on) > 0.5) {
    printf("  FAIL: normaliser left %+0.2f dB of level rise, wanted 0 +/- 0.5\n", on);
    ok = 0;
  }

  if (!(nul < -1.0)) {
    printf("  FAIL: Null was normalised - its output should still be quieter\n");
    ok = 0;
  }

  if (ok) {
    printf("  PASS: %+0.2f dB becomes %+0.2f dB, and Null keeps its %+0.2f dB\n", off, on, nul);
  }

  return ok;
}

static int test_cw_follow(void) {
  printf("  window following the filter; sidetone %d Hz\n", cw_keyer_sidetone_frequency);
  printf("  the passband must land about the zero beat, not a pitch away from it\n");
  int ok = 1;
  ok &= cw_follow_case(modeCWL, "CWL", 100.0, 1);
  ok &= cw_follow_case(modeCWU, "CWU", 100.0, 1);
  /* inside the plain-inversion window, outside the real one */
  ok &= cw_follow_case(modeCWL, "CWL", 900.0, 0);
  ok &= cw_follow_case(modeCWU, "CWU", 900.0, 0);
  return ok;
}

static int test_cw_zero(void) {
  printf("  hand-placed window, signal 100 Hz above the dial\n");
  printf("  target conj(h) = -2.11 dB, +37.75 deg  (CW pitch 800 Hz)\n");
  int ok = 1;
  ok &= cw_zero_case(modeCWL, "CWL");
  ok &= cw_zero_case(modeCWU, "CWU");
  ok &= cw_zero_case(modeUSB, "USB");
  return ok;
}

/* ------------------------------------------------------------------ */
/* 7. standing the combiner down on an empty band                     */
/* ------------------------------------------------------------------ */

/*
 * A signal that stops for a few seconds, and what is left applied.
 *
 * Holding is right while the signal is momentarily not measurable - a
 * fade, a gap between syllables, a key-up - because the weight in force
 * was measured on the real thing. It is wrong once the band is simply
 * empty: the weight then goes on adding the second antenna's noise to the
 * output for as long as nothing is there, which on a lopsided pair is
 * worth up to 12 dB (Findings 36 and 42).
 *
 * Three things are checked, in the order they have to happen:
 *
 *   1. a short gap does NOT stand the combiner down - that is what
 *      DIV_QUIET_DWELL is for, and without it a deep fade on a signal
 *      that never stops takes the weight away;
 *   2. a long one does, and the weight goes to the floor;
 *   3. when the signal comes back the weight is restored in one step
 *      rather than slewed up from zero, so the start of the over is not
 *      spent on the way back.
 */
static int test_standdown(void) {
  const int rate = 48000, nfft = 4096;
  const double hr = 0.62, hi = -0.48;
  const double nz = 0.03;
  rx0.sample_rate = rate;
  rx0.filter_low = 150;
  rx0.filter_high = 2850;
  vfo[0].mode = modeUSB;
  vfo[0].frequency = 14200000;
  vfo[0].ctun_frequency = 14200000;
  vfo[0].offset = 0;
  div_auto_ref = DIV_REF_BAND;
  div_auto_mode = DIV_AUTO_SUM;
  div_auto_follow_filter = 1;
  div_auto_tau = 2.0;
  div_auto_coherence_min = 0.20;
  div_auto_weighting = DIV_WEIGHT_FLAT;
  div_auto_resolution = 12.0;
  div_cos = 1.0;
  div_sin = 0.0;
  div_gain = 0.0;
  div_phase = 0.0;
  srand(71);
  diversity_auto_start();
  double ph = 0.0;
  /*
   * Negative, because the tapped buffer is inverted with respect to RF: a
   * USB filter of +150..+2850 puts the passband at -2850..-150 in the
   * frame these bins are indexed by. See div_shift_to_bin().
   */
  const double tone = -1500.0;
  const double blocks_per_s = (double)rate / (double)nfft;

  /* on, long enough for the loop to converge and for a slot to close */
  for (int b = 0; b < 140; b++) {
    for (int n = 0; n < nfft; n++) {
      ph += 2.0 * M_PI * tone / rate;
      const double s = cos(ph), t = sin(ph);
      diversity_auto_sample(s + nz * frand(), t + nz * frand(),
                            hr * s - hi * t + nz * frand(),
                            hr * t + hi * s + nz * frand());
    }

    settle();
  }

  g_usleep(300000);
  const double g_on = div_gain, p_on = div_phase;
  /*
   * A gap of one second, which is inside DIV_QUIET_DWELL. Nothing may be
   * stood down here.
   */
  const int shortgap = (int)(1.0 * blocks_per_s);

  for (int b = 0; b < shortgap; b++) {
    for (int n = 0; n < nfft; n++) {
      diversity_auto_sample(nz * frand(), nz * frand(), nz * frand(), nz * frand());
    }

    settle();
  }

  g_usleep(200000);
  const int short_ok = !div_auto_standdown;
  const double g_short = div_gain;
  /*
   * And then a real gap between overs. Eight seconds, not longer: past
   * about fifteen the coherence gate lets a no-signal block through - it
   * passes roughly one in twenty by design (Findings 26 and 29) - the
   * loop fits a weight to noise, and what is being measured stops being
   * this change.
   */
  int stood = -1;

  for (int b = 0; b < (int)(8.0 * blocks_per_s); b++) {
    for (int n = 0; n < nfft; n++) {
      diversity_auto_sample(nz * frand(), nz * frand(), nz * frand(), nz * frand());
    }

    settle();
    g_usleep(3000);

    if (stood < 0 && div_auto_standdown) { stood = b + 1; }
  }

  g_usleep(300000);
  const double g_off = div_gain;
  /* the signal returns */
  for (int b = 0; b < 12; b++) {
    for (int n = 0; n < nfft; n++) {
      ph += 2.0 * M_PI * tone / rate;
      const double s = cos(ph), t = sin(ph);
      diversity_auto_sample(s + nz * frand(), t + nz * frand(),
                            hr * s - hi * t + nz * frand(),
                            hr * t + hi * s + nz * frand());
    }

    settle();
  }

  g_usleep(300000);
  const double g_back = div_gain, p_back = div_phase;
  diversity_auto_stop();
  double dp = p_back - p_on;

  while (dp >  180.0) { dp -= 360.0; }

  while (dp < -180.0) { dp += 360.0; }

  const int long_ok  = (stood > 0) && (stood / blocks_per_s < 3.0) && (g_off < -20.0);
  const int back_ok  = (fabs(g_back - g_on) < 1.0) && (fabs(dp) < 10.0);
  printf("  short gap (1.0 s):  standdown=%d, %.2f dB  %s\n",
         div_auto_standdown ? 1 : 0, g_short, short_ok ? "OK" : "FAIL");

  if (stood > 0) {
    printf("  long gap:           stood down after %.1f s, %.2f dB  %s\n",
           stood / blocks_per_s, g_off, long_ok ? "OK" : "FAIL");
  } else {
    printf("  long gap:           never stood down (%.2f dB)  FAIL\n", g_off);
  }

  printf("  signal returns:     %.2f dB %+.1f deg against %.2f dB %+.1f deg  %s\n",
         g_back, p_back, g_on, p_on, back_ok ? "OK" : "FAIL");
  return short_ok && long_ok && back_ok;
}

int main(int argc, char **argv) {
  if (argc > 1) { verbose = 1; }

  memset(&rx0, 0, sizeof(rx0));
  rx0.id = 0;
  memset(vfo, 0, sizeof(vfo));
  vfo[0].frequency = 7100000;
  vfo[0].ctun_frequency = 7100000;
  vfo[0].mode = modeUSB;
  printf("Window reference checks\n\n");
  int a = test_placement();
  printf("\n");
  int b = test_weighting();
  printf("\n");
  int c = test_keyed();
  printf("\n");
  int d = test_cw_zero();
  printf("\n");
  int e = test_cw_follow();
  printf("\n");
  int g = test_normalise();
  printf("\n");
  int h = test_standdown();
  printf("\n%s\n", (a && b && c && d && e && g && h) ? "PASS" : "FAIL");
  return (a && b && c && d && e && g && h) ? 0 : 1;
}
