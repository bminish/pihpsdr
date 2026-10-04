/*
 * DEVELOPMENT TOOL. Not part of piHPSDR - see README.md.
 *
 * One RADE V2 receiver from rade_c (third_party/rade_c), wrapped so that
 * every OFDM symbol it processes leaves a record behind: sync state, the
 * cyclic-prefix correlation, the SNR estimate built from it, and what the
 * decoder produced. score_radev2 runs several of these side by side over
 * one capture; radev2_iq runs one over an 8 kHz I/Q file, which is how
 * the score is calibrated against true feature loss on synthetic signals.
 *
 * V2 has no pilots. Everything the receiver knows about the signal comes
 * from the cyclic prefix (timing, frequency, SNR) and from a neural frame
 * sync. That is why the probe reads the receiver's state directly rather
 * than only through rade_api.h: the API exposes the SNR estimate but not
 * the correlation it came from, nor whether the detector fired.
 * See docs/tools/radev2-scoring.md.
 */
#ifndef RADEV2_PROBE_H
#define RADEV2_PROBE_H

#include <stdio.h>
#include "rade_api.h"
#include "radev2_rx2.h"

/* One record per OFDM symbol (160 samples, 20 ms at 8 kHz). */
struct v2sym {
  double t;          /* seconds, at the end of the samples consumed      */
  int    sync;       /* receiver state after the symbol: 1 = sync        */
  int    sig;        /* detector: Ry_max > TSIG and not a sine           */
  int    valid;      /* the decoder produced features on this symbol     */
  int    eoo;        /* end of over detected                             */
  float  ry_max;     /* |Ry_smooth| at its peak, 0..1                     */
  float  snr;        /* rade's own SNR estimate, dB in 3 kHz             */
  float  foff;       /* tracked frequency offset, Hz                     */
  float  data;       /* soft BPSK aux bit, valid symbols only            */
  float  fsync;      /* FrameSyncNet's output on this symbol's latents,
                        valid symbols only (the parity that decoded)     */
};

struct v2probe {
  const char *name;
  struct rade *r;
  RADE_COMP   *buf;
  int          n, cap;
  float       *features;
  long         consumed;     /* 8 kHz samples handed to rade_rx()      */
  FILE        *feat_out;     /* optional: decoded features, .f32         */
  FILE        *csv_out;      /* optional: one row per symbol             */

  struct v2sym *sym;         /* every symbol, for paired comparison     */
  rx2_state    *x2;          /* set instead of r for the two-input receiver (radev2_rx2.h) */
  RADE_COMP    *buf1, *buf2; /* its second arm, and the combined stream for sync_both == 2 */
  long          nsym, symcap;
  int           n_acq0;      /* rx_v2.n_acq at open                     */
  long          n_eoo;
};

/* flags: RADE_* bits added to RADE_MODE_V2. agc: 1 (rade's default) or 0. */
int  v2probe_open(struct v2probe *p, const char *name, int verbose, int agc);
void v2probe_push(struct v2probe *p, float re, float im);

/* The two-input receiver, same records. One call per sample, both arms. */
int  v2probe_open2(struct v2probe *p, const char *name, const struct rx2_cfg *cfg, int agc);
void v2probe_push2(struct v2probe *p, float re0, float im0, float re1, float im1,
                   float re2, float im2);
void v2probe_drain(struct v2probe *p);
void v2probe_close(struct v2probe *p);

int  v2probe_acquisitions(const struct v2probe *p);

/*
 * The summary every tool prints. All fractions are of the probe's own
 * symbols; SNR means are in dB over the symbols named.
 */
struct v2summary {
  long   nsym;
  double seconds;
  double sync_frac;      /* symbols in sync                              */
  double sig_frac;       /* symbols where the detector fired             */
  long   frames;         /* decoder outputs (each 4 x 10 ms features)    */
  int    acq;
  long   eoo;
  double snr_sig;        /* mean snr over sig symbols                    */
  double snr_sync;       /* mean snr over sync symbols                   */
  double data_abs;       /* mean |aux bit| over decoded frames           */
  double data_conf;      /* fraction of decoded frames with |aux| > 0.5  */
  double data_med;       /* median |aux bit|: near 1 when the decoder is sure */
  double fsync;          /* mean FrameSyncNet output over decoded frames  */
};

void v2probe_summary(const struct v2probe *p, struct v2summary *s);

#endif
