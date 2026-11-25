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


// VFO dials as a frequency or as a wavelength (feet and inches, or metres) (devtest/Joker).

#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

#include "wavelength.h"

#define C_M_PER_S 299792458LL

int vfo_dial_units = DIAL_FEET;

int wavelength_from_hz(long long hz, WAVELENGTH *w) {
  memset(w, 0, sizeof(*w));
  w->den = 1;
  if (hz <= 0) { return 0; }
  // c * 144 / (0.0254 * hz) = c * 1440000 / (254 * hz), in 1/144 inch, rounded to the nearest
  long long d = 254LL * hz;
  long long units = (C_M_PER_S * (10000LL * WAVELENGTH_DENOM) + d / 2) / d;
  w->feet = units / (12 * WAVELENGTH_DENOM);
  units  %= 12 * WAVELENGTH_DENOM;
  w->inches = (int)(units / WAVELENGTH_DENOM);
  int num = (int)(units % WAVELENGTH_DENOM);
  if (num) {
    int a = WAVELENGTH_DENOM, b = num;
    while (b) { int t = a % b; a = b; b = t; }
    w->num = num / a;
    w->den = WAVELENGTH_DENOM / a;
  }
  return 1;
}

long long wavelength_um(long long hz) {
  if (hz <= 0) { return 0; }
  return (C_M_PER_S * 1000000LL + hz / 2) / hz;
}

double wavelength_shift_m(long long hz, long long offset) {
  if (hz <= 0 || hz + offset <= 0) { return 0.0; }
  double f = (double)hz;
  return -(double)C_M_PER_S * (double)offset / (f * (double)(hz + offset));
}

double wavelength_shift_mils(long long hz, long long offset) {
  return wavelength_shift_m(hz, offset) / 0.0254 * WAVELENGTH_MILS;
}

//
// The face: the first of these that is installed. Latin Modern Dunhill has the tall
// narrow figures of a Victorian drawing office; the others are of the same period style.
//
static const char *dial_font(void) {
  static const char *wanted[] = { "Latin Modern Roman Dunhill", "LM Roman Dunhill 10", "Old Standard TT",
                                  "Old Standard", "C059", "Century Schoolbook L", "Latin Modern Roman"
                                };
  static const char *found = NULL;
  if (found) { return found; }
  found = "serif";
  PangoFontFamily **fam = NULL;
  int n = 0;
  pango_font_map_list_families(pango_cairo_font_map_get_default(), &fam, &n);
  for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
    for (int j = 0; j < n; j++) {
      if (!g_ascii_strcasecmp(pango_font_family_get_name(fam[j]), wanted[i])) {
        found = wanted[i];
        g_free(fam);
        return found;
      }
    }
  }
  g_free(fam);
  return found;
}

static double advance(cairo_t *cr, double size, const char *s) {
  cairo_text_extents_t e;
  cairo_set_font_size(cr, size);
  cairo_text_extents(cr, s, &e);
  return e.x_advance;
}

static void put(cairo_t *cr, double x, double y, double size, const char *s) {
  cairo_set_font_size(cr, size);
  cairo_move_to(cr, x, y);
  cairo_show_text(cr, s);
}

// Tapered foot / inch marks, drawn because the face has no prime
static void prime(cairo_t *cr, double x, double y, double size) {
  cairo_move_to(cr, x + 0.02 * size, y - 0.44 * size);
  cairo_line_to(cr, x + 0.08 * size, y - 0.75 * size);
  cairo_line_to(cr, x + 0.17 * size, y - 0.72 * size);
  cairo_close_path(cr);
  cairo_fill(cr);
}

// Draws (or, with draw = 0, only measures) starting at x; returns the width
static double layout(cairo_t *cr, double x, double y, double size, const WAVELENGTH *w, int draw) {
  char ft[24], in[12], nu[12], de[12];
  const double x0 = x, small = 0.43 * size, gap = 0.16 * size;
  snprintf(ft, sizeof(ft), "%lld", w->feet);
  snprintf(in, sizeof(in), "%d", w->inches);
  snprintf(nu, sizeof(nu), "%d", w->num);
  snprintf(de, sizeof(de), "%d", w->den);
  if (w->feet > 0) {
    if (draw) { put(cr, x, y, size, ft); }
    x += advance(cr, size, ft);
    if (draw) { prime(cr, x, y, size); }
    x += 0.18 * size + gap;
  }
  if (w->inches > 0 || w->num == 0) {
    if (draw) { put(cr, x, y, size, in); }
    x += advance(cr, size, in) + 0.06 * size;
  }
  if (w->num) {
    double wn = advance(cr, small, nu), wd = advance(cr, small, de);
    double wf = fmax(wn, wd);
    if (draw) {
      put(cr, x + 0.5 * (wf - wn), y - 0.38 * size, small, nu);
      put(cr, x + 0.5 * (wf - wd), y + 0.14 * size, small, de);
      cairo_rectangle(cr, x, y - 0.32 * size, wf, fmax(1.0, 0.035 * size));
      cairo_fill(cr);
    }
    x += wf + 0.03 * size;
  }
  if (draw) {
    prime(cr, x, y, size);
    prime(cr, x + 0.13 * size, y, size);
  }
  return x + 0.31 * size - x0;
}

// Right-aligned, made smaller if it would not fit between left and right
void wavelength_draw(cairo_t *cr, double left, double right, double y, double size, const WAVELENGTH *w) {
  cairo_save(cr);
  cairo_select_font_face(cr, dial_font(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  double room = right - left, width = layout(cr, 0, 0, size, w, 0);
  if (width > room && room > 0) {
    size *= room / width;
    width = layout(cr, 0, 0, size, w, 0);
  }
  layout(cr, right - width, y, size, w, 1);
  cairo_restore(cr);
}

// Metres to three decimals, then the micrometres in smaller figures, as the Hz are on a frequency dial
void wavelength_draw_metres(cairo_t *cr, double left, double right, double y, double size, double small, long long um) {
  char big[32], rest[8];
  snprintf(big, sizeof(big), "%lld.%03d", um / 1000000LL, (int)(um / 1000LL % 1000LL));
  snprintf(rest, sizeof(rest), "%03d", (int)(um % 1000LL));
  cairo_save(cr);
  cairo_select_font_face(cr, dial_font(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  double room = right - left, w1, w2, w3, gap, width;
  for (int pass = 0; pass < 2; pass++) {
    gap = 0.10 * size;
    w1 = advance(cr, size, big);
    w2 = advance(cr, small, rest);
    w3 = advance(cr, small, "m");
    width = w1 + w2 + w3 + 2.0 * gap;
    if (pass || width <= room || room <= 0) { break; }
    size  *= room / width;
    small *= room / width;
  }
  double x = right - width;
  put(cr, x, y, size, big);
  put(cr, x + w1 + gap, y, small, rest);
  put(cr, x + w1 + w2 + 2.0 * gap, y, small, "m");
  cairo_restore(cr);
}
