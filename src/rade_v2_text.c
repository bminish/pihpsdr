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

//
// ---- receive ----
//
// The same code as freedv-gui's: rade_text_rx_symbol() and the LDPC decoder of ldpc_code.h (sum-product belief
// propagation, BPSK, at most 10 iterations, noise variance from the spread of the symbol magnitudes), the rotation
// sweep (8 rotations per symbol, a unique winner or nothing), the combining accumulators and the block reassembly.
// The optional "confirm combined-only decodes" of freedv-gui is off there by default and is not here.
//

#include <math.h>
#include <limits.h>

#define RX_ITER        10
#define ROT_PER_CALL   8
#define COMBINE_DECAY  0.7f
#define COMBINE_COPIES 2
#define LLR_MAX        10000.0f

static int   rx_nedge = 0;
static int   rx_cstart[NDATA + 1];
static short rx_edge_v[NDATA * NBITS];
static short rx_vstart[NBITS + 1];
static short rx_vedge[NDATA * NBITS];     // edge numbers sorted by variable

static void rx_tables(void) {
  if (rx_nedge) { return; }
  int ne = 0;
  for (int i = 0; i < NDATA; i++) {
    rx_cstart[i] = ne;
    for (int j = 0; j < NBITS; j++) {
      if (HRA_56_56[i][j]) { rx_edge_v[ne++] = (short)j; }
    }
  }
  rx_cstart[NDATA] = ne;
  int k = 0;
  for (int j = 0; j < NBITS; j++) {
    rx_vstart[j] = (short)k;
    for (int e = 0; e < ne; e++) {
      if (rx_edge_v[e] == j) { rx_vedge[k++] = (short)e; }
    }
  }
  rx_vstart[NBITS] = (short)k;
  rx_nedge = ne;
}

static float rx_phi(float x) {
  if (x < 1e-10f) { return LLR_MAX; }
  double e = exp((double)x);
  if (e < 1e-10) { return LLR_MAX; }
  return (float)log((e + 1.0) / (e - 1.0));
}

static float rx_clamp(float v) { return v < -LLR_MAX ? -LLR_MAX : v > LLR_MAX ? LLR_MAX : v; }

// syms +-1, amps magnitudes; msg gets the 112 hard decisions; 1 if all parity checks hold within max_iter
static int rx_ldpc(const float *syms, const float *amps, float nv, uint8_t *msg, int max_iter) {
  rx_tables();
  if (nv < 1e-10f) { nv = 1e-10f; }
  float mean = 0.0f, llr[NBITS];
  for (int k = 0; k < NBITS; k++) { mean += amps[k]; }
  mean /= NBITS;
  for (int k = 0; k < NBITS; k++) {
    float rel = amps[k] / mean;
    llr[k] = rx_clamp(2.0f * rel * rel * syms[k] / nv);
  }
  static float mvc[NDATA * NBITS], mcv[NDATA * NBITS];
  int ne = rx_nedge;
  for (int e = 0; e < ne; e++) { mvc[e] = llr[rx_edge_v[e]]; mcv[e] = 0.0f; }
  for (int iter = 0; iter < max_iter; iter++) {
    for (int i = 0; i < NDATA; i++) {
      float phv[NBITS];
      int sign = 1, n = 0;
      float sum = 0.0f;
      for (int e = rx_cstart[i]; e < rx_cstart[i + 1]; e++, n++) {
        float v = mvc[e];
        sign *= (v >= 0.0f) ? 1 : -1;
        float p = rx_phi(fabsf(v));
        phv[n] = p > 0.0f ? p : 0.0f;
        sum += phv[n];
      }
      n = 0;
      for (int e = rx_cstart[i]; e < rx_cstart[i + 1]; e++, n++) {
        int isign = (mvc[e] >= 0.0f) ? 1 : -1;
        float p = rx_phi(sum - phv[n]);
        mcv[e] = rx_clamp(isign * sign * (p > 0.0f ? p : 0.0f));
      }
    }
    for (int j = 0; j < NBITS; j++) {
      float total = llr[j];
      for (int k = rx_vstart[j]; k < rx_vstart[j + 1]; k++) { total += mcv[rx_vedge[k]]; }
      for (int k = rx_vstart[j]; k < rx_vstart[j + 1]; k++) { mvc[rx_vedge[k]] = rx_clamp(total - mcv[rx_vedge[k]]); }
      float L = llr[j];
      for (int k = rx_vstart[j]; k < rx_vstart[j + 1]; k++) { L += mcv[rx_vedge[k]]; }
      msg[j] = (L < 0.0f) ? 1 : 0;
    }
    int ok = 1;
    for (int i = 0; i < NDATA && ok; i++) {
      int ctr = 0;
      for (int e = rx_cstart[i]; e < rx_cstart[i + 1]; e++) { ctr += msg[rx_edge_v[e]]; }
      ok = (ctr & 1) == 0;
    }
    if (ok) { return 1; }
  }
  return 0;
}

static uint64_t get_bits(const uint8_t *a, int start, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) { if (a[start + i]) { v |= (uint64_t)1 << i; } }
  return v;
}

// one candidate window in codeword order after the stream's interleave: 1 and the fields if it decodes with a good CRC
static int rx_try(const float *window, rade_v2_text_fields_t *out) {
  float syms[NBITS], amps[NBITS];
  for (int i = 0; i < NBITS; i++) {
    float v = window[(INTERLEAVE * i) % NBITS];
    float a = fabsf(v);
    amps[i] = a;
    syms[i] = a > 0.0f ? v / a : 0.0f;
  }
  float mean = 0.0f, var = 0.0f;
  for (int i = 0; i < NBITS; i++) { mean += fabsf(window[i]); }
  mean /= NBITS;
  for (int i = 0; i < NBITS; i++) { float d = fabsf(window[i]) - mean; var += d * d; }
  var /= NBITS - 1;
  if (var < 1e-6f) { var = 1e-6f; }
  uint8_t cw[NBITS];
  if (!rx_ldpc(syms, amps, var, cw, RX_ITER)) { return 0; }
  uint8_t crc = (uint8_t)get_bits(cw, 0, 8);
  uint8_t last = cw[8] != 0;
  uint8_t blk = (uint8_t)get_bits(cw, 9, 2);
  uint64_t packed = get_bits(cw, 11, 42);
  uint8_t chars[CHARS_PER_BLOCK];
  for (int i = CHARS_PER_BLOCK - 1; i >= 0; i--) { chars[i] = (uint8_t)(packed % ALPHABET); packed /= ALPHABET; }
  uint8_t buf[1 + CHARS_PER_BLOCK];
  buf[0] = (uint8_t)(blk | (last ? 4 : 0));
  memcpy(buf + 1, chars, CHARS_PER_BLOCK);
  if (crc8(buf, sizeof buf) != crc) { return 0; }
  out->block = blk; out->last = last;
  memcpy(out->chars, chars, CHARS_PER_BLOCK);
  return 1;
}

void rade_v2_text_rx_reset(rade_v2_text_rx_t *rx) {
  memset(rx->circ, 0, sizeof rx->circ);
  rx->wr = rx->filled = 0;
  rx->sweep_next = NBITS; rx->sweep_n = 0;
  memset(rx->acc, 0, sizeof rx->acc);
  memset(rx->acc_idx, 0, sizeof rx->acc_idx);
  rx->total = 0;
  memset(rx->asm_got, 0, sizeof rx->asm_got);
  rx->asm_total = -1;
  rx->asm_delivered = 0;
}

void rade_v2_text_rx_init(rade_v2_text_rx_t *rx) {
  memset(rx, 0, sizeof *rx);
  rade_v2_text_rx_reset(rx);
}

static int rx_ingest(rade_v2_text_rx_t *rx, const rade_v2_text_fields_t *f, char *msg) {
  if (f->block >= RADE_V2_TEXT_BLOCKS) { return 0; }
  if (f->block == 0) {
    memset(rx->asm_got, 0, sizeof rx->asm_got);
    rx->asm_total = -1;
    rx->asm_delivered = 0;
  }
  memcpy(rx->asm_chars[f->block], f->chars, CHARS_PER_BLOCK);
  rx->asm_got[f->block] = 1;
  if (f->last) { rx->asm_total = f->block + 1; }
  if (rx->asm_total < 0 || rx->asm_delivered) { return 0; }
  for (int b = 0; b < rx->asm_total; b++) { if (!rx->asm_got[b]) { return 0; } }
  static const char alpha[] = "\0" "0123456789" "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "/";
  int n = 0;
  for (int b = 0; b < rx->asm_total; b++) {
    for (int i = 0; i < CHARS_PER_BLOCK; i++) {
      uint8_t v = rx->asm_chars[b][i];
      if (v == 0) { goto done; }              // the first null ends the text
      if (v <= 37) { msg[n++] = alpha[v]; }
    }
  }
done:
  msg[n] = 0;
  rx->asm_delivered = 1;
  return 1;
}

int rade_v2_text_rx_symbol(rade_v2_text_rx_t *rx, float sym, char *msg) {
  rx->circ[rx->wr] = sym;
  rx->wr = (rx->wr + 1) % NBITS;
  for (int b = 0; b < RADE_V2_TEXT_BLOCKS; b++) {
    float *a = &rx->acc[b][rx->acc_idx[b]];
    *a = COMBINE_DECAY * *a + sym;
    rx->acc_idx[b] = (rx->acc_idx[b] + 1) % ((b + 1) * NBITS);
  }
  if (rx->total < INT_MAX) { rx->total++; }
  int just = 0;
  if (rx->filled < NBITS) {
    rx->filled++;
    just = rx->filled == NBITS;
  }
  if (rx->filled < NBITS) { return 0; }
  float window[NBITS];
  for (int i = 0; i < NBITS; i++) { window[i] = rx->circ[(rx->wr + i) % NBITS]; }
  if (just) {
    memcpy(rx->snap, window, sizeof window);
    rx->sweep_next = 0;
    rx->sweep_n = 0;
  }
  if (rx->sweep_next < NBITS) {
    float rot[NBITS];
    int end = rx->sweep_next + ROT_PER_CALL < NBITS ? rx->sweep_next + ROT_PER_CALL : NBITS;
    for (; rx->sweep_next < end; rx->sweep_next++) {
      for (int i = 0; i < NBITS; i++) { rot[i] = rx->snap[(i + rx->sweep_next) % NBITS]; }
      rade_v2_text_fields_t c;
      if (rx_try(rot, &c)) { rx->sweep_n++; rx->sweep_win = c; }
    }
    if (rx->sweep_next == NBITS && rx->sweep_n == 1) { return rx_ingest(rx, &rx->sweep_win, msg); }
    return 0;
  }
  rade_v2_text_fields_t f;
  int ok = rx_try(window, &f);
  for (int b = 0; !ok && b < RADE_V2_TEXT_BLOCKS; b++) {
    int period = (b + 1) * NBITS;
    if (rx->total < COMBINE_COPIES * period) { break; }
    float comb[NBITS];
    int start = rx->acc_idx[b] - NBITS + period;
    for (int i = 0; i < NBITS; i++) { comb[i] = rx->acc[b][(start + i) % period]; }
    ok = rx_try(comb, &f);
  }
  return ok ? rx_ingest(rx, &f, msg) : 0;
}
