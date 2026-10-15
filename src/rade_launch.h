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


#ifndef _RADE_LAUNCH_H_
#define _RADE_LAUNCH_H_

#include <stddef.h>

//
// Starts a program that is not piHPSDR's: its own session, no standard input,
// its output appended to logfile (so a start-up failure leaves a trace), none
// of piHPSDR's open files. It is not waited for and is not stopped when
// piHPSDR exits.
//
// If the program ends, done(early, status, data) is called from the GLib main
// loop (and only while piHPSDR is still running); early is 1 if that was
// within grace_s seconds of the start, i.e. it did not start properly, and
// status is its exit status, or -signal. Returns 0, or -1 with a message in
// err.
//
typedef void (*rade_launch_done)(int early, int status, void *data);

int rade_launch(char *const argv[], const char *logfile, int grace_s,
                rade_launch_done done, void *data, char *err, size_t errlen);

#endif
