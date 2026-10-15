/*
 * Offline test of the receive engine in src/rade_native.c: plays a file of
 * complex baseband as WDSP's receive chain would deliver it (interleaved
 * float32 I,Q at 48 kHz, panel in binaural mode, WDSP's sign convention)
 * through rade_native_rx_block() in 1024-sample blocks, at SPEED times real
 * time, and writes the speech that comes back as a 48 kHz mono WAV.
 *
 *   test_rx_engine [--v2] [--invert] [--speed N] in.rxout48.f32 out.wav
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include "rade_native.h"

static void wav_header(FILE *f, int rate, uint32_t bytes) {
  uint32_t v; uint16_t h;
  fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
  h = 1; fwrite(&h, 2, 1, f); fwrite(&h, 2, 1, f);
  v = rate; fwrite(&v, 4, 1, f); v = rate * 2; fwrite(&v, 4, 1, f);
  h = 2; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

int main(int argc, char **argv) {
  int wf = RADE_NATIVE_V1, invert = 0, i = 1;
  double speed = 8.0;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (!strcmp(argv[i], "--v2")) wf = RADE_NATIVE_V2;
    else if (!strcmp(argv[i], "--invert")) invert = 1;
    else if (!strcmp(argv[i], "--speed")) speed = atof(argv[++i]);
  }
  if (argc - i != 2) { fprintf(stderr, "usage: test_rx_engine [opts] in.f32 out.wav\n"); return 1; }
  FILE *fi = fopen(argv[i], "rb"), *fo = fopen(argv[i + 1], "wb");
  if (!fi || !fo) { perror("open"); return 1; }
  fseek(fi, 0, SEEK_END); long n = ftell(fi) / 8; fseek(fi, 0, SEEK_SET);
  float *in = malloc(n * 8);
  if (fread(in, 8, n, fi) != (size_t)n) return 1;
  rade_native_configure(wf, 0);
  wav_header(fo, 48000, 0);
  long bytes = 0; double peak = 0, sum = 0; long cnt = 0;
  for (long p = 0; p + 1024 <= n; p += 1024) {
    double buf[2048];
    for (int k = 0; k < 2048; k++) buf[k] = in[2 * p + k];
    rade_native_rx_block(buf, 1024, invert, 1.0);
    int16_t pcm[1024];
    for (int k = 0; k < 1024; k++) {
      double v = buf[2 * k];
      peak = fmax(peak, fabs(v)); sum += v * v; cnt++;
      pcm[k] = (int16_t)fmax(-32767, fmin(32767, v * 32768.0));
    }
    fwrite(pcm, 2, 1024, fo); bytes += 2048;
    usleep((useconds_t)(1024.0 / 48000.0 * 1e6 / speed));
  }
  // let the worker finish what is queued, then drain with silence in
  for (int k = 0; k < 100; k++) {
    double buf[2048] = {0};
    rade_native_rx_block(buf, 1024, invert, 1.0);
    int16_t pcm[1024];
    for (int j = 0; j < 1024; j++) { double v = buf[2 * j]; peak = fmax(peak, fabs(v)); sum += v * v; cnt++; pcm[j] = (int16_t)fmax(-32767, fmin(32767, v * 32768.0)); }
    fwrite(pcm, 2, 1024, fo); bytes += 2048;
    usleep(20000);
  }
  fseek(fo, 0, SEEK_SET); wav_header(fo, 48000, bytes); fclose(fo);
  rade_native_status_t st; rade_native_get_status(&st);
  printf("blocks %lu  sync %d  snr %.1f  foff %.1f  underruns %lu  overruns %lu\n",
         st.blocks, st.sync, st.snr_db, st.foff_hz, st.underruns, st.overruns);
  printf("speech peak %.3f rms %.4f over %.1f s\n", peak, sqrt(sum / cnt), cnt / 48000.0);
  rade_native_configure(0, 0);
  return 0;
}
