/*
 * rade_tx_iq: run rade_tx() over a features file and write the complex
 * baseband it produces (interleaved float32 I,Q at 8 kHz) to a file.
 * The features file is what rade_tx_wav writes with -f.
 *
 *   rade_tx_iq [--v2] [--no_bpf] features.f32 out.iq.f32
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rade_api.h"

int main(int argc, char **argv) {
  int flags = RADE_USE_C_ENCODER | RADE_VERBOSE_0, i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (!strcmp(argv[i], "--v2")) { flags |= RADE_MODE_V2; }
    else if (!strcmp(argv[i], "--no_bpf")) { flags |= RADE_NO_TX_BPF; }
    else { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
  }
  if (argc - i != 2) {
    fprintf(stderr, "usage: rade_tx_iq [--v2] [--no_bpf] features.f32 out.iq.f32\n");
    return 1;
  }
  rade_initialize();
  struct rade *r = rade_open("", flags);
  if (!r) { fprintf(stderr, "rade_open failed\n"); return 1; }
  int nf = rade_n_features_in_out(r), no = rade_n_tx_out(r);
  float *feat = malloc(sizeof(float) * nf);
  RADE_COMP *out = malloc(sizeof(RADE_COMP) * no);
  FILE *fi = fopen(argv[i], "rb"), *fo = fopen(argv[i + 1], "wb");
  if (!fi || !fo) { perror("open"); return 1; }
  long frames = 0;
  while (fread(feat, sizeof(float), nf, fi) == (size_t)nf) {
    int n = rade_tx(r, out, feat);
    fwrite(out, sizeof(RADE_COMP), n, fo);
    frames++;
  }
  int ne = rade_n_tx_eoo_out(r);
  RADE_COMP *eoo = malloc(sizeof(RADE_COMP) * ne);
  rade_tx_eoo(r, eoo);
  fwrite(eoo, sizeof(RADE_COMP), ne, fo);
  fclose(fi); fclose(fo);
  fprintf(stderr, "%ld modem frames of %d samples, EOO %d samples\n", frames, no, ne);
  rade_close(r);
  rade_finalize();
  return 0;
}
