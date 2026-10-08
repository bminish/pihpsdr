//
// The CW / Morse reference (LC-017, LC-018), driven through the real
// engine with synthetic two-antenna data.
//
// 1. A keyed tone is tracked: the readout and the shaded span sit on it,
//    and the loop's weight is the channel's.
// 2. A steady carrier in the passband, with a different channel, does
//    not take the weight when the keying stops: key detection closes on
//    it. It is there throughout, about 20 dB over the noise, as the
//    heterodyne in Finding AD-50's 143433 was. (A strong carrier that
//    appears out of nothing is accepted until the activity floor has
//    climbed to it, at DIV_CW_ACT_RISE_DB: a 40 dB one for over 3 s.)
// 3. After a key-up gap several averaging times long, a station with a
//    new channel is followed at once. The averages age through the gap,
//    so the first keyed blocks dominate rather than being averaged into
//    the last station's data.
// 4. A notched tone takes no part in the estimate, and one whose notch is
//    moved clear is tracked again.
//
// The weight checked is div_track_gain/phase, the loop's own answer,
// rather than the applied one, which slews towards it.
//
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
#include "ref_slots.h"
#include "radio.h"

// ---- stubs for the piHPSDR globals the engine touches, as test_modes_live
static RECEIVER rx0;
RECEIVER *receiver[8] = { &rx0 };
int receivers = 2;
int diversity_enabled = 1;
int div_auto_mode = DIV_MANUAL;
ADC adc[3];
int radio_is_remote = 0;
int cw_keyer_sidetone_frequency = 800;
double auto_div_cos = 1.0, auto_div_sin = 0.0, auto_div_gain = 0.0, auto_div_phase = 0.0;
double div_norm = 1.0;
int div_indep_att = 0;
struct _vfo vfo[MAX_VFOS];
void t_print(const char *fmt, ...) { (void)fmt; }
const char *getProperty(const char *n) { (void)n; return NULL; }
void setProperty(const char *n, const char *v) { (void)n; (void)v; }
double myatof(const char *s) { return atof(s); }
gboolean diversity_menu_settings_changed(gpointer data) { (void)data; return G_SOURCE_REMOVE; }

#define RATE   192000
#define BLOCK  16384          // one engine block at the default 12 Hz Resolution
#define CHUNK  512

static double ph_tone = 0.0, ph_car = 0.0;

//
// nblocks engine blocks. The keyed tone is at raw frequency f_tone with
// arm 1 = h * arm 0; key is its on/off pattern in blocks (on, off), or
// {1, 0} for key down throughout. A steady carrier at f_car with channel
// g is added when car_amp > 0.
//
static void feed(int nblocks, double f_tone, double amp, double hr, double hi,
                 int on, int off, double f_car, double car_amp, double gr, double gi) {
  for (int b = 0; b < nblocks; b++) {
    const int keyed = (amp > 0.0) && (b % (on + off) < on);

    for (int c = 0; c < BLOCK / CHUNK; c++) {
      for (int n = 0; n < CHUNK; n++) {
        ph_tone += 2.0 * M_PI * f_tone / RATE;
        ph_car  += 2.0 * M_PI * f_car / RATE;
        const double s = keyed ? amp * cos(ph_tone) : 0.0;
        const double t = keyed ? amp * sin(ph_tone) : 0.0;
        const double u = car_amp * cos(ph_car), v = car_amp * sin(ph_car);
        const double n0r = 0.02 * (2.0 * rand() / RAND_MAX - 1.0);
        const double n0i = 0.02 * (2.0 * rand() / RAND_MAX - 1.0);
        const double n1r = 0.02 * (2.0 * rand() / RAND_MAX - 1.0);
        const double n1i = 0.02 * (2.0 * rand() / RAND_MAX - 1.0);
        diversity_auto_sample(s + u + n0r, t + v + n0i,
                              hr * s - hi * t + gr * u - gi * v + n1r,
                              hr * t + hi * s + gr * v + gi * u + n1i);
      }

      g_usleep(300);
    }
  }

  g_usleep(300000);
}

static int fails = 0;

//
// The Sum weight for arm 1 = h * arm 0 with equal noise on both arms is
// conj(h): the gain of h and minus its phase.
//
static int near_channel(double hr, double hi) {
  const double g = 20.0 * log10(sqrt(hr * hr + hi * hi));
  const double p = -atan2(hi, hr) * 180.0 / M_PI;
  double dp = div_track_phase - p;

  while (dp > 180.0)  { dp -= 360.0; }

  while (dp < -180.0) { dp += 360.0; }

  return fabs(div_track_gain - g) < 1.0 && fabs(dp) < 10.0;
}

static void expect(int ok, const char *what) {
  printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);

  if (!ok) { fails++; }
}

int main(void) {
  memset(&rx0, 0, sizeof(rx0));
  rx0.id = 0;
  rx0.sample_rate = RATE;
  rx0.filter_low = 400;
  rx0.filter_high = 1000;       // a 600 Hz CW filter around a 700 Hz note
  memset(vfo, 0, sizeof(vfo));
  vfo[0].frequency = 7020000;
  vfo[0].ctun_frequency = 7020000;
  vfo[0].mode = modeCWU;
  printf("CW / Morse reference\n");
  div_auto_ref = DIV_REF_CW;
  div_auto_mode = DIV_AUTO_SUM;
  div_auto_follow_filter = 1;
  div_auto_tau = 0.5;
  tool_ref_recall(DIV_REF_CW);
  diversity_auto_start();
  srand(42);
  //
  // The engine maps a shifted-frame frequency s to raw -(s + frame_off),
  // and in CWU frame_off is the offset less the sidetone: a 700 Hz note
  // is raw +100 Hz, and the centre of the filter.
  //
  const double note = 700.0, f_tone = -(note - cw_keyer_sidetone_frequency);
  const double h1r = 0.62, h1i = -0.48;
  //
  // 1. Keyed at 4 blocks on, 3 off (about 340 / 260 ms) for 8 s, with the
  //    carrier of 2. already there, 60 Hz below the note.
  //
  const double h2r = -0.30, h2i = 0.90, f_car = f_tone + 60.0, car = 0.0018;
  feed(94, f_tone, 0.5, h1r, h1i, 4, 3, f_car, car, h2r, h2i);
  printf("1. keyed tone: readout %+.1f Hz, span %+.1f..%+.1f, weight %+.2f dB %+.1f deg\n",
         div_auto_carrier, div_auto_occ_lo, div_auto_occ_hi, div_track_gain, div_track_phase);
  expect(div_auto_carrier_valid && fabs(div_auto_carrier - note) < 25.0, "the readout is on the note");
  expect(div_auto_occ_valid && div_auto_occ_lo < note && div_auto_occ_hi > note, "the shaded span covers it");
  expect(near_channel(h1r, h1i), "the weight is the keyed signal's channel");
  //
  // 2. The keyed station stops for 10 s; the carrier carries on. The
  //    weight must stay the keyed station's.
  //
  feed(117, f_tone, 0.0, h1r, h1i, 1, 0, f_car, car, h2r, h2i);
  printf("2. steady carrier: weight %+.2f dB %+.1f deg, holding %d\n",
         div_track_gain, div_track_phase, div_auto_holding);
  expect(near_channel(h1r, h1i), "a steady carrier does not take the weight");
  //
  // 3. Five seconds of nothing, then a station with a third channel,
  //    keyed. After its first two elements (the first 11 blocks: 4 on,
  //    3 off, 4 on) the weight must already be the new channel's.
  //
  const double h3r = 0.20, h3i = 0.95;
  feed(59, f_tone, 0.0, h1r, h1i, 1, 0, 0.0, 0.0, 0.0, 0.0);
  feed(11, f_tone, 0.5, h3r, h3i, 4, 3, 0.0, 0.0, 0.0, 0.0);
  printf("3. new station after a gap: weight %+.2f dB %+.1f deg\n", div_track_gain, div_track_phase);
  expect(near_channel(h3r, h3i), "the averages aged through the gap: the new channel is taken at once");
  //
  // 4. The operator notches the note out. With the only keyed signal in
  //    the passband notched away there is nothing left to track.
  //
  rx0.multi_notch_enable[0] = 1;
  rx0.multi_notch_center[0] = -f_tone;
  rx0.multi_notch_width[0] = 120.0;
  diversity_auto_reset();
  feed(47, f_tone, 0.5, h1r, h1i, 4, 3, 0.0, 0.0, 0.0, 0.0);
  printf("4. notched: holding %d, weight %+.2f dB %+.1f deg\n",
         div_auto_holding, div_track_gain, div_track_phase);
  expect(!near_channel(h1r, h1i), "a notched tone takes no part in the estimate");
  rx0.multi_notch_center[0] = -f_tone + 3000.0;
  feed(47, f_tone, 0.5, h1r, h1i, 4, 3, 0.0, 0.0, 0.0, 0.0);
  printf("   notch moved clear: weight %+.2f dB %+.1f deg\n", div_track_gain, div_track_phase);
  expect(near_channel(h1r, h1i), "tracked again once the notch is moved clear");
  diversity_auto_stop();
  printf("\n%s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
