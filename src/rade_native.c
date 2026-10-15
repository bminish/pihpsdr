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

#include <stdio.h>

#include "rade_api.h"
#include "rade_native.h"

double rade_native_centre(int waveform) {
  return waveform == RADE_NATIVE_V2 ? RADE_NATIVE_FC_V2 : RADE_NATIVE_FC_V1;
}

double rade_native_width(int waveform) {
  return waveform == RADE_NATIVE_V2 ? RADE_NATIVE_BW_V2 : RADE_NATIVE_BW_V1;
}

int rade_native_default_invert(int waveform, long long dial_hz) {
  return waveform == RADE_NATIVE_V1 && dial_hz < RADE_NATIVE_V1_INV_BELOW_HZ;
}

int rade_native_effective_invert(int waveform, long long dial_hz, int user_invert) {
  return rade_native_default_invert(waveform, dial_hz) ^ (user_invert != 0);
}

void rade_native_wdsp_edges(int waveform, int invert, double shift_hz,
                            double *low, double *high) {
  double fc = rade_native_centre(waveform) + shift_hz;
  double half = 0.5 * rade_native_width(waveform);
  if (invert) {
    *low = -(fc + half);
    *high = -(fc - half);
  } else {
    *low = fc - half;
    *high = fc + half;
  }
}

int rade_native_conjugate(int invert) {
  return !invert;
}

int rade_native_probe(char *msg, size_t len) {
  rade_initialize();
  struct rade *r1 = rade_open("", RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0);
  struct rade *r2 = rade_open("", RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0 | RADE_MODE_V2);
  int ok = r1 != NULL && r2 != NULL;
  if (ok) {
    snprintf(msg, len, "librade %d.%d: V1 nin_max=%d tx=%d, V2 nin_max=%d tx=%d",
             rade_version(), rade_version_minor(),
             rade_nin_max(r1), rade_n_tx_out(r1), rade_nin_max(r2), rade_n_tx_out(r2));
  } else {
    snprintf(msg, len, "rade_open failed (V1 %s, V2 %s)", r1 ? "ok" : "no", r2 ? "ok" : "no");
  }
  if (r1) { rade_close(r1); }
  if (r2) { rade_close(r2); }
  rade_finalize();
  return ok ? 0 : -1;
}
