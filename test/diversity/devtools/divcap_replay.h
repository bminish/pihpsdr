//
// DEVELOPMENT TOOL. Not part of piHPSDR - see README.md.
//
// Replaying a .divc capture into the correlator. Shared by replay_rade
// (which sweeps with it) and test_capture (which round-trips a synthetic
// capture through it).
//
#ifndef DIVCAP_REPLAY_H
#define DIVCAP_REPLAY_H

#include <stdio.h>

#include "diversity_capture.h"

struct divcap_result {
  int    blocks;
  double seconds;
  int    acquisitions;      // 0 -> 1 transitions of rade_corr_locked
  int    locked_blocks;
  double first_lock;        // seconds, -1 if it never locked
  double mean_snr;          // over locked blocks
  double mean_quality;
  double weight_jitter;     // rms deviation of the weight from its mean
  int    verify_checked;
  int    verify_bad;

  //
  // Held-out validation of the per-subcarrier channel estimate.
  //
  // Everything here is scored one frame ahead: the estimate carried into
  // a frame was formed from earlier frames only, and it is judged against
  // that frame's own untouched measurement. Nothing is scored against
  // data that shaped it, so a longer average cannot win by smoothing its
  // way towards its own answer - which is what makes these usable as a
  // tuning objective with no decoder in the loop.
  //
  int    xval_frames;       // frames that carried a usable estimate
  //
  // Mean error, in degrees, between the phase the estimate predicted for a
  // subcarrier and the phase that subcarrier actually measured, weighted
  // by cross-spectral magnitude. Phase rather than the weight itself
  // because the measured MRC weight is unbounded in a deep fade.
  //
  double xval_phase_deg;
  //
  // SNR given up against a genie that knows the channel exactly, in dB,
  // averaged over subcarriers. Lower is better.
  //
  // The absolute figure is NOT a real shortfall: the genie is handed the
  // same noisy measurement everyone else is scored against, so it wins
  // partly by fitting that noise, and several dB of the number is that.
  // What is comparable is the same figure between two tuning points, and
  // the difference between the two below on one run.
  //
  // xval_loss_perbin uses the per-subcarrier weights, xval_loss_scalar the
  // one wideband weight. The difference between them is what per-bin
  // equalization is worth on this capture, before any decoder is involved.
  //
  double xval_loss_perbin;
  double xval_loss_scalar;
};

//
// Open a capture and read its header. Returns NULL and complains on
// stderr if it is not one, or is a version this tool does not speak.
// *data_start is where the first block record begins.
//
extern FILE *divcap_open(const char *path, struct divcap_header *h, long *data_start);

//
// Options that change what the replay does, as opposed to what the
// correlator does with it. Zeroed by the caller; every field off means
// "exactly as recorded".
//
struct divcap_opts {
  double tau;          // > 0 overrides the recorded averaging time
  double hang;         // > 0 overrides the recorded hang time
  double noise;        // > 0 adds AWGN of this rms to each arm, per component
  unsigned seed;       // for the noise
  int    verify;       // check the replayed state against the recording
  int    from_block;   // ignore state mismatches before this block
  FILE  *weights;      // if set, one CSV row per block: the applied weight
};

//
// One pass over the file with whatever is currently in rade_tuning.
// Starts and stops the correlator itself, so successive calls cannot
// inherit state from each other. Returns 0 if the correlator will not
// run at the capture's sample rate. opts may be NULL.
//
//
// The noise generator, exposed so that score_rade adds exactly the same
// noise to the same capture at the same seed. Seed first, then add once
// per block, in block order.
//
extern void divcap_noise_seed(unsigned seed);
extern void divcap_add_noise(float *arm0, float *arm1, int nfft, double rms);

extern int divcap_replay(FILE *f, const struct divcap_header *h, long data_start,
                         const struct divcap_opts *opts, struct divcap_result *r);

#endif
