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

#ifndef _RADE_EOO_TEXT_H_
#define _RADE_EOO_TEXT_H_

//
// RADE V1 callsign in the end-of-over frame, as freedv-gui does it. See
// rade_eoo_text.c.
//
// The EOO frame is rade_n_eoo_bits() floats (180): 90 QPSK symbols as
// interleaved I,Q. rade_tx_set_eoo_bits() takes what rade_eoo_text_encode()
// writes, and rade_rx() returns the received version of the same.
//

// Fill eoo[0..nfloats) for the callsign (letters, digits, '/', up to 8
// characters; others are dropped). Returns 0, or -1 if nfloats is too small.
int rade_eoo_text_encode(const char *call, float *eoo, int nfloats);

// Decode a received EOO of nsyms symbols. Returns 1 and a NUL-terminated
// callsign in call (at least 10 bytes) when the LDPC decode converged and
// the CRC matches, else 0.
int rade_eoo_text_decode(const float *eoo, int nsyms, char *call);

#endif
