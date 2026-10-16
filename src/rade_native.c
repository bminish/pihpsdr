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
#include "rade_eoo_text.h"
#include "rade_v2_text.h"
#include "rade_native.h"
#include "rade_native_log.h"

double rade_native_centre(int waveform) {
  return waveform == RADE_NATIVE_V2 ? RADE_NATIVE_FC_V2 : RADE_NATIVE_FC_V1;
}

double rade_native_width(int waveform) {
  return waveform == RADE_NATIVE_V2 ? RADE_NATIVE_BW_V2 : RADE_NATIVE_BW_V1;
}

int rade_native_band_class(long long dial_hz) {
  if (dial_hz >= RADE_NATIVE_60M_LOW_HZ && dial_hz <= RADE_NATIVE_60M_HIGH_HZ) { return 1; }
  return dial_hz < RADE_NATIVE_V1_INV_BELOW_HZ ? 0 : 2;
}

int rade_native_default_invert(int waveform, long long dial_hz) {
  // V1 below 10 MHz is lower sideband, V2 and everything above 10 MHz upper; on 60 m, upper for both
  return waveform == RADE_NATIVE_V1 && rade_native_band_class(dial_hz) == 0;
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

static int stored_invert = 0;
int rade_native_get_invert(void) { return stored_invert; }
void rade_native_set_invert(int invert) { stored_invert = invert != 0; }

rade_native_plan_t rade_native_plan(int old_wave, int old_invert, int mode_sign, long long dial,
                                    int new_wave, int user_invert) {
  rade_native_plan_t p;
  int old_sign = old_wave ? (old_invert ? -1 : 1) : mode_sign;
  long long fe = dial + (long long)old_sign * RADE_NATIVE_DIAL_OFFSET;
  if (new_wave == 0) {
    p.invert = old_wave ? old_invert : (mode_sign < 0);
    p.dial = dial;                           // already on the carrier of that sideband
    return p;
  }
  p.invert = rade_native_effective_invert(new_wave, fe, user_invert);
  p.dial = fe - (long long)(p.invert ? -1 : 1) * RADE_NATIVE_DIAL_OFFSET;
  return p;
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
#define CAP_TX_RING    (1 << 24)         // 16 MB: 192 kHz complex float is 1.5 MB/s
#define CAP_MIC_RING   (1 << 20)

static struct {
  int active, run;
  char stem[512];
  FILE *iq, *sp, *csv, *ev;
  uint32_t iq_bytes, sp_bytes;
  uint8_t *iq_ring, *sp_ring, *tx_ring, *mic_ring;
  long iq_w, iq_r, sp_w, sp_r, tx_w, tx_r, mic_w, mic_r;
  FILE *txf, *micf;                      // opened by the writer when data comes
  uint32_t tx_bytes, mic_bytes;
  int tx_rate;
  // when each block of the TX capture reached us (unix seconds) and how many TX samples had been offered before it, for the
  // GPS timing analysis: the n-th sample of _tx.wav left WDSP at about the time of the block it is in
  double *txt_unix;
  long *txt_idx;
  long txt_n, txt_samples;
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
  float snr_ema, foff_last;              // last in-sync values, for the call log
  int snr_n;                             // in-sync SNR estimates behind snr_ema (0: none, the value means nothing)
  long nosync;                           // 8 kHz samples since sync was last seen
  rade_native_call_t calls[16];
  int ncalls;
  double last_call_t;
  char last_call[40];
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
  rade_v2_text_rx_t *vrx = E.waveform == RADE_NATIVE_V2 ? malloc(sizeof *vrx) : NULL;   // the V2 callsign decoder
  if (vrx) { rade_v2_text_rx_init(vrx); }
  long v2_last_pos = -1;                                         // input position of the last symbol fed
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
    char v2_call[RADE_V2_TEXT_MAXCHARS + 1];
    int v2_got = 0;
    if (vrx && nout > 0 && rade_sync(r)) {                     // one data symbol per modem frame, only with lock
      // lock is the gate: only symbols received with lock are fed, with the frames missed since the last one (320 samples
      // each); rade_v2_text_rx_feed decides whether a new lock continues the same sender's cycle or starts a new reception
      long gap = v2_last_pos >= 0 ? lround((double)(consumed - v2_last_pos) / 320.0) - 1 : 0;
      v2_last_pos = consumed;
      int resets = vrx->resets;
      v2_got = rade_v2_text_rx_feed(vrx, rade_rx_get_data_symbol(r), gap > 0 ? (int)gap : 0, v2_call);
      if (vrx->resets != resets) { E.last_call[0] = 0; }   // a new reception: its call is reported again
    }
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
    if (E.st.sync) {
      // a new reception (sync back after 1.5 s without) starts the average afresh; an estimate with no lock is never used
      E.snr_ema = E.snr_n == 0 ? E.st.snr_db : 0.9f * E.snr_ema + 0.1f * E.st.snr_db;
      if (E.snr_n < 1000000) { E.snr_n++; }
      E.foff_last = E.st.foff_hz;
      E.nosync = 0;
    } else {
      E.nosync += nin;
      if (E.nosync > 12000) { E.snr_n = 0; }
    }
    if (v2_got && E.st.sync) {
      // the call arrives once per pass through the sender's cycle; it is passed on when it is new in this reception
      if (strcmp(v2_call, E.last_call) != 0 && E.ncalls < 16) {
        rade_native_call_t *c = &E.calls[E.ncalls++];
        snprintf(c->call, sizeof c->call, "%s", v2_call);
        c->t_unix = (double)time(NULL);
        c->snr_db = E.snr_ema;
        c->snr_valid = E.snr_n > 0;
        c->foff_hz = E.foff_last;
        c->waveform = E.waveform;
        snprintf(E.last_call, sizeof E.last_call, "%s", v2_call);
      }
    }
    if (has_eoo && neoo > 0) {
      char call[16];
      if (rade_eoo_text_decode(eoo, neoo / 2, call)) {
        double t = (double)time(NULL);
        // every call decoded is passed on, even the same one again; only the same frame flagged twice
        // within a second is taken for one
        if (strcmp(call, E.last_call) != 0 || t - E.last_call_t >= 1.0) {
          if (E.ncalls < 16) {
            rade_native_call_t *c = &E.calls[E.ncalls++];
            snprintf(c->call, sizeof c->call, "%s", call);
            c->t_unix = t;
            c->snr_db = E.snr_ema;
            c->snr_valid = E.snr_n > 0;
            c->foff_hz = E.foff_last;
            c->waveform = E.waveform;
          }
          snprintf(E.last_call, sizeof E.last_call, "%s", call);
        }
        E.last_call_t = t;
      }
    }
    if (C.active) { cap_row(consumed, nout); }
    // Parameter log: with a lock every call is logged; without one, the change to it and then one row a second (nothing happens in
    // between that is worth a row: the SNR and offset are not measurements without a lock)
    static int log_prev_sync = -1;
    static long log_nosync_next = 0;
    int log_it = 1;
    if (!E.st.sync && log_prev_sync == 0 && consumed < log_nosync_next) { log_it = 0; }
    if (!E.st.sync && (log_prev_sync != 0 || consumed >= log_nosync_next)) { log_nosync_next = consumed + 8000; }
    log_prev_sync = E.st.sync ? 1 : 0;
    if (log_it && rade_log_active()) {
      struct rade_stats rs;
      rade_get_stats(r, &rs);
      rade_log_rx_t lr = { .rx_pos = consumed, .sync = E.st.sync, .eoo = has_eoo != 0, .nout = nout, .snr_db = E.st.snr_db,
                           .foff_hz = E.st.foff_hz, .delta_hat = rs.delta_hat, .est_foff = rs.freq_offset,
                           .gain = rs.gain, .snr_est = rs.snr_est, .in_fill = E.in_w - E.in_r,
                           .out_fill = E.out_w - E.out_r, .underruns = E.st.underruns, .overruns = E.st.overruns };
      rade_log_rx(&lr);
    }
    for (int i = 0; i < npcm; i++) { E.out_ring[(E.out_w + i) % OUT_RING] = pcm[i]; }
    E.out_w += npcm;
  }
  pthread_mutex_unlock(&E.mx);
  free(blk); free(feat); free(eoo); free(vrx);
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

#define CAP_TXT_MAX 200000                  // blocks: over an hour

void rade_native_capture_tx(const double *iq, int n, int rate) {
  pthread_mutex_lock(&E.mx);
  if (C.active) {
    C.tx_rate = rate;
    if (C.txt_unix && C.txt_n < CAP_TXT_MAX) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      C.txt_unix[C.txt_n] = ts.tv_sec + ts.tv_nsec * 1e-9;
      C.txt_idx[C.txt_n++] = C.txt_samples;
    }
    C.txt_samples += n;
    float f[2 * 1024];
    for (int done = 0; done < n; done += 1024) {
      int m = n - done < 1024 ? n - done : 1024;
      for (int k = 0; k < 2 * m; k++) { f[k] = (float)iq[2 * done + k]; }
      ring_put(C.tx_ring, CAP_TX_RING, &C.tx_w, C.tx_r, f, m * 8);
    }
  }
  pthread_mutex_unlock(&E.mx);
}

// called with E.mx held
static void cap_push_mic(const int16_t *pcm, int n) {
  if (C.active) { ring_put(C.mic_ring, CAP_MIC_RING, &C.mic_w, C.mic_r, pcm, n * 2); }
}

static long take(uint8_t *ring, long size, long *r, long w, uint8_t *dst, long max) {
  long n = w - *r;
  if (n > max) { n = max; }
  for (long i = 0; i < n; i++) { dst[i] = ring[(*r + i) % size]; }
  *r += n;
  return n;
}

static void drain_tx(void) {
  static uint8_t tmp[1 << 16];
  for (;;) {
    pthread_mutex_lock(&E.mx);
    long n = take(C.tx_ring, CAP_TX_RING, &C.tx_r, C.tx_w, tmp, sizeof tmp);
    int rate = C.tx_rate;
    pthread_mutex_unlock(&E.mx);
    if (n > 0 && !C.txf) {
      char path[1024];
      snprintf(path, sizeof path, "%s_tx.wav", C.stem);
      C.txf = fopen(path, "wb");
      if (C.txf) { wav_header(C.txf, 3, 2, rate, 32, 0); }
    }
    if (n > 0 && C.txf) { fwrite(tmp, 1, n, C.txf); C.tx_bytes += n; }
    if (n < (long)sizeof tmp) { break; }
  }
  for (;;) {
    pthread_mutex_lock(&E.mx);
    long n = take(C.mic_ring, CAP_MIC_RING, &C.mic_r, C.mic_w, tmp, sizeof tmp);
    pthread_mutex_unlock(&E.mx);
    if (n > 0 && !C.micf) {
      char path[1024];
      snprintf(path, sizeof path, "%s_txmic.wav", C.stem);
      C.micf = fopen(path, "wb");
      if (C.micf) { wav_header(C.micf, 1, 1, 16000, 16, 0); }
    }
    if (n > 0 && C.micf) { fwrite(tmp, 1, n, C.micf); C.mic_bytes += n; }
    if (n < (long)sizeof tmp) { break; }
  }
  if (C.txf) { wav_header(C.txf, 3, 2, C.tx_rate, 32, C.tx_bytes); fflush(C.txf); }
  if (C.micf) { wav_header(C.micf, 1, 1, 16000, 16, C.mic_bytes); fflush(C.micf); }
}

static void drain(int final) {
  drain_tx();
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
  C.tx_ring = malloc(CAP_TX_RING);
  C.mic_ring = malloc(CAP_MIC_RING);
  C.iq_w = C.iq_r = C.sp_w = C.sp_r = C.tx_w = C.tx_r = C.mic_w = C.mic_r = 0;
  C.tx_bytes = C.mic_bytes = 0;
  C.txt_unix = malloc(sizeof(double) * CAP_TXT_MAX);
  C.txt_idx = malloc(sizeof(long) * CAP_TXT_MAX);
  C.txt_n = C.txt_samples = 0;
  C.txf = C.micf = NULL;
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
  if (C.txt_unix && C.txt_n > 0) {            // _txblocks.csv: when each TX block arrived, for lining _tx.wav up with the clock
    snprintf(path, sizeof path, "%s_txblocks.csv", C.stem);
    FILE *m = fopen(path, "w");
    if (m) {
      fprintf(m, "unix,sample_index,rate\n");
      for (long i = 0; i < C.txt_n; i++) { fprintf(m, "%.6f,%ld,%d\n", C.txt_unix[i], C.txt_idx[i], C.tx_rate); }
      fclose(m);
    }
  }
  free(C.txt_unix); free(C.txt_idx);
  C.txt_unix = NULL; C.txt_idx = NULL;
  fclose(C.iq); fclose(C.sp); fclose(C.csv); fclose(C.ev);
  if (C.txf) { fclose(C.txf); }
  if (C.micf) { fclose(C.micf); }
  C.iq = C.sp = C.csv = C.ev = C.txf = C.micf = NULL;
  free(C.iq_ring); free(C.sp_ring); free(C.tx_ring); free(C.mic_ring);
  C.iq_ring = C.sp_ring = C.tx_ring = C.mic_ring = NULL;
}

const char *rade_native_capture_stem(void) {
  if (!C.active) { return ""; }
  const char *slash = strrchr(C.stem, '/');
  return slash ? slash + 1 : C.stem;
}

void rade_native_capture_info(char *buf, size_t len) {
  if (!C.active) { snprintf(buf, len, "not capturing"); return; }
  snprintf(buf, len, "%s  %.0f s  %.1f MB%s", strrchr(C.stem, '/') ? strrchr(C.stem, '/') + 1 : C.stem,
           now_sec() - C.t0, (C.iq_bytes + C.sp_bytes + C.tx_bytes + C.mic_bytes) / 1e6,
           C.dropped ? "  (DROPPED DATA)" : "");
}

static void tx_stop(void);

static void engine_stop(void) {
  rade_native_capture_stop(NULL);
  tx_stop();
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

//////////////////////////////////////////////////////////////////////////////
//
// Transmit engine
//
//////////////////////////////////////////////////////////////////////////////

#define TX_DEC         3                 // 48 kHz mic -> 16 kHz
#define TX_DEC_TAPS    36
#define TX_INT         6                 // 8 kHz modem -> 48 kHz
#define TX_INT_TAPS    48
#define TX_PCM_RING    8192              // 16 kHz int16
#define TX_OUT_RING    65536             // 48 kHz complex
#define TX_LEVEL_V1    0.357             // modem amplitude into WDSP: peaks at the ALC ceiling (offline), see alc_sweep.py
#define TX_LEVEL_V2    0.504             // V2's peaks are lower for the same rms, so it needs 3 dB more than V1
#define LV_BLOCK       4800              // 100 ms at 48 kHz
#define TX_TAIL        5120              // 48 kHz samples held after the EOO (107 ms; was 8000 = 167 ms). Measured on the air: the last EOO sample
                                         // is off WDSP 78 ms later and the RF is over 85 ms after it, so this leaves about 20 ms

static struct {
  int enabled, running, flush;
  struct rade *r;
  LPCNetEncState *enc;
  pthread_t thread;
  pthread_mutex_t mx;
  pthread_cond_t cv;
  int16_t pcm[TX_PCM_RING];
  long pcm_w, pcm_r;
  RADE_COMP out[TX_OUT_RING];
  long out_w, out_r;
  double dec_h[TX_DEC_TAPS], int_h[TX_INT_TAPS];
  double dec_hist[TX_DEC_TAPS];
  int dec_pos, dec_phase;
  double ihi[TX_INT_TAPS / TX_INT], ihq[TX_INT_TAPS / TX_INT];   // interpolator history
  int ipos;
  FILE *featdump;
  unsigned long underruns;
  // worker-only
  float *feat;
  RADE_COMP *mod;
  int nfeat, nmod, fidx;
  // end of over
  int active;                            // transmitting since the last reset
  int started;                           // the prefill is in, output is flowing
  int end_req;                           // asked for, not yet generated
  int end_state;                         // 0 running, 1 ending, 2 done
  long eoo_end_pos;                      // out_w after the EOO, -1 until generated
  long tail;                             // samples delivered since then
  char call[16];
  int v2;                                // this engine is V2: the callsign goes on the data symbol channel
  rade_v2_text_t text;                   // the stream for text_call (worker thread only)
  char text_call[16];
  // mic level
  double trim_db, trim_g;                // asked for / smoothed linear
  double drive_db;                       // level into WDSP relative to the defined one, -6 .. +6
  int keyed;                             // transmitting, for the leveller and the log
  // test harness: a file of speech standing in for the mic
  float *inj;                            // 48 kHz mono, owned here
  long inj_n, inj_pos;
  int inj_on;                            // armed: consumed only while keyed, zeros before
  double inj_t0, inj_t1;                 // unix time of the first and the last sample consumed
  // event times of the over, unix seconds (0 = not yet), for lining the engine up with what was heard off air
  double ev_key, ev_end_req, ev_eoo_gen, ev_eoo_out, ev_done, ev_unkey, ev_out0;
  double pk_ref;                         // loudest speech this over (400 ms, before the leveller's gain), LUFS
  int log_blocks;                        // 100 ms blocks since the loudness average was last logged
  int agc_on;
  double agc_db;                         // leveller gain, dB
  double k1x[2], k1y[2], k2x[2], k2y[2]; // K-weighting filter state
  double blk_ms[100];                    // mean square of each 100 ms block, last 10 s
  int blk_n, blk_pos;                    // blocks held, next slot
  double acc_ms;                         // accumulating block
  int acc_n;
  double peak_acc, peak_hold;            // peak of this block / of the last second
  int limit_hold;                        // blocks since the limiter acted
  int pk_blocks;
} T = { .mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .agc_on = 1, .trim_g = 1.0 };

static double ev_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void tx_design(void) {
  for (int k = 0; k < TX_DEC_TAPS; k++) {
    double m = k - 0.5 * (TX_DEC_TAPS - 1), fc = 7200.0 / 48000.0;
    double sinc = fabs(m) < 1e-9 ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
    double w = 0.42 - 0.5 * cos(2.0 * M_PI * k / (TX_DEC_TAPS - 1)) + 0.08 * cos(4.0 * M_PI * k / (TX_DEC_TAPS - 1));
    T.dec_h[k] = sinc * w;
  }
  for (int k = 0; k < TX_INT_TAPS; k++) {
    double m = k - 0.5 * (TX_INT_TAPS - 1), fc = 3900.0 / 48000.0;
    double sinc = fabs(m) < 1e-9 ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
    double w = 0.42 - 0.5 * cos(2.0 * M_PI * k / (TX_INT_TAPS - 1)) + 0.08 * cos(4.0 * M_PI * k / (TX_INT_TAPS - 1));
    T.int_h[k] = TX_INT * sinc * w;
  }
}

// Interpolate 8 kHz modem samples to 48 kHz and queue them. Called without
// T.mx held, from the worker only (which owns the interpolator history).
static void tx_emit(const RADE_COMP *mod, int n) {
  static RADE_COMP up[1152 * TX_INT];
  const int per = TX_INT_TAPS / TX_INT;
  int nup = 0;
  const double lvl = (E.waveform == RADE_NATIVE_V2 ? TX_LEVEL_V2 : TX_LEVEL_V1) * pow(10.0, T.drive_db / 20.0);
  for (int i = 0; i < n && nup + TX_INT <= (int)(sizeof up / sizeof up[0]); i++) {
    T.ihi[T.ipos] = mod[i].real * lvl;
    T.ihq[T.ipos] = mod[i].imag * lvl;
    T.ipos = (T.ipos + 1) % per;
    for (int ph = 0; ph < TX_INT; ph++) {
      double yi = 0.0, yq = 0.0;
      for (int k = 0; k < per; k++) {
        int idx = (T.ipos - 1 - k + 2 * per) % per;
        yi += T.int_h[ph + TX_INT * k] * T.ihi[idx];
        yq += T.int_h[ph + TX_INT * k] * T.ihq[idx];
      }
      up[nup].real = (float)yi;
      up[nup].imag = (float)yq;
      nup++;
    }
  }
  pthread_mutex_lock(&T.mx);
  if (!T.flush) {
    for (int i = 0; i < nup && T.out_w - T.out_r < TX_OUT_RING; i++) {
      T.out[T.out_w % TX_OUT_RING] = up[i];
      T.out_w++;
    }
  }
  pthread_mutex_unlock(&T.mx);
}

// One 10 ms speech frame in; a modem frame out when enough have arrived.
static void tx_frame(const int16_t *pcm) {
  lpcnet_compute_single_frame_features(T.enc, (int16_t *)pcm, &T.feat[T.fidx * NB_TOTAL_FEATURES], 0);
  if (++T.fidx * NB_TOTAL_FEATURES == T.nfeat) {
    T.fidx = 0;
    if (T.featdump) { fwrite(T.feat, sizeof(float), T.nfeat, T.featdump); }
    if (T.v2) {
      char c[16];
      pthread_mutex_lock(&T.mx);
      memcpy(c, T.call, sizeof c);
      pthread_mutex_unlock(&T.mx);
      if (strcmp(c, T.text_call) != 0) {             // a new callsign: a new stream, from the start
        memcpy(T.text_call, c, sizeof c);
        rade_v2_text_init(&T.text, c);
      }
      rade_tx_set_data_symbol(T.r, rade_v2_text_next(&T.text));
    }
    int n = rade_tx(T.r, T.mod, T.feat);
    tx_emit(T.mod, n);
  }
}

// The end of an over: what speech is left, silence to complete the modem
// frame, then the EOO frame with the callsign in it (V1).
static void tx_finish(void) {
  int16_t zeros[LPCNET_FRAME_SIZE] = {0};
  while (T.fidx != 0) { tx_frame(zeros); }
  int ne = rade_n_tx_eoo_out(T.r);
  RADE_COMP *eoo = malloc(sizeof(RADE_COMP) * ne);
  int nb = rade_n_eoo_bits(T.r);
  if (nb > 0 && T.call[0]) {
    float *bits = calloc(nb, sizeof(float));
    if (rade_eoo_text_encode(T.call, bits, nb) == 0) { rade_tx_set_eoo_bits(T.r, bits); }
    free(bits);
  }
  int n = rade_tx_eoo(T.r, eoo);
  tx_emit(eoo, n);
  free(eoo);
}

static void *tx_worker(void *arg) {
  T.nfeat = rade_n_features_in_out(T.r);
  T.nmod = rade_n_tx_out(T.r);
  T.feat = malloc(sizeof(float) * T.nfeat);
  T.mod = malloc(sizeof(RADE_COMP) * T.nmod);
  T.fidx = 0;
  pthread_mutex_lock(&T.mx);
  while (T.running) {
    if (T.flush) {
      T.flush = 0; T.pcm_r = T.pcm_w; T.out_r = T.out_w; T.fidx = 0;
      memset(T.ihi, 0, sizeof T.ihi); memset(T.ihq, 0, sizeof T.ihq); T.ipos = 0;
    }
    int16_t pcm[LPCNET_FRAME_SIZE];
    if (T.pcm_w - T.pcm_r >= LPCNET_FRAME_SIZE) {
      for (int i = 0; i < LPCNET_FRAME_SIZE; i++) { pcm[i] = T.pcm[(T.pcm_r + i) % TX_PCM_RING]; }
      T.pcm_r += LPCNET_FRAME_SIZE;
      pthread_mutex_unlock(&T.mx);
      tx_frame(pcm);
      pthread_mutex_lock(&T.mx);
      continue;
    }
    if (T.end_req) {
      // all queued speech has been processed (the branch above), finish the over
      T.end_req = 0;
      pthread_mutex_unlock(&T.mx);
      tx_finish();
      pthread_mutex_lock(&T.mx);
      T.eoo_end_pos = T.out_w;
      T.ev_eoo_gen = ev_now();
      continue;
    }
    pthread_cond_wait(&T.cv, &T.mx);
  }
  pthread_mutex_unlock(&T.mx);
  free(T.feat); free(T.mod);
  T.feat = NULL; T.mod = NULL;
  return NULL;
}

int rade_native_tx_enabled(void) { return T.enabled; }

void rade_native_tx_feature_dump(const char *path) {
  pthread_mutex_lock(&T.mx);
  if (T.featdump) { fclose(T.featdump); T.featdump = NULL; }
  if (path) { T.featdump = fopen(path, "wb"); }
  pthread_mutex_unlock(&T.mx);
}

static void tx_stop(void) {
  if (T.running) {
    pthread_mutex_lock(&T.mx);
    T.running = 0;
    pthread_cond_broadcast(&T.cv);
    pthread_mutex_unlock(&T.mx);
    pthread_join(T.thread, NULL);
  }
  if (T.r) { rade_close(T.r); T.r = NULL; }
  if (T.enc) { lpcnet_encoder_destroy(T.enc); T.enc = NULL; }
  T.enabled = 0;
}

void rade_native_tx_enable(int on) {
  if (!on) { tx_stop(); return; }
  if (T.enabled || E.waveform == 0) { return; }
  tx_design();
  static int inited = 0;
  if (!inited) { rade_initialize(); inited = 1; }
  int flags = RADE_USE_C_ENCODER | RADE_VERBOSE_0;
  if (E.waveform == RADE_NATIVE_V2) { flags |= RADE_MODE_V2; }
  T.r = rade_open("", flags);
  T.enc = lpcnet_encoder_create();
  if (!T.r || !T.enc) { tx_stop(); return; }
  T.nmod = rade_n_tx_out(T.r);
  T.v2 = (E.waveform == RADE_NATIVE_V2);
  T.text_call[0] = 1;                    // differs from any callsign: the stream is built on the first frame
  T.pcm_w = T.pcm_r = T.out_w = T.out_r = 0;
  memset(T.dec_hist, 0, sizeof T.dec_hist);
  T.dec_pos = T.dec_phase = 0;
  memset(T.ihi, 0, sizeof T.ihi); memset(T.ihq, 0, sizeof T.ihq); T.ipos = 0;
  T.flush = 0;
  T.active = 0; T.started = 0; T.end_req = 0; T.end_state = 0; T.eoo_end_pos = -1; T.tail = 0;
  T.underruns = 0;
  memset(T.k1x, 0, sizeof T.k1x); memset(T.k1y, 0, sizeof T.k1y);
  memset(T.k2x, 0, sizeof T.k2x); memset(T.k2y, 0, sizeof T.k2y);
  memset(T.blk_ms, 0, sizeof T.blk_ms);
  T.blk_n = T.blk_pos = T.acc_n = T.pk_blocks = 0;
  T.acc_ms = T.peak_acc = T.peak_hold = 0.0;
  T.limit_hold = 1000;
  T.agc_db = 0.0;
  T.trim_g = pow(10.0, T.trim_db / 20.0);
  T.running = 1;
  T.enabled = 1;
  pthread_create(&T.thread, NULL, tx_worker, NULL);
}


//
// Mic level: trim, leveller, limiter and BS.1770 loudness, at 48 kHz.
// Called with T.mx held, for every mic block, transmitting or not.
//
static double lv_db(double ms) { return -0.691 + 10.0 * log10(ms + 1e-20); }

// gated loudness over the blocks held: windows of 4 blocks (400 ms), absolute gate
// -70 LUFS, relative gate 10 LU below the mean of what passed the first
static int lv_gated(double *avg, double *mom) {
  int n = T.blk_n;
  if (n < 4) { return 0; }
  double w[100];
  int nw = 0;
  for (int i = 0; i + 4 <= n; i++) {
    double sum = 0.0;
    for (int k = 0; k < 4; k++) {
      int idx = (T.blk_pos - n + i + k + 200) % 100;
      sum += T.blk_ms[idx];
    }
    w[nw++] = sum / 4.0;
  }
  *mom = lv_db(w[nw - 1]);
  double s1 = 0.0;
  int n1 = 0;
  for (int i = 0; i < nw; i++) { if (lv_db(w[i]) > -70.0) { s1 += w[i]; n1++; } }
  if (n1 == 0) { return 0; }
  double rel = lv_db(s1 / n1) - 10.0;
  double s2 = 0.0;
  int n2 = 0;
  for (int i = 0; i < nw; i++) { if (lv_db(w[i]) > -70.0 && lv_db(w[i]) > rel) { s2 += w[i]; n2++; } }
  if (n2 < 8) { return 0; }                       // under 0.8 s of speech: too little to say
  *avg = lv_db(s2 / n2);
  return 1;
}

#define LV_MAX_DB   20.0                 // the leveller adds at most this ...
#define LV_MIN_DB  (-30.0)               // ... and takes off up to this
#define LV_FLOOR_LUFS (-35.0)            // speech is measured only above this, before the leveller's gain

// one row of the parameter log per 100 ms block; the gated average once a second
static void lv_log_row(void) {
  if (!rade_log_active() || !T.keyed) { return; }          // transmit rows only while transmitting
  double avg = NAN, mom = NAN, a;
  if (T.blk_n >= 4) {
    double sum = 0.0;
    for (int k = 0; k < 4; k++) { sum += T.blk_ms[(T.blk_pos - 1 - k + 200) % 100]; }
    mom = lv_db(sum / 4.0);
  }
  if (++T.log_blocks >= 10) {
    T.log_blocks = 0;
    double m;
    if (lv_gated(&a, &m)) { avg = a; }
  }
  rade_log_tx_t l = { .tx = T.keyed, .trim_db = T.trim_db, .agc_db = T.agc_on ? T.agc_db : 0.0,
                      .drive_db = T.drive_db, .loud_mom = isnan(mom) ? -99.0 : mom, .loud_avg = avg,
                      .peak_dbfs = 20.0 * log10((T.peak_hold > T.peak_acc ? T.peak_hold : T.peak_acc) + 1e-9),
                      .limiting = T.limit_hold < 10, .underruns = T.underruns,
                      .out_fill = T.out_w - T.out_r, .pcm_fill = T.pcm_w - T.pcm_r, .end_state = T.end_state };
  rade_log_tx(&l);
}

static void lv_block_done(void) {
  T.blk_ms[T.blk_pos] = T.acc_ms / LV_BLOCK;
  T.blk_pos = (T.blk_pos + 1) % 100;
  if (T.blk_n < 100) { T.blk_n++; }
  T.acc_ms = 0.0;
  T.acc_n = 0;
  if (T.peak_acc > T.peak_hold || ++T.pk_blocks >= 10) { T.peak_hold = T.peak_acc; T.pk_blocks = 0; }
  T.peak_acc = 0.0;
  if (T.limit_hold < 1000) { T.limit_hold++; }
  //
  // The leveller changes its gain only while transmitting, and only on speech: the 400 ms loudness, before
  // this gain, must be above LV_FLOOR_LUFS (a mic's hiss and the room are below it), within 15 LU of the
  // loudest speech of this over and within 10 LU of the speech average, so a pause - even one with the room
  // still audible, and even one longer than the 10 s average - never raises the gain. In every
  // other case the gain is held where it is, through receive and from one over to the next. It can add at
  // most 20 dB and take off 30, moving like a slow hand on a fader.
  //
  if (!T.keyed || !T.agc_on) {
    lv_log_row();
    return;
  }
  double avg, mom;
  int ok = lv_gated(&avg, &mom);
  double pre = mom - T.agc_db;                    // the mic itself, before the gain
  if (!ok) { pre = -200.0; }
  if (pre > T.pk_ref) { T.pk_ref = pre; } else { T.pk_ref -= 0.003; }    // forgets 0.03 dB/s
  if (ok && pre >= LV_FLOOR_LUFS && pre >= T.pk_ref - 15.0 && mom >= avg - 10.0) {
    double step = 0.0;
    if (mom > RADE_NATIVE_TARGET_LUFS + 8.0) {
      step = -0.2;                                // a shout: down 2 dB/s
    } else {
      double e = RADE_NATIVE_TARGET_LUFS - avg;
      if (e > 1.5) { step = e > 12 ? 0.05 : e > 6 ? 0.03 : 0.02; }   // too low: up, 0.2 to 0.5 dB/s
      else if (e < -1.5) { step = -0.05; }        // too high: down 0.5 dB/s
    }
    T.agc_db += step;
    if (T.agc_db > LV_MAX_DB) { T.agc_db = LV_MAX_DB; }
    if (T.agc_db < LV_MIN_DB) { T.agc_db = LV_MIN_DB; }
  }
  lv_log_row();
}

static void lv_process(double *buf, int n) {
  static const double b1[3] = {1.53512485958697, -2.69169618940638, 1.19839281085285};
  static const double a1[2] = {-1.69065929318241, 0.73248077421585};
  static const double a2[2] = {-1.99004745483398, 0.99007225036621};
  double target = pow(10.0, T.trim_db / 20.0);
  double ga = pow(10.0, (T.agc_on ? T.agc_db : 0.0) / 20.0);
  for (int i = 0; i < n; i++) {
    T.trim_g += (target - T.trim_g) * 0.002;      // ~10 ms, no zipper noise
    double x = buf[2 * i] * T.trim_g * ga;
    double ax = fabs(x);
    if (ax > T.peak_acc) { T.peak_acc = ax; }
    if (ax > 0.9) {                               // gentle limiter: 0.9 .. 1.0, never past full scale
      x = (x < 0 ? -1.0 : 1.0) * (0.9 + 0.1 * tanh((ax - 0.9) / 0.1));
      T.limit_hold = 0;
    }
    buf[2 * i] = x;
    // K-weighting: shelf then high-pass (BS.1770-4 coefficients for 48 kHz)
    double y1 = b1[0] * x + b1[1] * T.k1x[0] + b1[2] * T.k1x[1] - a1[0] * T.k1y[0] - a1[1] * T.k1y[1];
    T.k1x[1] = T.k1x[0]; T.k1x[0] = x; T.k1y[1] = T.k1y[0]; T.k1y[0] = y1;
    double y2 = y1 - 2.0 * T.k2x[0] + T.k2x[1] - a2[0] * T.k2y[0] - a2[1] * T.k2y[1];
    T.k2x[1] = T.k2x[0]; T.k2x[0] = y1; T.k2y[1] = T.k2y[0]; T.k2y[0] = y2;
    T.acc_ms += y2 * y2;
    if (++T.acc_n == LV_BLOCK) { lv_block_done(); }
  }
}

void rade_native_tx_set_trim_db(double db) {
  if (db < -20.0) { db = -20.0; }
  if (db > 20.0) { db = 20.0; }
  T.trim_db = db;
}

double rade_native_tx_get_trim_db(void) { return T.trim_db; }

int rade_native_tx_inject(const float *samples, long n) {
  pthread_mutex_lock(&T.mx);
  free(T.inj);
  T.inj = NULL; T.inj_n = T.inj_pos = 0; T.inj_on = 0; T.inj_t0 = T.inj_t1 = 0.0;
  if (samples != NULL && n > 0) {
    T.inj = malloc(sizeof(float) * n);
    if (T.inj == NULL) { pthread_mutex_unlock(&T.mx); return -1; }
    memcpy(T.inj, samples, sizeof(float) * n);
    T.inj_n = n;
    T.inj_on = 1;
  }
  pthread_mutex_unlock(&T.mx);
  return 0;
}

void rade_native_tx_times(double t[7]) {
  pthread_mutex_lock(&T.mx);
  t[0] = T.ev_key; t[1] = T.ev_end_req; t[2] = T.ev_eoo_gen; t[3] = T.ev_eoo_out; t[4] = T.ev_done; t[5] = T.ev_unkey; t[6] = T.ev_out0;
  pthread_mutex_unlock(&T.mx);
}

int rade_native_tx_inject_state(double *t0, double *t1, double *fraction) {
  pthread_mutex_lock(&T.mx);
  int on = T.inj_on;
  if (t0) { *t0 = T.inj_t0; }
  if (t1) { *t1 = T.inj_t1; }
  if (fraction) { *fraction = T.inj_n > 0 ? (double)T.inj_pos / T.inj_n : 0.0; }
  int done = T.inj_n > 0 && T.inj_pos >= T.inj_n;
  pthread_mutex_unlock(&T.mx);
  return on ? (done ? 2 : 1) : 0;
}

void rade_native_tx_set_drive_db(double db) {
  if (db < -60.0) { db = -60.0; }          // the window's slider stops at -6; the test harness goes lower
  if (db > 6.0) { db = 6.0; }
  T.drive_db = db;
}

double rade_native_tx_get_drive_db(void) { return T.drive_db; }
void rade_native_tx_set_agc(int on) { T.agc_on = on != 0; }
int rade_native_tx_get_agc(void) { return T.agc_on; }

void rade_native_tx_level(rade_native_txlevel_t *l) {
  pthread_mutex_lock(&T.mx);
  double avg = 0.0, mom = -99.0;
  l->valid = lv_gated(&avg, &mom);
  l->loud_avg = avg;
  l->momentary = mom;
  l->delta_db = avg - RADE_NATIVE_TARGET_LUFS;
  double pk = T.peak_acc > T.peak_hold ? T.peak_acc : T.peak_hold;
  l->peak_dbfs = 20.0 * log10(pk + 1e-9);
  l->limiting = T.limit_hold < 10;
  l->agc_gain_db = T.agc_on ? T.agc_db : 0.0;
  pthread_mutex_unlock(&T.mx);
}

void rade_native_tx_block(double *buf, int n, int invert, int transmitting) {
  if (!T.enabled) { return; }
  pthread_mutex_lock(&T.mx);
  if (transmitting && !T.keyed) {
    // key-down: the leveller keeps its gain, but the loudness average starts empty (measured in this over only)
    T.blk_n = T.blk_pos = T.acc_n = 0;
    T.acc_ms = 0.0;
    T.pk_ref = -200.0;
    T.ev_key = ev_now();
    T.ev_end_req = T.ev_eoo_gen = T.ev_eoo_out = T.ev_done = T.ev_unkey = T.ev_out0 = 0.0;
  }
  T.keyed = transmitting != 0;
  if (T.inj_on) {
    // the test harness: speech from a file in place of the mic, started with the key and ended with its last sample
    for (int i = 0; i < n; i++) {
      if (T.keyed && T.inj_pos < T.inj_n) {
        if (T.inj_pos == 0) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); T.inj_t0 = ts.tv_sec + ts.tv_nsec * 1e-9; }
        buf[2 * i] = T.inj[T.inj_pos++];
        if (T.inj_pos == T.inj_n) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); T.inj_t1 = ts.tv_sec + ts.tv_nsec * 1e-9; }
      } else {
        buf[2 * i] = 0.0;
      }
    }
  }
  lv_process(buf, n);               // trim, level, limit and measure the mic, keyed or not
  if (!transmitting) {
    if (T.ev_key > 0.0 && T.ev_unkey == 0.0) { T.ev_unkey = ev_now(); }     // the first block the radio was not keyed for
    // discard whatever is in flight so the next over starts clean
    if (T.active || T.pcm_w != T.pcm_r || T.out_w != T.out_r) { T.flush = 1; pthread_cond_signal(&T.cv); }
    T.active = 0; T.started = 0; T.end_req = 0; T.end_state = 0; T.eoo_end_pos = -1; T.tail = 0;
    pthread_mutex_unlock(&T.mx);
    memset(buf, 0, sizeof(double) * 2 * n);
    return;
  }
  T.active = 1;
  // 48 kHz mic -> 16 kHz speech (not once the over is ending)
  int16_t blk[1024 / TX_DEC + 2];
  int nb = 0;
  for (int i = 0; i < n && T.end_state == 0; i++) {
    T.dec_hist[T.dec_pos] = buf[2 * i];
    T.dec_pos = (T.dec_pos + 1) % TX_DEC_TAPS;
    if (++T.dec_phase == TX_DEC) {
      T.dec_phase = 0;
      double a = 0.0;
      for (int k = 0; k < TX_DEC_TAPS; k++) {
        a += T.dec_h[k] * T.dec_hist[(T.dec_pos - 1 - k + 2 * TX_DEC_TAPS) % TX_DEC_TAPS];
      }
      a *= 32768.0;
      if (nb < (int)(sizeof blk / sizeof blk[0])) {
        blk[nb++] = (int16_t)(a > 32767.0 ? 32767 : a < -32767.0 ? -32767 : a);
      }
    }
  }
  if (T.pcm_w - T.pcm_r + nb <= TX_PCM_RING) {
    for (int i = 0; i < nb; i++) { T.pcm[(T.pcm_w + i) % TX_PCM_RING] = blk[i]; }
    T.pcm_w += nb;
    pthread_cond_signal(&T.cv);
  }
  // modem signal back out; WDSP's convention needs the conjugate unless inverted
  const double sq = rade_native_conjugate(invert) ? -1.0 : 1.0;
  //
  // The worker produces the modem signal a frame at a time (V1 120 ms), and
  // as fast as the mic delivers speech, so with one frame queued the next
  // arrives just as that one runs out: any lateness puts a hole of zeros
  // into the middle of the signal. Start the output once two frames are
  // queued; after that a hole needs the worker to fall a frame behind.
  //
  if (!T.started && T.out_w - T.out_r >= 2L * T.nmod * TX_INT) { T.started = 1; T.ev_out0 = ev_now(); }
  for (int i = 0; i < n; i++) {
    if (T.started && T.out_r < T.out_w) {
      RADE_COMP c = T.out[T.out_r % TX_OUT_RING];
      T.out_r++;
      buf[2 * i] = c.real;
      buf[2 * i + 1] = sq * c.imag;
    } else {
      buf[2 * i] = 0.0;
      buf[2 * i + 1] = 0.0;
      if (T.started && T.end_state == 0) { T.underruns++; }
    }
  }
  // the over is finished once the EOO has gone into WDSP and WDSP has had
  // time to put it out (its TX filters delay by about 0.13 s)
  if (T.end_state == 1 && T.eoo_end_pos >= 0 && T.out_r >= T.eoo_end_pos) {
    if (T.ev_eoo_out == 0.0) { T.ev_eoo_out = ev_now(); }       // the last EOO sample has gone to WDSP
    T.tail += n;
    if (T.tail >= TX_TAIL) { T.end_state = 2; if (T.ev_done == 0.0) { T.ev_done = ev_now(); } }
  }
  pthread_mutex_unlock(&T.mx);
  pthread_mutex_lock(&E.mx);
  cap_push_mic(blk, nb);
  pthread_mutex_unlock(&E.mx);
}

//
// End of over. radio_set_mox(0) asks; as long as the answer is 1 it keeps
// the transmitter keyed and asks again when rade_native_tx_end_state() is 2.
//
int rade_native_tx_end_request(void) {
  if (!T.enabled) { return 0; }
  int hold = 0;
  pthread_mutex_lock(&T.mx);
  if (T.end_state == 1) {
    hold = 1;
  } else if (T.end_state == 0 && T.active) {
    T.end_state = 1;
    T.ev_end_req = ev_now();
    T.end_req = 1;
    T.eoo_end_pos = -1;
    T.tail = 0;
    pthread_cond_signal(&T.cv);
    hold = 1;
  }
  pthread_mutex_unlock(&T.mx);
  return hold;
}

int rade_native_tx_end_state(void) {
  pthread_mutex_lock(&T.mx);
  int st = T.end_state;
  pthread_mutex_unlock(&T.mx);
  return st;
}

// keyed again before the end of the over went out: carry on
void rade_native_tx_end_cancel(void) {
  if (!T.enabled) { return; }
  pthread_mutex_lock(&T.mx);
  T.end_state = 0; T.end_req = 0; T.eoo_end_pos = -1; T.tail = 0;
  pthread_mutex_unlock(&T.mx);
}

void rade_native_set_callsign(const char *call) {
  char c[16];
  int n = 0;
  for (int i = 0; call && call[i] && n < 7; i++) {
    unsigned char ch = (unsigned char)call[i];
    if (ch >= 'a' && ch <= 'z') { ch = ch - 'a' + 'A'; }
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '/') { c[n++] = ch; }
  }
  c[n] = 0;
  pthread_mutex_lock(&T.mx);
  strcpy(T.call, c);
  pthread_mutex_unlock(&T.mx);
}

unsigned long rade_native_tx_underruns(void) { return T.underruns; }

const char *rade_native_get_callsign(void) {
  return T.call;
}

int rade_native_waveform(void) { return E.waveform; }
int rade_native_user_invert(void) { return E.user_invert; }

void rade_native_configure(int waveform, int user_invert) {
  E.user_invert = user_invert != 0;
  if (waveform == E.waveform) { return; }
  engine_stop();
  E.waveform = 0;
  memset(&E.st, 0, sizeof E.st);
  E.snr_ema = E.foff_last = 0.0f;
  E.snr_n = 0; E.nosync = 0;
  E.ncalls = 0;
  E.last_call[0] = 0;
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

int rade_native_call_pop(rade_native_call_t *c) {
  int got = 0;
  pthread_mutex_lock(&E.mx);
  if (E.ncalls > 0) {
    *c = E.calls[0];
    memmove(&E.calls[0], &E.calls[1], sizeof(E.calls[0]) * (E.ncalls - 1));
    E.ncalls--;
    got = 1;
  }
  pthread_mutex_unlock(&E.mx);
  return got;
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
