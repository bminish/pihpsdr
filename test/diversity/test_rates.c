/*
 * The across-frequency noise floor at the sample rates the radio runs at.
 *
 * The floor is each arm's 8th-12th percentile of the bins outside the RX
 * filter, no further than DIV_NF_HALF_SPAN_HZ from the dial. The Sum
 * weight takes the branch noise ratio from it on the Window, Carrier and
 * CW references. These checks are what make it safe to hand over:
 *
 *   1. the measured noise ratio is right at 48, 192 and 1536 kHz, with
 *      strong carriers scattered across the span, and the Sum weight
 *      built on it is near the maximum-ratio optimum at each;
 *   2. at 1536 kHz the floor reads the noise near the dial, not a blend
 *      of the whole +/-614 kHz the DDC delivers;
 *   3. where too few bins are left outside the analysis window the floor
 *      says so, and the loop still produces a weight (the temporal
 *      minimum takes over);
 *   4. the CW reference at 192 and 1536 kHz; at 1536 kHz, where bins are
 *      23.4 Hz, a 100 Hz filter is an accepted limitation (it holds);
 *   5. operator resets arriving from another thread while blocks run
 *      are performed by the worker between blocks: no other thread ever
 *      sees the floor reset or half rebuilt.
 *
 * Each block is built in the frequency domain - per-bin complex Gaussian
 * noise of any shape, a noise-like wanted signal, carriers - and turned
 * into samples with one inverse FFT the size of the engine's block, so
 * noise shaping is exact and 1536 kHz costs little. The blocks are fed
 * from the engine's first sample, so they line up with its own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <gtk/gtk.h>
#include <fftw3.h>

#include "mode.h"
#include "receiver.h"
#include "vfo.h"
#include "adc.h"
#include "diversity_auto.h"
#include "radio.h"
#include "known_gaps.h"

static RECEIVER rx0;
RECEIVER *receiver[8] = { &rx0 };
int receivers = 2;
int diversity_enabled = 1;
int div_auto_mode = DIV_MANUAL;   /* TEST keeps these in radio.c */
ADC adc[3];
int radio_is_remote = 0;
int cw_keyer_sidetone_frequency = 800;
double auto_div_cos = 1.0, auto_div_sin = 0.0, auto_div_gain = 0.0, auto_div_phase = 0.0;
double div_norm = 1.0;
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
gboolean diversity_menu_settings_changed(gpointer data) { (void)data; return G_SOURCE_REMOVE; }

static int fails = 0;

static void expect(int ok, const char *what) {
  printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);

  if (!ok) { fails++; }
}

/* a standard complex Gaussian, unit power */
static void cgauss(double *re, double *im) {
  double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = (double)rand() / RAND_MAX;
  const double r = sqrt(-log(u1));
  *re = r * cos(2.0 * M_PI * u2);
  *im = r * sin(2.0 * M_PI * u2);
}

/* the channel from arm 0 to arm 1 for the wanted signal, and for the carriers */
static const double hr = 0.62, hi = -0.48;
static const double gr = -0.30, gi = 0.90;

/*
 * One scene, built per block in the frequency domain.
 *
 * sigma1_in / sigma1_out - arm 1's noise amplitude (arm 0's is 1) within
 *                          / beyond split_hz of the dial
 * sig                    - amplitude of the wanted signal, per bin, over
 *                          sig_lo..sig_hi (bin frequency, Hz)
 * ncar, car              - carriers, at random bins anywhere in the span
 *                          but the passband, of this amplitude
 */
struct scene {
  int    rate;
  double sigma1_in, sigma1_out, split_hz;
  double sig, sig_lo, sig_hi;
  int    ncar;
  double car;
};

static int     nfft = 0;
static fftw_complex *fb0 = NULL, *fb1 = NULL;
static fftw_plan     pl0, pl1;
static int     car_bin[64];

static void scene_begin(const struct scene *sc) {
  nfft = (int)lround((double)sc->rate / div_auto_binhz);
  fb0 = fftw_alloc_complex(nfft);
  fb1 = fftw_alloc_complex(nfft);
  pl0 = fftw_plan_dft_1d(nfft, fb0, fb0, FFTW_BACKWARD, FFTW_ESTIMATE);
  pl1 = fftw_plan_dft_1d(nfft, fb1, fb1, FFTW_BACKWARD, FFTW_ESTIMATE);

  for (int i = 0; i < sc->ncar && i < 64; i++) {
    int k;

    do {
      k = rand() % nfft - nfft / 2;
    } while (abs(k) > (int)(0.4 * nfft)
             || (k * (double)sc->rate / nfft > sc->sig_lo - 1500.0
                 && k * (double)sc->rate / nfft < sc->sig_hi + 1500.0));

    car_bin[i] = k;
  }
}

static void scene_end(void) {
  fftw_destroy_plan(pl0);
  fftw_destroy_plan(pl1);
  fftw_free(fb0);
  fftw_free(fb1);
}

static void feed_blocks(const struct scene *sc, int nblocks) {
  const double binhz = (double)sc->rate / nfft;
  const double scale = 1.0 / sqrt((double)nfft);

  for (int b = 0; b < nblocks; b++) {
    for (int k = 0; k < nfft; k++) {
      const int kk = (k < nfft / 2) ? k : k - nfft;
      const double f = kk * binhz;
      double ar, ai, br, bi;
      cgauss(&ar, &ai);
      cgauss(&br, &bi);
      const double s1 = (fabs(f) <= sc->split_hz) ? sc->sigma1_in : sc->sigma1_out;
      double x0r = ar, x0i = ai, x1r = s1 * br, x1i = s1 * bi;

      if (f >= sc->sig_lo && f <= sc->sig_hi) {
        double sr, si;
        cgauss(&sr, &si);
        sr *= sc->sig;
        si *= sc->sig;
        x0r += sr;
        x0i += si;
        x1r += hr * sr - hi * si;
        x1i += hr * si + hi * sr;
      }

      fb0[k][0] = x0r * scale;
      fb0[k][1] = x0i * scale;
      fb1[k][0] = x1r * scale;
      fb1[k][1] = x1i * scale;
    }

    for (int i = 0; i < sc->ncar && i < 64; i++) {
      const int k = (car_bin[i] + nfft) % nfft;
      const double p = 2.0 * M_PI * rand() / RAND_MAX;
      const double cr = sc->car * cos(p) * scale, ci = sc->car * sin(p) * scale;
      fb0[k][0] += cr;
      fb0[k][1] += ci;
      fb1[k][0] += gr * cr - gi * ci;
      fb1[k][1] += gr * ci + gi * cr;
    }

    fftw_execute(pl0);
    fftw_execute(pl1);

    for (int n = 0; n < nfft; n++) {
      diversity_auto_sample(fb0[n][0], fb0[n][1], fb1[n][0], fb1[n][1]);
    }

    g_usleep(4000);
  }

  g_usleep(300000);
}

static void setup(int rate, int mode, int lo, int hi_, int ref) {
  memset(&rx0, 0, sizeof(rx0));
  rx0.id = 0;
  rx0.sample_rate = rate;
  rx0.filter_low = lo;
  rx0.filter_high = hi_;
  memset(vfo, 0, sizeof(vfo));
  vfo[0].mode = mode;
  vfo[0].frequency = 7100000;
  vfo[0].ctun_frequency = 7100000;
  div_auto_ref = ref;
  div_auto_mode = DIV_AUTO_SUM;
  div_auto_follow_filter = 1;
  div_auto_tau = 1.0;
  div_auto_coherence_min = 0.20;
  div_auto_weighting = DIV_WEIGHT_FLAT;
  div_auto_resolution = 12.0;
  auto_div_cos = 1.0;
  auto_div_sin = 0.0;
  auto_div_gain = 0.0;
  auto_div_phase = 0.0;
}

/*
 * Output SINR over the passband for the weight in force, and for the
 * maximum-ratio optimum conj(h) N0/N1, with signal power S per bin and
 * branch noise powers N0, N1.
 */
static double sinr(double wr, double wi, double S, double N0, double N1) {
  const double cr = 1.0 + (wr * hr - wi * hi), ci = wr * hi + wi * hr;
  return 10.0 * log10((cr * cr + ci * ci) * S / (N0 + (wr * wr + wi * wi) * N1));
}

/* ------------------------------------------------------------------ */

static void check_rate(int rate) {
  /* arm 1 10 dB noisier everywhere; 40 carriers 30 dB over arm 0's noise */
  const struct scene sc = { rate, sqrt(10.0), sqrt(10.0), 1e9,
                            sqrt(100.0), 600.0, 2400.0, 40, sqrt(1000.0) };
  setup(rate, modeLSB, -2800, -200, DIV_REF_BAND);
  srand(11);
  diversity_auto_start();
  scene_begin(&sc);
  const double secs = 5.0;
  feed_blocks(&sc, (int)ceil(secs * div_auto_binhz));
  double n0 = 0.0, n1 = 0.0;
  const int valid = diversity_auto_noise_floor(&n0, &n1);
  const double ratio = valid ? 10.0 * log10(n1 / n0) : 0.0;
  const double wr = auto_div_cos, wi = auto_div_sin;
  const double S = sc.sig * sc.sig, N0 = 1.0, N1 = sc.sigma1_in * sc.sigma1_in;
  const double got = sinr(wr, wi, S, N0, N1);
  const double best = sinr(hr * N0 / N1, -hi * N0 / N1, S, N0, N1);
  printf("%7d Hz: nfft %5d, bins %.1f Hz; floor %s, ratio %+.2f dB (truth +10.00); "
         "Sum SINR %+.2f dB, optimum %+.2f\n",
         rate, nfft, div_auto_binhz, valid ? "valid" : "INVALID", ratio, got, best);
  char what[160];
  snprintf(what, sizeof(what), "%d Hz: the noise ratio within 0.5 dB, through 40 carriers", rate);
  expect(valid && fabs(ratio - 10.0) < 0.5, what);
  snprintf(what, sizeof(what), "%d Hz: the Sum weight within 1 dB of the maximum-ratio optimum", rate);
  expect(best - got < 1.0, what);
  scene_end();
  diversity_auto_stop();
}

/*
 * Best's per-arm SNR (div_arm_from_floor()) against a known answer. The
 * floor is a low percentile, about a tenth of the mean noise per bin, and
 * has to be scaled back to the mean before it can stand for the noise in
 * the window (DIV_NF_MEAN_FRAC). Without that a window of bare noise read
 * about 9 dB of SNR on each arm and counted as a valid comparison, and a
 * real difference came out compressed.
 *
 * Scene 1: noise only, equal on both arms - there is nothing to compare,
 * so the readout must be invalid.
 * Scene 2: the signal over 600-2400 Hz of a 2600 Hz window, arm 0's
 * in-window SNR +3 dB; arm 1 hears it through h (-2.11 dB) with noise
 * 6 dB lower, so arm 1 is ahead by 10*log10(|h|^2 / 0.25) = +3.91 dB.
 * The old floor read 3.0 dB here.
 * Scene 3: arm 0 buried (in-window SNR -10 dB), arm 1 20 dB quieter and
 * well clear of its floor. Best must still be able to choose: the
 * readout is valid and arm 1 well ahead (DIV_ARM_BURIED_DB).
 */
static void check_best_snr(void) {
  const struct scene quiet = { 192000, 1.0, 1.0, 1e9, 0.0, 600.0, 2400.0, 0, 0.0 };
  setup(192000, modeLSB, -2800, -200, DIV_REF_BAND);
  srand(21);
  diversity_auto_start();
  scene_begin(&quiet);
  feed_blocks(&quiet, (int)ceil(4.0 * div_auto_binhz));
  const int noise_valid = div_auto_arm_valid;
  printf("noise only:  antenna readout %s (%+.2f dB)\n",
         noise_valid ? "VALID" : "invalid", div_auto_arm_db);
  expect(!noise_valid, "Best: a window of bare noise is not a valid antenna comparison");
  scene_end();
  diversity_auto_stop();
  const double h2 = hr * hr + hi * hi;
  const double truth = 10.0 * log10(h2 / 0.25);
  const double sig = sqrt(2.0 * 2600.0 / 1800.0);
  const struct scene known = { 192000, 0.5, 0.5, 1e9, sig, 600.0, 2400.0, 0, 0.0 };
  setup(192000, modeLSB, -2800, -200, DIV_REF_BAND);
  srand(22);
  diversity_auto_start();
  scene_begin(&known);
  feed_blocks(&known, (int)ceil(5.0 * div_auto_binhz));
  printf("known:       antenna readout %s %+.2f dB (truth %+.2f)\n",
         div_auto_arm_valid ? "valid" : "INVALID", div_auto_arm_db, truth);
  expect(div_auto_arm_valid && fabs(div_auto_arm_db - truth) < 0.5,
         "Best: arm 1's advantage within 0.5 dB of the truth at +3 dB arm SNR");
  scene_end();
  diversity_auto_stop();
  const struct scene buried = { 192000, 0.1, 0.1, 1e9, sqrt(0.1 * 2600.0 / 1800.0), 600.0, 2400.0, 0, 0.0 };
  setup(192000, modeLSB, -2800, -200, DIV_REF_BAND);
  srand(23);
  diversity_auto_start();
  scene_begin(&buried);
  feed_blocks(&buried, (int)ceil(5.0 * div_auto_binhz));
  printf("arm 0 buried: antenna readout %s %+.2f dB (arm 1 at about +7.9 dB, arm 0 -10)\n",
         div_auto_arm_valid ? "valid" : "INVALID", div_auto_arm_db);
  expect(div_auto_arm_valid && div_auto_arm_db > 10.0,
         "Best: with arm 0 buried the readout stays valid and arm 1 leads by over 10 dB");
  scene_end();
  diversity_auto_stop();
}

static void check_span(void) {
  /* 1536 kHz: arm 1 10 dB noisier within 20 kHz of the dial, equal beyond */
  const struct scene sc = { 1536000, sqrt(10.0), 1.0, 20000.0,
                            sqrt(100.0), 600.0, 2400.0, 0, 0.0 };
  setup(1536000, modeLSB, -2800, -200, DIV_REF_BAND);
  srand(12);
  diversity_auto_start();
  scene_begin(&sc);
  feed_blocks(&sc, (int)ceil(5.0 * div_auto_binhz));
  double n0 = 0.0, n1 = 0.0;
  const int valid = diversity_auto_noise_floor(&n0, &n1);
  const double ratio = valid ? 10.0 * log10(n1 / n0) : 0.0;
  printf("1536000 Hz, noise ratio +10 dB within 20 kHz, 0 dB beyond: floor reads %+.2f dB\n", ratio);
  expect(valid && fabs(ratio - 10.0) < 0.5, "the floor reads the noise near the dial, not across +/-614 kHz");
  scene_end();
  diversity_auto_stop();
}

static void check_fallback(void) {
  /* 48 kHz with a hand-placed 40 kHz window: almost nothing left outside */
  const struct scene sc = { 48000, sqrt(10.0), sqrt(10.0), 1e9,
                            sqrt(100.0), 600.0, 2400.0, 0, 0.0 };
  setup(48000, modeLSB, -2800, -200, DIV_REF_BAND);
  div_auto_follow_filter = 0;
  div_auto_centre = 0.0;
  div_auto_width = 40000.0;
  srand(13);
  diversity_auto_start();
  scene_begin(&sc);
  int acted = 0, everv = 0;
  const int nblocks = (int)ceil(5.0 * div_auto_binhz);

  for (int b = 0; b < nblocks; b++) {
    feed_blocks(&sc, 1);
    double n0, n1;

    if (!div_auto_holding) { acted++; }

    if (diversity_auto_noise_floor(&n0, &n1)) { everv++; }
  }

  const double g = 20.0 * log10(hypot(auto_div_cos, auto_div_sin));
  printf("48000 Hz, 40 kHz window: floor valid on %d of %d blocks; loop acted on %d, weight %+.2f dB\n",
         everv, nblocks, acted, g);
  expect(everv == 0, "too few bins outside the window: the floor reports none");
  expect(acted > 0 && isfinite(g) && g > -30.0 && g < 21.0, "and the loop still produces a weight");
  scene_end();
  diversity_auto_stop();
}

/*
 * CW: a keyed tone in time-domain samples, as test_cw does it. At
 * 1536 kHz bins are 23.4 Hz, so a 100 Hz filter is about four of them.
 */
static int check_cw(int rate, int lo, int hi_, int gap) {
  const double note = 700.0;
  setup(rate, modeCWU, lo, hi_, DIV_REF_CW);
  div_auto_tau = 0.5;
  diversity_auto_start();
  srand(14);
  const int block = (int)lround(rate / div_auto_binhz);
  const double f_tone = -(note - cw_keyer_sidetone_frequency);
  double ph = 0.0;
  int acted = 0;
  const int nblocks = (int)ceil(6.0 * div_auto_binhz);

  for (int b = 0; b < nblocks; b++) {
    const int keyed = (b % 9) < 5;

    for (int n = 0; n < block; n++) {
      ph += 2.0 * M_PI * f_tone / rate;
      const double s = keyed ? 0.5 * cos(ph) : 0.0, t = keyed ? 0.5 * sin(ph) : 0.0;
      double a, c, d, e;
      cgauss(&a, &c);
      cgauss(&d, &e);
      diversity_auto_sample(s + 0.002 * a, t + 0.002 * c,
                            hr * s - hi * t + 0.002 * d, hr * t + hi * s + 0.002 * e);
    }

    g_usleep(4000);

    if (!div_auto_holding) { acted++; }
  }

  g_usleep(300000);
  const double g = div_track_gain, p = div_track_phase;
  const double wg = 20.0 * log10(hypot(hr, hi)), wp = -atan2(hi, hr) * 180.0 / M_PI;
  double dp = p - wp;

  while (dp > 180.0)  { dp -= 360.0; }

  while (dp < -180.0) { dp += 360.0; }

  const int near = fabs(g - wg) < 1.0 && fabs(dp) < 10.0;
  printf("%7d Hz CW, %d Hz filter (%.1f Hz bins): acted on %d of %d blocks, "
         "weight %+.2f dB %+.1f deg (channel %+.2f dB %+.1f deg)\n",
         rate, hi_ - lo, div_auto_binhz, acted, nblocks, g, p, wg, wp);
  diversity_auto_stop();
  const int ok = acted > 0 && near;
  char what[120];
  snprintf(what, sizeof(what), "CW at %.1f Hz bins, %d Hz filter: takes the keyed signal's channel",
           div_auto_binhz, hi_ - lo);

  if (gap) {
#ifdef GAP_CW_NARROW_1536
    known_gap(ok, GAP_CW_NARROW_1536);
    return ok;
#endif
  }

  expect(ok, what);
  return ok;
}

/* ------------------------------------------------------------------ */

static volatile int stress_run = 0;

static gpointer stress_thread(gpointer data) {
  (void)data;

  while (stress_run) {
    diversity_auto_reset();
    g_usleep(700);
  }

  return NULL;
}

static void check_reset_race(void) {
  const struct scene sc = { 192000, sqrt(10.0), sqrt(10.0), 1e9,
                            sqrt(100.0), 600.0, 2400.0, 20, sqrt(1000.0) };
  setup(192000, modeLSB, -2800, -200, DIV_REF_BAND);
  srand(15);
  diversity_auto_start();
  scene_begin(&sc);
  stress_run = 1;
  GThread *t = g_thread_new("reset-stress", stress_thread, NULL);
  int bad = 0, seen = 0;
  const int nblocks = (int)ceil(4.0 * div_auto_binhz);

  for (int b = 0; b < nblocks; b++) {
    feed_blocks(&sc, 1);
    double n0, n1;

    if (diversity_auto_noise_floor(&n0, &n1)) {
      seen++;

      if (!(n0 > 0.0) || !(n1 > 0.0) || fabs(10.0 * log10(n1 / n0) - 10.0) > 3.0) { bad++; }
    }
  }

  stress_run = 0;
  g_thread_join(t);
  feed_blocks(&sc, (int)ceil(2.0 * div_auto_binhz));
  double n0 = 0.0, n1 = 0.0;
  const int valid = diversity_auto_noise_floor(&n0, &n1);
  const double ratio = valid ? 10.0 * log10(n1 / n0) : 0.0;
  printf("192000 Hz, resets from another thread every 0.7 ms: floor valid after %d of %d blocks, "
         "%d of those bad; afterwards %+.2f dB\n", seen, nblocks, bad, ratio);
  //
  // The reset is the worker's to perform, between blocks, so another
  // thread never sees it half done: after every block the floor is there
  // again, rebuilt from that block. With the reset done on the calling
  // thread instead - the race this guards - the floor was found zeroed
  // after every one of them.
  //
  expect(seen == nblocks && bad == 0,
         "under a storm of resets from another thread the floor is never seen reset or wrong");
  expect(valid && fabs(ratio - 10.0) < 0.5, "and it is right once they stop");
  scene_end();
  diversity_auto_stop();
}

/*
 * LC-050: with Resolution on Auto the transform follows Averaging, and is
 * rebuilt only when the length Auto wants changes. Window at 192 kHz: 24 Hz
 * (nfft 8192) up to 1 s, 12 Hz (16384) above. At 48 kHz 24 Hz is reachable
 * too (nfft 2048, LC-049).
 */
static void check_auto_bins(void) {
  static const struct { int rate; double tau; int want; } c[] = {
    { 192000, 0.2, 8192 }, { 192000, 1.0, 8192 }, { 192000, 2.0, 16384 }, { 192000, 6.0, 16384 },
    {  48000, 0.2, 2048 }, {  48000, 1.0, 2048 }, {  48000, 2.0,  4096 }, {  48000, 6.0,  4096 },
  };
  setup(192000, modeLSB, -2800, -200, DIV_REF_BAND);
  div_auto_resolution = DIV_RES_AUTO;
  div_auto_tau = 0.2;
  srand(11);
  diversity_auto_start();
  int cur_rate = 192000;
  char what[160];

  for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
    if (c[i].rate != cur_rate) {
      diversity_auto_stop();
      setup(c[i].rate, modeLSB, -2800, -200, DIV_REF_BAND);
      div_auto_resolution = DIV_RES_AUTO;
      div_auto_tau = c[i].tau;
      diversity_auto_start();
      cur_rate = c[i].rate;
    } else {
      div_auto_tau = c[i].tau;
      diversity_auto_retarget();
    }

    const int got = (int)((double)cur_rate / div_auto_binhz + 0.5);
    snprintf(what, sizeof(what), "Auto, %d Hz, Averaging %.1f s: nfft %d (%.2f Hz bins), want %d",
             c[i].rate, c[i].tau, got, div_auto_binhz, c[i].want);
    expect(got == c[i].want, what);
  }

  /* a retarget that changes nothing must not rebuild the transform */
  const double before = div_auto_binhz;
  div_auto_tau = 5.5;
  diversity_auto_retarget();
  expect(div_auto_binhz == before, "Averaging moved inside a tier: the transform is left alone");
  /* a fixed bin width ignores Averaging */
  div_auto_resolution = 12.0;
  diversity_auto_restart();
  const double fixed = div_auto_binhz;
  div_auto_tau = 0.2;
  diversity_auto_retarget();
  expect(div_auto_binhz == fixed, "a fixed 12 Hz is not moved by Averaging");
  diversity_auto_stop();
}

int main(int argc, char **argv) {
  verbose = (argc > 1);
  printf("The across-frequency noise floor at 48, 192 and 1536 kHz\n\n");
  check_rate(48000);
  check_rate(192000);
  check_rate(1536000);
  printf("\n");
  check_span();
  printf("\n");
  check_fallback();
  printf("\n");
  check_cw(192000, 650, 750, 0);
  check_cw(1536000, 500, 900, 0);
  check_cw(1536000, 650, 750, 1);
  printf("\n");
  check_best_snr();
  printf("\n");
  check_reset_race();
  printf("\nAuto bin width follows Averaging (LC-050)\n");
  check_auto_bins();
  printf("\n%s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
