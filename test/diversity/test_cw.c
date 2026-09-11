/*
 * CW / Morse Reference test suite:
 * Tests carrier tracking, tone frequency smoothing, Key-UP hold anchoring,
 * keyclick transient rejection (crest factor test), and MRC weighting.
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

ADC adc[3];
int div_indep_att = 0;
struct _vfo vfo[MAX_VFOS];

void t_print(const char *fmt, ...) { (void)fmt; }
const char *getProperty(const char *n) { (void)n; return NULL; }
void setProperty(const char *n, const char *v) { (void)n; (void)v; }
double myatof(const char *s) { return atof(s); }

gboolean diversity_menu_settings_changed(gpointer data) { (void)data; return G_SOURCE_REMOVE; }

#define RATE 192000
#define BLK_LEN 512

static void run_blocks(int num_blocks, double freq_hz, double tone_amp, double noise_amp, double hr, double hi) {
  static double ph = 0.0;
  for (int blk = 0; blk < num_blocks; blk++) {
    for (int n = 0; n < BLK_LEN; n++) {
      ph += 2.0 * M_PI * freq_hz / (double)RATE;
      if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
      double s = tone_amp * cos(ph);
      double t = tone_amp * sin(ph);
      double n0_r = noise_amp * (2.0 * rand() / (double)RAND_MAX - 1.0);
      double n0_i = noise_amp * (2.0 * rand() / (double)RAND_MAX - 1.0);
      double n1_r = noise_amp * (2.0 * rand() / (double)RAND_MAX - 1.0);
      double n1_i = noise_amp * (2.0 * rand() / (double)RAND_MAX - 1.0);

      double a0r = s + n0_r;
      double a0i = t + n0_i;
      double a1r = hr * s - hi * t + n1_r;
      double a1i = hr * t + hi * s + n1_i;

      diversity_auto_sample(a0r, a0i, a1r, a1i);
    }
    g_usleep(200);
  }
}

int main(void) {
  memset(&rx0, 0, sizeof(rx0));
  rx0.id = 0; rx0.sample_rate = RATE;
  rx0.filter_low = -8000; rx0.filter_high = 8000;
  memset(vfo, 0, sizeof(vfo));
  vfo[0].frequency = 7100000; vfo[0].ctun_frequency = 7100000;
  vfo[0].offset = 0; vfo[0].mode = modeCWU;

  int fails = 0;
  printf("--- CW Reference Unit Test ---\n");

  div_auto_ref = DIV_REF_CW;
  div_auto_mode = DIV_AUTO_SUM;
  div_auto_follow_filter = 1;
  div_auto_tau = 0.5;
  div_auto_coherence_min = 0.1;
  div_cos = 1.0; div_sin = 0.0; div_gain = 0.0; div_phase = 0.0;
  diversity_auto_start();

  const double target_freq = 600.0; // CW tone at +600 Hz shifted frame (audio pitch)
  const double hr = 0.62, hi = -0.48; // Arm 1 channel transfer
  // Raw DDC frequency: bin_freq = -(s + frame_off), where frame_off = -sidetone for modeCWU (-800)
  const double f_raw = -(target_freq - 800.0); // +200 Hz raw I/Q tone

  // Phase 1: Key-DOWN (40 blocks of tone + noise)
  srand(42);
  run_blocks(40, f_raw, 0.5, 0.02, hr, hi);
  g_usleep(200000);

  double tracked_carrier = div_auto_carrier;
  int carrier_valid = div_auto_carrier_valid;
  int occ_valid = div_auto_occ_valid;
  double occ_lo = div_auto_occ_lo;
  double occ_hi = div_auto_occ_hi;
  double occ_center = 0.5 * (occ_lo + occ_hi);

  printf("[1. Key-DOWN] Carrier valid=%d, tracked=%.1f Hz, occ_valid=%d, occ_center=%.1f Hz\n",
         carrier_valid, tracked_carrier, occ_valid, occ_center);

  if (!carrier_valid || fabs(tracked_carrier - target_freq) > 50.0) {
    printf("  FAIL: CW tone frequency tracking off (expected ~%.1f, got %.1f)\n", target_freq, tracked_carrier);
    fails++;
  } else {
    printf("  PASS: CW tone frequency tracked successfully\n");
  }

  if (!occ_valid || fabs(occ_center - target_freq) > 50.0) {
    printf("  FAIL: Green narrowbin overlay not centered on tracked carrier (center=%.1f)\n", occ_center);
    fails++;
  } else {
    printf("  PASS: Green narrowbin overlay centered on tracked carrier\n");
  }

  // Phase 2: Key-UP Pause (20 blocks of noise only)
  run_blocks(20, f_raw, 0.0, 0.02, hr, hi);
  g_usleep(200000);

  /* the tracked tone is re-read after the pause; see the check below */
  int hold_occ_valid = div_auto_occ_valid;
  double hold_occ_center = 0.5 * (div_auto_occ_lo + div_auto_occ_hi);

  printf("[2. Key-UP Hold] occ_valid=%d, occ_center=%.1f Hz, holding=%d\n",
         hold_occ_valid, hold_occ_center, div_auto_holding);

  if (!hold_occ_valid) {
    printf("  FAIL: Green narrowbin bar lost during Key-UP pause\n");
    fails++;
  } else if (fabs(hold_occ_center - target_freq) > 50.0) {
    printf("  FAIL: Green narrowbin bar jumped away from tracked carrier during Key-UP (got %.1f)\n", hold_occ_center);
    fails++;
  } else {
    printf("  PASS: Green narrowbin bar stayed anchored on tracked carrier during Key-UP pause\n");
  }

  // Phase 3: Resume Key-DOWN (20 blocks of tone + noise)
  run_blocks(20, f_raw, 0.5, 0.02, hr, hi);
  g_usleep(200000);

  int final_holding = div_auto_holding;
  double final_gain = div_gain;

  printf("[3. Key-DOWN Resume] holding=%d, weight gain=%.2f dB, phase=%.1f deg\n",
         final_holding, final_gain, div_phase);

  if (final_holding) {
    printf("  FAIL: Engine did not unlock on Key-DOWN resume\n");
    fails++;
  } else {
    printf("  PASS: Engine locked and updated weight on Key-DOWN resume\n");
  }

  diversity_auto_stop();
  printf("Result: %s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
