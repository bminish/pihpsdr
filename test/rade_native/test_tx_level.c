/*
 * Offline test of the mic level measure, trim, leveller and limiter in
 * src/rade_native.c: plays a mic file (interleaved float32 I,Q at 48 kHz, mic
 * in I) through rade_native_tx_block() with the radio NOT keyed (the level is
 * measured before keying) and prints the meter once a second.
 *
 *   test_tx_level [--trim DB] [--agc 0|1] [--repeat N] in.f32
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rade_native.h"

int main(int argc, char **argv) {
  double trim = 0.0;
  int agc = 0, rep = 1, i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (!strcmp(argv[i], "--trim")) trim = atof(argv[++i]);
    else if (!strcmp(argv[i], "--agc")) agc = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--repeat")) rep = atoi(argv[++i]);
  }
  if (argc - i != 1) { fprintf(stderr, "usage: test_tx_level [opts] in.f32\n"); return 1; }
  FILE *fi = fopen(argv[i], "rb");
  if (!fi) { perror("open"); return 1; }
  fseek(fi, 0, SEEK_END); long n = ftell(fi) / 8; fseek(fi, 0, SEEK_SET);
  float *in = malloc(n * 8);
  if (fread(in, 8, n, fi) != (size_t)n) return 1;
  rade_native_configure(RADE_NATIVE_V1, 0);
  rade_native_tx_enable(1);
  rade_native_tx_set_trim_db(trim);
  rade_native_tx_set_agc(agc);
  double peak_out = 0.0;
  long done = 0;
  for (int r = 0; r < rep; r++) {
    for (long p = 0; p + 1024 <= n; p += 1024, done += 1024) {
      double buf[2048];
      for (int k = 0; k < 2048; k++) buf[k] = in[2 * p + k];
      // the engine zeroes the block when not transmitting, so look at it through a keyed block's input:
      // tx_block processes the mic first either way, and the processed mic is what the meter saw.
      rade_native_tx_block(buf, 1024, 0, 0);
      if (done % 48000 == 47104) {
        rade_native_txlevel_t l;
        rade_native_tx_level(&l);
        printf("%4ld s: valid %d avg %6.1f mom %6.1f delta %+5.1f peak %6.1f dBFS limiting %d agc %+5.1f dB\n",
               done / 48000 + 1, l.valid, l.loud_avg, l.momentary, l.delta_db, l.peak_dbfs, l.limiting, l.agc_gain_db);
      }
    }
  }
  (void)peak_out;
  rade_native_tx_enable(0);
  rade_native_configure(0, 0);
  return 0;
}
