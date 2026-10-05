/*
 * DEVELOPMENT TOOL. Not part of piHPSDR - see radev2_rx2.h.
 *
 * Derived from rade_rx_v2.c (Copyright (C) 2025 David Rowe, BSD-2-Clause, in
 * third_party/rade_c): the static helpers are copied from there with the
 * second arm added, and the process function follows it step for step. What is
 * new is marked NEW.
 */
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "radev2_rx2.h"
#include "rade_v2_constants.h"
#include "rade_v2_core.h"

#define ALPHA     0.95f
#define BETA      0.999f
#define TSIG      0.38f
#define TSIN      4.0f
#define TEOO      0.75f
#define ALPHA_EOO 0.70f
#define TIMING_SHIFT (RADE_V2_SYM_LEN / 4)
#define AGC_ALPHA 0.99875f

#define NC  RADE_V2_NC
#define M_  RADE_V2_M
#define NCP RADE_V2_NCP
#define SYM RADE_V2_SYM_LEN

int rx2_init(rx2_state *x, const struct rx2_cfg *cfg, int agc) {
  memset(x, 0, sizeof(*x));

  if (rade_rx_v2_init(&x->a, 1) != 0) { return -1; }

  x->a.agc_en = agc;
  x->cfg = *cfg;
  /* the second arm's filter: the same as the first's, its own state */
  {
    const rade_v2_ofdm *o = &x->a.ofdm;
    const float bandwidth = 1.2f * (o->w[NC - 1] - o->w[0]) * (float)RADE_FS / (2.0f * (float)M_PI);
    const float centre = (o->w[NC - 1] + o->w[0]) * (float)RADE_FS / (2.0f * (float)M_PI) / 2.0f;
    rade_bpf_init(&x->bpf1, RADE_BPF_NTAP, (float)RADE_FS, bandwidth, centre, SYM + RADE_BPF_NTAP);
    rade_bpf_init(&x->bpf2, RADE_BPF_NTAP, (float)RADE_FS, bandwidth, centre, SYM + RADE_BPF_NTAP);
  }
  return 0;
}

int rx2_nin(const rx2_state *x) { return x->a.nin; }

/* CP autocorrelation. NEW: with sync_both, both arms' sums go into one Ry. */
static void compute_autocorr(rx2_state *x) {
  rade_rx_v2_state *rx = &x->a;
  const int narm = (x->cfg.sync_both == 1) ? 2 : 1;

  for (int gamma = 0; gamma < SYM; gamma++) {
    const int idx = SYM + gamma;
    float Rr = 0.0f, Ri = 0.0f, D = 1e-12f;

    for (int k = 0; k < narm; k++) {
      const RADE_COMP *b = (x->cfg.sync_both == 2) ? x->rx_buf2 : k ? x->rx_buf1 : rx->rx_buf;

      for (int n = 0; n < NCP; n++) {
        const RADE_COMP p = b[idx - NCP + n], q = b[idx - NCP + M_ + n];
        Rr += p.real * q.real + p.imag * q.imag;
        Ri += p.imag * q.real - p.real * q.imag;
        D += p.real * p.real + p.imag * p.imag + q.real * q.real + q.imag * q.imag;
      }
    }

    rx->Ry_norm[gamma].real = 2.0f * Rr / D;
    rx->Ry_norm[gamma].imag = 2.0f * Ri / D;
    rx->Ry_smooth[gamma].real = ALPHA * rx->Ry_smooth[gamma].real + (1.0f - ALPHA) * rx->Ry_norm[gamma].real;
    rx->Ry_smooth[gamma].imag = ALPHA * rx->Ry_smooth[gamma].imag + (1.0f - ALPHA) * rx->Ry_norm[gamma].imag;
  }
}

static void detect_signal(rade_rx_v2_state *rx, int *sig_det, int *sine_det) {
  float max_val = -1.0f, min_val = 1e30f;
  int max_idx = 0;

  for (int g = 0; g < SYM; g++) {
    const float mag = sqrtf(rx->Ry_smooth[g].real * rx->Ry_smooth[g].real +
                            rx->Ry_smooth[g].imag * rx->Ry_smooth[g].imag);

    if (mag > max_val) { max_val = mag; max_idx = g; }

    if (mag < min_val) { min_val = mag; }
  }

  rx->delta_hat_g = (float)max_idx;
  rx->Ry_max = max_val;
  rx->Ry_min = min_val;
  *sig_det = (max_val > TSIG);
  *sine_det = (max_val / (min_val + 1e-12f) < TSIN);
  float rho = max_val;

  if (rho >= 1.0f) { rho = 1.0f - 1e-6f; }

  if (rho <= 0.0f) { rho = 1e-6f; }

  const float snr_raw = 10.0f * log10f(rho / (1.0f - rho)) - rx->snr_offset_dB;
  rx->snr_est_dB = rx->snr_corr_a * snr_raw + rx->snr_corr_b;
}

/* NEW: the same rotation and timing for both arms */
static void extract_symbol(rx2_state *x) {
  rade_rx_v2_state *rx = &x->a;
  const int delta_hat_rx = (int)rx->delta_hat - NCP;
  const float omega = 2.0f * (float)M_PI * rx->freq_offset / (float)RADE_FS;
  memmove(rx->rx_i, &rx->rx_i[SYM], sizeof(RADE_COMP) * SYM);
  memmove(x->rx_i1, &x->rx_i1[SYM], sizeof(RADE_COMP) * SYM);
  const int st = SYM + delta_hat_rx;

  for (int n = 0; n < SYM; n++) {
    RADE_COMP pstep;
    pstep.real = cosf(-omega);
    pstep.imag = sinf(-omega);
    rx->rx_phase = rade_cmul(rx->rx_phase, pstep);
    rx->rx_i[SYM + n] = rade_cmul(rx->rx_phase, rx->rx_buf[st + n]);
    x->rx_i1[SYM + n] = rade_cmul(rx->rx_phase, x->rx_buf1[st + n]);

    if (n >= NCP) { rx->rx_sym_td[n - NCP] = rx->rx_i[SYM + n]; }
  }

  const float pmag = sqrtf(rx->rx_phase.real * rx->rx_phase.real + rx->rx_phase.imag * rx->rx_phase.imag);
  rx->rx_phase.real /= pmag;
  rx->rx_phase.imag /= pmag;
}

/* ------------------------------------------------------------------ NEW: the combiner */


/*
 * comb 3, 4: one R per carrier. R[c] = (C01 - m n01) / (C00 - m n00), the sums taken over carrier c
 * and nb neighbours each side (m of them, fewer at the edges) and tau symbols, the noise per DFT bin
 * as measured for the band (white across it, as the oracle's perc3 takes it). Then the same MVDR
 * output as the band version, per carrier, so each carrier has its own gain and its own arm-1 phase
 * relative to arm 0's; arm 0's phase is kept.
 */
static void combine_pc(rx2_state *x, const float *z0, const float *z1,
                       double nn00, double nn11, double nn01r, double nn01i) {
  rade_rx_v2_state *rx = &x->a;
  const int s1 = NC * 2;
  const int nb = x->cfg.nb;
  double f0r[NC], f0i[NC], f1r[NC], f1i[NC], kk[NC];
  const double load = 1e-3 * 0.5 * (nn00 + nn11);
  const double p00 = nn00 + load, p11 = nn11 + load;
  const double det = p00 * p11 - (nn01r * nn01r + nn01i * nn01i);

  if (!(det > 0.0)) {
    memcpy(rx->az_hat, z0, sizeof(float) * RADE_V2_LATENT_DIM);
    return;
  }

  const double i00 = p11 / det, i11 = p00 / det;
  const double i01r = -nn01r / det, i01i = nn01i / det;
  const double i10r = -nn01r / det, i10i = -nn01i / det;

  for (int c = 0; c < NC; c++) {
    double a00 = 0, a01r = 0, a01i = 0;
    int m = 0;

    for (int j = c - nb; j <= c + nb; j++) {
      if (j < 0 || j >= NC) { continue; }

      a00 += x->pc00[j] / x->w;
      a01r += x->pc01r[j] / x->w;
      a01i += x->pc01i[j] / x->w;
      m++;
    }

    double den = a00 - m * nn00;

    if (den < 0.1 * a00) { den = 0.1 * a00; }

    const double Rr = (a01r - m * nn01r) / den, Ri = (a01i - m * nn01i) / den;
    f0r[c] = i00 + (Rr * i10r + Ri * i10i);
    f0i[c] = Rr * i10i - Ri * i10r;
    f1r[c] = i01r + Rr * i11;
    f1i[c] = i01i - Ri * i11;
    const double q = f0r[c] + (f1r[c] * Rr - f1i[c] * Ri);

    if (!(q > 0.0)) {                           /* this carrier: arm 0 through */
      f0r[c] = 1.0; f0i[c] = 0.0; f1r[c] = 0.0; f1i[c] = 0.0; kk[c] = 1.0;
    } else {
      kk[c] = sqrt(nn00) / sqrt(q);
    }
  }

  float out[RADE_V2_LATENT_DIM];
  double ec = 0.0;

  for (int i = 0; i < RADE_V2_LATENT_DIM / 2; i++) {
    const int c = i % NC;
    const double y0r = z0[2 * i], y0i = z0[2 * i + 1], y1r = z1[2 * i], y1i = z1[2 * i + 1];
    const double re = f0r[c] * y0r - f0i[c] * y0i + f1r[c] * y1r - f1i[c] * y1i;
    const double im = f0r[c] * y0i + f0i[c] * y0r + f1r[c] * y1i + f1i[c] * y1r;
    out[2 * i] = (float)(kk[c] * re);
    out[2 * i + 1] = (float)(kk[c] * im);

    if (i >= NC) { ec += kk[c] * kk[c] * (re * re + im * im) / NC; }
  }

  (void)s1;
  x->ec = exp(-1.0 / 12.0) * x->ec + (1 - exp(-1.0 / 12.0)) * ec;
  const double sc = (x->ec > 0.0 && x->we > 0.0) ? sqrt((x->e0 / x->we) / (x->ec / x->we)) : 1.0;

  for (int i = 0; i < RADE_V2_LATENT_DIM; i++) { rx->az_hat[i] = (float)(sc * out[i]); }
}

/*
 * z0, z1: the two arms' latents for the frame (2 symbols x NC x re/im), the
 * newest symbol last. Writes rx->az_hat.
 */
static void combine(rx2_state *x, const float *z0, const float *z1) {
  rade_rx_v2_state *rx = &x->a;
  const int s1 = NC * 2;                      /* offset of the newest symbol */
  double c00 = 0, c11 = 0, c01r = 0, c01i = 0;

  for (int c = 0; c < NC; c++) {
    const double y0r = z0[s1 + 2 * c], y0i = z0[s1 + 2 * c + 1];
    const double y1r = z1[s1 + 2 * c], y1i = z1[s1 + 2 * c + 1];
    c00 += y0r * y0r + y0i * y0i;
    c11 += y1r * y1r + y1i * y1i;
    c01r += y1r * y0r + y1i * y0i;            /* y1 conj(y0) */
    c01i += y1i * y0r - y1r * y0i;
  }

  /* the noise, from the CP of the newest symbol: x[n] - x[n+M], n = 16..31 */
  double d00 = 0, d11 = 0, d01r = 0, d01i = 0;

  for (int n = 16; n < NCP; n++) {
    const RADE_COMP a0 = rx->rx_i[SYM + n], b0 = rx->rx_i[SYM + n + M_];
    const RADE_COMP a1 = x->rx_i1[SYM + n], b1 = x->rx_i1[SYM + n + M_];
    const double e0r = a0.real - b0.real, e0i = a0.imag - b0.imag;
    const double e1r = a1.real - b1.real, e1i = a1.imag - b1.imag;
    d00 += e0r * e0r + e0i * e0i;
    d11 += e1r * e1r + e1i * e1i;
    d01r += e1r * e0r + e1i * e0i;
    d01i += e1i * e0r - e1r * e0i;
  }

  /* per DFT bin: x M, x Fs/975 (the difference is band-limited), /2 (two noisy copies), /16 samples */
  const double ns = (double)M_ * 8000.0 / 975.0 / 2.0 / (NCP - 16);
  const double as = exp(-1.0 / x->cfg.tau), an = exp(-1.0 / x->cfg.tau_n), ae = exp(-1.0 / 12.0);
  x->w  = as * x->w  + (1 - as);
  x->wn = an * x->wn + (1 - an);
  x->c00 = as * x->c00 + (1 - as) * c00;
  x->c11 = as * x->c11 + (1 - as) * c11;
  x->c01r = as * x->c01r + (1 - as) * c01r;
  x->c01i = as * x->c01i + (1 - as) * c01i;
  x->n00 = an * x->n00 + (1 - an) * ns * d00;
  x->n11 = an * x->n11 + (1 - an) * ns * d11;
  x->n01r = an * x->n01r + (1 - an) * ns * d01r;
  x->n01i = an * x->n01i + (1 - an) * ns * d01i;
  x->we = ae * x->we + (1 - ae);
  x->e0 = ae * x->e0 + (1 - ae) * c00 / NC;
  x->nacc++;

  if (x->cfg.comb >= 3) {
    for (int c = 0; c < NC; c++) {
      const double y0r = z0[s1 + 2 * c], y0i = z0[s1 + 2 * c + 1];
      const double y1r = z1[s1 + 2 * c], y1i = z1[s1 + 2 * c + 1];
      x->pc00[c] = as * x->pc00[c] + (1 - as) * (y0r * y0r + y0i * y0i);
      x->pc11[c] = as * x->pc11[c] + (1 - as) * (y1r * y1r + y1i * y1i);
      x->pc01r[c] = as * x->pc01r[c] + (1 - as) * (y1r * y0r + y1i * y0i);
      x->pc01i[c] = as * x->pc01i[c] + (1 - as) * (y1i * y0r - y1r * y0i);
    }
  }

  if (x->cfg.comb == 0 || x->nacc < 3) {      /* arm 0 through */
    memcpy(rx->az_hat, z0, sizeof(float) * RADE_V2_LATENT_DIM);
    x->R_re = x->R_im = 0.0;
    x->q_last = 1.0;
    return;
  }

  const double cc00 = x->c00 / x->w, cc01r = x->c01r / x->w, cc01i = x->c01i / x->w;
  const double nn00 = x->n00 / x->wn, nn11 = x->n11 / x->wn;
  const int full = (x->cfg.comb == 2 || x->cfg.comb == 4);
  const double nn01r = full ? x->n01r / x->wn : 0.0;     /* n01 = E[d1 conj(d0)] */
  const double nn01i = full ? x->n01i / x->wn : 0.0;

  if (x->cfg.comb >= 3) {
    combine_pc(x, z0, z1, nn00, nn11, nn01r, nn01i);
    return;
  }

  double den = cc00 - NC * nn00;

  if (den < 0.1 * cc00) { den = 0.1 * cc00; }

  const double Rr = (cc01r - NC * nn01r) / den, Ri = (cc01i - NC * nn01i) / den;
  /* Rnn = [[n00, conj(n01)], [n01, n11]], a little loaded; Rinv = adj / det */
  const double load = 1e-3 * 0.5 * (nn00 + nn11);
  const double p00 = nn00 + load, p11 = nn11 + load;
  const double det = p00 * p11 - (nn01r * nn01r + nn01i * nn01i);

  if (!(det > 0.0)) {
    memcpy(rx->az_hat, z0, sizeof(float) * RADE_V2_LATENT_DIM);
    return;
  }

  /* Rinv = 1/det [[p11, -conj(n01)], [-n01, p00]] */
  const double i00 = p11 / det, i11 = p00 / det;
  const double i01r = -nn01r / det, i01i = nn01i / det;      /* -conj(n01) */
  const double i10r = -nn01r / det, i10i = -nn01i / det;     /* -n01       */
  /* f = u^H Rinv, u = (1, R): f0 = i00 + conj(R) i10, f1 = i01 + conj(R) i11 */
  const double f0r = i00 + (Rr * i10r + Ri * i10i), f0i = (Rr * i10i - Ri * i10r);
  const double f1r = i01r + Rr * i11, f1i = i01i - Ri * i11;
  /* q = Re(f u) = f0 + f1 R */
  const double q = f0r + (f1r * Rr - f1i * Ri);
  x->R_re = Rr;
  x->R_im = Ri;
  x->q_last = q;

  if (!(q > 0.0)) {
    memcpy(rx->az_hat, z0, sizeof(float) * RADE_V2_LATENT_DIM);
    return;
  }

  const double k = sqrt(nn00) / sqrt(q);
  float out[RADE_V2_LATENT_DIM];
  double ec = 0.0;

  for (int i = 0; i < RADE_V2_LATENT_DIM / 2; i++) {
    const double y0r = z0[2 * i], y0i = z0[2 * i + 1], y1r = z1[2 * i], y1i = z1[2 * i + 1];
    const double re = f0r * y0r - f0i * y0i + f1r * y1r - f1i * y1i;
    const double im = f0r * y0i + f0i * y0r + f1r * y1i + f1i * y1r;
    out[2 * i] = (float)(k * re);
    out[2 * i + 1] = (float)(k * im);

    if (i >= NC) { ec += k * k * (re * re + im * im) / NC; }   /* the newest symbol */
  }

  /* back to arm 0's latent level, as the AGC would put a single arm */
  x->ec = exp(-1.0 / 12.0) * x->ec + (1 - exp(-1.0 / 12.0)) * ec;
  const double sc = (x->ec > 0.0 && x->we > 0.0) ? sqrt((x->e0 / x->we) / (x->ec / x->we)) : 1.0;

  for (int i = 0; i < RADE_V2_LATENT_DIM; i++) { rx->az_hat[i] = (float)(sc * out[i]); }
}

/* ------------------------------------------------------------------ copied from rade_rx_v2.c */

static int update_frame_sync_decode(rade_rx_v2_state *rx, float *features_out) {
  const float metric = rade_frame_sync(&rx->sync_model, rx->az_hat, 0);
  int winning = 0;

  if (rx->s % 2) {
    rx->frame_sync_odd = BETA * rx->frame_sync_odd + (1.0f - BETA) * metric;
    winning = (rx->frame_sync_odd > rx->frame_sync_even);
  } else {
    rx->frame_sync_even = BETA * rx->frame_sync_even + (1.0f - BETA) * metric;
    winning = (rx->frame_sync_even > rx->frame_sync_odd);
  }

  if (!winning) { return 0; }

  const int frames = RADE_V2_FRAMES_PER_STEP, num_feat = RADE_V2_NUM_FEATURES;
  const int num_used = RADE_V2_NUM_USED_FEATURES, nb_total = RADE_V2_NB_TOTAL_FEATURES;
  float dec_features[RADE_V2_FRAMES_PER_STEP * RADE_V2_NUM_FEATURES];
  rade_core_decoder_v2(&rx->dec_state, &rx->dec_model, dec_features, rx->az_hat, 0);
  memset(features_out, 0, sizeof(float) * RADE_V2_FEATURES_OUT);

  for (int f = 0; f < frames; f++) {
    float *dst = &features_out[f * nb_total], *src = &dec_features[f * num_feat];

    if (src[18] < -1.4f) { src[18] = -1.4f; }

    for (int j = 0; j < num_used; j++) { dst[j] = src[j]; }
  }

  rx->data_symbol = dec_features[num_used];
  return 1;
}

static int detect_eoo(rade_rx_v2_state *rx) {
  const float metric = rade_v2_ofdm_eoo_metric(&rx->ofdm, rx->rx_sym_td);
  rx->eoo_smooth = ALPHA_EOO * rx->eoo_smooth + (1.0f - ALPHA_EOO) * metric;
  return (rx->eoo_smooth > TEOO);
}

static int adjust_timing(rade_rx_v2_state *rx) {
  if (!rx->timing_adj) { return SYM; }

  const int shift = TIMING_SHIFT;
  int nin = SYM;

  if (rx->delta_hat > (float)(3 * SYM / 4)) {
    rx->delta_hat -= (float)shift;
    RADE_COMP tmp[TIMING_SHIFT];
    memcpy(tmp, rx->Ry_smooth, sizeof(RADE_COMP) * shift);
    memmove(rx->Ry_smooth, &rx->Ry_smooth[shift], sizeof(RADE_COMP) * (SYM - shift));
    memcpy(&rx->Ry_smooth[SYM - shift], tmp, sizeof(RADE_COMP) * shift);
    nin = SYM + shift;
  } else if (rx->delta_hat < (float)(SYM / 4)) {
    rx->delta_hat += (float)shift;
    RADE_COMP tmp[TIMING_SHIFT];
    memcpy(tmp, &rx->Ry_smooth[SYM - shift], sizeof(RADE_COMP) * shift);
    memmove(&rx->Ry_smooth[shift], rx->Ry_smooth, sizeof(RADE_COMP) * (SYM - shift));
    memcpy(rx->Ry_smooth, tmp, sizeof(RADE_COMP) * shift);
    nin = SYM - shift;
  }

  return nin;
}

static float compute_gain(rade_rx_v2_state *rx, const RADE_COMP *in, int nin) {
  if (!rx->agc_en) { return 1.0f; }

  float p = rx->agc_power;

  for (int i = 0; i < nin; i++) {
    p = AGC_ALPHA * p + (1.0f - AGC_ALPHA) * (in[i].real * in[i].real + in[i].imag * in[i].imag);
  }

  rx->agc_power = p;
  float gain = rx->agc_target / (sqrtf(p) + 1e-6f);

  if (gain < 0.1f) { gain = 0.1f; }

  if (gain > 10.0f) { gain = 10.0f; }

  return gain;
}

int rx2_process(rx2_state *x, float *features_out, const RADE_COMP *in0, const RADE_COMP *in1,
                const RADE_COMP *in2) {
  rade_rx_v2_state *rx = &x->a;
  const int buf_size = RADE_V2_RX_BUF_SIZE;
  const int nin = rx->nin;
  RADE_COMP f0[SYM + TIMING_SHIFT], f1[SYM + TIMING_SHIFT], f2[SYM + TIMING_SHIFT];

  rade_bpf_process(&rx->bpf, f0, in0, nin);
  rade_bpf_process(&x->bpf1, f1, in1, nin);

  if (x->cfg.sync_both == 2 && in2 != NULL) { rade_bpf_process(&x->bpf2, f2, in2, nin); }
  else { memset(f2, 0, sizeof(f2)); }

  /* NEW: one gain, from arm 0, for both: the arm ratio survives */
  const float gain = compute_gain(rx, f0, nin);
  rx->gain = gain;

  if (gain != 1.0f) {
    for (int i = 0; i < nin; i++) {
      f0[i].real *= gain; f0[i].imag *= gain;
      f1[i].real *= gain; f1[i].imag *= gain;
      f2[i].real *= gain; f2[i].imag *= gain;
    }
  }

  memmove(rx->rx_buf, &rx->rx_buf[nin], sizeof(RADE_COMP) * (buf_size - nin));
  memcpy(&rx->rx_buf[buf_size - nin], f0, sizeof(RADE_COMP) * nin);
  memmove(x->rx_buf1, &x->rx_buf1[nin], sizeof(RADE_COMP) * (buf_size - nin));
  memcpy(&x->rx_buf1[buf_size - nin], f1, sizeof(RADE_COMP) * nin);
  memmove(x->rx_buf2, &x->rx_buf2[nin], sizeof(RADE_COMP) * (buf_size - nin));
  memcpy(&x->rx_buf2[buf_size - nin], f2, sizeof(RADE_COMP) * nin);

  compute_autocorr(x);
  int sig_det, sine_det;
  detect_signal(rx, &sig_det, &sine_det);
  int valid_output = 0, eoo_flag = 0, next_state = rx->state;

  if (rx->state == RADE_RX_V2_IDLE) {
    if (sig_det && !sine_det) { rx->count++; } else { rx->count = 0; }

    if (rx->count == 5) {
      const int dg = (int)rx->delta_hat_g;
      const float delta_phi = atan2f(rx->Ry_smooth[dg].imag, rx->Ry_smooth[dg].real);
      rx->delta_hat = rx->delta_hat_g;
      rx->freq_offset = -delta_phi * (float)RADE_FS / (2.0f * (float)M_PI * M_);
      rx->count = 0;
      rx->count1 = 0;
      rx->frame_sync_even = 0.0f;
      rx->frame_sync_odd = 0.0f;
      rx->eoo_smooth = 0.0f;
      rx->n_acq++;
      next_state = RADE_RX_V2_SYNC;
      /* NEW: a new signal, a new channel */
      x->c00 = x->c11 = x->c01r = x->c01i = x->w = 0;
      memset(x->pc00, 0, sizeof(x->pc00)); memset(x->pc11, 0, sizeof(x->pc11));
      memset(x->pc01r, 0, sizeof(x->pc01r)); memset(x->pc01i, 0, sizeof(x->pc01i));
      x->n00 = x->n11 = x->n01r = x->n01i = x->wn = 0;
      x->e0 = x->ec = x->we = 0;
      x->nacc = 0;
    }
  } else {
    const int dg = (int)rx->delta_hat_g;
    const float delta_phi = atan2f(rx->Ry_smooth[dg].imag, rx->Ry_smooth[dg].real);
    rx->freq_offset_g = -delta_phi * (float)RADE_FS / (2.0f * (float)M_PI * M_);
    rx->delta_hat = BETA * rx->delta_hat + (1.0f - BETA) * (float)rx->delta_hat_g;
    rx->freq_offset = BETA * rx->freq_offset + (1.0f - BETA) * rx->freq_offset_g;

    if (!sig_det || sine_det) { rx->count++; } else { rx->count = 0; }

    if (rx->count == rx->hangover) {
      next_state = RADE_RX_V2_IDLE;
      rx->count = 0;
      rx->count1 = 0;
    }

    rx->new_sig_delta_hat = (fabsf((float)rx->delta_hat_g - rx->delta_hat) > (float)NCP);
    rx->new_sig_f_hat = (fabsf(rx->freq_offset_g - rx->freq_offset) > 5.0f);

    if (sig_det && (rx->new_sig_delta_hat || rx->new_sig_f_hat)) { rx->count1++; } else { rx->count1 = 0; }

    if (rx->count1 == 5) {
      next_state = RADE_RX_V2_IDLE;
      rx->count = 0;
      rx->count1 = 0;
    }

    extract_symbol(x);
    /* NEW: both arms through the receiver's own DFT, then combined */
    float z0[RADE_V2_LATENT_DIM], z1[RADE_V2_LATENT_DIM];
    rade_v2_ofdm_demod_frame(&rx->ofdm, z0, rx->rx_i, -16);
    rade_v2_ofdm_demod_frame(&rx->ofdm, z1, x->rx_i1, -16);
    combine(x, z0, z1);

    if (detect_eoo(rx)) {
      rx->count = 0;
      rx->count1 = 0;
      rx->eoo_smooth = 0.0f;
      memset(rx->Ry_smooth, 0, sizeof(rx->Ry_smooth));
      next_state = RADE_RX_V2_IDLE;
      eoo_flag = 1;
    } else {
      valid_output = update_frame_sync_decode(rx, features_out);

      if (valid_output) { rx->i++; }
    }

    rx->nin = adjust_timing(rx);
  }

  rx->s++;
  rx->state = next_state;

  if (rx->state == RADE_RX_V2_IDLE) { rx->nin = SYM; }

  return (valid_output ? 0x1 : 0) | (eoo_flag ? 0x2 : 0);
}
