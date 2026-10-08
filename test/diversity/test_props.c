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
#include "radio.h"
#include "known_gaps.h"

static RECEIVER rx0;
RECEIVER *receiver[8] = { &rx0 };
int receivers = 2, diversity_enabled = 1, radio_is_remote = 0;
int div_auto_mode = DIV_MANUAL;   /* TEST keeps these in radio.c */
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
static int  have_follow;
static char followval[32];
static int  have_tau, have_res;
static char tauval[32], resval[32];
/*
 * One group block, the AM one, for the scheme 3 check. The live keys
 * alone cannot show it: a group with a block of its own in the file is
 * restored from that block, not from them.
 */
static int  have_group;
static char groupref[32];
static char groupfollow[32];
const char *getProperty(const char *n) {
  if (!strcmp(n, "diversity_auto_ref")) { return refval[0] ? refval : NULL; }

  if (!strcmp(n, "diversity_auto_ref_scheme")) { return have_scheme ? schemeval : NULL; }

  if (!strcmp(n, "diversity_auto_weighting")) { return have_weighting ? weightingval : NULL; }

  if (!strcmp(n, "diversity_rade_cohmin")) { return have_radecoh ? radecohval : NULL; }

  if (!strcmp(n, "diversity_auto_follow_filter")) { return have_follow ? followval : NULL; }

  if (!strcmp(n, "diversity_auto_tau")) { return have_tau ? tauval : NULL; }

  if (!strcmp(n, "diversity_auto_resolution")) { return have_res ? resval : NULL; }

  if (!strcmp(n, "diversity_group[3].ref")) { return have_group ? groupref : NULL; }

  if (!strcmp(n, "diversity_group[3].follow_filter")) { return have_group ? groupfollow : NULL; }

  return NULL;
}
void setProperty(const char *n, const char *v) { (void)n; (void)v; }

static int check(const char *what, int stored, int scheme, int want) {
  snprintf(refval, sizeof(refval), "%d", stored);
  have_scheme = (scheme > 0);
  snprintf(schemeval, sizeof(schemeval), "%d", scheme);
  div_auto_ref = -1;
  diversity_auto_restore_state();
  const char *names[] = { "Window", "Carrier", "RADE V1", "Digital I/Q", "CW" };
  const int got = div_auto_ref;
  const int ok = (got == want);
  char sch[8];
  snprintf(sch, sizeof(sch), "%d", scheme);
  printf("  stored %d, scheme %-7s -> %-12s (want %-12s) %s\n",
         stored, scheme > 0 ? sch : "absent",
         (got >= 0 && got <= 4) ? names[got] : "??",
         names[want], ok ? "OK" : "FAIL");
  (void)what;
  return ok;
}

/*
 * Scheme 3: Carrier started honouring div_auto_follow_filter.
 *
 * The flag defaults on and was meaningless for Carrier before, so a file
 * written earlier carries a 1 that was never a decision. Adopting it
 * would move a hand-placed carrier search onto the passband without being
 * asked - on the one reference whose window an operator places by hand to
 * aim at a carrier other than the primary. It must be cleared for such a
 * file, and left alone for one that meant it.
 *
 * The migration runs twice, on the live keys and again per group, so both
 * are checked: the group case reads back through
 * diversity_auto_mode_changed(), which is how a group block reaches the
 * live values in the running radio.
 */
#ifndef GAP_CARRIER_FOLLOW
static int follow_case(const char *what, int ref, int stored, int scheme,
                       int group, int want) {
  snprintf(refval, sizeof(refval), "%d", ref);
  have_scheme = (scheme > 0);
  snprintf(schemeval, sizeof(schemeval), "%d", scheme);
  have_follow = 1;
  snprintf(followval, sizeof(followval), "%d", stored);
  have_group = group;
  snprintf(groupref, sizeof(groupref), "%d", ref);
  snprintf(groupfollow, sizeof(groupfollow), "%d", stored);
  div_auto_follow_filter = -1;
  diversity_auto_restore_state();

  if (group) {
    //
    // AM is DIV_GROUP_AM, the group whose block the keys above describe.
    // Restore leaves nothing adopted, so this is what puts it in force.
    //
    diversity_auto_mode_changed(modeAM);
  }

  const int got = div_auto_follow_filter;
  const int ok = (got == want);
  char sch[8];
  snprintf(sch, sizeof(sch), "%d", scheme);
  printf("  %-28s ref %d, follow %d, scheme %-6s -> %d (want %d) %s\n",
         what, ref, stored, scheme > 0 ? sch : "absent", got, want,
         ok ? "OK" : "FAIL");
  have_follow = 0;
  have_group = 0;
  return ok;
}
#endif

#ifndef GAP_CARRIER_FOLLOW
static int test_carrier_follow(void) {
  int ok = 1;
  /* the live keys */
  ok &= follow_case("old file, carrier",      DIV_REF_CARRIER, 1, 2, 0, 0);
  ok &= follow_case("no scheme at all",       DIV_REF_CARRIER, 1, 0, 0, 0);
  ok &= follow_case("this version, carrier",  DIV_REF_CARRIER, 1, 3, 0, 1);
  ok &= follow_case("this version, cleared",  DIV_REF_CARRIER, 0, 3, 0, 0);
  /* Window always meant it, so it is never touched */
  ok &= follow_case("old file, window",       DIV_REF_BAND,    1, 2, 0, 1);
  ok &= follow_case("old file, digital",      DIV_REF_DIGITAL_IQ, 1, 2, 0, 1);
  /* and again through a group block of its own */
  ok &= follow_case("old AM group block",     DIV_REF_CARRIER, 1, 2, 1, 0);
  ok &= follow_case("new AM group block",     DIV_REF_CARRIER, 1, 3, 1, 1);
  return ok;
}
#endif

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
#ifndef GAP_WIRE_HELPERS
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
  a.tau = 5.25;         a.hang = 11.5;
  a.coherence_min = 0.41;  a.resolution = DIV_RES_AUTO;
  a.band_centre = 101.0;    a.band_width = 202.0;
  a.carrier_centre = 303.0; a.carrier_width = 404.0;
  a.digital_centre = 505.0; a.digital_width = 606.0;
  a.cw_centre = 707.0;      a.cw_width = 808.0;
  a.band_cohmin = 0.11; a.carrier_cohmin = 0.22;
  a.digital_cohmin = 0.33; a.rade_cohmin = 0.44;
  a.cw_cohmin = 0.55; a.cw_activity = 0.66;
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
  CHK_D(cw_centre); CHK_D(cw_width);
  CHK_D(band_cohmin); CHK_D(carrier_cohmin);
  CHK_D(digital_cohmin); CHK_D(rade_cohmin);
  CHK_D(cw_cohmin); CHK_D(cw_activity);
#undef CHK_I
#undef CHK_D
  printf("  %d field(s) wrong out of 26; worst round-trip error %.2g (the wire quantises at ~1e-7)\n",
         bad, worst);
  return bad == 0;
}
#endif

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
#ifdef GAP_RADE_QUALITY_RETIRED
    printf("  stored RADE quality gate 15 %% -> %.2f\n", div_rade_cohmin);
    known_gap(0, GAP_RADE_QUALITY_RETIRED);
#else
    printf("    FAIL rade_cohmin stored 0.15 -> %.3f, want 0\n", div_rade_cohmin);
    bad++;
#endif
  } else {
    printf("  stored RADE quality gate 15 %% -> %.2f\n", div_rade_cohmin);
  }

  have_weighting = 0;
  have_radecoh = 0;
  return bad == 0;
}

/*
 * LC-016: a client cannot bring RADE V1's retired threshold back. The wire
 * carries only the live value, and LC-004 files a client's live value into
 * the selected reference's slot - so this is the route a stale client
 * would use. Both the live gate and the slot must stay at zero.
 */
static int test_rade_client_pinned(void) {
  div_auto_ref = DIV_REF_RADE_V1;
  div_auto_mode = DIV_MANUAL;
  DIV_SETTINGS s;
  diversity_auto_get_settings(&s);
  s.coherence_min = 0.30;
  s.rade_cohmin = 0.30;
  diversity_auto_apply_settings(&s, DIV_ACTION_NONE);
  const int ok = (div_auto_coherence_min == 0.0) && (div_rade_cohmin == 0.0);
  printf("  client sends 30 %% on RADE V1 -> live %.2f, slot %.2f   %s\n",
         div_auto_coherence_min, div_rade_cohmin, ok ? "OK" : "FAIL");
  div_auto_ref = DIV_REF_BAND;
  return ok;
}

/*
 * LC-048, LC-050, LC-051: the Averaging cap, the bin width pinned to Auto,
 * and the table Auto reads. The values come through the props file, which
 * is where div_settings_validate() runs.
 */
static int test_averaging_and_bins(void) {
  int bad = 0;
  static const struct { double tau, res, want_tau, want_res; const char *what; } c[] = {
    { 20.0, 3.0,   6.0, DIV_RES_AUTO, "tau 20 -> 6 s (the cap); 3 Hz -> Auto" },
    {  7.0, 40.0,  6.0, DIV_RES_AUTO, "tau 7 -> 6 s; 40 Hz -> Auto" },
    {  2.0, 12.0,  2.0, DIV_RES_AUTO, "a stored 12 Hz (an older file) -> Auto" },
    {  2.0, 0.0,   2.0, DIV_RES_AUTO, "resolution 0 -> Auto" },
    {  2.0, -1.0,  2.0, DIV_RES_AUTO, "resolution -1 stays Auto" },
    {  5.9, 24.0,  5.9, DIV_RES_AUTO, "5.9 s is left alone; 24 Hz -> Auto" },
    {  0.2, 6.0,   0.2, DIV_RES_AUTO, "0.2 s is left alone; 6 Hz -> Auto" },
  };

  for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
    have_tau = have_res = 1;
    snprintf(tauval, sizeof(tauval), "%.6f", c[i].tau);
    snprintf(resval, sizeof(resval), "%.6f", c[i].res);
    diversity_auto_restore_state();
    const int ok = fabs(div_auto_tau - c[i].want_tau) < 1.0e-9 &&
                   fabs(div_auto_resolution - c[i].want_res) < 1.0e-9;
    printf("  %-44s -> %.1f s, %.1f Hz   %s\n", c[i].what, div_auto_tau, div_auto_resolution,
           ok ? "OK" : "FAIL");
    bad += !ok;
  }

  /* diversity_auto_bin_policy(): reference, then the Averaging time */
  static const struct { int ref; double tau, want; } p[] = {
    { DIV_REF_BAND, 0.2, 24 }, { DIV_REF_BAND, 0.5, 24 }, { DIV_REF_BAND, 1.0, 24 },
    { DIV_REF_BAND, 1.01, 12 }, { DIV_REF_BAND, 2.0, 12 }, { DIV_REF_BAND, 5.01, 12 },
    { DIV_REF_BAND, 6.0, 12 },
    { DIV_REF_CARRIER, 0.2, 24 }, { DIV_REF_CARRIER, 1.0, 24 }, { DIV_REF_CARRIER, 1.5, 12 },
    { DIV_REF_CARRIER, 6.0, 12 },
    { DIV_REF_DIGITAL_IQ, 0.2, 12 }, { DIV_REF_DIGITAL_IQ, 1.0, 12 }, { DIV_REF_DIGITAL_IQ, 6.0, 12 },
    { DIV_REF_CW, 0.2, 12 }, { DIV_REF_CW, 1.0, 12 }, { DIV_REF_CW, 6.0, 12 },
    { DIV_REF_RADE_V1, 0.2, 12 }, { DIV_REF_RADE_V1, 2.0, 12 }, { DIV_REF_RADE_V1, 6.0, 12 },
  };
  int pol = 0;

  for (size_t i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
    const double got = diversity_auto_bin_policy(p[i].ref, p[i].tau);

    if (got != p[i].want) {
      printf("    FAIL policy ref %d tau %.2f -> %.0f Hz, want %.0f\n", p[i].ref, p[i].tau, got, p[i].want);
      pol++;
    }
  }

  printf("  Auto's table: %zu cases, %d wrong   %s\n", sizeof(p) / sizeof(p[0]), pol, pol ? "FAIL" : "OK");
  have_tau = have_res = 0;
  div_auto_resolution = DIV_RES_AUTO;
  div_auto_tau = 2.0;
  return bad + pol == 0;
}

int main(void) {
  memset(&rx0, 0, sizeof(rx0));
  memset(vfo, 0, sizeof(vfo));
  printf("diversity_auto_ref migration\n\n");
  int ok = 1;
  /* scheme 1: BAND CARRIER RADE_BAND RADE_V1 DIGITAL_IQ */
  {
    int old = 1;
    old &= check("old window",   0, 0, DIV_REF_BAND);
    old &= check("old carrier",  1, 0, DIV_REF_CARRIER);
    old &= check("old radeband", 2, 0, DIV_REF_DIGITAL_IQ);
    old &= check("old radev1",   3, 0, DIV_REF_RADE_V1);
    old &= check("old digital",  4, 0, DIV_REF_DIGITAL_IQ);
#ifdef GAP_REF_SCHEME
    ok &= known_gap(old, GAP_REF_SCHEME);
#else
    ok &= old;
#endif
  }
  printf("\n");
  /* scheme 2: values mean themselves */
  ok &= check("new window",   0, 2, DIV_REF_BAND);
  ok &= check("new carrier",  1, 2, DIV_REF_CARRIER);
  ok &= check("new radev1",   2, 2, DIV_REF_RADE_V1);
  ok &= check("new digital",  3, 2, DIV_REF_DIGITAL_IQ);
  printf("\ncarrier follow-filter migration (scheme 3)\n");
#ifdef GAP_CARRIER_FOLLOW
  ok &= known_gap(0, GAP_CARRIER_FOLLOW);
#else
  ok &= test_carrier_follow();
#endif
  printf("\nDIV_SETTINGS over the wire\n");
#ifdef GAP_WIRE_HELPERS
  ok &= known_gap(0, GAP_WIRE_HELPERS);
#else
  ok &= test_wire_round_trip();
#endif
  printf("\nretired controls are pinned, not ranged\n");
  ok &= test_retired_pinned();
  ok &= test_rade_client_pinned();
  printf("\nAveraging cap, bin width pinned to Auto\n");
  ok &= test_averaging_and_bins();
  printf("\n%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
