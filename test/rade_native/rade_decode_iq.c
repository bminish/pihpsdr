/*
 * rade_decode_iq: decode complex baseband (interleaved float32 I,Q at 8 kHz)
 * with librade and write everything the analysis wants:
 *
 *   rade_decode_iq [--v2] in.iq.f32 stats.csv events.txt feat.f32 speech.wav
 *
 *   stats.csv   one row per rade_rx() call: t_sec, sync, snr_3k_db, foff_hz, nout
 *   events.txt  "eoo <t_sec> <callsign|->" for each end-of-over frame found
 *   feat.f32    the decoded vocoder features (the layout rade_tx takes)
 *   speech.wav  the speech FARGAN makes from them, 16 kHz mono 16 bit
 *
 * t_sec counts input samples consumed, so it is a position in the file.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rade_api.h"
#include "fargan.h"
#include "lpcnet.h"
#include "rade_eoo_text.h"

static void wav_header(FILE *f, uint32_t bytes) {
  uint32_t v;
  uint16_t h;
  fseek(f, 0, SEEK_SET);
  fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
  h = 1; fwrite(&h, 2, 1, f); fwrite(&h, 2, 1, f);
  v = 16000; fwrite(&v, 4, 1, f); v = 32000; fwrite(&v, 4, 1, f);
  h = 2; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

int main(int argc, char **argv) {
  int flags = RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0, i = 1;
  if (i < argc && !strcmp(argv[i], "--v2")) { flags |= RADE_MODE_V2; i++; }
  if (argc - i != 5) {
    fprintf(stderr, "usage: rade_decode_iq [--v2] in.iq.f32 stats.csv events.txt feat.f32 speech.wav\n");
    return 1;
  }
  rade_initialize();
  struct rade *r = rade_open("", flags);
  if (!r) { fprintf(stderr, "rade_open failed\n"); return 1; }
  FILE *fi = fopen(argv[i], "rb"), *fs = fopen(argv[i + 1], "w"), *fe = fopen(argv[i + 2], "w");
  FILE *ff = fopen(argv[i + 3], "wb"), *fw = fopen(argv[i + 4], "wb");
  if (!fi || !fs || !fe || !ff || !fw) { perror("open"); return 1; }
  int nmax = rade_nin_max(r), nf = rade_n_features_in_out(r), neoo = rade_n_eoo_bits(r);
  RADE_COMP *in = malloc(sizeof(RADE_COMP) * nmax);
  float *feat = malloc(sizeof(float) * nf);
  float *eoo = malloc(sizeof(float) * (neoo > 0 ? neoo : 1));
  FARGANState fargan;
  float cont[5 * NB_TOTAL_FEATURES];
  int cont_n = 0, ready = 0;
  fargan_init(&fargan);
  wav_header(fw, 0);
  uint32_t bytes = 0;
  long consumed = 0;
  fprintf(fs, "t_sec,sync,snr_db,foff_hz,nout\n");
  for (;;) {
    int nin = rade_nin(r);
    if (fread(in, sizeof(RADE_COMP), nin, fi) != (size_t)nin) { break; }
    int has_eoo = 0;
    int nout = rade_rx(r, feat, &has_eoo, neoo > 0 ? eoo : NULL, in);
    consumed += nin;
    if (has_eoo) {
      char call[16] = "";
      int ok = neoo > 0 && rade_eoo_text_decode(eoo, neoo / 2, call);
      fprintf(fe, "eoo %.3f %s\n", consumed / 8000.0, ok ? call : "-");
    }
    for (int fi2 = 0; fi2 < nout / NB_TOTAL_FEATURES; fi2++) {
      float *f = &feat[fi2 * NB_TOTAL_FEATURES];
      fwrite(f, sizeof(float), NB_TOTAL_FEATURES, ff);
      if (!ready) {
        memcpy(&cont[cont_n * NB_TOTAL_FEATURES], f, sizeof(float) * NB_TOTAL_FEATURES);
        if (++cont_n >= 5) {
          float packed[5 * NB_FEATURES], zeros[FARGAN_CONT_SAMPLES];
          for (int k = 0; k < 5; k++) {
            memcpy(&packed[k * NB_FEATURES], &cont[k * NB_TOTAL_FEATURES], sizeof(float) * NB_FEATURES);
          }
          memset(zeros, 0, sizeof zeros);
          fargan_cont(&fargan, zeros, packed);
          ready = 1;
        }
        continue;
      }
      float fpcm[LPCNET_FRAME_SIZE];
      int16_t pcm[LPCNET_FRAME_SIZE];
      fargan_synthesize(&fargan, fpcm, f);
      for (int k = 0; k < LPCNET_FRAME_SIZE; k++) {
        float v = fpcm[k] * 32768.0f;
        pcm[k] = (int16_t)(v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : floorf(v + 0.5f));
      }
      fwrite(pcm, 2, LPCNET_FRAME_SIZE, fw);
      bytes += 2 * LPCNET_FRAME_SIZE;
    }
    fprintf(fs, "%.4f,%d,%.2f,%.2f,%d\n", consumed / 8000.0, rade_sync(r),
            rade_snrdB_3k_est(r), rade_freq_offset(r), nout);
  }
  wav_header(fw, bytes);
  fclose(fi); fclose(fs); fclose(fe); fclose(ff); fclose(fw);
  rade_close(r);
  rade_finalize();
  return 0;
}
