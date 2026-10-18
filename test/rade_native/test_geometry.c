/*
 * Offline test of the engine-independent part of src/rade_native.c:
 * default/effective invert, WDSP passband edges, conjugation rule, and
 * that the module links against librade and opens both waveforms.
 */
#include <stdio.h>
#include <math.h>
#include "rade_native.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
  double lo, hi;
  // defaults
  CHECK(rade_native_default_invert(RADE_NATIVE_V1, 3650000) == 1);
  CHECK(rade_native_default_invert(RADE_NATIVE_V1, 14236000) == 0);
  CHECK(rade_native_default_invert(RADE_NATIVE_V1, 10000000) == 0);
  CHECK(rade_native_default_invert(RADE_NATIVE_V2, 3650000) == 0);
  // user Invert is XORed with the default
  CHECK(rade_native_effective_invert(RADE_NATIVE_V2, 7080000, 1) == 1);
  CHECK(rade_native_effective_invert(RADE_NATIVE_V1, 3650000, 1) == 0);
  CHECK(rade_native_effective_invert(RADE_NATIVE_V1, 3650000, 0) == 1);
  // edges
  rade_native_wdsp_edges(RADE_NATIVE_V1, 0, 0.0, &lo, &hi);
  CHECK(fabs(lo - 675.0) < 1e-9 && fabs(hi - 2275.0) < 1e-9);
  rade_native_wdsp_edges(RADE_NATIVE_V1, 1, 0.0, &lo, &hi);
  CHECK(fabs(lo + 2275.0) < 1e-9 && fabs(hi + 675.0) < 1e-9 && lo < hi);
  rade_native_wdsp_edges(RADE_NATIVE_V2, 0, 0.0, &lo, &hi);
  CHECK(fabs(lo - 1000.0) < 1e-9 && fabs(hi - 2000.0) < 1e-9);
  rade_native_wdsp_edges(RADE_NATIVE_V2, 0, 20.0, &lo, &hi);
  CHECK(fabs(lo - 1020.0) < 1e-9 && fabs(hi - 2020.0) < 1e-9);
  // conjugation: normal conjugates, inverted does not
  CHECK(rade_native_conjugate(0) == 1 && rade_native_conjugate(1) == 0);
  // the plan: the centre of the energy stays still
  rade_native_plan_t q;
  #define PLAN(ow, oi, ms, d, nw, ui) rade_native_plan((ow), (oi), (ms), (d), (nw), (ui))
  // 160 m, centre 1985.5 kHz: LSB 1987.0 -> V1 (inverted, same carrier) -> V2 (USB carrier 1984.0) -> V1 ...
  q = PLAN(0, 0, -1, 1987000, RADE_NATIVE_V1, 0);
  CHECK(q.invert == 1 && q.dial == 1987000);
  q = PLAN(RADE_NATIVE_V1, 1, -1, 1987000, RADE_NATIVE_V2, 0);
  CHECK(q.invert == 0 && q.dial == 1984000);
  q = PLAN(RADE_NATIVE_V2, 0, 1, 1984000, RADE_NATIVE_V1, 0);
  CHECK(q.invert == 1 && q.dial == 1987000);
  // Invert on a V1 signal: 3 kHz up or down; and back
  q = PLAN(RADE_NATIVE_V1, 1, -1, 1987000, RADE_NATIVE_V1, 1);
  CHECK(q.invert == 0 && q.dial == 1984000);
  q = PLAN(RADE_NATIVE_V1, 0, 1, 1984000, RADE_NATIVE_V1, 0);
  CHECK(q.invert == 1 && q.dial == 1987000);
  // leaving RADE: stay on the carrier of the last sideband, in LSB or USB (V2 below 10 MHz, not 60 m: LSB, the dial 3 kHz up)
  q = PLAN(RADE_NATIVE_V1, 1, -1, 1987000, 0, 0);
  CHECK(q.invert == 1 && q.dial == 1987000);          // -> LSB at 1987.0
  q = PLAN(RADE_NATIVE_V2, 0, 1, 1984000, 0, 0);
  CHECK(q.invert == 1 && q.dial == 1987000);          // -> V2 below 10 MHz leaves in LSB: the carrier 3 kHz up, the energy stays
  q = PLAN(RADE_NATIVE_V2, 1, -1, 1987000, 0, 0);
  CHECK(q.invert == 1 && q.dial == 1987000);          // V2 already inverted: LSB where it is
  q = PLAN(RADE_NATIVE_V2, 0, 1, 14236000, 0, 0);
  CHECK(q.invert == 0 && q.dial == 14236000);         // 20 m: USB, nothing moves
  q = PLAN(RADE_NATIVE_V2, 0, 1, 5357000, 0, 0);
  CHECK(q.invert == 0 && q.dial == 5357000);          // 60 m is upper sideband
  q = PLAN(RADE_NATIVE_V2, 0, 1, 7053000, 0, 0);
  CHECK(q.invert == 1 && q.dial == 7056000);          // 40 m
  // 14 MHz: V1 and V2 are both upper sideband, nothing moves between them; Invert moves 3 kHz
  q = PLAN(0, 0, 1, 14236000, RADE_NATIVE_V1, 0);
  CHECK(q.invert == 0 && q.dial == 14236000);
  q = PLAN(RADE_NATIVE_V1, 0, 1, 14236000, RADE_NATIVE_V2, 0);
  CHECK(q.invert == 0 && q.dial == 14236000);
  q = PLAN(RADE_NATIVE_V2, 0, 1, 14236000, RADE_NATIVE_V2, 1);
  CHECK(q.invert == 1 && q.dial == 14239000);
  // from USB SSB on 7 MHz the band default for V1 is inverted: the dial goes up 3 kHz, the energy stays
  q = PLAN(0, 0, 1, 7053000, RADE_NATIVE_V1, 0);
  CHECK(q.invert == 1 && q.dial == 7056000);
  q = PLAN(RADE_NATIVE_V1, 1, -1, 7056000, 0, 0);
  CHECK(q.invert == 1 && q.dial == 7056000);          // back to LSB on the carrier
  // from a mode with no sideband (AM, CW): the energy centre is the dial
  q = PLAN(0, 0, 0, 14236000, RADE_NATIVE_V2, 0);
  CHECK(q.invert == 0 && q.dial == 14234500);
  // whatever the sequence, the energy centre is where it started
  {
    long long fe0 = 1985500, d = 1987000;
    int w = 0, inv = 1, ms = -1;
    int steps[][2] = {{RADE_NATIVE_V1, 0}, {RADE_NATIVE_V2, 0}, {RADE_NATIVE_V2, 1}, {RADE_NATIVE_V1, 1}, {RADE_NATIVE_V1, 0}, {RADE_NATIVE_V2, 0}};
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++) {
      q = PLAN(w, inv, ms, d, steps[i][0], steps[i][1]);
      d = q.dial; inv = q.invert; w = steps[i][0]; ms = inv ? -1 : 1;
      CHECK(d + (inv ? -1 : 1) * 1500 == fe0);
    }
  }
  // 60 m: upper sideband by default for both; Invert there is below the carrier for both
  CHECK(rade_native_effective_invert(RADE_NATIVE_V1, 5357000, 0) == 0 && rade_native_effective_invert(RADE_NATIVE_V2, 5357000, 0) == 0);
  CHECK(rade_native_effective_invert(RADE_NATIVE_V1, 5357000, 1) == 1 && rade_native_effective_invert(RADE_NATIVE_V2, 5357000, 1) == 1);
  CHECK(rade_native_effective_invert(RADE_NATIVE_V1, 7053000, 0) == 1 && rade_native_effective_invert(RADE_NATIVE_V2, 7053000, 0) == 0);
  CHECK(rade_native_effective_invert(RADE_NATIVE_V1, 5200000, 0) == 1 && rade_native_effective_invert(RADE_NATIVE_V1, 5500000, 0) == 1);   // 5 MHz either side of 60 m
  CHECK(rade_native_band_class(5357000) == 1 && rade_native_band_class(3700000) == 0 && rade_native_band_class(14236000) == 2);
  q = PLAN(0, 0, 1, 5357000, RADE_NATIVE_V1, 0);                    // from USB on 60 m: V1 stays upper, the dial does not move
  CHECK(q.invert == 0 && q.dial == 5357000);
  q = PLAN(RADE_NATIVE_V1, 0, 1, 5357000, RADE_NATIVE_V2, 0);
  CHECK(q.invert == 0 && q.dial == 5357000);
  q = PLAN(RADE_NATIVE_V2, 0, 1, 5357000, RADE_NATIVE_V2, 1);       // Invert: below the carrier, the dial 3 kHz up
  CHECK(q.invert == 1 && q.dial == 5360000);
  // library
  char msg[200];
  int rc = rade_native_probe(msg, sizeof msg);
  printf("%s\n", msg);
  CHECK(rc == 0);
  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails != 0;
}
