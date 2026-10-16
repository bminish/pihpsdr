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

#ifndef _RADE_NATIVE_H_
#define _RADE_NATIVE_H_

#include <stddef.h>

//
// Native RADE V1/V2 through the WDSP I/Q chain. See docs/rade-native.md.
//
// The modem sits at a centre frequency above (normal) or below (inverted)
// the dial frequency; "which sideband" is not a question here. The
// geometry below is the part that is independent of the engine, so it can
// be tested offline.
//
enum {
  RADE_NATIVE_V1 = 1,
  RADE_NATIVE_V2 = 2
};

#define RADE_NATIVE_FC_V1       1475.0   // modem centre, Hz
#define RADE_NATIVE_FC_V2       1500.0
#define RADE_NATIVE_BW_V1       1600.0   // channel / TX filter width, Hz
#define RADE_NATIVE_BW_V2       1000.0
#define RADE_NATIVE_V1_INV_BELOW_HZ 10000000L  // V1 default: inverted (lower sideband) below this ...
#define RADE_NATIVE_60M_LOW_HZ       5250000L   // ... except on 60 m, where USB is the convention and both V1 and V2 default
#define RADE_NATIVE_60M_HIGH_HZ      5450000L   //     to upper sideband; Invert there means below the carrier, for both
int rade_native_band_class(long long dial_hz);      // 0: below 10 MHz (not 60 m), 1: 60 m, 2: 10 MHz and above

double rade_native_centre(int waveform);

//
// Whether the modem is inverted (below the dial frequency) by default on
// this dial frequency: V1 below 10 MHz, V2 never. The operator's Invert
// control is XORed with this.
//
int rade_native_default_invert(int waveform, long long dial_hz);
int rade_native_effective_invert(int waveform, long long dial_hz, int user_invert);

//
// The sideband in use (invert), as chosen when the waveform or the user's Invert last changed. The
// receiver and transmitter code read this; it is not recomputed from the dial, because the dial moves by
// 3 kHz when the sideband changes (see rade_native_plan()).
//
int  rade_native_get_invert(void);
void rade_native_set_invert(int invert);

//
// Where the energy sits. The dial of an SSB radio is its suppressed carrier; the modem's energy is
// RADE_NATIVE_DIAL_OFFSET above it (upper sideband, normal) or below it (lower sideband, inverted), for V1
// as for V2 (V1's carriers are centred on 1475 Hz; that stays in the filter, 1500 is used for the dial).
// The centre of the energy, Fe = dial + sign * 1500, is what is kept still: when the waveform, the invert
// or the mode changes, the dial moves so that Fe does not. So V1 on 1985.5 kHz is the LSB carrier 1987.0,
// V2 on 1985.5 kHz is the USB carrier 1984.0, and leaving RADE leaves the radio on the carrier of the last
// RADE sideband, in LSB or USB.
//
#define RADE_NATIVE_DIAL_OFFSET  1500

typedef struct {
  int       invert;        // the sideband to use: 1 lower (LSB-like), 0 upper
  long long dial;          // where the dial goes
} rade_native_plan_t;

//
// old_wave / old_invert: what RADE was doing (old_wave 0: it was off, and mode_sign, +1 for USB or DIGU,
// -1 for LSB or DIGL, 0 for anything else, says where the SSB energy is); new_wave 0 is "leaving RADE":
// the result is the sideband to hand back to, on the same dial. Otherwise the sideband is the band default
// for the energy centre, exclusive-or the user's Invert (V2: never inverted by default).
//
rade_native_plan_t rade_native_plan(int old_wave, int old_invert, int mode_sign, long long dial,
                                    int new_wave, int user_invert);

//
// WDSP passband edges (filter_low < filter_high) for the modem centred on
// its nominal frequency, shifted by shift_hz. Positive for a normal
// signal, negative for an inverted one: WDSP selects the sideband with the
// sign of the edges.
//
void rade_native_wdsp_edges(int waveform, int invert, double shift_hz,
                            double *low, double *high);

//
// WDSP's I/Q convention is the conjugate of librade's (measured, T-11):
// its positive passband selects the signal at negative frequency of
// I + jQ. The samples handed to or taken from librade are therefore
// conjugated for a normal signal and left alone for an inverted one,
// at both the RX and the TX interface.
//
int rade_native_conjugate(int invert);

//
// ---- receive engine (one receiver) ----
//
// The WDSP receive chain, set up with the RADE passband and its panel in
// binaural mode, delivers complex baseband at 48 kHz in the audio buffer
// (left = I, right = Q). rade_native_rx_block() takes that block, hands
// the signal to librade on a worker thread, and replaces the block with
// the decoded speech (left = right) at 48 kHz.
//
// waveform 0 switches the engine off. Changing the waveform closes and
// reopens the decoder, so call it from the menu thread, not the receiver
// thread.
//
int  rade_native_waveform(void);
int  rade_native_user_invert(void);
void rade_native_configure(int waveform, int user_invert);

//
// invert: the effective invert (see rade_native_effective_invert());
// gain: the RX volume as an amplitude.
//
void rade_native_rx_block(double *buf, int n, int invert, double gain);

typedef struct {
  int           waveform;
  int           sync;
  float         snr_db;      // librade's estimate, dB in 3 kHz
  float         foff_hz;     // librade's frequency offset
  unsigned long blocks;      // rade_rx() calls
  unsigned long underruns;   // 48 kHz speech samples not available in time
  unsigned long overruns;    // 8 kHz input samples dropped
  char          rx_call[40]; // V2: the callsign of the reception in progress ("" if none yet, or while a new lock is being checked against the last)
} rade_native_status_t;

void rade_native_get_status(rade_native_status_t *st);

//
// ---- transmit engine ----
//
// Mic samples (48 kHz, in the I slot of WDSP's TX input buffer) go through
// LPCNet feature extraction and rade_tx() on a worker thread, and come back
// as complex baseband at 48 kHz for the WDSP TX chain (panel select 3, the
// speech stages off, the TX bandpass set to the RADE edges).
//
// rade_native_tx_block() replaces the n samples in buf (interleaved I,Q;
// I holds the mic) by the modem signal, conjugated unless invert. When
// transmitting is 0 it clears buf and discards what is in flight. The
// output lags the input by the modem frame (V1 about 0.2 s).
//
// Needs a waveform to be set with rade_native_configure(). Not for use
// with PureSignal.
//
//
// Test harness: speech from memory in place of the mic. rade_native_tx_inject() takes n samples of 48 kHz
// mono (copied; NULL stops and frees). They are consumed from the first mic block after the key goes down,
// zeros before that, and the mic is silent again after the last. _state() returns 0 not armed, 1 armed or
// running, 2 all consumed, and the unix times of the first and last sample and how much has gone.
//
int  rade_native_tx_inject(const float *samples, long n);
int  rade_native_tx_inject_state(double *t0, double *t1, double *fraction);
// Unix times (seconds, CLOCK_REALTIME) of this over's events inside the transmit engine, 0 if not yet: [0] first keyed block,
// [1] end of over asked for, [2] end-of-over frame generated, [3] its last sample handed to WDSP, [4] hold finished (the
// radio may be released), [5] first block with the radio not keyed, [6] the first block of modem signal handed to WDSP (after the two-frame prefill). For lining the engine up with what a GPS-stamped receiver heard.
void rade_native_tx_times(double t[7]);

int  rade_native_tx_enabled(void);
#define RADE_LIVE_SAMPLES 8192      // 170 ms at 48 kHz, whatever the block size
int  rade_native_tx_live(void);      // a RADE over is being sent (not TUNE, two-tone, noise or SSB); what the Reporter is told
void rade_native_tx_enable(int on);
void rade_native_tx_block(double *buf, int n, int invert, int transmitting);

//
// ---- mic level ----
//
// The mic is trimmed, optionally levelled, and measured before it goes into
// the vocoder; it is measured whether or not the radio is transmitting, so it
// can be set before keying. The measure is ITU-R BS.1770 loudness (LUFS,
// K-weighted, mono), averaged with the standard gating over the last 10 s so
// that pauses do not count. The ideal is RADE_NATIVE_TARGET_LUFS: the median
// of the speech clips that come with rade_c (-24.7 to -17.1, median -19.9).
// librade passes the level through (its output is 1.5 to 2.7 dB below its
// input) and loses little between 10 dB under and 3 dB over that; louder
// than that the speech clips.
//
#define RADE_NATIVE_TARGET_LUFS   (-20.0)
#define RADE_NATIVE_RIGHT_DB      3.0     // "about right" is within this of the target

void rade_native_tx_set_trim_db(double db);       // -20 .. +20
double rade_native_tx_get_trim_db(void);
//
// Level into the WDSP TX chain, in dB around the defined one (which is 0: the level at which the
// ALC peaks just below its ceiling). -6 .. +6; above 0 the ALC starts to compress.
//
void rade_native_tx_set_drive_db(double db);        // -60 .. +6 (the slider in the window: -6 .. +6)
double rade_native_tx_get_drive_db(void);
void rade_native_tx_set_agc(int on);              // slow leveller towards the target
int  rade_native_tx_get_agc(void);

typedef struct {
  int    valid;          // 0: not enough speech yet to say (loud_avg and delta_db are meaningless)
  double loud_avg;       // gated average loudness, LUFS
  double momentary;      // last 400 ms, LUFS
  double delta_db;       // loud_avg - target: negative is too low
  double peak_dbfs;      // peak of the last second
  int    limiting;       // the limiter acted in the last second
  double agc_gain_db;    // what the leveller is adding (0 with the leveller off)
} rade_native_txlevel_t;

void rade_native_tx_level(rade_native_txlevel_t *l);

//
// End of over. With RADE transmit on, releasing the transmitter has to wait
// for the last speech, the end-of-over frame (with the callsign) and the
// delay through WDSP's TX filters, about 0.4 s in all. radio_set_mox(0)
// calls rade_native_tx_end_request(): a 1 means "hold the transmitter and
// ask again"; rade_native_tx_end_state() is 1 while the over is ending and
// 2 when it is safe to release. A key-down meanwhile calls
// rade_native_tx_end_cancel().
//
unsigned long rade_native_tx_underruns(void);   // output samples the worker was late with, since enable
int  rade_native_tx_end_request(void);
int  rade_native_tx_end_state(void);
void rade_native_tx_end_cancel(void);

//
// The callsign sent in the end-of-over frame (V1) and on the data-symbol channel (V2).
// Letters, digits and '/', upper-cased, at most 7 characters; anything else
// is dropped. Empty means none is sent.
//
void rade_native_set_callsign(const char *call);
const char *rade_native_get_callsign(void);

//
// Callsigns decoded from received end-of-over frames (V1) and from the V2 data-symbol channel. The engine
// queues them; the caller takes them with rade_native_call_pop() and adds
// what only it knows (dial frequency, mode).
//
typedef struct {
  char   call[40];       // V1: up to 8 characters; V2: up to 32
  double t_unix;
  float  snr_db;       // smoothed SNR over the over, before it ended
  float  foff_hz;
  int    waveform;
  int    snr_valid;    // 0: there was no lock to take an SNR from; snr_db means nothing and must not be reported
} rade_native_call_t;

int rade_native_call_pop(rade_native_call_t *c);

//
// Test hook: write the vocoder features fed to rade_tx() (float32, 36 per
// 10 ms frame, the layout rade_tx_wav -f writes) to path; NULL stops.
//
void rade_native_tx_feature_dump(const char *path);

//
// ---- capture (for iterating on the receiver) ----
//
// Records, into dir/stem_*:
//   _rx48.wav    the 48 kHz complex baseband WDSP delivered (float32,
//                left = I, right = Q, as the engine received it)
//   _speech.wav  the decoded speech as played (48 kHz mono, 16 bit)
//   _stats.csv   one row per rade_rx() call: sync, SNR, offset, buffer
//                levels, and the V2 per-symbol statistics
//   _events.log  timestamped state changes, as reported by the caller
//   _meta.txt    what the caller says about the radio at start and end
// The directory must exist. Returns 0 on success.
//
int  rade_native_capture_start(const char *dir, const char *stem, const char *meta);
void rade_native_capture_stop(const char *end_meta);
int  rade_native_capture_active(void);
void rade_native_capture_event(const char *text);

//
// Transmit side of a capture, called with the WDSP TX output block while
// transmitting: _tx.wav (float32 stereo at rate) and, from the engine,
// _txmic.wav (the 16 kHz speech that went into the vocoder).
//
void rade_native_capture_tx(const double *iq, int n, int rate);
void rade_native_capture_info(char *buf, size_t len);
const char *rade_native_capture_stem(void);   // file stem of the running capture, else ""

//
// Opens a V1 and a V2 context, checks the library version, and writes a
// one-line description to msg. Returns 0 on success.
//
int rade_native_probe(char *msg, size_t len);

#endif
