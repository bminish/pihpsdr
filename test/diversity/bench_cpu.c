/*
 * CPU cost of the diversity analysis, measured rather than estimated.
 *
 * Drives the real engine with synthetic two-antenna data, for each
 * reference at each sample rate the radio can run at, and reports per
 * analysis block:
 *
 *   worker  - CPU on the analysis thread (process CPU less the feeder's
 *             own thread CPU): the FFTs, the reference's solve, and on
 *             RADE V1 the decimator and the pilot correlator.
 *   feeder  - CPU in diversity_auto_sample(), which the radio runs on the
 *             protocol receive thread once per sample pair.
 *
 * each also as a share of one core over the real block period, nfft /
 * rate: 85.3 ms up to 768 kHz, 42.7 ms at 1536 kHz (the transform is
 * capped at 65536).
 *
 *   ./bench_cpu            paced as the radio: one block per block period.
 *                          On a governor that clocks down between bursts
 *                          this is the figure that predicts the radio.
 *   ./bench_cpu --hot      a block every 15 ms: the core stays at full
 *                          clock; the best case.
 *   ./bench_cpu --quick    48, 192 and 1536 kHz only.
 *   ./bench_cpu --transitions   only the start / stop / restart timings.
 *
 * Lines starting "TSV" carry the same figures for analysis.
 *
 * Samples are generated once, before the clock starts, and then replayed.
 * A first version synthesised them inside the timed loop and spent most
 * of its time in cos() - it was measuring the harness, not the engine.
 *
 * RADE V1 is reported in two states. Searching for a pilot is the most
 * expensive thing the engine does; tracking one is much cheaper. Both are
 * scored on a single-sideband passband, which is the ordinary case: the
 * search follows the passband, one pilot bank unless it straddles the
 * carrier. An AM passband, which searched both banks, would be a real
 * outlier and was dropped from the rows on 2026-10-08. The search case is fed
 * noise so it can never lock; the locked case is given enough blocks to
 * acquire first, and says whether it did.
 *
 * The transform size is the engine's own: Auto bin width (LC-052, LC-053)
 * asks for 24 Hz bins on Window and Carrier at an Averaging time of 1 s or
 * less, and 12 Hz otherwise, and the size is the smallest power of two
 * from 2048 up to 65536 that gets there. The bench works the same size out
 * from diversity_auto_bin_policy(), starts the engine, and checks the
 * achieved width (div_auto_binhz) against it, so a policy that changes
 * cannot leave this file measuring something else: a mismatch shows as
 * NFFT-MISMATCH in the state column.
 *
 * The first six reference names are the ones the 2026-10-02 Pi 5 run used,
 * with the same settings, so pi5/compare_pi5.py can line the new figures up
 * against it. The rest are new: Auto's 24 Hz tier, the other objectives
 * (Level output rides on Sum and Best, so Sum against Null is its cost),
 * and the operator's notches.
 *
 * "drops" counts analysis blocks the worker could not keep up with. Any
 * drop means the worker needed more than a block period: that row's
 * figures are for the blocks it did process, and the rate is not
 * sustainable on this machine in that configuration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <gtk/gtk.h>

#include "mode.h"
#include "receiver.h"
#include "vfo.h"
#include "adc.h"
#include "diversity_auto.h"
#include "rade_correlator.h"
#include "radio.h"

static RECEIVER rx0;
RECEIVER *receiver[8] = { &rx0 };
int receivers = 2;
int diversity_enabled = 1;
int div_auto_mode = DIV_MANUAL;   /* TEST keeps these in radio.c */
ADC adc[3];
int radio_is_remote = 0;
int cw_keyer_sidetone_frequency = 800;
double auto_div_cos = 1.0, auto_div_sin = 0.0, auto_div_gain = 0.0, auto_div_phase = 0.0;
double div_norm = 1.0;   /* the output-level normaliser; receiver.c applies it */
//
// The engine reads the two step attenuators as part of its analysis
// context, so a change of either restarts the statistics.
//
ADC adc[3];
int div_indep_att = 0;
struct _vfo vfo[MAX_VFOS];
//
// Quiet, except that the worker's "dropped %d analysis block(s)" is
// counted: it is how a worker that cannot keep up shows itself.
//
static volatile int drops = 0;
void t_print(const char *fmt, ...) {
  if (strstr(fmt, "dropped")) {
    va_list ap;
    va_start(ap, fmt);
    (void)va_arg(ap, const char *);
    drops += va_arg(ap, int);
    va_end(ap);
  }
}
const char *getProperty(const char *n) { (void)n; return NULL; }
void setProperty(const char *n, const char *v) { (void)n; (void)v; }
double myatof(const char *s) { return atof(s); }

/*
 * The engine tells the menu when a mode change swapped one block of modal
 * settings for another. There is no menu here.
 */
gboolean diversity_menu_settings_changed(gpointer data) { (void)data; return G_SOURCE_REMOVE; }

static double cpu_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static double thread_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static double frand(void) { return 2.0 * ((double)rand() / RAND_MAX) - 1.0; }

#define BENCH_MIN_NFFT 2048  /* diversity_auto.c DIV_MIN_NFFT */
#define BENCH_MAX_NFFT 65536 /* diversity_auto.c DIV_MAX_NFFT */
#define BENCH_FRAMES 40      /* modem frames held in the replay buffer */

/*
 * A continuous RADE-like stream: pilot symbol then RADE_CORR_NS data
 * symbols, repeating, at the DDC rate.
 *
 * The buffer is a whole number of modem frames long so that replaying it
 * end to end is seamless and the correlator's pilot timing stays valid
 * across the wrap. Without that the correlator loses lock once per wrap
 * and the "locked" measurement never happens.
 *
 * signal == 0 gives noise only, which keeps RADE V1 searching for ever.
 */
static long gen_stream(float **buf, int decim, int signal) {
  const long nsym = (long)BENCH_FRAMES * (RADE_CORR_NS + 1);
  const long n8   = (long)BENCH_FRAMES * RADE_CORR_NMF;
  const long nd   = n8 * decim;
  const double hr = 0.62, hi = -0.48;
  const double rs = (double)RADE_CORR_FS / RADE_CORR_M;
  const int c1 = (int)lround((1500.0 - rs * RADE_CORR_NC / 2.0) / rs);
  static const double barker13[13] = { 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1 };
  double *s8 = malloc(sizeof(double) * 2 * n8);
  *buf = malloc(sizeof(float) * 4 * nd);
  long idx = 0;

  for (long sym = 0; sym < nsym; sym++) {
    int pilot = ((sym % (RADE_CORR_NS + 1)) == 0);
    double re[RADE_CORR_M], im[RADE_CORR_M];
    memset(re, 0, sizeof(re));
    memset(im, 0, sizeof(im));

    for (int c = 0; c < RADE_CORR_NC; c++) {
      double w = 2.0 * M_PI * (c1 + c) / RADE_CORR_M;
      double a = pilot ? sqrt(2.0) * barker13[c % 13] : ((rand() & 1) ? 1.0 : -1.0);
      double b = pilot ? 0.0 : ((rand() & 1) ? 1.0 : -1.0);

      for (int n = 0; n < RADE_CORR_M; n++) {
        double th = w * n;
        re[n] += (a * cos(th) - b * sin(th)) / RADE_CORR_M;
        im[n] += (a * sin(th) + b * cos(th)) / RADE_CORR_M;
      }
    }

    /* cyclic prefix, then the symbol */
    for (int n = 0; n < RADE_CORR_NCP; n++) {
      s8[2 * idx] = re[RADE_CORR_M - RADE_CORR_NCP + n];
      s8[2 * idx + 1] = im[RADE_CORR_M - RADE_CORR_NCP + n];
      idx++;
    }

    for (int n = 0; n < RADE_CORR_M; n++) {
      s8[2 * idx] = re[n];
      s8[2 * idx + 1] = im[n];
      idx++;
    }
  }

  for (long i = 0; i < nd; i++) {
    double sr = signal ? s8[2 * (i / decim)]     : 0.0;
    double si = signal ? s8[2 * (i / decim) + 1] : 0.0;
    (*buf)[4 * i + 0] = (float)(sr + 0.002 * frand());
    (*buf)[4 * i + 1] = (float)(si + 0.002 * frand());
    (*buf)[4 * i + 2] = (float)(hr * sr - hi * si + 0.002 * frand());
    (*buf)[4 * i + 3] = (float)(hr * si + hi * sr + 0.002 * frand());
  }

  free(s8);
  return nd;
}

static int    hot = 0;          /* --hot: a block every 15 ms */
static double feeder_cpu = 0.0; /* thread CPU spent in diversity_auto_sample() */

//
// One block of samples, then wait. Paced as the radio, the wait makes the
// block period up: samples arrive in a burst here rather than spread out,
// but the worker sees one block per period either way, which is what
// decides whether the core clocks down between blocks. --hot waits 15 ms,
// fast enough to finish quickly while leaving the worker headroom: feeding
// flat out overruns the queue, and a dropped block makes RADE V1
// re-acquire. CPU time is what is measured, so the wait does not enter it.
//
static void feed(const float *buf, long nd, long *pos, int nfft, double period) {
  const double t0 = now();
  const double c0 = thread_seconds();

  for (int n = 0; n < nfft; n++) {
    long i = *pos;
    diversity_auto_sample(buf[4 * i + 0], buf[4 * i + 1],
                          buf[4 * i + 2], buf[4 * i + 3]);
    *pos = (i + 1) % nd;
  }

  feeder_cpu += thread_seconds() - c0;
  double wait = hot ? 0.015 : period - (now() - t0);

  if (wait > 0.0) { g_usleep((gulong)(1e6 * wait)); }
}

int main(int argc, char **argv) {
  int quick = 0, only_tr = 0;

  for (int a = 1; a < argc; a++) {
    if (!strcmp(argv[a], "--hot")) { hot = 1; }
    else if (!strcmp(argv[a], "--quick")) { quick = 1; }
    else if (!strcmp(argv[a], "--transitions")) { only_tr = 1; }
    else { fprintf(stderr, "usage: %s [--hot] [--quick] [--transitions]\n", argv[0]); return 2; }
  }

  const int all_rates[] = { 48000, 96000, 192000, 384000, 768000, 1536000 };
  const int quick_rates[] = { 48000, 192000, 1536000 };
  const int *rates = quick ? quick_rates : all_rates;
  const int nrates = only_tr ? 0 : (quick ? 3 : 6);
  //
  // The filter matters for the RADE modes: the pilot search only looks at
  // the bank the operator's passband names, so an SSB passband halves it.
  // Both are reported, because AM/SAM/FM leave the passband straddling
  // the carrier, which says nothing and costs the full search.
  //
  struct {
    const char *name; int ref; int mode; int carrier; int settle; int flo; int fhi;
    int obj; double tau; int notches;
  } modes[] = {
    { "Window",           DIV_REF_BAND,       modeAM,  1,  4, -8000, 8000, DIV_AUTO_SUM,  2.0, 0 },
    { "Carrier",          DIV_REF_CARRIER,    modeAM,  1,  4, -8000, 8000, DIV_AUTO_SUM,  2.0, 0 },
    { "Digital I/Q",      DIV_REF_DIGITAL_IQ, modeAM,  1,  4,  -2800, -200, DIV_AUTO_SUM, 2.0, 0 },
    { "CW",               DIV_REF_CW,         modeCWU, 1,  4,    400, 1000, DIV_AUTO_SUM, 2.0, 0 },
    //
    // The generator's modem lands below the tuned frequency in the raw
    // sample buffer, whose spectrum is the mirror of the dial's (test_rade
    // conjugates to put it above). So the lower passband is the one that
    // finds it. The search row uses the same passband, so it searches the
    // same bank, but is fed noise.
    //
    { "RADE V1 (search)", DIV_REF_RADE_V1,    modeDIGL, 0,  4, -2800, -200, DIV_AUTO_SUM, 2.0, 0 },
    //
    // Declaring lock needs a grid detection - at 8, 16 or 32 passes of one
    // modem frame, so 1 to 3.8 s - followed by RADE_PROBATION frames of
    // confirmation, about a second. This settle is generous.
    //
    { "RADE V1 (locked)", DIV_REF_RADE_V1,    modeDIGL, 1, 80, -2800, -200, DIV_AUTO_SUM, 2.0, 0 },
    //
    // New since the 2026-10-02 run.
    //
    { "Window 24Hz tier", DIV_REF_BAND,       modeAM,  1,  4, -8000, 8000, DIV_AUTO_SUM,  0.5, 0 },
    { "Carrier 24Hz tier", DIV_REF_CARRIER,   modeAM,  1,  4, -8000, 8000, DIV_AUTO_SUM,  0.5, 0 },
    { "Window Best",      DIV_REF_BAND,       modeAM,  1,  4, -8000, 8000, DIV_AUTO_BEST, 2.0, 0 },
    { "Window Null",      DIV_REF_BAND,       modeAM,  1,  4, -8000, 8000, DIV_AUTO_NULL, 2.0, 0 },
    { "Window 3 notches", DIV_REF_BAND,       modeAM,  1,  4, -8000, 8000, DIV_AUTO_SUM,  2.0, 3 },
    { "CW Best",          DIV_REF_CW,         modeCWU, 1,  4,    400, 1000, DIV_AUTO_BEST, 2.0, 0 },
  };
  memset(&rx0, 0, sizeof(rx0));
  rx0.id = 0;
  memset(vfo, 0, sizeof(vfo));
  vfo[0].frequency = 7100000;
  vfo[0].ctun_frequency = 7100000;
  //
  // Numbers without a machine attached to them are not much use: this is
  // scalar double-precision work, so a Pi is several times slower than a
  // desktop and the percentages move accordingly.
  //
  {
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];

    if (f) {
      while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "model name", 10) || !strncmp(line, "Model", 5)) {
          printf("host: %s", strchr(line, ':') ? strchr(line, ':') + 2 : line);
          break;
        }
      }

      fclose(f);
    }
  }
  printf("Added CPU per analysis block, %s. Share of one core over the block period.\n\n",
         hot ? "hot (a block every 15 ms)" : "paced as the radio (a block per block period)");
  printf("%-18s %6s %6s %7s %10s %8s %10s %8s %6s\n", "reference", "rate", "nfft", "period",
         "worker ms", "% core", "feeder ms", "% core", "drops");
  printf("TSV\tpacing\treference\trate\tnfft\tperiod_ms\tworker_ms\tworker_pct\tfeeder_ms\tfeeder_pct\tdrops\tstate\n");

  for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
    for (int r = 0; r < nrates; r++) {
      //
      // Auto's own choice of bin width, and the transform size it leads to.
      // BENCH_MIN_NFFT / BENCH_MAX_NFFT are diversity_auto.c's DIV_MIN_NFFT /
      // DIV_MAX_NFFT, which are private to it; the check after the engine
      // starts is what catches them drifting apart.
      //
      div_auto_resolution = DIV_RES_AUTO;
      div_auto_tau = modes[m].tau;
      const double target = diversity_auto_bin_policy(modes[m].ref, modes[m].tau);
      int nfft = BENCH_MIN_NFFT;

      while (nfft < BENCH_MAX_NFFT && (double)rates[r] / nfft > target) { nfft <<= 1; }

      const double period = (double)nfft / rates[r];
      rx0.sample_rate = rates[r];
      rx0.filter_low = modes[m].flo;
      rx0.filter_high = modes[m].fhi;
      vfo[0].mode = modes[m].mode;
      div_auto_ref = modes[m].ref;
      div_auto_mode = modes[m].obj;
      div_auto_follow_filter = 1;
      div_auto_coherence_min = 0.1;
      //
      // The operator's notches: three, across the passband, away from the
      // wanted signal so the loop is not disturbed, each wide enough to
      // take out many bins.
      //
      for (int i = 0; i < 3; i++) {
        rx0.multi_notch_enable[i] = (i < modes[m].notches);
        rx0.multi_notch_center[i] = -2200.0 - 900.0 * i;
        rx0.multi_notch_width[i]  = 300.0;
      }

      float *buf = NULL;
      long pos = 0;
      srand(4);
      long nd = gen_stream(&buf, rates[r] / RADE_CORR_FS, modes[m].carrier);
      diversity_auto_start();
      const double achieved = (double)rates[r] / nfft;
      const int nfft_ok = fabs(div_auto_binhz - achieved) < 0.01 * achieved;

      for (int b = 0; b < modes[m].settle; b++) { feed(buf, nd, &pos, nfft, period); }

      const int nblk = 40;
      g_usleep(200000);
      drops = 0;
      feeder_cpu = 0.0;
      double t0 = cpu_seconds();

      for (int b = 0; b < nblk; b++) { feed(buf, nd, &pos, nfft, period); }

      g_usleep(400000);   /* let the worker finish the last block */
      const double cpu = cpu_seconds() - t0;
      const int locked = rade_corr_locked;
      const int ndrop = drops;
      diversity_auto_stop();
      free(buf);
      const double fms = 1000.0 * feeder_cpu / nblk;
      const double wms = 1000.0 * (cpu - feeder_cpu) / (nblk - ndrop > 0 ? nblk - ndrop : 1);
      const double pms = 1000.0 * period;
      const char *state = nfft_ok ? "-" : "NFFT-MISMATCH";

      if (!nfft_ok) {
        // keep the mismatch visible whatever else the row is
      } else if (modes[m].ref == DIV_REF_RADE_V1) {
        state = modes[m].carrier ? (locked ? "locked" : "DID-NOT-LOCK")
                : (locked ? "UNEXPECTED-LOCK" : "searching");
      }

      printf("%-18s %5dk %6d %6.1fms %10.2f %7.2f%% %10.3f %7.2f%% %6d  %s\n",
             r ? "" : modes[m].name, rates[r] / 1000, nfft, pms, wms, 100.0 * wms / pms,
             fms, 100.0 * fms / pms, ndrop, state);
      printf("TSV\t%s\t%s\t%d\t%d\t%.2f\t%.4f\t%.3f\t%.4f\t%.3f\t%d\t%s\n",
             hot ? "hot" : "paced", modes[m].name, rates[r], nfft, pms, wms,
             100.0 * wms / pms, fms, 100.0 * fms / pms, ndrop, state);
      fflush(stdout);
    }
  }

  //
  // Transitions: how long the engine takes to start, stop and restart, in
  // wall-clock milliseconds, because these calls are made from the menu and
  // from rxtx() and hold the caller until they return. Switching diversity
  // on or off, changing the reference, and an Averaging move across 1 s
  // (Auto's 24 -> 12 Hz tier, which changes the transform) all come down to
  // them. The first start of the process allocates the buffers at the
  // largest size and is reported on its own, as "first"; the rest are the
  // steady-state ones, at every transform size Auto can pick.
  //
  printf("\nTransitions, wall-clock ms (the caller waits for these)\n");
  printf("%-8s %6s %6s %6s %10s %10s %12s\n", "when", "rate", "tau", "nfft", "start ms", "stop ms", "restart ms");
  printf("TSV\ttransition\twhen\trate\ttau\tnfft\tstart_ms\tstop_ms\trestart_ms\n");
  {
    const struct { int rate; double tau; } tr[] = {
      { 1536000, 2.0 }, { 48000, 0.5 }, { 48000, 2.0 }, { 96000, 0.5 }, { 96000, 2.0 },
      { 192000, 0.5 }, { 192000, 2.0 }, { 384000, 0.5 }, { 384000, 2.0 },
      { 768000, 0.5 }, { 768000, 2.0 }, { 1536000, 0.5 },
    };

    for (unsigned t = 0; t < sizeof(tr) / sizeof(tr[0]); t++) {
      rx0.sample_rate = tr[t].rate;
      rx0.filter_low = -8000;
      rx0.filter_high = 8000;
      vfo[0].mode = modeAM;
      div_auto_ref = DIV_REF_BAND;
      div_auto_mode = DIV_AUTO_SUM;
      div_auto_resolution = DIV_RES_AUTO;
      div_auto_tau = tr[t].tau;
      const double target = diversity_auto_bin_policy(DIV_REF_BAND, tr[t].tau);
      int nfft = BENCH_MIN_NFFT;

      while (nfft < BENCH_MAX_NFFT && (double)tr[t].rate / nfft > target) { nfft <<= 1; }

      double t0 = now();
      diversity_auto_start();
      const double start_ms = 1000.0 * (now() - t0);

      for (int n = 0; n < 2 * nfft; n++) {
        diversity_auto_sample(frand(), frand(), frand(), frand());
      }

      g_usleep(300000);
      t0 = now();
      diversity_auto_restart();
      const double restart_ms = 1000.0 * (now() - t0);
      g_usleep(100000);
      t0 = now();
      diversity_auto_stop();
      const double stop_ms = 1000.0 * (now() - t0);
      const char *when = (t == 0) ? "first" : "steady";
      printf("%-8s %5dk %6.1f %6d %10.2f %10.2f %12.2f\n", when, tr[t].rate / 1000, tr[t].tau, nfft,
             start_ms, stop_ms, restart_ms);
      printf("TSV\ttransition\t%s\t%d\t%.1f\t%d\t%.3f\t%.3f\t%.3f\n", when, tr[t].rate, tr[t].tau, nfft,
             start_ms, stop_ms, restart_ms);
      fflush(stdout);
    }
  }

  return 0;
}
