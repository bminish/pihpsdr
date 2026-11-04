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

#ifndef _RADE_NATIVE_TEST_H_
#define _RADE_NATIVE_TEST_H_

#include <stddef.h>

//
// Test harness for RADE transmit: a control socket a script can drive, so that over-the-air tests are
// repeatable without anyone clicking. It is the operator who enables it, by ticking "Test harness" in the
// RADE window (off whenever piHPSDR starts, off again when that window closes); until then the socket
// answers status and refuses everything else, and there is a hard limit on what can ever be keyed (the
// limits are constants in rade_native_menu.c: drive, band, over length).
//
// The socket is $XDG_STATE_HOME/pihpsdr-rade/control.sock (default ~/.local/state/pihpsdr-rade/), mode 0600.
// One request per line, one reply line: "ok key=value ..." or "err text". Values have no spaces. Any number of
// clients; a client that goes away during a play releases the transmitter.
//
//   status                                         what piHPSDR is doing
//   set wave=0|1|2 dial=HZ invert=0|1 drive_db=-60..6 trim_db=-20..20 agc=0|1 tx=0|1
//   capture on|off                                  the Capture button (reply: stem=)
//   log on|off                                      the Log button (reply: file=)
//   play file=/path.wav [gain_db=0] [max_s=90]      key, send the file as the mic, unkey, report times
//   abort                                           stop and unkey now (always honoured)
//
// "play" is run here, by polling "poll" and using "inject" / "key" / "unkey", which the owner's exec function
// (called on the GTK main thread) provides.
//
typedef int (*rade_test_exec_fn)(const char *verb, const char *args, char *reply, size_t n);

void        rade_test_start(rade_test_exec_fn fn);        // once; starts the socket thread

// 48 kHz mono PCM16 or float32 WAV -> samples (malloc'd, scaled by gain_db). 0 ok, -1 with a message.
int         rade_test_load_wav(const char *path, double gain_db, double max_s, float **out, long *n, char *err, size_t errn);

// key=value lookup in an argument string; returns 1 and copies the value if found
int         rade_test_arg(const char *args, const char *key, char *val, size_t n);

#endif
