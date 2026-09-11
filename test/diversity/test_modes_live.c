//
// Drive the real diversity_auto engine with synthetic two-antenna data
// and check that every reference mode actually produces a weight.
//
// This exists because a mode that silently never starts is the failure
// this code keeps producing: the carrier reference once sat on
// "searching" for ever because its bin range was computed before the
// transform that its carrier tracker needed.
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

// ---- stubs for the piHPSDR globals the engine touches ------------------
// The real struct types are used deliberately: stub structs with a
// plausible-looking subset of fields read the wrong offsets and silently
// produce nonsense.
static RECEIVER rx0;
RECEIVER *receiver[8] = { &rx0 };
int receivers = 2;
int diversity_enabled = 1;
int radio_is_remote = 0;
int cw_keyer_sidetone_frequency = 800;
double div_cos = 1.0, div_sin = 0.0, div_gain = 0.0, div_phase = 0.0;
double div_norm = 1.0;   /* the output-level normaliser; receiver.c applies it */
//
// The engine reads the two step attenuators as part of its analysis
// context, so a change of either restarts the statistics.
//
ADC adc[3];
int div_indep_att = 0;
struct _vfo vfo[MAX_VFOS];
void t_print(const char *fmt, ...){ va_list a; va_start(a,fmt); vprintf(fmt,a); va_end(a); }
const char *getProperty(const char *n){ (void)n; return NULL; }
void setProperty(const char *n, const char *v){ (void)n; (void)v; }
double myatof(const char *s){ return atof(s); }

/*
 * The engine tells the menu when a mode change swapped one block of modal
 * settings for another. There is no menu here.
 */
gboolean diversity_menu_settings_changed(gpointer data) { (void)data; return G_SOURCE_REMOVE; }

int main(void) {
  memset(&rx0, 0, sizeof(rx0));
  rx0.id = 0; rx0.sample_rate = 192000;
  rx0.filter_low = -8000; rx0.filter_high = 8000;
  memset(vfo, 0, sizeof(vfo));
  vfo[0].frequency = 7100000; vfo[0].ctun_frequency = 7100000;
  vfo[0].offset = 0; vfo[0].mode = modeAM;

  const double hr = 0.62, hi = -0.48;   // arm1 = h * arm0
  struct { const char *name; int ref; int obj; } cases[] = {
    { "Window/Null",  DIV_REF_BAND,       DIV_AUTO_NULL },
    { "Window/Sum",   DIV_REF_BAND,       DIV_AUTO_SUM  },
    { "Carrier/Sum",  DIV_REF_CARRIER,    DIV_AUTO_SUM  },
    { "Digital/Sum",  DIV_REF_DIGITAL_IQ, DIV_AUTO_SUM  },
    { "Digital/Null", DIV_REF_DIGITAL_IQ, DIV_AUTO_NULL },
    { "CW/Sum",       DIV_REF_CW,         DIV_AUTO_SUM  },
    { "CW/Null",      DIV_REF_CW,         DIV_AUTO_NULL },
    //
    // Best does not share the Null/Sum path: div_apply_best() holds
    // instead of producing a weight whenever div_auto_arm_valid is 0, and
    // div_arm_from_floor() has two independent gates that can leave it
    // there indefinitely. So "Best silently never starts" is exactly the
    // failure this test exists to catch, on a reference that places its
    // own window and on one that does not.
    //
    { "Window/Best",  DIV_REF_BAND,       DIV_AUTO_BEST },
    { "Digital/Best", DIV_REF_DIGITAL_IQ, DIV_AUTO_BEST },
    { "CW/Best",      DIV_REF_CW,         DIV_AUTO_BEST },
  };
  int fails = 0;
  for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
    div_auto_ref = cases[c].ref;
    div_auto_mode = cases[c].obj;
    div_auto_follow_filter = 1;
    div_auto_tau = 1.0;
    div_auto_coherence_min = 0.1;
    div_cos = 1.0; div_sin = 0.0; div_gain = 0.0; div_phase = 0.0;
    diversity_auto_start();
    // a carrier at +37 Hz plus a little noise, on both arms
    // A carrier near the tuned frequency for the carrier tracker, plus a
    // component inside the RADE passband so that window has signal too.
    double ph = 0, ph2 = 0;
    srand(9);
    for (int blk = 0; blk < 400; blk++) {
      for (int n = 0; n < 512; n++) {
        ph  += 2.0*M_PI*37.0/192000.0;
        ph2 += 2.0*M_PI*1500.0/192000.0;
        double s = cos(ph) + 0.7*cos(ph2), t = sin(ph) + 0.7*sin(ph2);
        double n0 = 0.01*(2.0*rand()/RAND_MAX-1.0);
        double n1 = 0.01*(2.0*rand()/RAND_MAX-1.0);
        double a0r = s + n0,            a0i = t + n0;
        double a1r = hr*s - hi*t + n1,  a1i = hr*t + hi*s + n1;
        diversity_auto_sample(a0r, a0i, a1r, a1i);
      }
      g_usleep(200);
    }
    g_usleep(400000);
    int moved = (fabs(div_gain) > 0.01) || (fabs(div_phase) > 0.5);
    printf("%-14s -> gain %+7.2f dB  phase %+7.1f deg  coherence %3.0f%%  %s\n",
           cases[c].name, div_gain, div_phase, 100.0*div_auto_coherence,
           moved ? "OK" : "*** NEVER PRODUCED A WEIGHT ***");
    if (!moved) fails++;
    diversity_auto_stop();
  }
  //
  // ...and the same references again with the operator's notch sitting on
  // the only signal in the window.
  //
  // The analysis runs on the raw antenna streams, upstream of WDSP, so
  // the carrier is still there at full strength; div_bin_notched() is the
  // only thing keeping it out of the estimate. Every reference that works
  // from the transform has to honour it, which is what this checks - a
  // carve-out applied to one reference and forgotten in another is the
  // failure mode here.
  //
  // RADE V1 is deliberately absent: it is handed the block in the time
  // domain and has no bins to leave out.
  //
  // The notch centre is in the raw frame, so the bin frequency is
  // -centre; the carrier sits at +37 Hz, so the notch goes at -37.
  //
  printf("\n--- with a notch over the signal ---\n");
  struct { const char *name; int ref; } nocases[] = {
    { "Window", DIV_REF_BAND       },
    { "Carrier", DIV_REF_CARRIER   },
    { "Digital", DIV_REF_DIGITAL_IQ },
    { "CW",      DIV_REF_CW        },
  };

  for (unsigned c = 0; c < sizeof(nocases)/sizeof(nocases[0]); c++) {
    div_auto_ref = nocases[c].ref;
    div_auto_mode = DIV_AUTO_SUM;
    div_auto_follow_filter = 1;
    div_auto_tau = 1.0;
    div_auto_coherence_min = 0.1;
    div_cos = 1.0; div_sin = 0.0; div_gain = 0.0; div_phase = 0.0;
    //
    // Wide enough to swallow both tones whole at any bin width this
    // test can run at, so "nothing is left" is unambiguous.
    //
    rx0.multi_notch_enable[0] = 1;
    rx0.multi_notch_center[0] = -37.0;
    rx0.multi_notch_width[0]  = 400.0;
    rx0.multi_notch_enable[1] = 1;
    rx0.multi_notch_center[1] = -1500.0;
    rx0.multi_notch_width[1]  = 400.0;
    diversity_auto_start();
    double ph = 0, ph2 = 0;
    srand(9);

    for (int blk = 0; blk < 400; blk++) {
      for (int n = 0; n < 512; n++) {
        ph  += 2.0*M_PI*37.0/192000.0;
        ph2 += 2.0*M_PI*1500.0/192000.0;
        double s = cos(ph) + 0.7*cos(ph2), t = sin(ph) + 0.7*sin(ph2);
        double n0 = 0.01*(2.0*rand()/RAND_MAX-1.0);
        double n1 = 0.01*(2.0*rand()/RAND_MAX-1.0);
        double a0r = s + n0,            a0i = t + n0;
        double a1r = hr*s - hi*t + n1,  a1i = hr*t + hi*s + n1;
        diversity_auto_sample(a0r, a0i, a1r, a1i);
      }

      g_usleep(200);
    }

    g_usleep(400000);
    //
    // The engine may legitimately hold, or solve on what is left of the
    // window - what it must not do is converge on the notched tone, which
    // is the only thing in here with a channel of hr/hi.
    //
    const double gain_h = 20.0*log10(sqrt(hr*hr + hi*hi));
    const double phase_h = atan2(hi, hr)*180.0/M_PI;
    int locked_on_notched = (fabs(div_gain - gain_h) < 1.0) &&
                            (fabs(div_phase + phase_h) < 10.0);
    printf("%-8s -> gain %+7.2f dB  phase %+7.1f deg  holding %d  %s\n",
           nocases[c].name, div_gain, div_phase, div_auto_holding,
           locked_on_notched ? "*** SOLVED ON A NOTCHED SIGNAL ***" : "OK");

    if (locked_on_notched) { fails++; }

    diversity_auto_stop();
    rx0.multi_notch_enable[0] = 0;
    rx0.multi_notch_enable[1] = 0;
  }

  printf("%s\n", fails ? "FAIL" : "PASS - every mode produced a weight, and none used a notched bin");
  return fails ? 1 : 0;
}
