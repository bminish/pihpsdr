#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>

#include "radio.h"
#include "radae_handler.h"
#include "rade_api.h"
#include "lpcnet.h"
#include "fargan.h"
#include "wdsp.h"
#include "main.h"
#include "vfo.h"
#include "cpu_support.h"
#include "message.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Global RADAE status variables
int rx_rade_snr = -99;
int rx_rade_sync = 0;
float rx_rade_foff = 0.0f;
char decoded_callsign[16] = "";

int rade_iq_toggle = 0;

// RADE Contexts
static struct rade *rade_rx_ctx = NULL;
static struct rade *rade_tx_ctx = NULL;
static LPCNetEncState *tx_lpcnet = NULL;
static FARGANState rx_fargan;
static int rx_fargan_ready = 0;
static float rx_cont_buf[5 * 36];
static int rx_cont_frames = 0;

// Resamplers
#define MAX_RADAE_RX 16
static void *rx_resampler_audio[MAX_RADAE_RX] = {NULL};
static void *rx_resampler_iq[MAX_RADAE_RX] = {NULL};

static void *tx_resampler_audio = NULL;
static void *tx_resampler_iq = NULL;

static int current_rx_sample_rate[MAX_RADAE_RX] = {0};
static int current_tx_sample_rate = 48000;

// TX FIR 2.5kHz Bandwidth Filter (1.25kHz Cutoff at 48kHz)
#define TX_FILT_TAPS 101
static double tx_filt_coeffs[TX_FILT_TAPS];
static double tx_filt_state_i[TX_FILT_TAPS];
static double tx_filt_state_q[TX_FILT_TAPS];
static int tx_filt_idx = 0;

// EOO Transmit State
static int tx_eoo_phase = 0;
static gint64 tx_eoo_start_time = 0;
static int tx_dropped_ptt = 0;

// Circular buffers
#define RX_IQ_BUF_SIZE 65536
static RADE_COMP rx_iq_buf[RX_IQ_BUF_SIZE];
static int rx_iq_head = 0;
static int rx_iq_tail = 0;

static void rx_iq_push(RADE_COMP val) {
  int next = (rx_iq_head + 1) % RX_IQ_BUF_SIZE;
  if (next != rx_iq_tail) {
    rx_iq_buf[rx_iq_head] = val;
    rx_iq_head = next;
  }
}

static RADE_COMP rx_iq_pop(void) {
  RADE_COMP val = {0.0f, 0.0f};
  if (rx_iq_head != rx_iq_tail) {
    val = rx_iq_buf[rx_iq_tail];
    rx_iq_tail = (rx_iq_tail + 1) % RX_IQ_BUF_SIZE;
  }
  return val;
}

static int rx_iq_count(void) {
  return (rx_iq_head - rx_iq_tail + RX_IQ_BUF_SIZE) % RX_IQ_BUF_SIZE;
}

#define RX_AUDIO_BUF_SIZE 131072
static double rx_audio_buf[RX_AUDIO_BUF_SIZE];
static int rx_audio_head = 0;
static int rx_audio_tail = 0;

static void rx_audio_push(double val) {
  int next = (rx_audio_head + 1) % RX_AUDIO_BUF_SIZE;
  if (next != rx_audio_tail) {
    rx_audio_buf[rx_audio_head] = val;
    rx_audio_head = next;
  }
}

static double rx_audio_pop(void) {
  double val = 0.0;
  if (rx_audio_head != rx_audio_tail) {
    val = rx_audio_buf[rx_audio_tail];
    rx_audio_tail = (rx_audio_tail + 1) % RX_AUDIO_BUF_SIZE;
  }
  return val;
}

static int rx_audio_count(void) {
  return (rx_audio_head - rx_audio_tail + RX_AUDIO_BUF_SIZE) % RX_AUDIO_BUF_SIZE;
}

#define TX_SPEECH_BUF_SIZE 65536
static double tx_speech_buf[TX_SPEECH_BUF_SIZE];
static int tx_speech_head = 0;
static int tx_speech_tail = 0;

static void tx_speech_push(double val) {
  int next = (tx_speech_head + 1) % TX_SPEECH_BUF_SIZE;
  if (next != tx_speech_tail) {
    tx_speech_buf[tx_speech_head] = val;
    tx_speech_head = next;
  }
}

static double tx_speech_pop(void) {
  double val = 0.0;
  if (tx_speech_head != tx_speech_tail) {
    val = tx_speech_buf[tx_speech_tail];
    tx_speech_tail = (tx_speech_tail + 1) % TX_SPEECH_BUF_SIZE;
  }
  return val;
}

static int tx_speech_count(void) {
  return (tx_speech_head - tx_speech_tail + TX_SPEECH_BUF_SIZE) % TX_SPEECH_BUF_SIZE;
}

#define TX_OFDM_BUF_SIZE 131072
static RADE_COMP tx_ofdm_buf[TX_OFDM_BUF_SIZE];
static int tx_ofdm_head = 0;
static int tx_ofdm_tail = 0;

static void tx_ofdm_push(RADE_COMP val) {
  int next = (tx_ofdm_head + 1) % TX_OFDM_BUF_SIZE;
  if (next != tx_ofdm_tail) {
    tx_ofdm_buf[tx_ofdm_head] = val;
    tx_ofdm_head = next;
  }
}

static RADE_COMP tx_ofdm_pop(void) {
  RADE_COMP val = {0.0f, 0.0f};
  if (tx_ofdm_head != tx_ofdm_tail) {
    val = tx_ofdm_buf[tx_ofdm_tail];
    tx_ofdm_tail = (tx_ofdm_tail + 1) % TX_OFDM_BUF_SIZE;
  }
  return val;
}

static int tx_ofdm_count(void) {
  return (tx_ofdm_head - tx_ofdm_tail + TX_OFDM_BUF_SIZE) % TX_OFDM_BUF_SIZE;
}

// Scratch buffers
static float *rx_feat_buf = NULL;
static float *rx_eoo_buf = NULL;
static float *tx_feat_buf = NULL;
static RADE_COMP *tx_out_buf = NULL;
static RADE_COMP *tx_eoo_out = NULL;
static int tx_feat_idx = 0;
static int tx_ofdm_n = 0;
static int rx_ofdm_n = 0;

static void init_tx_filter(double fc, int rate) {
  int N = TX_FILT_TAPS;
  int M = (N - 1) / 2;
  double fs = (double)rate;
  double sum = 0.0;
  for (int n = 0; n < N; n++) {
    double w = 0.54 - 0.46 * cos(2.0 * M_PI * n / (N - 1));
    if (n == M) {
      tx_filt_coeffs[n] = 2.0 * fc / fs * w;
    } else {
      tx_filt_coeffs[n] = sin(2.0 * M_PI * fc * (n - M) / fs) / (M_PI * (n - M)) * w;
    }
    sum += tx_filt_coeffs[n];
  }
  for (int n = 0; n < N; n++) {
    tx_filt_coeffs[n] /= sum;
  }
  memset(tx_filt_state_i, 0, sizeof(tx_filt_state_i));
  memset(tx_filt_state_q, 0, sizeof(tx_filt_state_q));
  tx_filt_idx = 0;
}

static void filter_tx_sample(double *i_val, double *q_val) {
  tx_filt_state_i[tx_filt_idx] = *i_val;
  tx_filt_state_q[tx_filt_idx] = *q_val;
  
  double sum_i = 0.0;
  double sum_q = 0.0;
  for (int n = 0; n < TX_FILT_TAPS; n++) {
    int state_n = (tx_filt_idx - n + TX_FILT_TAPS) % TX_FILT_TAPS;
    sum_i += tx_filt_coeffs[n] * tx_filt_state_i[state_n];
    sum_q += tx_filt_coeffs[n] * tx_filt_state_q[state_n];
  }
  *i_val = sum_i;
  *q_val = sum_q;
  
  tx_filt_idx = (tx_filt_idx + 1) % TX_FILT_TAPS;
}

static void recreate_rx_resamplers(int rx_id, int rate) {
  if (rx_id < 0 || rx_id >= MAX_RADAE_RX) return;
  if (rate <= 0) return;
  
  if (rx_resampler_audio[rx_id]) destroy_resampleV(rx_resampler_audio[rx_id]);
  if (rx_resampler_iq[rx_id]) destroy_resampleV(rx_resampler_iq[rx_id]);

  rx_resampler_audio[rx_id] = create_resampleV(16000, rate);
  rx_resampler_iq[rx_id] = create_resampleV(rate, 8000);
  current_rx_sample_rate[rx_id] = rate;
}

static void recreate_tx_resamplers(int rate) {
  if (tx_resampler_audio) destroy_resampleV(tx_resampler_audio);
  if (tx_resampler_iq) destroy_resampleV(tx_resampler_iq);

  tx_resampler_audio = create_resampleV(48000, 16000);
  tx_resampler_iq = create_resampleV(8000, rate);
  current_tx_sample_rate = rate;
}

void radae_init(void) {
  rade_initialize();
  init_tx_filter(1250.0, 8000);
}

void radae_cleanup(void) {
  if (rade_rx_ctx) { rade_close(rade_rx_ctx); rade_rx_ctx = NULL; }
  if (rade_tx_ctx) { rade_close(rade_tx_ctx); rade_tx_ctx = NULL; }
  if (tx_lpcnet) { lpcnet_encoder_destroy(tx_lpcnet); tx_lpcnet = NULL; }
  for (int i = 0; i < MAX_RADAE_RX; i++) {
    if (rx_resampler_audio[i]) { destroy_resampleV(rx_resampler_audio[i]); rx_resampler_audio[i] = NULL; }
    if (rx_resampler_iq[i]) { destroy_resampleV(rx_resampler_iq[i]); rx_resampler_iq[i] = NULL; }
  }
  if (tx_resampler_audio) { destroy_resampleV(tx_resampler_audio); tx_resampler_audio = NULL; }
  if (tx_resampler_iq) { destroy_resampleV(tx_resampler_iq); tx_resampler_iq = NULL; }

  free(rx_feat_buf); rx_feat_buf = NULL;
  free(rx_eoo_buf); rx_eoo_buf = NULL;
  free(tx_feat_buf); tx_feat_buf = NULL;
  free(tx_out_buf); tx_out_buf = NULL;
  free(tx_eoo_out); tx_eoo_out = NULL;

  rade_finalize();
}

void radae_rx_start(void) {
  if (!rade_rx_ctx) {
    rade_rx_ctx = rade_open("", RADE_VERBOSE_0);
    int n_feat_out = rade_n_features_in_out(rade_rx_ctx);
    int n_eoo_bits = rade_n_eoo_bits(rade_rx_ctx);
    rx_feat_buf = malloc(n_feat_out * sizeof(float));
    rx_eoo_buf = malloc(n_eoo_bits * sizeof(float));
  }
  
  fargan_init(&rx_fargan);
  rx_fargan_ready = 0;
  rx_cont_frames = 0;
  memset(rx_cont_buf, 0, sizeof(rx_cont_buf));

  rx_iq_head = rx_iq_tail = 0;
  rx_audio_head = rx_audio_tail = 0;

  decoded_callsign[0] = '\0';
  rx_rade_snr = -99;
  rx_rade_sync = 0;
  rx_rade_foff = 0.0f;
}

void radae_rx_stop(void) {
  // context kept open to avoid slow recreate time on fast tuning
}

void radae_process_rx_iq(RECEIVER *rx, double *iq_in, int count) {
  if (!rade_rx_ctx) return;
  int id = rx->id;
  if (id < 0 || id >= MAX_RADAE_RX) return;

  if (!rx_resampler_iq[id] || current_rx_sample_rate[id] != rx->sample_rate) {
    recreate_rx_resamplers(id, rx->sample_rate);
  }

  int out_samps = 0;
  double *iq_res = malloc((count * 4 + 1024) * 2 * sizeof(double));
  xresampleV(iq_in, iq_res, count, &out_samps, rx_resampler_iq[id]);

  for (int i = 0; i < out_samps; i++) {
    double r = iq_res[2 * i];
    double im = iq_res[2 * i + 1];

    double phase = 2.0 * M_PI * 1500.0 * rx_ofdm_n / 8000.0;
    double c = cos(phase);
    double s = sin(phase);

    double shifted_r = r * c - im * s;
    double shifted_im = r * s + im * c;

    RADE_COMP sample = {(float)shifted_r, (float)shifted_im};
    rx_iq_push(sample);
    rx_ofdm_n = (rx_ofdm_n + 1) % 16;
  }

  free(iq_res);

  int nin = rade_nin(rade_rx_ctx);
  while (rx_iq_count() >= nin) {
    RADE_COMP *rx_block = malloc(nin * sizeof(RADE_COMP));
    for (int i = 0; i < nin; i++) {
      rx_block[i] = rx_iq_pop();
    }

    int has_eoo = 0;
    int n_out = rade_rx(rade_rx_ctx, rx_feat_buf, &has_eoo, rx_eoo_buf, rx_block);

    rx_rade_sync = rade_sync(rade_rx_ctx);
    if (rx_rade_sync) {
      rx_rade_snr = rade_snrdB_3k_est(rade_rx_ctx);
      rx_rade_foff = rade_freq_offset(rade_rx_ctx);
    } else {
      rx_rade_snr = -99;
      rx_rade_foff = 0.0f;
    }

    if (has_eoo) {
      char call_dec[32];
      memset(call_dec, 0, sizeof(call_dec));
      int n_chars = rade_rx_get_eoo_callsign(rx_eoo_buf, rade_n_eoo_bits(rade_rx_ctx), call_dec);
      if (n_chars > 0) {
        strncpy(decoded_callsign, call_dec, sizeof(decoded_callsign) - 1);
        decoded_callsign[sizeof(decoded_callsign) - 1] = '\0';
        t_print("RADAE: EOO IDENT Callsign: %s\n", decoded_callsign);
      }
    }

    if (n_out > 0) {
      int n_frames = n_out / 36;
      for (int fi = 0; fi < n_frames; fi++) {
        float *feat = &rx_feat_buf[fi * 36];

        if (!rx_fargan_ready) {
          memcpy(&rx_cont_buf[rx_cont_frames * 36], feat, 36 * sizeof(float));
          if (++rx_cont_frames >= 5) {
            float packed[5 * 18];
            for (int i = 0; i < 5; i++) {
              memcpy(&packed[i * 18], &rx_cont_buf[i * 36], 18 * sizeof(float));
            }
            float zeros[320];
            memset(zeros, 0, sizeof(zeros));
            fargan_cont(&rx_fargan, zeros, packed);
            rx_fargan_ready = 1;
          }
          continue;
        }

        float fpcm[160];
        fargan_synthesize(&rx_fargan, fpcm, feat);

        // Interpolate speech from 16k to 48k (complex)
        double in_speech[320];
        for (int s = 0; s < 160; s++) {
          in_speech[2 * s] = (double)fpcm[s];
          in_speech[2 * s + 1] = 0.0; // Q=0
        }
        int out_speech = 0;
        double out_speech_buf[2000];
        xresampleV(in_speech, out_speech_buf, 160, &out_speech, rx_resampler_audio);
        
        for (int s = 0; s < out_speech; s++) {
          rx_audio_push(out_speech_buf[2 * s]); // I
        }
      }
    }

    free(rx_block);
  }

  // Populate receiver output buffer
  for (int i = 0; i < count; i++) {
    double val = 0.0;
    if (rx_audio_count() > 0) {
      val = rx_audio_pop();
    }
    iq_in[2 * i] = val;
    iq_in[2 * i + 1] = val;
  }
}

void radae_tx_start(void) {
  if (!rade_tx_ctx) {
    rade_tx_ctx = rade_open("", RADE_VERBOSE_0);
    int n_feat_in = rade_n_features_in_out(rade_tx_ctx);
    int n_tx_out = rade_n_tx_out(rade_tx_ctx);
    int n_eoo_out = rade_n_tx_eoo_out(rade_tx_ctx);
    tx_feat_buf = malloc(n_feat_in * sizeof(float));
    tx_out_buf = malloc(n_tx_out * sizeof(RADE_COMP));
    tx_eoo_out = malloc(n_eoo_out * sizeof(RADE_COMP));
  }
  
  if (!tx_lpcnet) {
    tx_lpcnet = lpcnet_encoder_create();
  }

  tx_speech_head = tx_speech_tail = 0;
  tx_ofdm_head = tx_ofdm_tail = 0;
  tx_feat_idx = 0;
  tx_eoo_phase = 0;
  tx_dropped_ptt = 0;
  tx_eoo_start_time = 0;
}

void radae_tx_stop(void) {
  // keep contexts
}

void radae_tx_start_eoo(void) {
  if (tx_eoo_phase) return;
  tx_eoo_phase = 1;
  tx_eoo_start_time = g_get_monotonic_time();
}

int radae_tx_is_eoo_pending(void) {
  return tx_eoo_phase && !tx_dropped_ptt;
}

int radae_tx_active(void) {
  return (tx_ofdm_count() > 0) || tx_eoo_phase;
}

extern void rxtx(int state);
extern int mox;
extern int vox;
extern void vox_cancel(void);
extern void ext_vfo_update(void);

static gboolean drop_ptt_idle(gpointer data) {
  rxtx(0);
  mox = 0;
  vox = 0;
  vox_cancel();
  g_idle_add((GSourceFunc)ext_vfo_update, NULL);
  t_print("RADAE: PTT released after EOO complete.\n");
  return FALSE;
}

static void force_drop_ptt(void) {
  if (!tx_dropped_ptt) {
    tx_dropped_ptt = 1;
    g_idle_add(drop_ptt_idle, NULL);
  }
}

void radae_process_tx_audio(TRANSMITTER *tx, double *mic_in, int mic_count, double *iq_out, int out_count) {
  if (!rade_tx_ctx || !tx_lpcnet) return;

  static int last_filter_high = -1;
  static int last_filter_low = -1;

  if (!tx_resampler_audio || current_tx_sample_rate != tx->iq_output_rate) {
    current_tx_sample_rate = tx->iq_output_rate;
    recreate_tx_resamplers(tx->iq_output_rate);
    last_filter_high = tx->filter_high;
    last_filter_low = tx->filter_low;
    double new_fc = (tx->filter_high - tx->filter_low) / 2.0;
    if (new_fc < 500.0) new_fc = 1250.0;
    init_tx_filter(new_fc, 8000);
  } else if (last_filter_high != tx->filter_high || last_filter_low != tx->filter_low) {
    last_filter_high = tx->filter_high;
    last_filter_low = tx->filter_low;
    double new_fc = (tx->filter_high - tx->filter_low) / 2.0;
    if (new_fc < 500.0) new_fc = 1250.0;
    init_tx_filter(new_fc, 8000);
  }

  // Watchdog timer (max 3 seconds in EOO phase)
  if (tx_eoo_phase && !tx_dropped_ptt) {
    gint64 elapsed = g_get_monotonic_time() - tx_eoo_start_time;
    if (elapsed > 3000000LL) {
      t_print("RADAE: Watchdog trigger - forcing PTT release!\n");
      force_drop_ptt();
      tx_eoo_phase = 0;
    }
  }

  if (!tx_eoo_phase) {
    // Normal transmit: Resample speech from 48k to 16k (complex)
    double *mono_in = malloc(mic_count * 2 * sizeof(double));
    for (int i = 0; i < mic_count; i++) {
      mono_in[2 * i] = mic_in[2 * i];
      mono_in[2 * i + 1] = 0.0;
    }
    int out_samps = 0;
    double *res_speech = malloc((mic_count + 1024) * 2 * sizeof(double));
    xresampleV(mono_in, res_speech, mic_count, &out_samps, tx_resampler_audio);
    for (int i = 0; i < out_samps; i++) {
      tx_speech_push(res_speech[2 * i]);
    }
    free(mono_in); free(res_speech);

    // Process speech into frames
    while (tx_speech_count() >= 160) {
      opus_int16 pcm[160];
      for (int s = 0; s < 160; s++) {
        double v = tx_speech_pop() * 32768.0;
        if (v > 32767.0) v = 32767.0;
        if (v < -32767.0) v = -32767.0;
        pcm[s] = (opus_int16)floor(0.5 + v);
      }

      int arch = opus_select_arch();
      lpcnet_compute_single_frame_features(tx_lpcnet, pcm, &tx_feat_buf[tx_feat_idx * 36], arch);
      tx_feat_idx++;

      if (tx_feat_idx >= 12) {
        int n_out = rade_tx(rade_tx_ctx, tx_out_buf, tx_feat_buf);

        // Resample from 8k to 48k
        double *iq_in = malloc(n_out * 2 * sizeof(double));
        for (int i = 0; i < n_out; i++) {
          double r = tx_out_buf[i].real;
          double im = tx_out_buf[i].imag;

          double phase = 2.0 * M_PI * 1500.0 * tx_ofdm_n / 8000.0;
          double c = cos(phase);
          double s = sin(phase);

          double shifted_r = r * c + im * s;
          double shifted_im = -r * s + im * c;

          filter_tx_sample(&shifted_r, &shifted_im);

          iq_in[2 * i] = shifted_r;
          iq_in[2 * i + 1] = shifted_im;

          tx_ofdm_n = (tx_ofdm_n + 1) % 16;
        }

        int out_samps = 0;
        double *iq_res = malloc((n_out * 24 + 1024) * 2 * sizeof(double));
        xresampleV(iq_in, iq_res, n_out, &out_samps, tx_resampler_iq);

        for (int i = 0; i < out_samps; i++) {
          RADE_COMP sample = {(float)iq_res[2 * i], (float)iq_res[2 * i + 1]};
          tx_ofdm_push(sample);
        }
        free(iq_res); free(iq_in);

        tx_feat_idx = 0;
      }
    }
  } else {
    // EOO Phase - check if we need to generate EOO frame
    static int eoo_generated = 0;
    if (!eoo_generated) {
      eoo_generated = 1;
      
      // Flush partial frame
      if (tx_feat_idx > 0) {
        memset(&tx_feat_buf[tx_feat_idx * 36], 0, (12 - tx_feat_idx) * 36 * sizeof(float));
        int n_out = rade_tx(rade_tx_ctx, tx_out_buf, tx_feat_buf);
        double *iq_in = malloc(n_out * 2 * sizeof(double));
        for (int i = 0; i < n_out; i++) {
          double r = tx_out_buf[i].real;
          double im = tx_out_buf[i].imag;

          double phase = 2.0 * M_PI * 1500.0 * tx_ofdm_n / 8000.0;
          double c = cos(phase);
          double s = sin(phase);

          double shifted_r = r * c + im * s;
          double shifted_im = -r * s + im * c;

          filter_tx_sample(&shifted_r, &shifted_im);

          iq_in[2 * i] = shifted_r;
          iq_in[2 * i + 1] = shifted_im;

          tx_ofdm_n = (tx_ofdm_n + 1) % 16;
        }

        int out_samps = 0;
        double *iq_res = malloc((n_out * 24 + 1024) * 2 * sizeof(double));
        xresampleV(iq_in, iq_res, n_out, &out_samps, tx_resampler_iq);

        for (int i = 0; i < out_samps; i++) {
          RADE_COMP sample = {(float)iq_res[2 * i], (float)iq_res[2 * i + 1]};
          tx_ofdm_push(sample);
        }
        free(iq_res); free(iq_in);
        tx_feat_idx = 0;
      }
      
      // Encode callsign into EOO Soft bits
      rade_tx_set_eoo_callsign(rade_tx_ctx, station_callsign);
      
      // Generate EOO Frame
      int n_out = rade_tx_eoo(rade_tx_ctx, tx_eoo_out);
      double *iq_in = malloc(n_out * 2 * sizeof(double));
      for (int i = 0; i < n_out; i++) {
        double r = tx_eoo_out[i].real;
        double im = tx_eoo_out[i].imag;

        double phase = 2.0 * M_PI * 1500.0 * tx_ofdm_n / 8000.0;
        double c = cos(phase);
        double s = sin(phase);

        double shifted_r = r * c + im * s;
        double shifted_im = -r * s + im * c;

        filter_tx_sample(&shifted_r, &shifted_im);

        iq_in[2 * i] = shifted_r;
        iq_in[2 * i + 1] = shifted_im;

        tx_ofdm_n = (tx_ofdm_n + 1) % 16;
      }

      int out_samps = 0;
      double *iq_res = malloc((n_out * 24 + 1024) * 2 * sizeof(double));
      xresampleV(iq_in, iq_res, n_out, &out_samps, tx_resampler_iq);

      for (int i = 0; i < out_samps; i++) {
        RADE_COMP sample = {(float)iq_res[2 * i], (float)iq_res[2 * i + 1]};
        tx_ofdm_push(sample);
      }
      free(iq_res); free(iq_in);
      
      // Add silence buffer (same length as EOO frame) to let RX finish processing before PTT release
      for (int i = 0; i < out_samps; i++) {
        RADE_COMP silence = {0.0f, 0.0f};
        tx_ofdm_push(silence);
      }
    }
  }

  // Populate output buffer
  for (int i = 0; i < out_count; i++) {
    RADE_COMP sample = {0.0f, 0.0f};
    if (tx_ofdm_count() > 0) {
      sample = tx_ofdm_pop();
    }
    iq_out[2 * i] = sample.real;
    iq_out[2 * i + 1] = sample.imag;
  }

  if (tx_eoo_phase && tx_ofdm_count() == 0) {
    force_drop_ptt();
    tx_eoo_phase = 0;
  }
}
