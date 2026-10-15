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
#include <string.h>

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
    for (int i = 0; i < npcm; i++) { E.out_ring[(E.out_w + i) % OUT_RING] = pcm[i]; }
    E.out_w += npcm;
  }
  pthread_mutex_unlock(&E.mx);
  free(blk); free(feat); free(eoo);
  return NULL;
}

static void engine_stop(void) {
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
  pthread_mutex_unlock(&E.mx);
}
