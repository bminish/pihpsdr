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

#ifndef _RADE_V2_TEXT_H_
#define _RADE_V2_TEXT_H_

#include <stdint.h>

//
// RADE V2 callsign, sent the way freedv-gui sends it: one BPSK symbol (+1.0 / -1.0) per modem frame on
// rade_tx_set_data_symbol(), 25 per second, carrying the callsign as a repeating LDPC(112,56) stream. See
// rade_v2_text.c. The callsign is fixed when the object is initialised; a different callsign means a new init.
//
// The stream is the one freedv-gui's rade_text_tx_next_symbol() makes (src/pipeline/rade_text.cpp):
// characters 0-9 A-Z / (others dropped, lower case raised), up to 32, in blocks of 8; each block is one 112 bit
// codeword (CRC-8, last-block flag, block number, 8 characters packed in base 38), interleaved, one bit per frame;
// after the last block the stream starts again at block 0.
//
// Single owner: the transmit worker thread calls next(). No allocation, no locks.
//

#define RADE_V2_TEXT_MAXCHARS  32
#define RADE_V2_TEXT_BLOCKS     4
#define RADE_V2_TEXT_BITS     112

typedef struct {
  uint8_t bits[RADE_V2_TEXT_BLOCKS][RADE_V2_TEXT_BITS];   // as transmitted (interleaved), 0 or 1
  int     blocks;                                          // 0: nothing to send (no callsign)
  int     blk, sym;                                        // the cursor
  char    text[RADE_V2_TEXT_MAXCHARS + 1];                 // what goes on air (normalised callsign)
} rade_v2_text_t;

// Build the stream for call. Returns the number of blocks (1-4), or 0 if no character of call can be sent
// (the stream is then constant, see rade_v2_text_next).
int rade_v2_text_init(rade_v2_text_t *t, const char *call);

// The next data symbol for rade_tx_set_data_symbol(): once per modem frame. With nothing to send, -1.0.
float rade_v2_text_next(rade_v2_text_t *t);

// Go back to block 0, bit 0.
void rade_v2_text_restart(rade_v2_text_t *t);

#endif
