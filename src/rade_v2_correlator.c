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

#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rade_v2_correlator.h"

//
// Deliberately free of the rest of piHPSDR (no GLib, no t_print, no radio.h),
// so that test/diversity/devtools can build it alone and replay captures
// through it. Diagnostics are the extern variables in the header.
//
// ----------------------------------------------------------------------
// Chain
// ----------------------------------------------------------------------
//
//   raw DDC IQ (both arms)
//        |  NCO shift by +frame_off, as rade_correlator.c does
//        v
//   polyphase FIR decimator, DDC rate -> 8000 Hz         (shared)
//        |
//        |  per bank: conjugate for bank 1, mix the band centre to zero,
//        |  129-tap low-pass (880 Hz wide), ring buffer
//        v
//   every 160 samples: lag-128 products over a sliding 32-sample window,
//   at all 160 offsets, into two exponentially weighted folded profiles
//        |
//        v
//   detection profile (0.5 s): bump position, contrast, lock, hold
//   weight profile (operator's averaging time): the weight
//
// Bank 0 is the tapped buffer as it is (LSB), bank 1 its mirror (USB), the
// same convention as the V1 pilot banks. With the passband naming the
// sideband only one is run; with -1 both are, at twice the cost.
//
// Everything is validated against test/diversity/devtools/py/cp_probe.py,
// which does the same sums over a whole capture at once.
//
typedef double complex cplx;

#define V2_L          RADE_V2C_M        // lag: the repeat distance of the prefix
#define V2_W          RADE_V2C_NCP      // correlation window: the prefix length
#define V2_P          RADE_V2C_SYM      // fold period: one symbol
#define V2_FS         ((double)RADE_V2C_FS)

//
// The band the modem occupies, 1062.5 ... 1875 Hz, with a little room. The
// low-pass is half that wide; the mix to zero puts the centre at 1470 Hz.
//
#define V2_FC         1470.0
#define V2_BW         880.0
#define V2_NTAP       129

//
// Offsets further than this from the bump are "off the prefix": the bump is a
// triangle of half-width V2_W and multipath tails it, so the reference for
// the floor and the tone pedestal stays well clear.
//
#define V2_GUARD      48

//
// Detection. The statistic is the contrast of the folded profile at the
// matched peak over its mean away from the bump ("dpk"). Measured over 14
// captures, about 1960 one-second windows of everything but V2: median 0.09,
// 5% above 0.23. A decodable V2 signal is 0.5 to 0.6.
//
// A single window is not a lock. LOCK_K of the last LOCK_N windows, taken a
// second apart, must clear V2_T_LOCK with the bump within V2_LOCK_TOL
// samples of each other. The tolerance is wide because on multipath the bump
// wanders by 10 to 20 samples.
//
// The values below were swept over all 171 captures in captures/ against
// rade_c's own V2 receiver (score_radev2: sync 20% or more and frame-sync
// 0.6 or more counts as V2; 24 captures do). py/cp_sweep.py repeats it.
//
//   T_LOCK  K of N  tol    found   time to lock           locked on non-V2
//   0.25    3 of 4  25     24/24   4-5 s, weak 12-31 s    13
//   0.30    3 of 4  20     22/24                          4
//   0.30    4 of 5  20     21/24   5-7 s, weak 17-23 s    2   <- these
//   0.35    4 of 5  15     20/24                          2
//   0.30    5 of 6  15     20/24   6-8 s, weak 34-42 s    0
//
// The two that remain are recordings of an AM broadcast on 698.6 kHz, which
// fool rade_c's own cyclic-prefix sync just the same (95% sync, but the
// frame-sync net sits at 0.49 and the aux bit at 0.47: nothing decodes). The
// strictest row clears them and loses the weakest real signals, which are the
// ones diversity is for. A false lock only issues weights while the prefix
// measures present, but on those two recordings it does.
//
#define V2_TAU_D      0.5               // s, detection profile
#ifndef V2_T_LOCK                       // overridable with -D, for sweeping against captures
#define V2_T_LOCK     0.30
#endif
#define V2_T_HOLD     0.20              // below this the prefix is not measurable: hold the weight
#ifndef V2_LOCK_K
#define V2_LOCK_K     4
#endif
#ifndef V2_LOCK_N
#define V2_LOCK_N     5
#endif
#ifndef V2_LOCK_TOL
#define V2_LOCK_TOL   20
#endif
#define V2_LOCK_SYMS  50                // one second between lock samples
#define V2_TICK_SYMS  5                 // 0.1 s between evaluations and weights

//
// The weight issued is the unit co-phase direction, conj(h1/h0)/|h1/h0|:
// arm 1 is added in phase with arm 0 and at equal gain, and the phase comes
// from the prefix correlation, which the interferer is not in. Smoothing is
// the engine's (div_apply_weight() slews it). The maximal-ratio weight, with
// the per-arm noise term, is computed alongside and published for logging
// only, so that a run can be replayed and the two compared; its gain is
// capped at the value the combining notes found harmless.
//
#define V2_GAIN_MAX   4.0
#define V2_N_FLOOR    0.02              // noise floor as a fraction of the arm's power

#define V2_DEC_TAPS_PER_PHASE 16
#define V2_DEC_CUTOFF 3000.0

#define V2_RING       8192              // samples per arm per bank; a power of two

typedef struct {
  cplx   a00[V2_P], a11[V2_P], a10[V2_P], a01[V2_P];
  double b0[V2_P], b1[V2_P];
} prof_t;

typedef struct {
  int      active;
  int      bank;
  float   *dl0, *dl1;                   // FIR delay lines, complex
  int      dpos;
  int64_t  nin;                         // samples in, for the mixer phase
  float   *ring0, *ring1;
  int64_t  total;
  int64_t  next_sym;
  int64_t  nsym;
  prof_t   det, wgt;
  int      g;
  double   dpk, rho;
  cplx     ag;                          // pedestal-free sum of the two arms' prefix correlation at g
  double   hist_dpk[V2_LOCK_N];
  int      hist_g[V2_LOCK_N];
  int      nhist;
  int      locked, present, updated;
  cplx     out;
  int      out_valid;
} ctx_t;

int    rade_v2c_locked    = 0;
int    rade_v2c_present   = 0;
double rade_v2c_quality   = 0.0;
double rade_v2c_snr       = 0.0;
int    rade_v2c_mirrored  = 0;
int    rade_v2c_gamma     = 0;
double rade_v2c_freq_off  = 0.0;
double rade_v2c_arm_db    = 0.0;
int    rade_v2c_arm_valid = 0;
double rade_v2c_arm_cos   = 1.0;
double rade_v2c_arm_sin   = 0.0;
double rade_v2c_mrc_re    = 0.0;
double rade_v2c_mrc_im    = 0.0;
double rade_v2c_snr0      = 0.0;
double rade_v2c_snr1      = 0.0;
double rade_v2c_pedestal  = 0.0;

static int     running = 0;
static int     decim = 0;
static int     ntaps = 0;
static float  *taps = NULL;
static float  *dline0 = NULL, *dline1 = NULL;
static int     dpos = 0;
static int     phase = 0;
static double  nco_phase = 0.0;
static float   btaps[V2_NTAP];
static ctx_t   ctx[2];
static double  cur_tau = 1.0;

static void make_taps(int rate) {
  const double fc = V2_DEC_CUTOFF / (double)rate;
  ntaps = V2_DEC_TAPS_PER_PHASE * decim;

  if ((ntaps & 1) == 0) { ntaps++; }

  taps = malloc(sizeof(float) * (size_t)ntaps);
  double sum = 0.0;

  for (int i = 0; i < ntaps; i++) {
    double x = (double)i - 0.5 * (double)(ntaps - 1);
    double s = (fabs(x) < 1e-9) ? (2.0 * fc) : (sin(2.0 * M_PI * fc * x) / (M_PI * x));
    double t = 2.0 * M_PI * (double)i / (double)(ntaps - 1);
    double w = 0.35875 - 0.48829 * cos(t) + 0.14128 * cos(2.0 * t) - 0.01168 * cos(3.0 * t);
    taps[i] = (float)(s * w);
    sum += s * w;
  }

  for (int i = 0; i < ntaps; i++) { taps[i] = (float)(taps[i] / sum); }

  //
  // The band-pass is a low-pass at half the band's width, after the mix.
  //
  const double fb = 0.5 * V2_BW / V2_FS;
  sum = 0.0;

  for (int i = 0; i < V2_NTAP; i++) {
    double x = (double)i - 0.5 * (double)(V2_NTAP - 1);
    double s = (fabs(x) < 1e-9) ? (2.0 * fb) : (sin(2.0 * M_PI * fb * x) / (M_PI * x));
    double w = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / (double)(V2_NTAP - 1));
    btaps[i] = (float)(s * w);
    sum += s * w;
  }

  for (int i = 0; i < V2_NTAP; i++) { btaps[i] = (float)(btaps[i] / sum); }
}

static void ctx_free(ctx_t *c) {
  free(c->dl0);
  free(c->dl1);
  free(c->ring0);
  free(c->ring1);
  memset(c, 0, sizeof *c);
}

static void ctx_reset(ctx_t *c, int bank) {
  ctx_free(c);
  c->bank = bank;
  c->dl0 = calloc(2 * V2_NTAP, sizeof(float));
  c->dl1 = calloc(2 * V2_NTAP, sizeof(float));
  c->ring0 = calloc(2 * V2_RING, sizeof(float));
  c->ring1 = calloc(2 * V2_RING, sizeof(float));
  c->out = 1.0;
}

static void publish_idle(void) {
  rade_v2c_locked = 0;
  rade_v2c_present = 0;
  rade_v2c_quality = 0.0;
  rade_v2c_snr = 0.0;
  rade_v2c_mirrored = 0;
  rade_v2c_gamma = 0;
  rade_v2c_freq_off = 0.0;
  rade_v2c_arm_db = 0.0;
  rade_v2c_arm_valid = 0;
  rade_v2c_arm_cos = 1.0;
  rade_v2c_arm_sin = 0.0;
  rade_v2c_mrc_re = rade_v2c_mrc_im = 0.0;
  rade_v2c_snr0 = rade_v2c_snr1 = 0.0;
  rade_v2c_pedestal = 0.0;
}

int rade_v2c_start(int ddc_rate) {
  if (running) { return 1; }

  if (ddc_rate <= 0 || (ddc_rate % RADE_V2C_FS) != 0) { return 0; }

  decim = ddc_rate / RADE_V2C_FS;
  make_taps(ddc_rate);
  dline0 = calloc(2 * (size_t)ntaps, sizeof(float));
  dline1 = calloc(2 * (size_t)ntaps, sizeof(float));
  dpos = 0;
  phase = 0;
  nco_phase = 0.0;
  running = 1;
  rade_v2c_reset();
  return 1;
}

void rade_v2c_stop(void) {
  if (!running) { return; }

  running = 0;
  free(taps);
  free(dline0);
  free(dline1);
  taps = NULL;
  dline0 = dline1 = NULL;
  ctx_free(&ctx[0]);
  ctx_free(&ctx[1]);
  publish_idle();
}

void rade_v2c_reset(void) {
  ctx_free(&ctx[0]);
  ctx_free(&ctx[1]);
  publish_idle();
}

//
// Circular distance between two offsets in the fold period.
//
static inline int cdist(int a, int b) {
  int d = a - b;

  if (d < 0) { d = -d; }

  return d > V2_P - d ? V2_P - d : d;
}

//
// Triangle-smoothed copy of rho, matched to the prefix: half-width V2_W.
// Only the peak is wanted, so this returns the position and the value.
//
static int tri_peak(const double *rho, double *val) {
  static double tw[V2_W];
  static int init = 0;

  if (!init) {
    for (int d = 0; d < V2_W; d++) { tw[d] = (1.0 - (double)d / V2_W) / V2_W; }

    init = 1;
  }

  int bg = 0;
  double bv = -1.0;

  for (int g = 0; g < V2_P; g++) {
    double s = tw[0] * rho[g];

    for (int d = 1; d < V2_W; d++) {
      s += tw[d] * (rho[(g + d) % V2_P] + rho[(g - d + V2_P) % V2_P]);
    }

    if (s > bv) { bv = s; bg = g; }
  }

  if (val) { *val = bv; }

  return bg;
}

//
// The detection statistic on the two arms' power-summed profile. Pass 1
// finds the bump on the smoothed |a|, where a tone's pedestal is flat and
// does not move it. Pass 2 takes that pedestal as the complex mean of the
// offsets away from the bump, subtracts it and scores rho at the matched
// peak against the mean of the offsets away from that.
//
static void detect(ctx_t *c) {
  double rho[V2_P], B[V2_P];
  cplx A[V2_P];

  for (int g = 0; g < V2_P; g++) {
    A[g] = c->det.a00[g] + c->det.a11[g];
    B[g] = c->det.b0[g] + c->det.b1[g];
    rho[g] = 2.0 * cabs(A[g]) / (B[g] + 1e-30);
  }

  int g0 = tri_peak(rho, NULL);
  cplx ped = 0.0;
  int n = 0;

  for (int g = 0; g < V2_P; g++) {
    if (cdist(g, g0) > V2_GUARD) { ped += A[g]; n++; }
  }

  ped /= (double)n;

  for (int g = 0; g < V2_P; g++) { rho[g] = 2.0 * cabs(A[g] - ped) / (B[g] + 1e-30); }

  int g = tri_peak(rho, NULL);
  double mu = 0.0;
  n = 0;

  for (int k = 0; k < V2_P; k++) {
    if (cdist(k, g) > V2_GUARD) { mu += rho[k]; n++; }
  }

  mu /= (double)n;
  c->g = g;
  c->dpk = rho[g] - mu;
  c->rho = rho[g];
  c->ag = A[g] - ped;
}

static void prof_add(prof_t *p, double al, const cplx *r00, const cplx *r11, const cplx *r10,
                     const cplx *r01, const double *e0, const double *e1) {
  const double d = 1.0 - al;

  for (int g = 0; g < V2_P; g++) {
    p->a00[g] = d * p->a00[g] + al * r00[g];
    p->a11[g] = d * p->a11[g] + al * r11[g];
    p->a10[g] = d * p->a10[g] + al * r10[g];
    p->a01[g] = d * p->a01[g] + al * r01[g];
    p->b0[g]  = d * p->b0[g]  + al * e0[g];
    p->b1[g]  = d * p->b1[g]  + al * e1[g];
  }
}

static inline cplx rget(const float *r, int64_t a) {
  int i = (int)(a & (V2_RING - 1));
  return (double)r[2 * i] + I * (double)r[2 * i + 1];
}

//
// The weight, from the weight profile at the detected bump.
//
// With the tone pedestal out (the complex mean of the offsets away from the
// bump, in each of the four sums), at the bump:
//
//   ry0, ry1   arm k's own prefix correlation   |h_k|^2 S W, phase -theta
//   x10, x01   y1[n]*conj(y0[n+128]) and the other way round:
//              h1*conj(h0) S W, with +/-theta
//
// theta is the carrier offset's phase over the lag and comes off arm 0's
// ry0, so the cross term is X = x10*e^(-j theta) + conj(x01*e^(j theta)),
// twice h1*conj(h0) S W, whose phase is that of h1/h0; |h1/h0|^2 is |ry1|/|ry0|.
// The noise of each arm is what is left of its power after the prefix-correlated part: |ry_k|/W is
// that arm's S, b_k/2W its total. The weight issued is the unit direction
// conj(r)/|r|; the maximal-ratio weight, conj(r) N0/N1, is published for the
// log.
//
static void weight(ctx_t *c) {
  const int g = c->g;
  cplx ped00 = 0.0, ped11 = 0.0, ped10 = 0.0, ped01 = 0.0;
  int n = 0;

  for (int k = 0; k < V2_P; k++) {
    if (cdist(k, g) > V2_GUARD) {
      ped00 += c->wgt.a00[k];
      ped11 += c->wgt.a11[k];
      ped10 += c->wgt.a10[k];
      ped01 += c->wgt.a01[k];
      n++;
    }
  }

  ped00 /= n; ped11 /= n; ped10 /= n; ped01 /= n;
  const cplx ry0 = c->wgt.a00[g] - ped00;
  const cplx ry1 = c->wgt.a11[g] - ped11;
  const cplx x10 = c->wgt.a10[g] - ped10;
  const cplx x01 = c->wgt.a01[g] - ped01;
  const double m0 = cabs(ry0);

  if (m0 <= 0.0) { return; }

  const double th = carg(ry0);
  const cplx X = x10 * cexp(-I * th) + conj(x01 * cexp(I * th));
  //
  // The phase is the cross term's. Its magnitude is not used: averaged over
  // a window in which the inter-arm phase moves, |X| shrinks, and the gain
  // ratio read from it follows. The two arms' own prefix correlations are
  // not affected, and |h1/h0|^2 is their ratio.
  //
  const double ax = cabs(X);

  if (ax <= 0.0) { return; }

  cplx r = sqrt(cabs(ry1) / m0) * (X / ax);

  if (c->bank == 1) { r = conj(r); }         // bank 1 was conjugated going in

  const double P0 = c->wgt.b0[g] / (2.0 * V2_W);
  const double P1 = c->wgt.b1[g] / (2.0 * V2_W);

  if (P0 <= 0.0 || P1 <= 0.0 || cabs(r) <= 0.0) { return; }

  double N0 = P0 - m0 / V2_W;
  double N1 = P1 - cabs(ry1) / V2_W;

  if (N0 < V2_N_FLOOR * P0) { N0 = V2_N_FLOOR * P0; }

  if (N1 < V2_N_FLOOR * P1) { N1 = V2_N_FLOOR * P1; }

  const double rm = cabs(r);
  c->out = conj(r) / rm;
  c->out_valid = 1;
  c->updated = 1;
  rade_v2c_arm_db = 10.0 * log10(rm * rm * N0 / N1);
  rade_v2c_arm_cos =  creal(r) / rm;
  rade_v2c_arm_sin = -cimag(r) / rm;
  rade_v2c_arm_valid = 1;

  //
  // For the log: the maximal-ratio weight that was not used, each arm's own
  // signal-to-noise on the prefix, and how much of the power is coherent at
  // lag 128 away from the prefix, which is what a tone or a comb is.
  //
  cplx mrc = conj(r) * (N0 / N1);
  const double mm = cabs(mrc);

  if (mm > V2_GAIN_MAX) { mrc *= V2_GAIN_MAX / mm; }

  rade_v2c_mrc_re = creal(mrc);
  rade_v2c_mrc_im = cimag(mrc);
  rade_v2c_snr0 = 10.0 * log10((m0 / V2_W) / N0 + 1e-12);
  rade_v2c_snr1 = 10.0 * log10((cabs(ry1) / V2_W) / N1 + 1e-12);
  rade_v2c_pedestal = (cabs(ped00) + cabs(ped11)) / V2_W / (P0 + P1);
}

//
// One symbol: the lag-128 products over a 32-sample window at every offset
// of the period, into both profiles.
//
static void symbol(ctx_t *c) {
  static cplx cs00[V2_P + V2_W], cs11[V2_P + V2_W], cs10[V2_P + V2_W], cs01[V2_P + V2_W];
  static double es0[V2_P + V2_W], es1[V2_P + V2_W];
  cplx r00[V2_P], r11[V2_P], r10[V2_P], r01[V2_P];
  double e0[V2_P], e1[V2_P];
  const int64_t n0 = c->next_sym;
  const int nk = V2_P + V2_W - 1;

  cs00[0] = cs11[0] = cs10[0] = cs01[0] = 0.0;
  es0[0] = es1[0] = 0.0;

  for (int k = 0; k < nk; k++) {
    const cplx y0a = rget(c->ring0, n0 + k), y0b = rget(c->ring0, n0 + k + V2_L);
    const cplx y1a = rget(c->ring1, n0 + k), y1b = rget(c->ring1, n0 + k + V2_L);
    cs00[k + 1] = cs00[k] + y0a * conj(y0b);
    cs11[k + 1] = cs11[k] + y1a * conj(y1b);
    cs10[k + 1] = cs10[k] + y1a * conj(y0b);
    cs01[k + 1] = cs01[k] + y0a * conj(y1b);
    es0[k + 1] = es0[k] + creal(y0a * conj(y0a)) + creal(y0b * conj(y0b));
    es1[k + 1] = es1[k] + creal(y1a * conj(y1a)) + creal(y1b * conj(y1b));
  }

  for (int g = 0; g < V2_P; g++) {
    r00[g] = cs00[g + V2_W] - cs00[g];
    r11[g] = cs11[g + V2_W] - cs11[g];
    r10[g] = cs10[g + V2_W] - cs10[g];
    r01[g] = cs01[g + V2_W] - cs01[g];
    e0[g] = es0[g + V2_W] - es0[g];
    e1[g] = es1[g + V2_W] - es1[g];
  }

  const double dt = (double)V2_P / V2_FS;
  prof_add(&c->det, 1.0 - exp(-dt / V2_TAU_D), r00, r11, r10, r01, e0, e1);
  prof_add(&c->wgt, 1.0 - exp(-dt / (cur_tau > 0.05 ? cur_tau : 0.05)), r00, r11, r10, r01, e0, e1);
  c->next_sym += V2_P;
  c->nsym++;

  if (c->nsym % V2_TICK_SYMS) { return; }

  detect(c);

  if (c->nsym % V2_LOCK_SYMS == 0) {
    //
    // A lock sample. See V2_LOCK_K.
    //
    if (c->nhist == V2_LOCK_N) {
      memmove(c->hist_dpk, c->hist_dpk + 1, sizeof(double) * (V2_LOCK_N - 1));
      memmove(c->hist_g, c->hist_g + 1, sizeof(int) * (V2_LOCK_N - 1));
      c->nhist--;
    }

    c->hist_dpk[c->nhist] = c->dpk;
    c->hist_g[c->nhist] = c->g;
    c->nhist++;

    if (!c->locked && c->nhist == V2_LOCK_N) {
      int ok[V2_LOCK_N], nok = 0;

      for (int i = 0; i < V2_LOCK_N; i++) {
        if (c->hist_dpk[i] > V2_T_LOCK) { ok[nok++] = c->hist_g[i]; }
      }

      if (nok >= V2_LOCK_K) {
        int near = 0;

        for (int i = 0; i < nok; i++) {
          if (cdist(ok[i], ok[nok - 1]) <= V2_LOCK_TOL) { near++; }
        }

        if (near >= V2_LOCK_K) {
          c->locked = 1;
          c->out_valid = 0;
        }
      }
    }
  }

  c->present = c->locked && c->dpk > V2_T_HOLD;

  if (c->present) { weight(c); }
}

//
// One sample at 8 kHz from each arm into a bank: conjugate for bank 1, mix the
// band centre to zero, low-pass, into the ring, and run every symbol that is
// now complete (it needs 128 + 32 samples beyond its own 160).
//
static void bank_push(ctx_t *c, cplx x0, cplx x1) {
  if (c->bank) { x0 = conj(x0); x1 = conj(x1); }

  const double ph = -2.0 * M_PI * V2_FC * (double)(c->nin % (int64_t)RADE_V2C_FS) / V2_FS;
  const cplx rot = cexp(I * ph);
  x0 *= rot;
  x1 *= rot;
  c->nin++;
  c->dl0[2 * c->dpos] = (float)creal(x0);
  c->dl0[2 * c->dpos + 1] = (float)cimag(x0);
  c->dl1[2 * c->dpos] = (float)creal(x1);
  c->dl1[2 * c->dpos + 1] = (float)cimag(x1);
  c->dpos++;

  if (c->dpos >= V2_NTAP) { c->dpos = 0; }

  double a0r = 0.0, a0i = 0.0, a1r = 0.0, a1i = 0.0;
  int idx = c->dpos - 1;

  for (int k = 0; k < V2_NTAP; k++) {
    if (idx < 0) { idx += V2_NTAP; }

    const double t = btaps[k];
    a0r += t * c->dl0[2 * idx];
    a0i += t * c->dl0[2 * idx + 1];
    a1r += t * c->dl1[2 * idx];
    a1i += t * c->dl1[2 * idx + 1];
    idx--;
  }

  const int w = (int)(c->total & (V2_RING - 1));
  c->ring0[2 * w] = (float)a0r;
  c->ring0[2 * w + 1] = (float)a0i;
  c->ring1[2 * w] = (float)a1r;
  c->ring1[2 * w + 1] = (float)a1i;
  c->total++;

  while (c->next_sym + V2_P + V2_W + V2_L - 1 < c->total) { symbol(c); }
}

int rade_v2c_process(const float *arm0, const float *arm1, int n,
                     int expect_bank, double frame_off, double tau,
                     double *wr, double *wi) {
  if (!running) { return 0; }

  cur_tau = tau;

  //
  // The passband names the bank, as for V1. Switching it starts that bank
  // afresh; the other is dropped.
  //
  for (int b = 0; b < 2; b++) {
    const int want = (expect_bank < 0) || (expect_bank == b);

    if (want && !ctx[b].active) {
      ctx_reset(&ctx[b], b);
      ctx[b].active = 1;
    } else if (!want && ctx[b].active) {
      ctx_free(&ctx[b]);
    }
  }

  const double dphi = 2.0 * M_PI * frame_off / (double)(decim * RADE_V2C_FS);
  const double cd = cos(dphi), sd = sin(dphi);
  double c = cos(nco_phase), s = sin(nco_phase);

  for (int b = 0; b < 2; b++) { ctx[b].updated = 0; }

  for (int i = 0; i < n; i++) {
    double r0 = arm0[2 * i] * c - arm0[2 * i + 1] * s, m0 = arm0[2 * i] * s + arm0[2 * i + 1] * c;
    double r1 = arm1[2 * i] * c - arm1[2 * i + 1] * s, m1 = arm1[2 * i] * s + arm1[2 * i + 1] * c;
    dline0[2 * dpos] = (float)r0;
    dline0[2 * dpos + 1] = (float)m0;
    dline1[2 * dpos] = (float)r1;
    dline1[2 * dpos + 1] = (float)m1;
    double ct = c;
    c = ct * cd - s * sd;
    s = ct * sd + s * cd;
    dpos++;

    if (dpos >= ntaps) { dpos = 0; }

    if (++phase < decim) { continue; }

    phase = 0;
    double a0r = 0.0, a0i = 0.0, a1r = 0.0, a1i = 0.0;
    int idx = dpos - 1;

    for (int k = 0; k < ntaps; k++) {
      if (idx < 0) { idx += ntaps; }

      const double tk = taps[k];
      a0r += tk * dline0[2 * idx];
      a0i += tk * dline0[2 * idx + 1];
      a1r += tk * dline1[2 * idx];
      a1i += tk * dline1[2 * idx + 1];
      idx--;
    }

    for (int b = 0; b < 2; b++) {
      if (ctx[b].active) { bank_push(&ctx[b], a0r + I * a0i, a1r + I * a1i); }
    }
  }

  nco_phase = fmod(nco_phase + dphi * (double)n, 2.0 * M_PI);

  //
  // Report the bank that has the better prefix, if any is locked.
  //
  const ctx_t *best = NULL;

  for (int b = 0; b < 2; b++) {
    if (ctx[b].active && ctx[b].locked && (!best || ctx[b].dpk > best->dpk)) { best = &ctx[b]; }
  }

  if (!best) {
    //
    // Nothing locked. Show what the strongest candidate looks like, which is
    // what a menu wants while it searches.
    //
    rade_v2c_locked = 0;
    rade_v2c_present = 0;
    rade_v2c_quality = 0.0;

    for (int b = 0; b < 2; b++) {
      if (ctx[b].active && ctx[b].dpk > rade_v2c_quality) { rade_v2c_quality = ctx[b].dpk; }
    }

    if (rade_v2c_quality > 1.0) { rade_v2c_quality = 1.0; }

    return 0;
  }

  double rho = best->rho;

  if (rho > 1.0 - 1e-6) { rho = 1.0 - 1e-6; }

  if (rho < 1e-6) { rho = 1e-6; }

  rade_v2c_locked = 1;
  rade_v2c_present = best->present;
  rade_v2c_quality = best->dpk < 0.0 ? 0.0 : (best->dpk > 1.0 ? 1.0 : best->dpk);
  rade_v2c_snr = 10.0 * log10(rho / (1.0 - rho));
  rade_v2c_mirrored = best->bank;
  rade_v2c_gamma = best->g;
  //
  // The carriers sit on the 62.5 Hz grid, so a carrier with no offset has no phase over the lag. The
  // mix to 1470 Hz is not on the grid and turns the correlation by 2*pi*1470*128/8000 on its own.
  //
  rade_v2c_freq_off = -carg(best->ag * cexp(-I * 2.0 * M_PI * V2_FC * (double)V2_L / V2_FS))
                      * V2_FS / (2.0 * M_PI * V2_L) * (best->bank ? -1.0 : 1.0);

  if (best->updated) {
    *wr = creal(best->out);
    *wi = cimag(best->out);
    return 1;
  }

  return 0;
}
