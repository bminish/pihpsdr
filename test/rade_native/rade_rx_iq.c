/*
 * rade_rx_iq: feed complex baseband (interleaved float32 I,Q at 8 kHz) to
 * rade_rx() and write what it says.
 *
 *   rade_rx_iq [--v2] in.iq.f32 feat.f32 stats.csv
 *
 * feat.f32 gets every block of decoded features (the layout rade_tx takes);
 * stats.csv gets one line per rade_rx() call:
 *   t_sec, sync, snr_3k_dB, freq_offset_Hz, nout
 * Comparing feat.f32 with the features that went into rade_tx measures the
 * link without a vocoder in the way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rade_api.h"

int main(int argc, char **argv) {
  int flags = RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0, i = 1;
  if (i < argc && !strcmp(argv[i], "--v2")) { flags |= RADE_MODE_V2; i++; }
  if (argc - i != 3) {
    fprintf(stderr, "usage: rade_rx_iq [--v2] in.iq.f32 feat.f32 stats.csv\n");
    return 1;
  }
  rade_initialize();
  struct rade *r = rade_open("", flags);
  if (!r) { fprintf(stderr, "rade_open failed\n"); return 1; }
  FILE *fi = fopen(argv[i], "rb"), *ff = fopen(argv[i + 1], "wb"), *fs = fopen(argv[i + 2], "w");
  if (!fi || !ff || !fs) { perror("open"); return 1; }
  int nmax = rade_nin_max(r), nf = rade_n_features_in_out(r), neoo = rade_n_eoo_bits(r);
  RADE_COMP *in = malloc(sizeof(RADE_COMP) * nmax);
  float *feat = malloc(sizeof(float) * nf);
  float *eoo = malloc(sizeof(float) * (neoo > 0 ? neoo : 1));
  long consumed = 0;
  fprintf(fs, "t_sec,sync,snr_db,foff_hz,nout\n");
  for (;;) {
    int nin = rade_nin(r);
    if (fread(in, sizeof(RADE_COMP), nin, fi) != (size_t)nin) { break; }
    int has_eoo = 0;
    int nout = rade_rx(r, feat, &has_eoo, neoo > 0 ? eoo : NULL, in);
    consumed += nin;
    if (nout > 0) { fwrite(feat, sizeof(float), nout, ff); }
    fprintf(fs, "%.4f,%d,%.2f,%.2f,%d\n", consumed / 8000.0, rade_sync(r),
            rade_snrdB_3k_est(r), rade_freq_offset(r), nout);
  }
  fclose(fi); fclose(ff); fclose(fs);
  rade_close(r);
  rade_finalize();
  return 0;
}
