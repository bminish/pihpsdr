/*
 * DEVELOPMENT TOOL. Not part of piHPSDR - see docs/tools/radev2-scoring.md.
 *
 * Runs one RADE V2 receiver over an 8 kHz complex float32 I/Q file (the
 * format radae_tx --v2 writes) and reports what the probe sees. This is
 * the calibration half of the V2 scorer: py/radev2_calib.py builds
 * synthetic channels from a known transmission, decodes them with this,
 * and compares the probe's numbers with the true feature loss.
 *
 *   ./radev2_iq rx.iq [--features out.f32] [--csv sym.csv] [--no-agc] [-v]
 *
 * The summary is one key=value line, for scripts.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "radev2_probe.h"

int main(int argc, char **argv) {
  const char *in = NULL, *feat = NULL, *csv = NULL;
  int verbose = 0, agc = 1;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--features") && i + 1 < argc) { feat = argv[++i]; }
    else if (!strcmp(argv[i], "--csv") && i + 1 < argc) { csv = argv[++i]; }
    else if (!strcmp(argv[i], "--no-agc")) { agc = 0; }
    else if (!strcmp(argv[i], "-v")) { verbose = 1; }
    else if (argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "usage: %s IN.iq [--features F.f32] [--csv S.csv] [--no-agc] [-v]\n", argv[0]);
      return 2;
    } else { in = argv[i]; }
  }

  if (in == NULL) {
    fprintf(stderr, "usage: %s IN.iq [--features F.f32] [--csv S.csv] [--no-agc] [-v]\n", argv[0]);
    return 2;
  }

  FILE *f = strcmp(in, "-") ? fopen(in, "rb") : stdin;

  if (f == NULL) { perror(in); return 1; }

  rade_initialize();
  struct v2probe p;

  if (!v2probe_open(&p, "iq", verbose, agc)) { return 1; }

  if (feat != NULL && (p.feat_out = fopen(feat, "wb")) == NULL) { perror(feat); return 1; }

  if (csv != NULL) {
    if ((p.csv_out = fopen(csv, "w")) == NULL) { perror(csv); return 1; }

    fprintf(p.csv_out, "t,sync,sig,valid,eoo,ry_max,snr,foff,data,fsync\n");
  }

  float z[2 * 160];
  size_t got;

  while ((got = fread(z, 2 * sizeof(float), 160, f)) > 0) {
    for (size_t k = 0; k < got; k++) { v2probe_push(&p, z[2 * k], z[2 * k + 1]); }

    v2probe_drain(&p);
  }

  struct v2summary s;
  v2probe_summary(&p, &s);
  printf("seconds=%.2f sync=%.4f sig=%.4f frames=%ld acq=%d eoo=%ld "
         "snr_sig=%.3f snr_sync=%.3f data_abs=%.4f data_conf=%.4f data_med=%.4f fsync=%.4f\n",
         s.seconds, s.sync_frac, s.sig_frac, s.frames, s.acq, s.eoo,
         s.snr_sig, s.snr_sync, s.data_abs, s.data_conf, s.data_med, s.fsync);

  if (p.feat_out != NULL) { fclose(p.feat_out); }

  if (p.csv_out != NULL) { fclose(p.csv_out); }

  v2probe_close(&p);
  rade_finalize();

  if (f != stdin) { fclose(f); }

  return 0;
}
