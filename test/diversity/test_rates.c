/*
 * The across-frequency noise floor at the sample rates the radio runs at.
 *
 * The floor is each arm's 8th-12th percentile of the bins outside the RX
 * filter, no further than DIV_NF_HALF_SPAN_HZ from the dial. The Sum
 * weight takes the branch noise ratio from it on the Window and Carrier
 * references (and on CW, from LC-030). These checks are what make it safe to hand over:
 *
 *   1. the measured noise ratio is right at 48, 192 and 1536 kHz, with
 *      strong carriers scattered across the span, and the Sum weight
 *      built on it is near the maximum-ratio optimum at each;
 *   2. at 1536 kHz the floor reads the noise near the dial, not a blend
 *      of the whole +/-614 kHz the DDC delivers;
 *   3. where too few bins are left outside the analysis window the floor
 *      says so, and the loop still produces a weight (the temporal
 *      minimum takes over);
 *   4. operator resets arriving from another thread while blocks run
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
  check_reset_race();
  printf("\n%s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
