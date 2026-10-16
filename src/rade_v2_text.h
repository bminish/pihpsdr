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

//
// Receive: the decoder of freedv-gui's rade_text_rx_symbol(), ported. Feed it rade_rx_get_data_symbol() once after each
// rade_rx() that returned features. The first 112 symbols (4.5 s) are swept for the codeword alignment (a unique CRC-valid
// rotation is taken), after that every new symbol is tried as a codeword, and failing that the same window of the
// combining accumulators (one per cycle length of 1-4 blocks, decay 0.7, copies added coherently). Blocks are reassembled
// by number; the message is delivered once per pass through the transmitter's cycle (block 0 starts a pass). On loss of
// sync call reset (the position in the cycle is lost with it). Single owner: the receive worker thread.
//

#define RADE_V2_TEXT_JOIN        96     // symbols (3.8 s) that decide whether a new lock is the same sender's cycle
#define RADE_V2_TEXT_MAXGAP    1500     // longest lock loss (frames, 60 s) that is bridged
#define RADE_V2_TEXT_COMBINE_MAX (RADE_V2_TEXT_BITS * RADE_V2_TEXT_BLOCKS)

typedef struct {
  uint8_t block, last;
  uint8_t chars[8];
} rade_v2_text_fields_t;

typedef struct {
  float circ[RADE_V2_TEXT_BITS];
  int   wr, filled;
  float snap[RADE_V2_TEXT_BITS];
  int   sweep_next, sweep_n;
  rade_v2_text_fields_t sweep_win;
  float acc[RADE_V2_TEXT_BLOCKS][RADE_V2_TEXT_COMBINE_MAX];
  int   acc_idx[RADE_V2_TEXT_BLOCKS];
  int   total;
  uint8_t asm_chars[RADE_V2_TEXT_BLOCKS][8];
  uint8_t asm_got[RADE_V2_TEXT_BLOCKS];
  int   asm_total;       // -1 until a last block is seen
  int   asm_delivered;
  float pend[RADE_V2_TEXT_JOIN];   // the first symbols after a lock loss, held until they are checked against what was collected
  int   pend_n, pend_gap, holding;
  int   resets;                    // counts the times what was collected was thrown away
} rade_v2_text_rx_t;

void rade_v2_text_rx_init(rade_v2_text_rx_t *rx);
void rade_v2_text_rx_reset(rade_v2_text_rx_t *rx);

// One data symbol in. Returns 1 and the message in msg (at least RADE_V2_TEXT_MAXCHARS + 1 bytes) when a message
// is complete, else 0.
int rade_v2_text_rx_symbol(rade_v2_text_rx_t *rx, float sym, char *msg);

// The same, for the engine, which has lock as its gate: only symbols received with lock are fed, and `gap` is the number of
// modem frames since the previous one (0 when it follows directly). While lock lasts every repetition of the call is combined
// (as rade_v2_text_rx_symbol). A lock that was lost might have been a drop-out or the end of one sender's over and the start of
// another's, and what was collected must not be carried over to another sender. So after a gap the first RADE_V2_TEXT_JOIN
// symbols are held and compared with what was collected, at the position in the sender's cycle the elapsed time predicts
// (+-1 frame): the same sender's cycle runs on through the gap and correlates; another sender's has its own timing and does
// not. Correlating: the gap is bridged (counted as erased symbols) and the call is built over the whole transmission;
// otherwise, or for a gap over RADE_V2_TEXT_MAXGAP, everything is thrown away (rx->resets counts that) and a new reception starts.
int rade_v2_text_rx_feed(rade_v2_text_rx_t *rx, float sym, int gap, char *msg);

#endif
