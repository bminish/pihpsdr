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
  // library
  char msg[200];
  int rc = rade_native_probe(msg, sizeof msg);
  printf("%s\n", msg);
  CHECK(rc == 0);
  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails != 0;
}
