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

#ifndef _RADE_V2_CORRELATOR_H_
#define _RADE_V2_CORRELATOR_H_

//
// Cyclic-prefix tracking and a blind scalar weight for FreeDV RADE V2.
//
// V2 has no pilots, so there is nothing known to correlate against. What it
// does have is structure: each 160-sample symbol (20 ms) starts with a copy
// of its own last 32 samples. The lag-128 autocorrelation over a 32-sample
// window, folded over the 160-sample period, is therefore a triangle about
// 64 samples wide where the window sits on the prefix, and flat elsewhere.
// A tone or a comb on the 62.5 Hz grid gives a flat pedestal, which is
// subtracted; speech, SSB, noise and V1 give no triangle at all (V1 repeats
// at lag 160, period 192). Measured on the captures, see
// test/diversity/devtools/py/cp_probe.py, which this was ported from.
//
// The same prefix gives the relative channel of the two arms with the
// interference left out: y1[n] * conj(y0[n+128]) over the prefix is
// h1*conj(h0)*S, and a common-mode interferer, uncorrelated 16 ms later,
// does not contribute. The lag-0 cross-spectrum measures whichever source is
// strongest instead.
//
// Waveform parameters, from rade_v2_ofdm.h in rade_c:
//
//   Fs   8000 Hz     modem sample rate
//   Nc   14          carriers, 62.5 Hz apart, 1062.5 ... 1875 Hz
//   M    128         samples per symbol, Ncp 32, symbol 160 = 20 ms
//   Ns   2           symbols per modem frame
//
#define RADE_V2C_FS      8000
#define RADE_V2C_M       128
#define RADE_V2C_NCP     32
#define RADE_V2C_SYM     (RADE_V2C_M + RADE_V2C_NCP)

//
// Status. Written by the analysis, read by whoever is interested.
//
extern int    rade_v2c_locked;       // a V2 signal has been confirmed and a weight is held
extern int    rade_v2c_present;      // ... and the prefix is measurable now (else the weight is held)
extern double rade_v2c_quality;      // 0..1, prefix contrast ("dpk"); about 0.5 on a decodable signal
extern double rade_v2c_snr;          // dB, rho/(1-rho) on the prefix, the receiver's own CP estimate
extern int    rade_v2c_mirrored;     // 1 if the modem is above the tuned frequency
extern int    rade_v2c_gamma;        // samples (8 kHz), where the prefix sits in the 160-sample period
extern double rade_v2c_freq_off;     // Hz from the nominal carrier grid, ambiguous over +/-31 Hz

//
// Which arm carries the better signal-to-noise, in dB, positive meaning arm 1,
// and the unit direction that brings arm 1 onto arm 0 in phase. As for V1.
//
extern double rade_v2c_arm_db;
extern int    rade_v2c_arm_valid;
extern double rade_v2c_arm_cos;
extern double rade_v2c_arm_sin;

//
// ddc_rate must be a whole multiple of RADE_V2C_FS. Returns 0 if not.
//
extern int  rade_v2c_start(int ddc_rate);
extern void rade_v2c_stop(void);
extern void rade_v2c_reset(void);

//
// Feed one block of n sample pairs at the DDC rate.
//
// expect_bank, frame_off and tau are as for rade_corr_process(): the bank the
// operator's passband names (0 modem below the tuned frequency, 1 above, -1
// both searched), the displacement of WDSP's frame from the dial, and the
// operator's averaging time.
//
// There is no hang: once locked the weight is held, however long the prefix
// is gone, until it comes back. Returns 1 when a new weight is available in
// *wr/*wi, in the sense of div_cos/div_sin: ready to be applied as z0 + w*z1.
// The weight is conj(h1/h0) * N0/N1, i.e. maximal-ratio with arm 0's phase,
// its magnitude capped at 4 and its phase slew limited to 1 Hz, because the
// V2 decoder tolerates a phase ramp but not steps.
//
extern int rade_v2c_process(const float *arm0, const float *arm1, int n,
                            int expect_bank, double frame_off, double tau,
                            double *wr, double *wi);

#endif
