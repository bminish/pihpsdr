/* Copyright (C)
*  2026 - Brendan Minish, EI6IZ
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rade_api.h"
#include "fargan.h"
#include "lpcnet.h"
#include "rade_native.h"

double rade_native_centre(int waveform) {
  return waveform == RADE_NATIVE_V2 ? RADE_NATIVE_FC_V2 : RADE_NATIVE_FC_V1;
}

double rade_native_width(int waveform) {
  return waveform == RADE_NATIVE_V2 ? RADE_NATIVE_BW_V2 : RADE_NATIVE_BW_V1;
}

int rade_native_default_invert(int waveform, long long dial_hz) {
  return waveform == RADE_NATIVE_V1 && dial_hz < RADE_NATIVE_V1_INV_BELOW_HZ;
}

int rade_native_effective_invert(int waveform, long long dial_hz, int user_invert) {
  return rade_native_default_invert(waveform, dial_hz) ^ (user_invert != 0);
}

void rade_native_wdsp_edges(int waveform, int invert, double shift_hz,
                            double *low, double *high) {
  double fc = rade_native_centre(waveform) + shift_hz;
  double half = 0.5 * rade_native_width(waveform);
  if (invert) {
    *low = -(fc + half);
    *high = -(fc - half);
  } else {
    *low = fc - half;
    *high = fc + half;
  }
}

int rade_native_conjugate(int invert) {
  return !invert;
}

int rade_native_probe(char *msg, size_t len) {
  rade_initialize();
  struct rade *r1 = rade_open("", RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0);
  struct rade *r2 = rade_open("", RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0 | RADE_MODE_V2);
  int ok = r1 != NULL && r2 != NULL;
  if (ok) {
    snprintf(msg, len, "librade %d.%d: V1 nin_max=%d tx=%d, V2 nin_max=%d tx=%d",
             rade_version(), rade_version_minor(),
             rade_nin_max(r1), rade_n_tx_out(r1), rade_nin_max(r2), rade_n_tx_out(r2));
  } else {
    snprintf(msg, len, "rade_open failed (V1 %s, V2 %s)", r1 ? "ok" : "no", r2 ? "ok" : "no");
  }
  if (r1) { rade_close(r1); }
  if (r2) { rade_close(r2); }
  rade_finalize();
  return ok ? 0 : -1;
}

//////////////////////////////////////////////////////////////////////////////
//
// Receive engine
//
//////////////////////////////////////////////////////////////////////////////

#define DEC            6                 // 48 kHz -> 8 kHz
#define DEC_TAPS       48                // 6 per output
#define INT            3                 // 16 kHz -> 48 kHz
#define INT_TAPS       24                // 8 per polyphase branch
#define IN_RING        16384             // 8 kHz complex samples, 2 s
#define OUT_RING       32768             // 16 kHz speech samples, 2 s
//
// Speech arrives in bursts, one modem frame at a time (V1 120 ms, V2 40 ms),
// so the cushion has to be more than a frame: playing starts once PREBUFFER
// samples (16 kHz) are queued, and anything beyond PREBUFFER + MAX_EXTRA
// is trimmed.
//
#define PREBUFFER_V1   3200              // 200 ms
#define PREBUFFER_V2   1600              // 100 ms
#define MAX_EXTRA      4800              // 300 ms
#define TARGET_RMS     0.9               // level handed to librade

//
// Capture: the receiver thread and the worker put their data into rings
// (under E.mx); a writer thread takes it to disk.
//
#define CAP_IQ_RING    (1 << 22)         // 4 MB, ~10 s of 48 kHz complex float
#define CAP_SP_RING    (1 << 20)

static struct {
  int active, run;
  char stem[512];
  FILE *iq, *sp, *csv, *ev;
  uint32_t iq_bytes, sp_bytes;
  uint8_t *iq_ring, *sp_ring;
  long iq_w, iq_r, sp_w, sp_r;
  long pos48;                            // 48 kHz samples captured
  long dropped;                          // bytes lost to a full ring
  double t0;
  pthread_t thread;
} C;

static struct {
  int waveform, user_invert;
  struct rade *r;
  int nin_max, nfeat;
  pthread_t thread;
  int running;
  pthread_mutex_t mx;
  pthread_cond_t cv;
  RADE_COMP *in_ring;
  float *out_ring;
  long in_w, in_r, out_w, out_r;         // counts, never wrapped
  int playing;
  long prebuffer;
  // receiver-thread DSP state
  double dec_h[DEC_TAPS], int_h[INT_TAPS];
  double hist_i[DEC_TAPS], hist_q[DEC_TAPS];
  int hist_pos, dec_phase;
  double pwr;                            // smoothed power at 8 kHz
  double ihist[INT_TAPS / INT];
  int ihist_pos, int_phase;
  double ybuf[INT];
  // status
  rade_native_status_t st;
} E = { .mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void cap_row(long consumed, int nout);

static void design_filters(void) {
  // Windowed-sinc low-pass filters. Decimator: cutoff 3.2 kHz at 48 kHz;
  // interpolator: cutoff 7.4 kHz at 48 kHz, gain INT.
  for (int k = 0; k < DEC_TAPS; k++) {
    double m = k - 0.5 * (DEC_TAPS - 1);
    double fc = 3200.0 / 48000.0;
    double sinc = fabs(m) < 1e-9 ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
    double w = 0.42 - 0.5 * cos(2.0 * M_PI * k / (DEC_TAPS - 1)) + 0.08 * cos(4.0 * M_PI * k / (DEC_TAPS - 1));
    E.dec_h[k] = sinc * w;
  }
  for (int k = 0; k < INT_TAPS; k++) {
    double m = k - 0.5 * (INT_TAPS - 1);
    double fc = 7400.0 / 48000.0;
    double sinc = fabs(m) < 1e-9 ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
    double w = 0.42 - 0.5 * cos(2.0 * M_PI * k / (INT_TAPS - 1)) + 0.08 * cos(4.0 * M_PI * k / (INT_TAPS - 1));
    E.int_h[k] = INT * sinc * w;
  }
}

//
// The worker: librade at 8 kHz in, FARGAN speech at 16 kHz out.
//
static void *worker(void *arg) {
  struct rade *r = E.r;
  int nmax = E.nin_max, nfeat = E.nfeat, neoo = rade_n_eoo_bits(r);
  RADE_COMP *blk = malloc(sizeof(RADE_COMP) * nmax);
  float *feat = malloc(sizeof(float) * nfeat);
  float *eoo = malloc(sizeof(float) * (neoo > 0 ? neoo : 1));
  long consumed = 0;
  FARGANState fargan;
  float cont[5 * NB_TOTAL_FEATURES];
  int cont_n = 0, ready = 0;
  fargan_init(&fargan);
  pthread_mutex_lock(&E.mx);
  while (E.running) {
    int nin = rade_nin(r);
    if (E.in_w - E.in_r < nin) {
      pthread_cond_wait(&E.cv, &E.mx);
      continue;
    }
    for (int i = 0; i < nin; i++) { blk[i] = E.in_ring[(E.in_r + i) % IN_RING]; }
    E.in_r += nin;
    consumed += nin;
    pthread_mutex_unlock(&E.mx);
    int has_eoo = 0;
    int nout = rade_rx(r, feat, &has_eoo, neoo > 0 ? eoo : NULL, blk);
    float pcm[LPCNET_FRAME_SIZE * 16];
    int npcm = 0;
    for (int fi = 0; fi < nout / NB_TOTAL_FEATURES && npcm + LPCNET_FRAME_SIZE <= (int)(sizeof pcm / sizeof pcm[0]); fi++) {
      float *f = &feat[fi * NB_TOTAL_FEATURES];
      if (!ready) {
        memcpy(&cont[cont_n * NB_TOTAL_FEATURES], f, sizeof(float) * NB_TOTAL_FEATURES);
        if (++cont_n >= 5) {
          float packed[5 * NB_FEATURES], zeros[FARGAN_CONT_SAMPLES];
          for (int i = 0; i < 5; i++) {
            memcpy(&packed[i * NB_FEATURES], &cont[i * NB_TOTAL_FEATURES], sizeof(float) * NB_FEATURES);
          }
          memset(zeros, 0, sizeof zeros);
          fargan_cont(&fargan, zeros, packed);
          ready = 1;
        }
        continue;
      }
      fargan_synthesize(&fargan, &pcm[npcm], f);
      npcm += LPCNET_FRAME_SIZE;
    }
    pthread_mutex_lock(&E.mx);
    E.st.blocks++;
    E.st.sync = rade_sync(r);
    E.st.snr_db = rade_snrdB_3k_est(r);
    E.st.foff_hz = rade_freq_offset(r);
    if (C.active) { cap_row(consumed, nout); }
    for (int i = 0; i < npcm; i++) { E.out_ring[(E.out_w + i) % OUT_RING] = pcm[i]; }
    E.out_w += npcm;
  }
  pthread_mutex_unlock(&E.mx);
  free(blk); free(feat); free(eoo);
  return NULL;
}


//////////////////////////////////////////////////////////////////////////////
//
// Capture
//
//////////////////////////////////////////////////////////////////////////////

static void wav_header(FILE *f, int format, int channels, int rate, int bits, uint32_t bytes) {
  uint32_t v;
  uint16_t h;
  fseek(f, 0, SEEK_SET);
  fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
  h = format; fwrite(&h, 2, 1, f);
  h = channels; fwrite(&h, 2, 1, f);
  v = rate; fwrite(&v, 4, 1, f);
  v = rate * channels * bits / 8; fwrite(&v, 4, 1, f);
  h = channels * bits / 8; fwrite(&h, 2, 1, f);
  h = bits; fwrite(&h, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
  fseek(f, 0, SEEK_END);
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

// called with E.mx held
static void ring_put(uint8_t *ring, long size, long *w, long r, const void *src, long len) {
  if (*w - r + len > size) { C.dropped += len; return; }
  for (long i = 0; i < len; i++) { ring[(*w + i) % size] = ((const uint8_t *)src)[i]; }
  *w += len;
}

static void cap_push_iq(const double *buf, int n) {
  float f[2 * 1024];
  for (int done = 0; done < n; done += 1024) {
    int m = n - done < 1024 ? n - done : 1024;
    for (int k = 0; k < 2 * m; k++) { f[k] = (float)buf[2 * done + k]; }
    ring_put(C.iq_ring, CAP_IQ_RING, &C.iq_w, C.iq_r, f, m * 8);
  }
  C.pos48 += n;
}

static void cap_push_speech(const double *buf, int n) {
  int16_t s[1024];
  for (int done = 0; done < n; done += 1024) {
    int m = n - done < 1024 ? n - done : 1024;
    for (int k = 0; k < m; k++) {
      double v = buf[2 * (done + k)] * 32768.0;
      s[k] = (int16_t)(v > 32767.0 ? 32767 : v < -32767.0 ? -32767 : v);
    }
    ring_put(C.sp_ring, CAP_SP_RING, &C.sp_w, C.sp_r, s, m * 2);
  }
}

// called with E.mx held, from the worker
static void cap_row(long consumed, int nout) {
  struct rade_stats rs;
  rade_get_stats(E.r, &rs);
  fprintf(C.csv, "%.4f,%ld,%d,%.2f,%.2f,%d,%ld,%ld,%lu,%lu,%.2f,%.2f,%.2f,%.2f\n",
          consumed / 8000.0, C.pos48, E.st.sync, E.st.snr_db, E.st.foff_hz, nout,
          E.in_w - E.in_r, E.out_w - E.out_r, E.st.underruns, E.st.overruns,
          rs.delta_hat, rs.freq_offset, rs.gain, rs.snr_est);
}

static void drain(int final) {
  static uint8_t tmp[1 << 16];
  for (;;) {
    long n;
    pthread_mutex_lock(&E.mx);
    n = C.iq_w - C.iq_r;
    if (n > (long)sizeof tmp) { n = sizeof tmp; }
    for (long i = 0; i < n; i++) { tmp[i] = C.iq_ring[(C.iq_r + i) % CAP_IQ_RING]; }
    C.iq_r += n;
    pthread_mutex_unlock(&E.mx);
    if (n > 0) { fwrite(tmp, 1, n, C.iq); C.iq_bytes += n; }
    if (n < (long)sizeof tmp) { break; }
  }
  for (;;) {
    long n;
    pthread_mutex_lock(&E.mx);
    n = C.sp_w - C.sp_r;
    if (n > (long)sizeof tmp) { n = sizeof tmp; }
    for (long i = 0; i < n; i++) { tmp[i] = C.sp_ring[(C.sp_r + i) % CAP_SP_RING]; }
    C.sp_r += n;
    pthread_mutex_unlock(&E.mx);
    if (n > 0) { fwrite(tmp, 1, n, C.sp); C.sp_bytes += n; }
    if (n < (long)sizeof tmp) { break; }
  }
  // keep the headers valid at all times, not just at a clean stop
  wav_header(C.iq, 3, 2, 48000, 32, C.iq_bytes);
  wav_header(C.sp, 1, 1, 48000, 16, C.sp_bytes);
  fflush(C.iq); fflush(C.sp);
  pthread_mutex_lock(&E.mx);
  if (C.csv) { fflush(C.csv); }
  if (C.ev) { fflush(C.ev); }
  pthread_mutex_unlock(&E.mx);
  (void)final;
}

static void *cap_writer(void *arg) {
  while (C.run) {
    usleep(200000);
    drain(0);
  }
  return NULL;
}

int rade_native_capture_active(void) { return C.active; }

int rade_native_capture_start(const char *dir, const char *stem, const char *meta) {
  if (C.active || !E.running) { return -1; }
  char path[1024];
  snprintf(C.stem, sizeof C.stem, "%s/%s", dir, stem);
  snprintf(path, sizeof path, "%s_meta.txt", C.stem);
  FILE *m = fopen(path, "w");
  if (!m) { return -1; }
  fputs(meta, m);
  fclose(m);
  snprintf(path, sizeof path, "%s_rx48.wav", C.stem); C.iq = fopen(path, "wb");
  snprintf(path, sizeof path, "%s_speech.wav", C.stem); C.sp = fopen(path, "wb");
  snprintf(path, sizeof path, "%s_stats.csv", C.stem); C.csv = fopen(path, "w");
  snprintf(path, sizeof path, "%s_events.log", C.stem); C.ev = fopen(path, "w");
  if (!C.iq || !C.sp || !C.csv || !C.ev) {
    if (C.iq) { fclose(C.iq); }
    if (C.sp) { fclose(C.sp); }
    if (C.csv) { fclose(C.csv); }
    if (C.ev) { fclose(C.ev); }
    C.iq = C.sp = C.csv = C.ev = NULL;
    return -1;
  }
  C.iq_bytes = C.sp_bytes = 0;
  wav_header(C.iq, 3, 2, 48000, 32, 0);
  wav_header(C.sp, 1, 1, 48000, 16, 0);
  fputs("t_sec,rx48_pos,sync,snr_db,foff_hz,nout,in_fill,out_fill,underruns,overruns,"
        "v2_delta_hat,v2_foff,v2_gain,v2_snr\n", C.csv);
  C.iq_ring = malloc(CAP_IQ_RING);
  C.sp_ring = malloc(CAP_SP_RING);
  C.iq_w = C.iq_r = C.sp_w = C.sp_r = 0;
  C.pos48 = 0; C.dropped = 0;
  C.t0 = now_sec();
  C.run = 1;
  pthread_create(&C.thread, NULL, cap_writer, NULL);
  pthread_mutex_lock(&E.mx);
  C.active = 1;
  pthread_mutex_unlock(&E.mx);
  return 0;
}

void rade_native_capture_event(const char *text) {
  pthread_mutex_lock(&E.mx);
  if (C.active) { fprintf(C.ev, "%.3f %s\n", now_sec() - C.t0, text); }
  pthread_mutex_unlock(&E.mx);
}

void rade_native_capture_stop(const char *end_meta) {
  if (!C.active) { return; }
  pthread_mutex_lock(&E.mx);
  C.active = 0;
  pthread_mutex_unlock(&E.mx);
  C.run = 0;
  pthread_join(C.thread, NULL);
  drain(1);
  char path[1024];
  if (end_meta) {
    snprintf(path, sizeof path, "%s_meta.txt", C.stem);
    FILE *m = fopen(path, "a");
    if (m) { fputs(end_meta, m); fprintf(m, "duration_s: %.1f\nrx48_samples: %ld\ndropped_bytes: %ld\n",
                                         now_sec() - C.t0, C.pos48, C.dropped); fclose(m); }
  }
  fclose(C.iq); fclose(C.sp); fclose(C.csv); fclose(C.ev);
  C.iq = C.sp = C.csv = C.ev = NULL;
  free(C.iq_ring); free(C.sp_ring);
  C.iq_ring = C.sp_ring = NULL;
}

void rade_native_capture_info(char *buf, size_t len) {
  if (!C.active) { snprintf(buf, len, "not capturing"); return; }
  snprintf(buf, len, "%s  %.0f s  %.1f MB%s", strrchr(C.stem, '/') ? strrchr(C.stem, '/') + 1 : C.stem,
           now_sec() - C.t0, (C.iq_bytes + C.sp_bytes) / 1e6, C.dropped ? "  (DROPPED DATA)" : "");
}

static void engine_stop(void) {
  rade_native_capture_stop(NULL);
  if (E.running) {
    pthread_mutex_lock(&E.mx);
    E.running = 0;
    pthread_cond_broadcast(&E.cv);
    pthread_mutex_unlock(&E.mx);
    pthread_join(E.thread, NULL);
  }
  if (E.r) { rade_close(E.r); E.r = NULL; }
  free(E.in_ring); E.in_ring = NULL;
  free(E.out_ring); E.out_ring = NULL;
}

int rade_native_waveform(void) { return E.waveform; }
int rade_native_user_invert(void) { return E.user_invert; }

void rade_native_configure(int waveform, int user_invert) {
  E.user_invert = user_invert != 0;
  if (waveform == E.waveform) { return; }
  engine_stop();
  E.waveform = 0;
  memset(&E.st, 0, sizeof E.st);
  if (waveform == 0) { return; }
  design_filters();
  static int inited = 0;
  if (!inited) { rade_initialize(); inited = 1; }
  int flags = RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0;
  if (waveform == RADE_NATIVE_V2) { flags |= RADE_MODE_V2; }
  E.r = rade_open("", flags);
  if (!E.r) { return; }
  E.nin_max = rade_nin_max(E.r);
  E.nfeat = rade_n_features_in_out(E.r);
  E.in_ring = calloc(IN_RING, sizeof(RADE_COMP));
  E.out_ring = calloc(OUT_RING, sizeof(float));
  E.in_w = E.in_r = E.out_w = E.out_r = 0;
  E.playing = 0;
  E.prebuffer = waveform == RADE_NATIVE_V2 ? PREBUFFER_V2 : PREBUFFER_V1;
  memset(E.hist_i, 0, sizeof E.hist_i); memset(E.hist_q, 0, sizeof E.hist_q);
  memset(E.ihist, 0, sizeof E.ihist);
  E.hist_pos = E.dec_phase = E.ihist_pos = E.int_phase = 0;
  E.pwr = 0.0;
  E.waveform = waveform;
  E.st.waveform = waveform;
  E.running = 1;
  pthread_create(&E.thread, NULL, worker, NULL);
}

void rade_native_get_status(rade_native_status_t *st) {
  pthread_mutex_lock(&E.mx);
  *st = E.st;
  pthread_mutex_unlock(&E.mx);
}

//
// Called by the receiver thread with one block of WDSP output.
//
void rade_native_rx_block(double *buf, int n, int invert, double gain) {
  pthread_mutex_lock(&E.mx);
  if (!E.running) {
    pthread_mutex_unlock(&E.mx);
    memset(buf, 0, sizeof(double) * 2 * n);
    return;
  }
  if (C.active) { cap_push_iq(buf, n); }
  const double sq = rade_native_conjugate(invert) ? -1.0 : 1.0;   // sign of Q
  // 1. decimate to 8 kHz, normalise, queue
  RADE_COMP out8[n / DEC + 2];
  int n8 = 0;
  for (int i = 0; i < n; i++) {
    E.hist_i[E.hist_pos] = buf[2 * i];
    E.hist_q[E.hist_pos] = sq * buf[2 * i + 1];
    E.hist_pos = (E.hist_pos + 1) % DEC_TAPS;
    if (++E.dec_phase == DEC) {
      E.dec_phase = 0;
      double si = 0.0, sqq = 0.0;
      for (int k = 0; k < DEC_TAPS; k++) {
        int idx = (E.hist_pos - 1 - k + 2 * DEC_TAPS) % DEC_TAPS;
        si += E.dec_h[k] * E.hist_i[idx];
        sqq += E.dec_h[k] * E.hist_q[idx];
      }
      // slow level normaliser, ~0.5 s
      double p = si * si + sqq * sqq;
      E.pwr += (p - E.pwr) * (1.0 / 4000.0);
      double g = TARGET_RMS / sqrt(E.pwr + 1e-18);
      if (g > 1e6) { g = 1e6; }
      out8[n8].real = (float)(si * g);
      out8[n8].imag = (float)(sqq * g);
      n8++;
    }
  }
  if (E.in_w - E.in_r + n8 > IN_RING) {
    E.st.overruns += n8;
  } else {
    for (int i = 0; i < n8; i++) { E.in_ring[(E.in_w + i) % IN_RING] = out8[i]; }
    E.in_w += n8;
    pthread_cond_signal(&E.cv);
  }
  // 2. speech out: wait for a cushion, trim if the cushion grows too deep
  long avail = E.out_w - E.out_r;
  if (!E.playing && avail >= E.prebuffer) { E.playing = 1; }
  if (avail > E.prebuffer + MAX_EXTRA) { E.out_r = E.out_w - E.prebuffer; }
  for (int i = 0; i < n; i++) {
    if (E.int_phase == 0) {
      double s = 0.0;
      if (E.playing && E.out_r < E.out_w) {
        s = E.out_ring[E.out_r % OUT_RING];
        E.out_r++;
      } else if (E.playing) {
        E.st.underruns++;
        E.playing = 0;            // re-buffer
      }
      E.ihist[E.ihist_pos] = s;
      E.ihist_pos = (E.ihist_pos + 1) % (INT_TAPS / INT);
      for (int ph = 0; ph < INT; ph++) {
        double y = 0.0;
        for (int k = 0; k < INT_TAPS / INT; k++) {
          int idx = (E.ihist_pos - 1 - k + 2 * (INT_TAPS / INT)) % (INT_TAPS / INT);
          y += E.int_h[ph + INT * k] * E.ihist[idx];
        }
        E.ybuf[ph] = y;
      }
    }
    double y = gain * E.ybuf[E.int_phase];
    buf[2 * i] = y;
    buf[2 * i + 1] = y;
    E.int_phase = (E.int_phase + 1) % INT;
  }
  if (C.active) { cap_push_speech(buf, n); }
  pthread_mutex_unlock(&E.mx);
}
