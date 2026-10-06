/*
 * DEVELOPMENT TOOL. Not part of piHPSDR - see docs/diversity-radeV2-combining.md.
 *
 * A two-input copy of rade_c's V2 receiver (rade_rx_v2.c at the pin). Both
 * antennas come in; the combining is done on the OFDM latents, after each
 * arm's own DFT, and everything after the latents is rade_c's own, unchanged
 * (FrameSyncNet, the decoder). rade_c is not modified.
 *
 *   per arm      the receiver's band filter, one AGC gain (from arm 0)
 *   sync         the CP correlation of each arm, summed (sync_both) or arm 0's
 *                alone; timing and frequency are shared, as the clocks are
 *   demod        the receiver's own DFT, per arm, on the same timing
 *   noise        the 2x2 covariance of the arms from x[n] - x[n+M] over CP
 *                samples 16..31, per symbol, smoothed with tau_n symbols
 *   signal       the pooled 2x2 covariance of the latents, tau symbols; R = h1/h0
 *                with the noise taken off
 *   combine      u = (1, R); out = u^H Rnn^-1 y / sqrt(q), arm 0's phase kept,
 *                one R for both symbols of a frame, scaled to arm 0's latent level.
 *                comb 1, 2: one R for the whole band (the sums run over all carriers).
 *                comb 3, 4: one R per carrier, from the carrier and nb neighbours each side
 *
 * comb 0 passes arm 0 through, so with sync_both 0 it is the stock receiver:
 * that is the regression gate (the same output, bit for bit).
 */
#ifndef RADEV2_RX2_H
#define RADEV2_RX2_H

#include <stdio.h>
#include "rade_rx_v2.h"

struct rx2_cfg {
  int   sync_both;   /* CP correlation from: 0 arm 0 only, 1 both arms summed (an average of the
                        arms' correlations, not a gain), 2 a third, combined stream given by the caller */
  int   comb;        /* 0 arm 0, 1 diagonal Rnn (mode c), 2 full 2x2 Rnn (MVDR); the same two with one R per
                        carrier instead of one for the band: 3 diagonal, 4 full 2x2 */
  float tau;         /* signal covariance, symbols */
  float tau_n;       /* noise covariance, symbols  */
  int   nb;          /* comb 3, 4: carriers pooled each side of a carrier when R is formed (the oracle's perc3: 1) */
};

typedef struct {
  rade_rx_v2_state a;                          /* arm 0, and everything shared */
  rade_bpf         bpf1, bpf2;
  RADE_COMP        rx_buf1[RADE_V2_RX_BUF_SIZE], rx_buf2[RADE_V2_RX_BUF_SIZE];
  RADE_COMP        rx_i1[2 * RADE_V2_SYM_LEN];
  struct rx2_cfg   cfg;

  double c00, c11, c01r, c01i, w;              /* pooled signal covariance (IIR sums) */
  double n00, n11, n01r, n01i, wn;             /* noise covariance, per bin           */
  double pc00[14], pc11[14], pc01r[14], pc01i[14];   /* comb 3, 4: the same sums, per carrier */
  FILE  *lat_out;                                    /* if set: z0, z1, combined (3 x 56 floats) of every frame decoded */
  double e0, ec, we;                           /* latent energy: arm 0, combined      */
  long   nacc;                                 /* symbols accumulated since acquisition */
  double R_re, R_im, q_last;                   /* the last estimate, for the probe    */
} rx2_state;

int rx2_init(rx2_state *x, const struct rx2_cfg *cfg, int agc);
int rx2_nin(const rx2_state *x);
/* in2: the combined stream for sync_both == 2, else NULL */
int rx2_process(rx2_state *x, float *features_out, const RADE_COMP *in0, const RADE_COMP *in1,
                const RADE_COMP *in2);

#endif
