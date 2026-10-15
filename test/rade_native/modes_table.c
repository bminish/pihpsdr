/*
 * What the dial and the sideband do when the waveform, Invert or RADE itself changes, using the real rule
 * in src/rade_native.c (rade_native_plan): the centre of the energy, dial + sign * 1500 Hz, stays put.
 * The first column of each row is the state before; the dial is the carrier of the sideband in use.
 */
#include <stdio.h>
#include <stdlib.h>
#include "rade_native.h"

typedef struct { int wave, inv, user, sign; long long dial; } st_t;

static const char *name(const st_t *s) {
  static char b[4][32];
  static int k = 0;
  k = (k + 1) % 4;
  if (s->wave == 0) { snprintf(b[k], sizeof b[k], "%s", s->sign < 0 ? "LSB" : s->sign > 0 ? "USB" : "other"); }
  else { snprintf(b[k], sizeof b[k], "V%d %s", s->wave, s->inv ? "lower" : "upper"); }
  return b[k];
}

static void step(st_t *s, int new_wave, int new_user, const char *what) {
  st_t old = *s;
  rade_native_plan_t p = rade_native_plan(s->wave, s->inv, s->sign, s->dial, new_wave, new_user);
  s->wave = new_wave; s->user = new_user; s->inv = p.invert; s->dial = p.dial;
  s->sign = new_wave ? (p.invert ? -1 : 1) : (p.invert ? -1 : 1);
  long long fe_old = old.dial + (long long)(old.wave ? (old.inv ? -1 : 1) : old.sign) * RADE_NATIVE_DIAL_OFFSET;
  long long fe_new = s->dial + (long long)(s->inv ? -1 : 1) * RADE_NATIVE_DIAL_OFFSET;
  if (new_wave == 0) { fe_new = fe_old; }       // back in SSB: the energy is where the carrier's sideband puts it
  printf("  %-26s %-9s %10.3f kHz -> %-9s %10.3f kHz   dial %+5lld Hz   centre %.3f kHz\n", what, name(&old),
         old.dial / 1e3, name(s), s->dial / 1e3, s->dial - old.dial, fe_new / 1e3);
}

int main(void) {
  printf("160 m, energy centre 1985.5 kHz:\n");
  st_t a = {0, 1, 0, -1, 1987000};
  step(&a, RADE_NATIVE_V1, 0, "RADE V1");
  step(&a, RADE_NATIVE_V2, 0, "V1 -> V2");
  step(&a, RADE_NATIVE_V1, 0, "V2 -> V1");
  step(&a, RADE_NATIVE_V1, 1, "Invert on");
  step(&a, RADE_NATIVE_V1, 0, "Invert off");
  step(&a, RADE_NATIVE_V2, 1, "V2, Invert on");
  step(&a, 0, 0, "RADE off");
  printf("\n40 m, from USB SSB at 7.053:\n");
  st_t b = {0, 0, 0, 1, 7053000};
  step(&b, RADE_NATIVE_V1, 0, "RADE V1 (lower on 40 m)");
  step(&b, RADE_NATIVE_V2, 0, "V1 -> V2");
  step(&b, 0, 0, "RADE off");
  printf("\n20 m, from USB SSB at 14.236:\n");
  st_t c = {0, 0, 0, 1, 14236000};
  step(&c, RADE_NATIVE_V1, 0, "RADE V1 (upper on 20 m)");
  step(&c, RADE_NATIVE_V2, 0, "V1 -> V2");
  step(&c, RADE_NATIVE_V2, 1, "V2, Invert on");
  step(&c, RADE_NATIVE_V2, 0, "Invert off");
  step(&c, 0, 0, "RADE off");
  return 0;
}
