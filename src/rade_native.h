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

#ifndef _RADE_NATIVE_H_
#define _RADE_NATIVE_H_

#include <stddef.h>

//
// Native RADE V1/V2 through the WDSP I/Q chain. See docs/rade-native.md.
//
// The modem sits at a centre frequency above (normal) or below (inverted)
// the dial frequency; "which sideband" is not a question here. The
// geometry below is the part that is independent of the engine, so it can
// be tested offline.
//
enum {
  RADE_NATIVE_V1 = 1,
  RADE_NATIVE_V2 = 2
};

#define RADE_NATIVE_FC_V1       1475.0   // modem centre, Hz
#define RADE_NATIVE_FC_V2       1500.0
#define RADE_NATIVE_BW_V1       1600.0   // channel / TX filter width, Hz
#define RADE_NATIVE_BW_V2       1000.0
#define RADE_NATIVE_V1_INV_BELOW_HZ 10000000L  // V1 default: inverted below this

double rade_native_centre(int waveform);
double rade_native_width(int waveform);

//
// Whether the modem is inverted (below the dial frequency) by default on
// this dial frequency: V1 below 10 MHz, V2 never. The operator's Invert
// control is XORed with this.
//
int rade_native_default_invert(int waveform, long long dial_hz);
int rade_native_effective_invert(int waveform, long long dial_hz, int user_invert);

//
// WDSP passband edges (filter_low < filter_high) for the modem centred on
// its nominal frequency, shifted by shift_hz. Positive for a normal
// signal, negative for an inverted one: WDSP selects the sideband with the
// sign of the edges.
//
void rade_native_wdsp_edges(int waveform, int invert, double shift_hz,
                            double *low, double *high);

//
// WDSP's I/Q convention is the conjugate of librade's (measured, T-11):
// its positive passband selects the signal at negative frequency of
// I + jQ. The samples handed to or taken from librade are therefore
// conjugated for a normal signal and left alone for an inverted one,
// at both the RX and the TX interface.
//
int rade_native_conjugate(int invert);

//
// ---- receive engine (one receiver) ----
//
// The WDSP receive chain, set up with the RADE passband and its panel in
// binaural mode, delivers complex baseband at 48 kHz in the audio buffer
// (left = I, right = Q). rade_native_rx_block() takes that block, hands
// the signal to librade on a worker thread, and replaces the block with
// the decoded speech (left = right) at 48 kHz.
//
// waveform 0 switches the engine off. Changing the waveform closes and
// reopens the decoder, so call it from the menu thread, not the receiver
// thread.
//
int  rade_native_waveform(void);
int  rade_native_user_invert(void);
void rade_native_configure(int waveform, int user_invert);

//
// invert: the effective invert (see rade_native_effective_invert());
// gain: the RX volume as an amplitude.
//
void rade_native_rx_block(double *buf, int n, int invert, double gain);

typedef struct {
  int           waveform;
  int           sync;
  float         snr_db;      // librade's estimate, dB in 3 kHz
  float         foff_hz;     // librade's frequency offset
  unsigned long blocks;      // rade_rx() calls
  unsigned long underruns;   // 48 kHz speech samples not available in time
  unsigned long overruns;    // 8 kHz input samples dropped
} rade_native_status_t;

void rade_native_get_status(rade_native_status_t *st);

//
// Opens a V1 and a V2 context, checks the library version, and writes a
// one-line description to msg. Returns 0 on success.
//
int rade_native_probe(char *msg, size_t len);

#endif
