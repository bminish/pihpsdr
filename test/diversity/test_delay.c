/*
 * Standalone test for Phase 1 Differential Delay Estimation & FIR Compensation.
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
#define DECIM (RATE / RADE_CORR_FS)
#define FRAMES 40

static double frand(void) { return 2.0 * ((double)rand() / RAND_MAX) - 1.0; }

static long gen_delay(float **buf, double delay_sec, double noise) {
  const long nsym = (long)FRAMES * (RADE_CORR_NS + 1);
  const long n8   = (long)FRAMES * RADE_CORR_NMF;
  const long nd   = n8 * DECIM;
  const double rs = (double)RADE_CORR_FS / RADE_CORR_M;
  const int c1 = (int)lround((1500.0 - rs * RADE_CORR_NC / 2.0) / rs);
  static const double barker13[13] = { 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1 };
  double *s8_0 = malloc(sizeof(double) * 2 * n8);
  double *s8_1 = malloc(sizeof(double) * 2 * n8);
  *buf = malloc(sizeof(float) * 4 * nd);
  long idx = 0;

  for (long sym = 0; sym < nsym; sym++) {
    int pilot = ((sym % (RADE_CORR_NS + 1)) == 0);
    double re0[RADE_CORR_M], im0[RADE_CORR_M];
    double re1[RADE_CORR_M], im1[RADE_CORR_M];
    memset(re0, 0, sizeof(re0)); memset(im0, 0, sizeof(im0));
    memset(re1, 0, sizeof(re1)); memset(im1, 0, sizeof(im1));

    for (int c = 0; c < RADE_CORR_NC; c++) {
      double f_c = 750.0 + (double)c * 50.0;
      double dphi = -2.0 * M_PI * f_c * delay_sec;
      double cos_dp = cos(dphi), sin_dp = sin(dphi);

      double w = 2.0 * M_PI * (c1 + c) / RADE_CORR_M;
      double a0 = pilot ? sqrt(2.0) * barker13[c % 13] : ((rand() & 1) ? 1.0 : -1.0);
      double b0 = pilot ? 0.0 : ((rand() & 1) ? 1.0 : -1.0);
      
      double a1 = a0 * cos_dp - b0 * sin_dp;
      double b1 = a0 * sin_dp + b0 * cos_dp;

      for (int n = 0; n < RADE_CORR_M; n++) {
        double th = w * n;
        re0[n] += (a0 * cos(th) - b0 * sin(th)) / RADE_CORR_M;
        im0[n] += (a0 * sin(th) + b0 * cos(th)) / RADE_CORR_M;
        re1[n] += (a1 * cos(th) - b1 * sin(th)) / RADE_CORR_M;
        im1[n] += (a1 * sin(th) + b1 * cos(th)) / RADE_CORR_M;
      }
    }

    for (int n = 0; n < RADE_CORR_NCP; n++) {
      s8_0[2 * idx] = re0[RADE_CORR_M - RADE_CORR_NCP + n];
      s8_0[2 * idx + 1] = im0[RADE_CORR_M - RADE_CORR_NCP + n];
      s8_1[2 * idx] = re1[RADE_CORR_M - RADE_CORR_NCP + n];
      s8_1[2 * idx + 1] = im1[RADE_CORR_M - RADE_CORR_NCP + n];
      idx++;
    }

    for (int n = 0; n < RADE_CORR_M; n++) {
      s8_0[2 * idx] = re0[n];
      s8_0[2 * idx + 1] = im0[n];
      s8_1[2 * idx] = re1[n];
      s8_1[2 * idx + 1] = im1[n];
      idx++;
    }
  }

  for (long i = 0; i < nd; i++) {
    double tr0 = s8_0[2 * (i / DECIM)];
    double ti0 = s8_0[2 * (i / DECIM) + 1];
    double tr1 = s8_1[2 * (i / DECIM)];
    double ti1 = s8_1[2 * (i / DECIM) + 1];

    (*buf)[4 * i + 0] = (float)(tr0 + noise * frand());
    (*buf)[4 * i + 1] = (float)(ti0 + noise * frand());
    (*buf)[4 * i + 2] = (float)(tr1 + noise * frand());
    (*buf)[4 * i + 3] = (float)(ti1 + noise * frand());
  }

  free(s8_0);
  free(s8_1);
  return nd;
}

//
// What matters is not that the filter delays by tau, but that feeding it a
// pair which is tau apart brings the pair together. Those are different
// claims, and only the second one is the job: an implementation that
// delayed the wrong arm would pass the first and double the error.
//
// Checked for both signs, because either arm can be the early one and the
// estimate is not clamped.
//
static double align_error_deg(double rate, double tau_sec, double signal_tau) {
  double freq = 1200.0;
  double err_sum = 0.0;
  int count = 0;

  div_delay_enabled = 1;

  for (int n = 0; n < 4000; n++) {
    double t = (double)n / rate;
    //
    // arm 1 arrives signal_tau later than arm 0.
    //
    double i0 = cos(2.0 * M_PI * freq * t);
    double q0 = sin(2.0 * M_PI * freq * t);
    double i1 = cos(2.0 * M_PI * freq * (t - signal_tau));
    double q1 = sin(2.0 * M_PI * freq * (t - signal_tau));

    div_delay_apply(rate, tau_sec, &i0, &q0, &i1, &q1);

    if (n > 200) {
      //
      // Residual phase of arm 1 relative to arm 0. Zero means aligned.
      //
      double re = i1 * i0 + q1 * q0;
      double im = q1 * i0 - i1 * q0;
      err_sum += fabs(atan2(im, re)) * 180.0 / M_PI;
      count++;
    }
  }

  return err_sum / (double)count;
}

static void test_fractional_delay_filter(void) {
  printf("--- Test 1: Differential Delay Alignment ---\n");
  const double rate = 48000.0;
  const double freq = 1200.0;

  struct { double tau; const char *what; } cases[] = {
    {  100e-6, "arm 1 late  (+100 us)" },
    { -100e-6, "arm 1 early (-100 us)" },
    {   37e-6, "arm 1 late  (+37 us, sub-sample)" },
  };

  int bad = 0;

  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const double tau = cases[i].tau;
    const double uncorrected = fabs(360.0 * freq * tau);
    //
    // Feed the estimate the correlator would produce for this pair.
    //
    const double after = align_error_deg(rate, tau, tau);
    printf("  %-34s uncorrected %6.2f deg -> corrected %6.3f deg\n",
           cases[i].what, uncorrected, after);

    if (after > 0.5) { bad = 1; }
  }

  if (!bad) {
    printf("--> PASS: both signs of differential delay are aligned to <0.5 deg!\n");
  } else {
    printf("--> FAIL: differential delay not corrected!\n");
    exit(1);
  }
}

static void test_rade_delay_estimation(double test_delay_us) {
  printf("\n--- Test 2: RADE V1 Pilot Delay Estimation (Target: %.1f us) ---\n", test_delay_us);
  
  memset(&rx0, 0, sizeof(rx0));
  rx0.sample_rate = RATE;
  rx0.buffer_size = NFFT;
  rx0.filter_low  = -2800;
  rx0.filter_high = -200;

  vfo[0].mode = modeDIGL;
  vfo[0].frequency = 7100000;
  vfo[0].ctun_frequency = 7100000;
  vfo[0].offset = 0;

  div_auto_mode = DIV_AUTO_SUM;
  div_auto_ref  = DIV_REF_RADE_V1;
  div_auto_tau  = 2.0;

  double target_delay_sec = test_delay_us * 1e-6;
  float *buf = NULL;
  long pos = 0;
  long nd = gen_delay(&buf, target_delay_sec, 0.01);

  diversity_auto_start();

  // Feed 100 blocks
  for (int b = 0; b < 100; b++) {
    for (int n = 0; n < NFFT; n++) {
      diversity_auto_sample(buf[4 * pos + 0], buf[4 * pos + 1],
                            buf[4 * pos + 2], buf[4 * pos + 3]);
      pos = (pos + 1) % nd;
    }
    g_usleep(12000);
  }

  g_usleep(100000);

  printf("Lock status: %d, delay_valid: %d, estimated delay: %.2f us (target: %.2f us), slope: %.6f rad/Hz\n",
         rade_corr_locked, rade_corr_delay_valid, rade_corr_delay_sec * 1e6, test_delay_us, rade_corr_phase_slope);

  if (rade_corr_locked && rade_corr_delay_valid) {
    double err_us = fabs(rade_corr_delay_sec * 1e6 - test_delay_us);
    if (err_us < 8.0) {
      printf("--> PASS: Delay estimated within %.2f us error!\n", err_us);
    } else {
      printf("--> FAIL: Delay estimation error %.2f us exceeds threshold!\n", err_us);
      exit(1);
    }
  } else {
    printf("--> FAIL: RADE pilot did not lock or delay invalid!\n");
    exit(1);
  }

  diversity_auto_stop();
  free(buf);
}

int main(int argc, char **argv) {
  printf("====================================================\n");
  printf("   Phase 1 Differential Delay Compensation Test     \n");
  printf("====================================================\n\n");

  test_fractional_delay_filter();
  test_rade_delay_estimation(10.0);  // 10 us delay
  test_rade_delay_estimation(50.0);  // 50 us delay
  test_rade_delay_estimation(100.0); // 100 us delay

  printf("\nALL PHASE 1 DIFFERENTIAL DELAY TESTS PASSED SUCCESSFULLY!\n");
  return 0;
}
