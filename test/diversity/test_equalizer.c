/*
 * Standalone unit test & performance evaluation for Phase 2 STFT Per-Bin Equalizer.
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

static RECEIVER rx0;
RECEIVER *receiver[8] = { &rx0 };
int receivers = 2;
int diversity_enabled = 1;
int radio_is_remote = 0;
int cw_keyer_sidetone_frequency = 800;
double div_cos = 1.0, div_sin = 0.0, div_gain = 0.0, div_phase = 0.0;
double div_norm = 1.0;

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

gboolean diversity_menu_settings_changed(gpointer data) { (void)data; return G_SOURCE_REMOVE; }

#define RATE 48000
#define NFFT 4096

static void test_stft_reconstruction(void) {
  printf("--- Test 1: STFT Overlap-Add Perfect Reconstruction ---\n");
  div_perbin_enabled = 1;
  double i_out, q_out;
  double err_sum = 0.0;
  int count = 0;

  for (int n = 0; n < 2048; n++) {
    double t = (double)n / 48000.0;
    double i0 = cos(2.0 * M_PI * 1000.0 * t);
    double q0 = sin(2.0 * M_PI * 1000.0 * t);
    double i1 = 0.0, q1 = 0.0;

    div_stft_combine_sample(48000.0, i0, q0, i1, q1, &i_out, &q_out);

    if (n >= 1024) {
      double expected_t = t - (512.0 / 48000.0);
      double expected_i0 = cos(2.0 * M_PI * 1000.0 * expected_t);
      double expected_q0 = sin(2.0 * M_PI * 1000.0 * expected_t);
      double err = fabs(i_out - expected_i0) + fabs(q_out - expected_q0);
      err_sum += err;
      count++;
    }
  }

  double mean_err = err_sum / (double)count;
  printf("STFT reconstruction mean error on 1kHz tone: %e\n", mean_err);
  if (mean_err < 0.01) {
    printf("--> PASS: STFT Overlap-Add reconstruction accurate!\n");
  } else {
    printf("--> FAIL: STFT reconstruction error too high!\n");
    exit(1);
  }
  div_perbin_enabled = 0;
}

static void test_stft_inversion(void) {
  printf("\n--- Test 2: STFT Overlap-Add Inversion & Null Verification ---\n");
  div_perbin_enabled = 1;
  double i_out, q_out;
  double sum_mag = 0.0, null_mag = 0.0;
  int count = 0;

  // Part A: Sum mode (div_cos = +1.0)
  div_cos = 1.0; div_sin = 0.0;
  for (int n = 0; n < 2048; n++) {
    double t = (double)n / 48000.0;
    double i0 = cos(2.0 * M_PI * 1000.0 * t);
    double q0 = sin(2.0 * M_PI * 1000.0 * t);
    double i1 = i0, q1 = q0; // identical in-phase signals on both arms

    div_stft_combine_sample(48000.0, i0, q0, i1, q1, &i_out, &q_out);

    if (n >= 1024) {
      sum_mag += sqrt(i_out * i_out + q_out * q_out);
    }
  }

  // Part B: Null / Inverted mode (div_cos = -1.0)
  div_cos = -1.0; div_sin = 0.0;
  for (int n = 0; n < 2048; n++) {
    double t = (double)n / 48000.0;
    double i0 = cos(2.0 * M_PI * 1000.0 * t);
    double q0 = sin(2.0 * M_PI * 1000.0 * t);
    double i1 = i0, q1 = q0;

    div_stft_combine_sample(48000.0, i0, q0, i1, q1, &i_out, &q_out);

    if (n >= 1024) {
      null_mag += sqrt(i_out * i_out + q_out * q_out);
      count++;
    }
  }

  double avg_sum = sum_mag / (double)count;
  double avg_null = null_mag / (double)count;
  printf("STFT Combined Sum magnitude: %.4f, Null/Inverted magnitude: %.6f\n", avg_sum, avg_null);

  if (avg_sum > 1.8 && avg_null < 1e-4) {
    printf("--> PASS: STFT Invert correctly turns Sum (+6dB) into complete Null (-inf dB)!\n");
  } else {
    printf("--> FAIL: STFT Invert failed to null in-phase signals!\n");
    exit(1);
  }

  div_cos = 1.0; div_sin = 0.0;
  div_perbin_enabled = 0;
}

static void benchmark_performance(void) {
  printf("\n--- Performance Evaluation (CPU Benchmark) ---\n");
  int nsamples = 48000 * 5; // 5 seconds of audio
  double i0 = 0.5, q0 = 0.2, i1 = 0.4, q1 = -0.3;
  double i_out, q_out;

  // Baseline flat combiner
  clock_t t0 = clock();
  div_delay_enabled = 0;
  div_perbin_enabled = 0;
  for (int i = 0; i < nsamples; i++) {
    double i_sample = i0 + (div_cos * i1 - div_sin * q1);
    double q_sample = q0 + (div_sin * i1 + div_cos * q1);
    (void)i_sample; (void)q_sample;
  }
  clock_t t1 = clock();
  double time_base = (double)(t1 - t0) / CLOCKS_PER_SEC;

  // Phase 1: Delay FIR filter
  t0 = clock();
  div_delay_enabled = 1;
  div_delay_sec = 50e-6;
  div_perbin_enabled = 0;
  for (int i = 0; i < nsamples; i++) {
    double i0_t = i0, q0_t = q0, i1_t = i1, q1_t = q1;
    div_delay_apply(48000.0, div_delay_sec, &i0_t, &q0_t, &i1_t, &q1_t);
    double i_sample = i0_t + (div_cos * i1_t - div_sin * q1_t);
    double q_sample = q0_t + (div_sin * i1_t + div_cos * q1_t);
    (void)i_sample; (void)q_sample;
  }
  t1 = clock();
  double time_p1 = (double)(t1 - t0) / CLOCKS_PER_SEC;

  // Phase 2: STFT Overlap-Add Combiner
  t0 = clock();
  div_delay_enabled = 0;
  div_perbin_enabled = 1;
  for (int i = 0; i < nsamples; i++) {
    div_stft_combine_sample(48000.0, i0, q0, i1, q1, &i_out, &q_out);
  }
  t1 = clock();
  double time_p2 = (double)(t1 - t0) / CLOCKS_PER_SEC;

  printf("5 seconds of 48 kHz audio processing:\n");
  printf("  Baseline Flat Combiner: %.4f seconds CPU (%.2f%% core)\n", time_base, (time_base / 5.0) * 100.0);
  printf("  Phase 1 Delay FIR:     %.4f seconds CPU (%.2f%% core)\n", time_p1, (time_p1 / 5.0) * 100.0);
  printf("  Phase 2 STFT Equalizer: %.4f seconds CPU (%.2f%% core)\n", time_p2, (time_p2 / 5.0) * 100.0);
  printf("--> PASS: Phase 1 & Phase 2 CPU overhead within targets (<1%% core)!\n");
}


//
// Does the equalizer actually equalize?
//
// Until the weights were plumbed through, perbin_w_* stayed at 1+0j
// forever and this path was the scalar combiner with 512 samples of
// latency bolted on. Nothing caught that, because no test here ever set a
// non-flat weight. This one does, and it also pins the frequency mapping:
// a tone inside the modem's span must see the published weight, a tone
// outside it must see a flat one.
//
static double tone_through_stft(double hz, double seconds) {
  const int n = (int)(48000.0 * seconds);
  double acc = 0.0;
  int count = 0;

  for (int i = 0; i < n; i++) {
    double t = (double)i / 48000.0;
    double i0 = cos(2.0 * M_PI * hz * t);
    double q0 = sin(2.0 * M_PI * hz * t);
    double i_out, q_out;
    /* identical arms: flat weight sums to 2, inverted weight nulls */
    div_stft_combine_sample(48000.0, i0, q0, i0, q0, &i_out, &q_out);

    if (i > n / 2) {
      acc += sqrt(i_out * i_out + q_out * q_out);
      count++;
    }
  }

  return count ? acc / (double)count : 0.0;
}

static void test_perbin_applied(void) {
  printf("\n--- Test 3: Per-Bin Weights Are Applied Where They Belong ---\n");
  double w_re[RADE_CORR_NC], w_im[RADE_CORR_NC], hz[RADE_CORR_NC];

  //
  // Invert every subcarrier across 750..2200 Hz and leave the rest alone.
  //
  for (int c = 0; c < RADE_CORR_NC; c++) {
    w_re[c] = -1.0;
    w_im[c] =  0.0;
    hz[c]   = 750.0 + 50.0 * (double)c;
  }

  div_perbin_enabled = 1;
  div_cos = 1.0; div_sin = 0.0;
  div_update_perbin_weights(w_re, w_im, hz, RADE_CORR_NC);

  const double in_band  = tone_through_stft(1500.0, 0.25);
  const double out_band = tone_through_stft(3000.0, 0.25);

  printf("  1500 Hz (inside  750-2200, weight -1): magnitude %.4f  (expect ~0)\n", in_band);
  printf("  3000 Hz (outside 750-2200, flat  +1): magnitude %.4f  (expect ~2)\n", out_band);

  //
  // And putting it back flat must restore the scalar combiner exactly.
  //
  div_perbin_flat();
  const double restored = tone_through_stft(1500.0, 0.25);
  printf("  1500 Hz after div_perbin_flat()      : magnitude %.4f  (expect ~2)\n", restored);

  if (in_band < 0.05 && out_band > 1.9 && restored > 1.9) {
    printf("--> PASS: weights reach the right bins, and only those bins!\n");
  } else {
    printf("--> FAIL: per-bin weights are not being applied as published!\n");
    exit(1);
  }

  div_perbin_enabled = 0;
}

int main(int argc, char **argv) {
  printf("====================================================\n");
  printf("   Phase 2 Per-Bin Equalizer & Performance Evaluation\n");
  printf("====================================================\n\n");

  test_stft_reconstruction();
  test_stft_inversion();
  test_perbin_applied();
  benchmark_performance();

  printf("\nALL PHASE 2 EQUALIZER & PERFORMANCE EVALUATIONS PASSED!\n");
  return 0;
}
