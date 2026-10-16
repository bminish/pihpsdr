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

//
// RADE V2 callsign on the data symbol channel, as freedv-gui sends it. A C port of the transmit half of the V2
// text channel in freedv-gui's backend (src/pipeline/rade_text.cpp and ldpc_code.h, HRA_56_56.h) by David Rowe and
// Mooneer Salem, whose terms apply to the matrix header it includes (rade_eoo_text_hra.h, a copy of
// src/pipeline/HRA_56_56.h) and to the format. Nothing here is shared with the receive side: freedv-gui's own
// receiver decodes this stream (test/rade_native/test_v2_text.sh checks that against its code).
//
// Per block of 8 characters (an empty callsign sends nothing and the symbol stays -1.0):
//   characters  0 null, '0'-'9' 1-10, 'A'-'Z' 11-36, '/' 37; others dropped, at most 32 characters, 1-4 blocks
//   packed      the 8 characters in base 38, the first the most significant, 42 bits
//   CRC-8       polynomial 0x1D, start 0, over the 9 bytes [block | (last ? 4 : 0), c0 .. c7]
//   payload     56 bits, each field least significant bit first: CRC [0..7], last block [8], block [9..10],
//               packed characters [11..52], 3 reserved zeros
//   LDPC        systematic [payload | parity], parity i = row i of the left half of HRA_56_56 against the payload,
//               plus parity i-1 (the right half is lower bidiagonal)
//   interleave  out[(37 * i) mod 112] = codeword[i]
//   symbol      bit 1 -> -1.0, bit 0 -> +1.0; one per modem frame, block after block, then round again
//

#include <string.h>

#include "rade_v2_text.h"
#include "rade_eoo_text_hra.h"

#define NDATA  56
#define NBITS  112
#define CHARS_PER_BLOCK 8
#define ALPHABET 38
#define INTERLEAVE 37

static int to_ota(const char *in, uint8_t *out) {
  int n = 0;
  for (int i = 0; in && in[i] && n < RADE_V2_TEXT_MAXCHARS; i++) {
    char c = in[i];
    if (c >= '0' && c <= '9') { out[n++] = (uint8_t)(c - '0' + 1); }
    else if (c >= 'A' && c <= 'Z') { out[n++] = (uint8_t)(c - 'A' + 11); }
    else if (c >= 'a' && c <= 'z') { out[n++] = (uint8_t)(c - 'a' + 11); }
    else if (c == '/') { out[n++] = 37; }
  }
  return n;
}

static uint8_t crc8(const uint8_t *p, int n) {
  uint8_t crc = 0;
  for (int i = 0; i < n; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) { crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x1D) : (uint8_t)(crc << 1); }
  }
  return crc;
}

static void put_bits(uint8_t *a, int start, int n, uint64_t v) {
  for (int i = 0; i < n; i++) { a[start + i] = (uint8_t)((v >> i) & 1); }
}

int rade_v2_text_init(rade_v2_text_t *t, const char *call) {
  memset(t, 0, sizeof *t);
  uint8_t ota[RADE_V2_TEXT_MAXCHARS];
  int n = to_ota(call, ota);
  static const char alpha[] = "\0" "0123456789" "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "/";
  for (int i = 0; i < n; i++) { t->text[i] = alpha[ota[i]]; }
  t->text[n] = 0;
  if (n == 0) { return 0; }
  int blocks = (n + CHARS_PER_BLOCK - 1) / CHARS_PER_BLOCK;
  for (int b = 0; b < blocks; b++) {
    uint8_t c[CHARS_PER_BLOCK] = {0};
    for (int i = 0; i < CHARS_PER_BLOCK; i++) {
      if (b * CHARS_PER_BLOCK + i < n) { c[i] = ota[b * CHARS_PER_BLOCK + i]; }
    }
    int last = (b == blocks - 1);
    uint8_t buf[1 + CHARS_PER_BLOCK];
    buf[0] = (uint8_t)(b | (last ? 4 : 0));
    memcpy(buf + 1, c, CHARS_PER_BLOCK);
    uint8_t crc = crc8(buf, sizeof buf);
    uint64_t packed = 0;
    for (int i = 0; i < CHARS_PER_BLOCK; i++) { packed = packed * ALPHABET + c[i]; }
    uint8_t s[NDATA] = {0}, cw[NBITS];
    put_bits(s, 0, 8, crc);
    s[8] = (uint8_t)last;
    put_bits(s, 9, 2, (uint64_t)b);
    put_bits(s, 11, 42, packed);
    memcpy(cw, s, NDATA);
    for (int i = 0; i < NDATA; i++) {
      int ctr = 0;
      for (int j = 0; j < NDATA; j++) { ctr += cw[j] * HRA_56_56[i][j]; }
      if (i > 0) { ctr += cw[NDATA + i - 1]; }
      cw[NDATA + i] = (uint8_t)(ctr & 1);
    }
    for (int i = 0; i < NBITS; i++) { t->bits[b][(INTERLEAVE * i) % NBITS] = cw[i]; }
  }
  t->blocks = blocks;
  return blocks;
}

float rade_v2_text_next(rade_v2_text_t *t) {
  if (t->blocks == 0) { return -1.0f; }
  float s = t->bits[t->blk][t->sym] ? -1.0f : 1.0f;
  if (++t->sym >= NBITS) {
    t->sym = 0;
    t->blk = (t->blk + 1) % t->blocks;
  }
  return s;
}

void rade_v2_text_restart(rade_v2_text_t *t) {
  t->blk = t->sym = 0;
}
