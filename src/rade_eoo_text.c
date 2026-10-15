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
// RADE V1 end-of-over text: a callsign carried in the EOO frame, in the form
// freedv-gui sends and expects.
//
// A C port of the V1 text channel in freedv-gui (src/pipeline/rade_text.cpp,
// ldpc_encode.cpp, ldpc_decode.cpp, HRA_56_56.h): up to 8 characters from a
// 6-bit set, a CRC-8, LDPC(112,56), a stride-37 interleave over the 56 QPSK
// symbols, and the rest of the EOO's 90 symbols a known sequence that the
// receiver uses to estimate the noise. Original work by David Rowe and
// Mooneer Salem, whose terms apply to this file:
//
//==========================================================================
//
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//==========================================================================

//
// Used by src/rade_eoo_text.c (piHPSDR). Copied unchanged from freedv-gui
// (src/pipeline/HRA_56_56.h), with the licence above.
//

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rade_eoo_text.h"
#include "rade_eoo_text_hra.h"

#define NSYM        56            // QPSK symbols carrying the codeword
#define NBITS       112           // codeword bits
#define NDATA       56            // data bits
#define MAXCHARS    8
#define LLR_MAX     10000.0f
#define INTERLEAVER_B 37

// 6 bit character set: 0 null, 1-9 ASCII 38-46, 10-19 '0'-'9',
// 20-45 'A'-'Z', 46 '/'
static int to_ota(const char *in, unsigned char *out, int max) {
  int n = 0;
  for (int i = 0; i < max && in[i] && n < MAXCHARS; i++) {
    char c = in[i];
    if (c >= 38 && c <= 46) { out[n++] = c - 37; }
    else if (c == '/') { out[n++] = 46; }
    else if (c >= '0' && c <= '9') { out[n++] = c - '0' + 10; }
    else if (isalpha((unsigned char)c)) { out[n++] = toupper((unsigned char)c) - 'A' + 20; }
  }
  return n;
}

static int from_ota(const unsigned char *in, int n, char *out) {
  int k = 0;
  for (int i = 0; i < n && in[i]; i++) {
    unsigned char c = in[i];
    if (c >= 1 && c <= 9) { out[k++] = c + 37; }
    else if (c >= 10 && c <= 19) { out[k++] = c - 10 + '0'; }
    else if (c >= 20 && c <= 45) { out[k++] = c - 20 + 'A'; }
    else if (c == 46) { out[k++] = '/'; }
  }
  out[k] = 0;
  return k;
}

static unsigned char crc8(const unsigned char *p, int n) {
  unsigned char crc = 0;
  for (int i = 0; i < n && p[i]; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) { crc = (crc & 0x80) ? (unsigned char)((crc << 1) ^ 0x1D) : (unsigned char)(crc << 1); }
  }
  return crc;
}

static void ldpc_encode(const uint8_t *s, uint8_t *cw) {
  for (int i = 0; i < NDATA; i++) { cw[i] = s[i]; }
  for (int i = 0; i < NDATA; i++) {
    int ctr = 0;
    for (int j = 0; j < NDATA; j++) { ctr += cw[j] * HRA_56_56[i][j]; }
    if (i > 0) { ctr += cw[NDATA + i - 1]; }
    cw[NDATA + i] = ctr & 1;
  }
}

int rade_eoo_text_encode(const char *call, float *eoo, int nfloats) {
  if (nfloats < 2 * NSYM) { return -1; }
  unsigned char ota[MAXCHARS + 1];
  memset(ota, 0, sizeof ota);
  int n = to_ota(call, ota, MAXCHARS);
  unsigned char crc = crc8(ota, n);
  uint8_t data[NDATA] = {0}, cw[NBITS], inter[NBITS];
  for (int b = 0; b < 8; b++) { data[b] = (crc >> b) & 1; }
  for (int b = 8; b < NDATA; b++) {
    int k = b - 8;
    data[b] = (ota[k / 6] >> (k % 6)) & 1;
  }
  ldpc_encode(data, cw);
  for (int i = 0; i < NSYM; i++) {
    int j = (INTERLEAVER_B * i) % NSYM;
    inter[2 * j] = cw[2 * i];
    inter[2 * j + 1] = cw[2 * i + 1];
  }
  for (int i = 0; i < NSYM; i++) {
    int b0 = inter[2 * i], b1 = inter[2 * i + 1];
    eoo[2 * i] = b0 ? (b1 ? -1.0f : 0.0f) : (b1 ? 0.0f : 1.0f);
    eoo[2 * i + 1] = b0 ? (b1 ? 0.0f : -1.0f) : (b1 ? 1.0f : 0.0f);
  }
  for (int i = 2 * NSYM; i < nfloats; i++) { eoo[i] = (i % 2) ? 0.0f : 1.0f; }
  return 0;
}

static float max_star(float x, float y) {
  float m = x > y ? x : y, d = fabsf(x - y), l = 0.0f;
  if (d <= 2.45f) { l = -0.24f * d + 0.596f; }
  else if (d <= 3.5f) { l = 0.048f; }
  return m + l;
}

static float like(const float *s, float var, float re, float im, float amp, float avg) {
  float er = s[0] - re, ei = s[1] - im, a = amp / avg;
  return -var * a * a * (er * er + ei * ei);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float phi(float x) {
  if (x < 1e-10f) { return LLR_MAX; }
  double e = exp((double)x);
  if (e < 1e-10) { return LLR_MAX; }
  return (float)log((e + 1.0) / (e - 1.0));
}

// returns 1 if all parity checks are satisfied
static int ldpc_decode(const float *syms, const float *amps, float noise_var, uint8_t *msg, int max_iter) {
  static int edge_v[NSYM * NBITS];
  static int nedge = 0;
  static int cstart[NSYM + 1];
  if (!nedge) {
    for (int i = 0; i < NSYM; i++) {
      cstart[i] = nedge;
      for (int j = 0; j < NBITS; j++) {
        if (HRA_56_56[i][j]) { edge_v[nedge] = j; nedge++; }
      }
    }
    cstart[NSYM] = nedge;
  }
  if (noise_var < 1e-10f) { noise_var = 1e-10f; }
  float llr[NBITS];
  float mean = 0.0f;
  for (int k = 0; k < NSYM; k++) { mean += amps[k]; }
  mean /= NSYM;
  float esno = 1.0f / (2.0f * noise_var);
  for (int k = 0; k < NSYM; k++) {
    float a = amps[k], num[2], den[2];
    float L[4] = {like(&syms[2 * k], esno, 1, 0, a, mean), like(&syms[2 * k], esno, 0, 1, a, mean),
                  like(&syms[2 * k], esno, 0, -1, a, mean), like(&syms[2 * k], esno, -1, 0, a, mean)};
    num[0] = num[1] = den[0] = den[1] = -LLR_MAX;
    den[0] = max_star(den[0], L[0]);
    den[1] = max_star(den[1], L[0]);
    den[0] = max_star(den[0], L[1]);
    num[1] = max_star(num[1], L[1]);
    num[0] = max_star(num[0], L[2]);
    den[1] = max_star(den[1], L[2]);
    num[0] = max_star(num[0], L[3]);
    num[1] = max_star(num[1], L[3]);
    llr[2 * k] = -clampf(num[0] - den[0], -LLR_MAX, LLR_MAX);
    llr[2 * k + 1] = -clampf(num[1] - den[1], -LLR_MAX, LLR_MAX);
  }
  static float mvc[NSYM * NBITS], mcv[NSYM * NBITS];
  for (int e = 0; e < nedge; e++) { mvc[e] = llr[edge_v[e]]; mcv[e] = 0.0f; }
  for (int iter = 0; iter < max_iter; iter++) {
    for (int i = 0; i < NSYM; i++) {
      int sign = 1;
      float sum = 0.0f;
      for (int e = cstart[i]; e < cstart[i + 1]; e++) {
        sign *= (mvc[e] >= 0.0f) ? 1 : -1;
        float p = phi(fabsf(mvc[e]));
        sum += p > 0.0f ? p : 0.0f;
      }
      for (int e = cstart[i]; e < cstart[i + 1]; e++) {
        int isign = (mvc[e] >= 0.0f) ? 1 : -1;
        float p = phi(sum - phi(fabsf(mvc[e])));
        mcv[e] = clampf(isign * sign * (p > 0.0f ? p : 0.0f), -LLR_MAX, LLR_MAX);
      }
    }
    float total[NBITS];
    for (int j = 0; j < NBITS; j++) { total[j] = llr[j]; }
    for (int e = 0; e < nedge; e++) { total[edge_v[e]] += mcv[e]; }
    for (int e = 0; e < nedge; e++) { mvc[e] = clampf(total[edge_v[e]] - mcv[e], -LLR_MAX, LLR_MAX); }
    for (int j = 0; j < NBITS; j++) { msg[j] = (total[j] < 0.0f) ? 1 : 0; }
    int ok = 1;
    for (int i = 0; i < NSYM && ok; i++) {
      int ctr = 0;
      for (int j = 0; j < NBITS; j++) { ctr += HRA_56_56[i][j] * msg[j]; }
      ok = (ctr & 1) == 0;
    }
    if (ok) { return 1; }
  }
  return 0;
}

int rade_eoo_text_decode(const float *eoo, int nsyms, char *call) {
  call[0] = 0;
  if (nsyms < NSYM + 2) { return 0; }
  float syms[2 * NSYM], amps[NSYM];
  for (int i = 0; i < NSYM; i++) {
    int j = (INTERLEAVER_B * i) % NSYM;
    syms[2 * i] = eoo[2 * j];
    syms[2 * i + 1] = eoo[2 * j + 1];
  }
  // noise from the known tail, which is the symbol (1, 0)
  float ss = 0.0f;
  int cnt = 0;
  for (int i = NSYM; i < nsyms; i++) {
    float a = sqrtf(eoo[2 * i] * eoo[2 * i] + eoo[2 * i + 1] * eoo[2 * i + 1]);
    if (a > 0.0f) {
      float re = eoo[2 * i] / a, im = eoo[2 * i + 1] / a;
      ss += (1.0f - re) * (1.0f - re) + im * im;
      cnt += 2;
    }
  }
  float noise_var = cnt > 1 ? ss / (cnt - 1) : 1.0f;
  for (int i = 0; i < NSYM; i++) {
    float a = sqrtf(syms[2 * i] * syms[2 * i] + syms[2 * i + 1] * syms[2 * i + 1]);
    amps[i] = a;
    if (a > 0.0f) { syms[2 * i] /= a; syms[2 * i + 1] /= a; }
  }
  uint8_t msg[NBITS];
  if (!ldpc_decode(syms, amps, noise_var, msg, 30)) { return 0; }
  unsigned char crc = 0, ota[MAXCHARS + 1];
  memset(ota, 0, sizeof ota);
  for (int b = 0; b < 8; b++) { if (msg[b]) { crc |= 1 << b; } }
  for (int b = 8; b < NDATA; b++) {
    int k = b - 8;
    if (msg[b]) { ota[k / 6] |= 1 << (k % 6); }
  }
  if (crc != crc8(ota, MAXCHARS)) { return 0; }
  return from_ota(ota, MAXCHARS, call) > 0;
}
