/*
 * Where does the transmitted energy sit, relative to the dial, in every state we can be in, and what
 * happens to it on each change of state at a fixed dial? Uses the real rules in src/rade_native.c.
 * Offsets are the centre of the passband WDSP is given (positive: above the dial frequency).
 */
#include <stdio.h>
#include <stdlib.h>
#include "rade_native.h"

typedef struct { const char *name; int wave; int user_inv; long long dial; } state_t;

static double centre(const state_t *s) {
  if (s->wave == 0) { return 0; }
  double lo, hi;
  int inv = rade_native_effective_invert(s->wave, s->dial, s->user_inv);
  rade_native_wdsp_edges(s->wave, inv, 0.0, &lo, &hi);
  return 0.5 * (lo + hi);
}

int main(void) {
  const char *wn[] = {"SSB", "V1", "V2"};
  long long dials[] = {7053000LL, 14236000LL};
  printf("Centre of the transmitted energy relative to the dial, Hz (SSB speech passband 150..2850 USB, -2850..-150 LSB):\n\n");
  printf("%-10s %-14s %-9s %12s   %s\n", "dial MHz", "state", "invert", "RADE edges", "centre");
  for (int d = 0; d < 2; d++) {
    for (int w = 1; w <= 2; w++) {
      for (int u = 0; u <= 1; u++) {
        state_t s = {"", w, u, dials[d]};
        int inv = rade_native_effective_invert(w, dials[d], u);
        double lo, hi;
        rade_native_wdsp_edges(w, inv, 0.0, &lo, &hi);
        printf("%-10.3f %-14s %-9s %5.0f..%5.0f   %+6.0f\n", dials[d] / 1e6, wn[w], inv ? "inverted" : "normal", lo, hi, centre(&s));
      }
    }
    printf("%-10.3f %-14s %-9s %12s   %+6.0f\n", dials[d] / 1e6, "SSB USB/DIGU", "-", "150..2850", 1500.0);
    printf("%-10.3f %-14s %-9s %12s   %+6.0f\n", dials[d] / 1e6, "SSB LSB/DIGL", "-", "-2850..-150", -1500.0);
    printf("\n");
  }
  printf("Changes at a FIXED dial: how far the transmitted energy moves (Hz), and the dial change that would undo it\n\n");
  printf("%-10s %-34s %12s   %s\n", "dial MHz", "change", "energy moves", "dial change that keeps it");
  struct { const char *what; state_t a, b; } ch[] = {
    {"7 MHz: V1 (default, LSB-like) -> V2",       {"", 1, 0, 7053000}, {"", 2, 0, 7053000}},
    {"7 MHz: V1 -> Invert on (-> normal)",         {"", 1, 0, 7053000}, {"", 1, 1, 7053000}},
    {"7 MHz: V2 -> Invert on",                     {"", 2, 0, 7053000}, {"", 2, 1, 7053000}},
    {"14 MHz: V1 -> V2",                           {"", 1, 0, 14236000}, {"", 2, 0, 14236000}},
    {"14 MHz: V1 -> Invert on",                    {"", 1, 0, 14236000}, {"", 1, 1, 14236000}},
    {"14 MHz: V2 -> Invert on",                    {"", 2, 0, 14236000}, {"", 2, 1, 14236000}},
  };
  for (unsigned i = 0; i < sizeof ch / sizeof ch[0]; i++) {
    double a = centre(&ch[i].a), b = centre(&ch[i].b);
    printf("%-10.3f %-34s %+10.0f   %+8.0f\n", ch[i].a.dial / 1e6, ch[i].what, b - a, a - b);
  }
  return 0;
}
