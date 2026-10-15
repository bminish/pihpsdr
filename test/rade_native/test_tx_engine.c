/*
 * Offline test of the transmit engine in src/rade_native.c: plays a file of
 * mic samples (interleaved float32 I,Q at 48 kHz, mic in I, as WDSP's TX
 * input buffer holds it) through rade_native_tx_block() in 1024-sample
 * blocks and writes the modem signal that comes back (interleaved float32
 * I,Q at 48 kHz), then the over is ended the way radio_set_mox(0) ends it (end of over frame, hold).
 *
 *   test_tx_engine [--v2] [--invert] [--speed N] [--features f.f32] [--call CALL] in.f32 out.f32
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rade_native.h"

int main(int argc, char **argv) {
  int wf = RADE_NATIVE_V1, invert = 0, i = 1;
  double speed = 8.0;
  const char *feat = NULL, *call = NULL;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (!strcmp(argv[i], "--v2")) wf = RADE_NATIVE_V2;
    else if (!strcmp(argv[i], "--invert")) invert = 1;
    else if (!strcmp(argv[i], "--speed")) speed = atof(argv[++i]);
    else if (!strcmp(argv[i], "--features")) feat = argv[++i];
    else if (!strcmp(argv[i], "--call")) call = argv[++i];
  }
  if (argc - i != 2) { fprintf(stderr, "usage: test_tx_engine [opts] in.f32 out.f32\n"); return 1; }
  FILE *fi = fopen(argv[i], "rb"), *fo = fopen(argv[i + 1], "wb");
  if (!fi || !fo) { perror("open"); return 1; }
  fseek(fi, 0, SEEK_END); long n = ftell(fi) / 8; fseek(fi, 0, SEEK_SET);
  float *in = calloc(n, 8);
  if (fread(in, 8, n, fi) != (size_t)n) return 1;
  rade_native_configure(wf, 0);
  if (call) rade_native_set_callsign(call);
  rade_native_tx_enable(1);
  if (!rade_native_tx_enabled()) { fprintf(stderr, "tx enable failed\n"); return 1; }
  if (feat) rade_native_tx_feature_dump(feat);
  for (long p = 0; p + 1024 <= n; p += 1024) {
    double buf[2048];
    for (int k = 0; k < 2048; k++) buf[k] = in[2 * p + k];
    rade_native_tx_block(buf, 1024, invert, 1);
    float of[2048];
    for (int k = 0; k < 2048; k++) of[k] = buf[k];
    fwrite(of, 4, 2048, fo);
    usleep((useconds_t)(1024.0 / 48000.0 * 1e6 / speed));
  }
  // release: the way radio_set_mox(0) does, keep keyed until the end of the over has gone out
  long held = 0;
  rade_native_tx_end_request();
  while (rade_native_tx_end_state() != 2 && held < 48000 * 5) {
    double buf[2048] = {0};
    rade_native_tx_block(buf, 1024, invert, 1);
    float of[2048];
    for (int j = 0; j < 2048; j++) of[j] = buf[j];
    fwrite(of, 4, 2048, fo);
    held += 1024;
    usleep(2000);
  }
  printf("held %.0f ms after release%s  tx underruns %lu\n", held / 48.0,
         rade_native_tx_end_state() == 2 ? "" : "  (NOT DONE)", rade_native_tx_underruns());
  // then the transmitter is released: a few silent blocks, as the radio would send
  for (int k = 0; k < 8; k++) {
    double buf[2048] = {0};
    rade_native_tx_block(buf, 1024, invert, 0);
    float of[2048];
    for (int j = 0; j < 2048; j++) of[j] = buf[j];
    fwrite(of, 4, 2048, fo);
  }
  fclose(fo);
  rade_native_tx_feature_dump(NULL);
  rade_native_tx_enable(0);
  rade_native_configure(0, 0);
  return 0;
}
