/*
 * diversity_auto_ref survives the removal of a reference mode.
 *
 * The RADE passband reference was value 2, with RADE V1 at 3 and Digital
 * I/Q at 4. Removing it moved everything above it down, so a stored 2 is
 * either the old RADE passband or the new RADE V1 and a stored 3 either
 * the old RADE V1 or the new Digital I/Q - the two numberings cannot be
 * told apart by inspecting the value. diversity_auto_ref_scheme is what
 * makes the migration a decision rather than a guess, and this checks
 * both numberings resolve to the mode the operator actually chose.
 *
 * Loading the wrong reference is a silent failure: the menu comes up on a
 * plausible-looking mode and measures the wrong thing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <gtk/gtk.h>
#include "mode.h"
#include "receiver.h"
#include "vfo.h"
#include "adc.h"
#include "diversity_auto.h"
#include "client_server.h"

static RECEIVER rx0;
RECEIVER *receiver[8] = { &rx0 };
int receivers = 2, diversity_enabled = 1, radio_is_remote = 0;
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
void t_print(const char *f, ...) { (void)f; }
double myatof(const char *s) { return atof(s); }

/*
 * The engine tells the menu when a mode change swapped one block of modal
 * settings for another. There is no menu here.
 */
gboolean diversity_menu_settings_changed(gpointer data) { (void)data; return G_SOURCE_REMOVE; }

/* a tiny property store the test drives directly */
static char refval[32];
static int  have_scheme;
static char schemeval[32];
static int  have_weighting;
static char weightingval[32];
static int  have_radecoh;
static char radecohval[32];
const char *getProperty(const char *n) {
  if (!strcmp(n, "diversity_auto_ref")) { return refval[0] ? refval : NULL; }

  if (!strcmp(n, "diversity_auto_ref_scheme")) { return have_scheme ? schemeval : NULL; }

  if (!strcmp(n, "diversity_auto_weighting")) { return have_weighting ? weightingval : NULL; }

  if (!strcmp(n, "diversity_rade_cohmin")) { return have_radecoh ? radecohval : NULL; }

  return NULL;
}
void setProperty(const char *n, const char *v) { (void)n; (void)v; }

static int check(const char *what, int stored, int scheme, int want) {
  snprintf(refval, sizeof(refval), "%d", stored);
  have_scheme = (scheme > 0);
  snprintf(schemeval, sizeof(schemeval), "%d", scheme);
  div_auto_ref = -1;
  diversity_auto_restore_state();
  const char *names[] = { "Window", "Carrier", "RADE V1", "Digital I/Q" };
  const int got = div_auto_ref;
  const int ok = (got == want);
  printf("  stored %d, scheme %-7s -> %-12s (want %-12s) %s\n",
         stored, scheme > 0 ? "2" : "absent",
         (got >= 0 && got <= 3) ? names[got] : "??",
         names[want], ok ? "OK" : "FAIL");
  (void)what;
  return ok;
}

/*
 * Every field of DIV_SETTINGS has to survive the wire.
 *
 * The failure this exists to catch is silent and has happened twice: a
 * field is added to DIV_SETTINGS, the three copies of the pack/unpack
 * field list are not all updated, and the receiver leaves that field
 * holding whatever was on the stack. Nothing fails to compile; a control
 * simply stops working at the far end, and only at the far end. The three
 * lists are now one pair of functions in client_server.h, and this fills
 * every field with a value it could not arrive at by accident and checks
 * it comes back.
 */
static int test_wire_round_trip(void) {
  DIV_SETTINGS a, b;
  DIV_SETTINGS_COMMAND c;
  //
  // Distinctive, in range, and different from each other and from zero -
  // a field left unassigned by the pack or the unpack shows up as 0 and
  // fails, and one crossed with its neighbour fails too.
  //
  memset(&a, 0, sizeof(a));
  a.mode = DIV_AUTO_BEST;
  a.ref = DIV_REF_DIGITAL_IQ;
  a.follow_filter = 1;
  a.weighting = DIV_WEIGHT_FLAT;
  a.hold = 1;
  a.normalise = 1;
  a.centre = -1234.0;   a.width = 2345.0;
  a.tau = 7.25;         a.hang = 11.5;
  a.coherence_min = 0.41;  a.resolution = 6.0;
  a.band_centre = 101.0;    a.band_width = 202.0;
  a.carrier_centre = 303.0; a.carrier_width = 404.0;
  a.digital_centre = 505.0; a.digital_width = 606.0;
  a.band_cohmin = 0.11; a.carrier_cohmin = 0.22;
  a.digital_cohmin = 0.33; a.rade_cohmin = 0.44;
  memset(&c, 0xA5, sizeof(c));          /* so an unwritten field is obvious */
  div_settings_to_command(&c, &a);
  memset(&b, 0x5A, sizeof(b));          /* and so is one the unpack forgets */
  div_settings_from_command(&b, &c);
  int bad = 0;
  double worst = 0.0;
  //
  // to_double() carries a double as (x + 9e8) * 1e10 in a uint64, which
  // near 9e18 has a double ulp of about a thousand - so the wire quantises
  // every one of these to around 1e-7 whatever the field means. That is
  // the protocol's own resolution and not something this test is checking;
  // the tolerance is set well inside it and the worst error is printed so
  // that a real drop-out cannot hide under it.
  //
#define CHK_I(f)  do { if (a.f != b.f) { printf("    FAIL %-16s %d -> %d\n", #f, (int)a.f, (int)b.f); bad++; } } while (0)
#define CHK_D(f)  do { const double e = fabs(a.f - b.f); if (e > worst) { worst = e; } \
                       if (e > 1.0e-6) { printf("    FAIL %-16s %.12g -> %.12g\n", #f, a.f, b.f); bad++; } } while (0)
  CHK_I(mode); CHK_I(ref); CHK_I(follow_filter); CHK_I(weighting);
  CHK_I(hold); CHK_I(normalise);
  CHK_D(centre); CHK_D(width); CHK_D(tau); CHK_D(hang);
  CHK_D(coherence_min); CHK_D(resolution);
  CHK_D(band_centre); CHK_D(band_width);
  CHK_D(carrier_centre); CHK_D(carrier_width);
  CHK_D(digital_centre); CHK_D(digital_width);
  CHK_D(band_cohmin); CHK_D(carrier_cohmin);
  CHK_D(digital_cohmin); CHK_D(rade_cohmin);
#undef CHK_I
#undef CHK_D
  printf("  %d field(s) wrong out of 22; worst round-trip error %.2g (the wire quantises at ~1e-7)\n",
         bad, worst);
  return bad == 0;
}

/*
 * A retired control is pinned on the way in, not merely range-checked.
 *
 * Weighting and Hang both still travel - the wire format and the props
 * file keep their shape - but neither has a control any more and neither
 * is a setting an operator can improve on. The failure this catches is
 * quiet: a props file written by an older build, or a settings block from
 * an older client, restores a value that no longer has a way to be seen
 * or changed, and the radio runs on it. Coherence weighting in particular
 * would come back as the default it used to be, alongside the 0.20
 * threshold that was chosen to replace it - which is the one pairing the
 * measurements say is worse than either half. RADE V1's threshold is the
 * same shape of problem with a sharper edge: every reachable setting it
 * ever offered holds the loop through a working decode, so a stored one
 * has to be replaced rather than clamped. See Findings 27, 29, 33, 40
 * and 42, and DIV_HANG_DEFAULT.
 */
static int test_retired_pinned(void) {
  int bad = 0;
  snprintf(refval, sizeof(refval), "%d", DIV_REF_BAND);
  have_scheme = 1;
  snprintf(schemeval, sizeof(schemeval), "2");
  have_weighting = 1;
  snprintf(weightingval, sizeof(weightingval), "%d", DIV_WEIGHT_COHERENCE);
  have_radecoh = 1;
  snprintf(radecohval, sizeof(radecohval), "0.15");
  div_auto_weighting = DIV_WEIGHT_COHERENCE;
  div_auto_hang = 1.0;
  div_rade_cohmin = 0.15;
  diversity_auto_restore_state();

  if (div_auto_weighting != DIV_WEIGHT_FLAT) {
    printf("    FAIL weighting  stored coherence -> %d, want flat (%d)\n",
           div_auto_weighting, DIV_WEIGHT_FLAT);
    bad++;
  } else {
    printf("  stored coherence -> flat\n");
  }

  /* DIV_HANG_DEFAULT is private to diversity_auto.c; this is its value */
  const double want_hang = 10.0;

  if (fabs(div_auto_hang - want_hang) > 1.0e-9) {
    printf("    FAIL hang       stored 1.0 -> %.3f, want %.3f\n",
           div_auto_hang, want_hang);
    bad++;
  } else {
    printf("  stored hang 1.0 s -> %.1f s\n", div_auto_hang);
  }

  if (div_rade_cohmin != 0.0) {
    printf("    FAIL rade_cohmin stored 0.15 -> %.3f, want 0\n", div_rade_cohmin);
    bad++;
  } else {
    printf("  stored RADE quality gate 15 %% -> %.2f\n", div_rade_cohmin);
  }

  have_weighting = 0;
  have_radecoh = 0;
  return bad == 0;
}

int main(void) {
  memset(&rx0, 0, sizeof(rx0));
  memset(vfo, 0, sizeof(vfo));
  printf("diversity_auto_ref migration\n\n");
  int ok = 1;
  /* scheme 1: BAND CARRIER RADE_BAND RADE_V1 DIGITAL_IQ */
  ok &= check("old window",   0, 0, DIV_REF_BAND);
  ok &= check("old carrier",  1, 0, DIV_REF_CARRIER);
  ok &= check("old radeband", 2, 0, DIV_REF_DIGITAL_IQ);
  ok &= check("old radev1",   3, 0, DIV_REF_RADE_V1);
  ok &= check("old digital",  4, 0, DIV_REF_DIGITAL_IQ);
  printf("\n");
  /* scheme 2: values mean themselves */
  ok &= check("new window",   0, 2, DIV_REF_BAND);
  ok &= check("new carrier",  1, 2, DIV_REF_CARRIER);
  ok &= check("new radev1",   2, 2, DIV_REF_RADE_V1);
  ok &= check("new digital",  3, 2, DIV_REF_DIGITAL_IQ);
  printf("\nDIV_SETTINGS over the wire\n");
  ok &= test_wire_round_trip();
  printf("\nretired controls are pinned, not ranged\n");
  ok &= test_retired_pinned();
  printf("\n%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
