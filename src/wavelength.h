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

#ifndef _WAVELENGTH_H
#define _WAVELENGTH_H

#include <cairo.h>

#define WAVELENGTH_DENOM 144       // the dial reads to 1/144 of an inch
#define WAVELENGTH_MILS 1000        // mils per inch, for RIT/XIT

// What the VFO dials show (Theme menu)
enum { DIAL_FREQUENCY = 0, DIAL_FEET, DIAL_METRES, DIAL_UNITS };
extern int vfo_dial_units;

typedef struct {
  long long feet;
  int inches;                      // 0 ... 11
  int num, den;                    // fraction in lowest terms; num = 0 if there is none
} WAVELENGTH;

int wavelength_from_hz(long long hz, WAVELENGTH *w);             // 0 if hz is not a positive frequency
long long wavelength_um(long long hz);                           // micrometres, rounded; 0 if hz is not positive
double wavelength_shift_m(long long hz, long long offset);       // change of wavelength (metres) when hz is moved by offset
double wavelength_shift_mils(long long hz, long long offset);   // the same in mils
void wavelength_draw_metres(cairo_t *cr, double left, double right, double y, double size, double small, long long um);
void wavelength_draw(cairo_t *cr, double left, double right, double y, double size, const WAVELENGTH *w);

#endif
