/* Copyright (C)
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

#ifndef _SIDETONE_H
#define _SIDETONE_H

//
// Local CW side tone synthesised in the audio output callback (see docs/synth-sidetone/README.md).
//
// The module knows nothing about the radio: frequency and amplitude are passed in by the caller, so it can
// be tested stand-alone (test/sidetone/test_sidetone.c).
//

//
// Producer side. Same arguments as tx_queue_cw_event(): "wait" is in 1/48000 sec and is counted from the
// previous event being executed. Callable from the threads that call tx_queue_cw_event().
//
extern void sidetone_queue(int down, int wait);

//
// Consumer side (audio callback thread only).
//
// sidetone_render() writes n samples. The waits are counted in output samples, so the timing of the callbacks
// has no influence on the lengths of the elements. "amp" is the full amplitude (the product of volume and
// device factors).
//
extern void sidetone_render(double *out, int n, int freq, double amp);

//
// Drop all queued events and release the key. Called when not transmitting CW.
//
extern void sidetone_reset(void);

#endif
