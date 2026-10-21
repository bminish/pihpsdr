/*
 * DEVELOPMENT TOOL. Replays a .divc capture through src/rade_v2_correlator.c, the C port of
 * py/cp_probe.py, and prints one line a second: quality (dpk), lock, present, bump position,
 * the weight last issued and the arm figures. Builds on its own: no GTK, no engine.
 *
 *   cp_replay FILE [bank [tau]]       bank 0, 1, or -1 for both (default -1); tau seconds (1.0)
 *
 * The frame offset is the block's recorded vfo offset. Layout as in py/divc.py.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rade_v2_correlator.h"

#define HDR 280
#define BLK 208

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s FILE [bank [tau]]\n", argv[0]); return 2; }

  const int bank = argc > 2 ? atoi(argv[2]) : -1;
  const double tau = argc > 3 ? atof(argv[3]) : 1.0;
  FILE *f = fopen(argv[1], "rb");

  if (!f) { perror(argv[1]); return 1; }

  unsigned char h[HDR];

  if (fread(h, 1, HDR, f) != HDR) { return 1; }

  uint32_t rate, nfft, bb;
  memcpy(&rate, h + 12, 4);
  memcpy(&nfft, h + 16, 4);
  memcpy(&bb, h + 20, 4);

  if (!rade_v2c_start((int)rate)) { fprintf(stderr, "rate %u: cannot run\n", rate); return 1; }

  float *buf = malloc(bb);
  unsigned char m[BLK];
  double wr = 1.0, wi = 0.0;
  long nblk = 0, nupd = 0;
  double next = 1.0;

  while (fread(m, 1, BLK, f) == BLK && fread(buf, 1, bb, f) == bb) {
    uint32_t magic;
    int64_t off;
    memcpy(&magic, m, 4);
    memcpy(&off, m + 32, 8);

    if (magic != 0x214B4C42) { break; }

    nblk++;
    nupd += rade_v2c_process(buf, buf + 2 * nfft, (int)nfft, bank, (double)off, tau, &wr, &wi);
    const double t = (double)nblk * nfft / rate;

    if (t >= next) {
      next += 1.0;
      printf("%5.0f s  dpk %.2f  %s%s  gamma %3d  bank %d  snr %+5.1f dB  f %+6.1f Hz  w %.2f /%+4.0f deg  arm1 %+5.1f dB\n",
             t, rade_v2c_quality, rade_v2c_locked ? "LOCKED" : "search", rade_v2c_present ? "+" : " ",
             rade_v2c_gamma, rade_v2c_mirrored, rade_v2c_snr, rade_v2c_freq_off,
             hypot(wr, wi), atan2(wi, wr) * 180.0 / M_PI, rade_v2c_arm_db);
    }
  }

  fprintf(stderr, "%ld blocks, %ld weights\n", nblk, nupd);
  rade_v2c_stop();
  return 0;
}
