/*
 * DEVELOPMENT TOOL. Replays a .divc capture through src/rade_v2_correlator.c, the C port of
 * py/cp_probe.py, and prints one line a second: quality (dpk), lock, present, bump position,
 * the weight last issued and the arm figures. Builds on its own: no GTK, no engine.
 *
 *   cp_replay FILE [bank [tau [CSV]]]
 *
 * bank is 0, 1 or -1 (both), tau seconds. Both default to what the engine was handed on each
 * block, as the capture recorded it (expect_bank, tau), and so does the frame offset, which is
 * always the recorded one. The correlator is reset where the engine reset it (the capture's
 * ENGINE_RESET flag) and after a dropped block, as the engine does.
 *
 * A capture taken on the RADE V2 reference also holds the engine's own state entering each
 * block: the replay is checked against it, and a line says how many blocks differ.
 *
 * CSV: one row per block of everything the module publishes, plus the prefix SNR each way of
 * combining would have given (arm 0 alone, arm 1 alone, equal gain with the issued weight,
 * maximal ratio). These are the tracker's own estimates, band SNR in about 880 Hz, not decode
 * results.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diversity_capture.h"
#include "rade_v2_correlator.h"

#define REF_RADE_V2 5

static double db(double x) { return 10.0 * log10(x > 1e-30 ? x : 1e-30); }

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s FILE [bank [tau [CSV]]]\n", argv[0]); return 2; }

  const int have_bank = argc > 2, have_tau = argc > 3;
  const int bank_arg = have_bank ? atoi(argv[2]) : -1;
  const double tau_arg = have_tau ? atof(argv[3]) : 1.0;
  FILE *f = fopen(argv[1], "rb");
  FILE *csv = argc > 4 ? fopen(argv[4], "w") : NULL;

  if (!f) { perror(argv[1]); return 1; }

  struct divcap_header h;

  if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, DIVCAP_MAGIC, 8)) {
    fprintf(stderr, "%s: not a capture\n", argv[1]);
    return 1;
  }

  if (!rade_v2c_start((int)h.sample_rate)) { fprintf(stderr, "rate %u: cannot run\n", h.sample_rate); return 1; }

  if (csv) {
    fprintf(csv, "t,locked,present,quality,snr_db,gamma,bank,freq_off_hz,arm_db,snr0_db,snr1_db,pedestal,"
            "w_re,w_im,mrc_re,mrc_im,updated,p0,p1,snr_arm0_db,snr_arm1_db,snr_egc_db,snr_mrc_db\n");
  }

  const size_t nfft = h.nfft;
  float *buf = malloc(h.block_bytes);
  struct divcap_block m;
  double wr = 1.0, wi = 0.0;
  long nblk = 0, nupd = 0, ncmp = 0, nbad = 0;
  double next = 1.0;
  int on_v2 = 0;

  while (fread(&m, sizeof m, 1, f) == 1 && m.rec_magic == DIVCAP_REC_MAGIC && fread(buf, 1, h.block_bytes, f) == h.block_bytes) {
    if (m.rec_flags & DIVCAP_FLAG_ENGINE_RESET || m.dropped > 0) { rade_v2c_reset(); }

    on_v2 = (m.ref == REF_RADE_V2);

    //
    // The state entering this block, against the engine's, where the engine was running V2.
    //
    if (on_v2 && h.version >= 5) {
      ncmp++;
      if (rade_v2c_locked != m.live_locked || rade_v2c_mirrored != (rade_v2c_locked ? m.live_mirrored : rade_v2c_mirrored) ||
          fabs(rade_v2c_quality - m.live_quality) > 1e-3) {
        if (nbad++ < 5) {
          fprintf(stderr, "block %ld: replay locked %d mirrored %d quality %.4f, engine %d %d %.4f\n", nblk,
                  rade_v2c_locked, rade_v2c_mirrored, rade_v2c_quality, m.live_locked, m.live_mirrored, m.live_quality);
        }
      }
    }

    nblk++;
    const double tau = have_tau ? tau_arg : m.tau;
    const int bank = have_bank ? bank_arg : m.expect_bank;
    const int upd = rade_v2c_process(buf, buf + 2 * nfft, (int)nfft, bank, m.frame_off, tau, &wr, &wi);
    nupd += upd;
    const double t = (double)nblk * nfft / h.sample_rate;

    if (csv) {
      //
      // Prefix SNR of each choice. With S_k and N_k from the arm's power P_k and its SNR q_k
      // (S = P q/(1+q), N = P/(1+q)): equal gain is (sqrt S0 + sqrt S1)^2 / (N0 + N1), maximal
      // ratio is S0/N0 + S1/N1.
      //
      const double q0 = pow(10.0, rade_v2c_snr0 / 10.0), q1 = pow(10.0, rade_v2c_snr1 / 10.0);
      const double S0 = rade_v2c_p0 * q0 / (1.0 + q0), N0 = rade_v2c_p0 / (1.0 + q0);
      const double S1 = rade_v2c_p1 * q1 / (1.0 + q1), N1 = rade_v2c_p1 / (1.0 + q1);
      const double egc = (sqrt(S0) + sqrt(S1)) * (sqrt(S0) + sqrt(S1)) / (N0 + N1);
      fprintf(csv, "%.3f,%d,%d,%.4f,%.2f,%d,%d,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.6g,%.6g,%.2f,%.2f,%.2f,%.2f\n", t,
              rade_v2c_locked, rade_v2c_present, rade_v2c_quality, rade_v2c_snr, rade_v2c_gamma,
              rade_v2c_mirrored, rade_v2c_freq_off, rade_v2c_arm_db, rade_v2c_snr0, rade_v2c_snr1,
              rade_v2c_pedestal, wr, wi, rade_v2c_mrc_re, rade_v2c_mrc_im, upd, rade_v2c_p0, rade_v2c_p1,
              rade_v2c_snr0, rade_v2c_snr1, db(egc), db(S0 / N0 + S1 / N1));
    }

    if (t >= next) {
      next += 1.0;
      printf("%5.0f s  dpk %.2f  %s%s  gamma %3d  bank %d  snr %+5.1f dB  f %+6.1f Hz  w %.2f /%+4.0f deg  arm1 %+5.1f dB\n",
             t, rade_v2c_quality, rade_v2c_locked ? "LOCKED" : "search", rade_v2c_present ? "+" : " ",
             rade_v2c_gamma, rade_v2c_mirrored, rade_v2c_snr, rade_v2c_freq_off,
             hypot(wr, wi), atan2(wi, wr) * 180.0 / M_PI, rade_v2c_arm_db);
    }
  }

  fprintf(stderr, "%ld blocks, %ld weights\n", nblk, nupd);

  if (ncmp) {
    fprintf(stderr, "against the engine's own V2 state: %ld of %ld blocks differ%s\n", nbad, ncmp,
            (have_bank || have_tau) ? " (bank or tau overridden, so expect some)" : "");
  }

  rade_v2c_stop();

  if (csv) { fclose(csv); }

  return 0;
}
