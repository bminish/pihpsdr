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

//
// Stereo recorder for RX1's audio output: exactly the pair handed to
// audio_write(), so with the diversity ear split on it is both ears.
// 16-bit PCM WAV, 48 kHz, streamed to disk by a writer thread, no length
// limit. The caller hands it audio from before the AF gain (see
// rx_process_buffer()), so the level does not follow the knob.
// Optionally a CSV beside it with one row per rx_process_buffer() pass
// (see ear_record_block()), used by test/diversity/devtools/py/ears.py.
//
// One recording at a time. The owner tag says which control started it,
// so each control stops only its own.
//
#ifndef _EAR_RECORD_H_
#define _EAR_RECORD_H_

#include <stddef.h>

enum {
  EAR_REC_NONE = 0,
  EAR_REC_WAV,          // the Diversity menu's WAV button
  EAR_REC_DIVCAP        // beside an I/Q capture (make DIVCAP=1)
};

//
// Read once per output frame by the receive thread, with no lock.
//
extern volatile int ear_record_active;

//
// GTK thread. start returns 1 if the files opened; csv may be NULL.
// stop does nothing unless owner started the recording in progress.
//
extern int  ear_record_start(const char *wav_path, const char *csv_path, int owner);
extern void ear_record_stop(int owner);
extern int  ear_record_owner(void);

//
// "12.3 s" while recording, "" otherwise.
//
extern void ear_record_status(char *buf, size_t len);

//
// Receive thread. put: one output frame. block: one row per
// rx_process_buffer() pass - who (receiver id), mode (div_split in force,
// 0 when not split), paired (RX1's split pass: whether RX0's half was
// there, else -1), rx1_cnt (RX0's pass: receiver[1]->samples when RX0's
// buffer filled, 1023 = aligned, else -1), nsamp, the balance gains, and
// the AF gain setting in dB that was taken out.
//
extern void ear_record_put(double left, double right);
extern void ear_record_block(int who, int mode, int paired, int rx1_cnt, int nsamp,
                             double bal_l, double bal_r, double af_db);

#endif
