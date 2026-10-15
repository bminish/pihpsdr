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
// Menu for the native RADE receiver (feature/rade-native). See
// docs/rade-native.md. Receiver 0 only.
//

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#include "agc.h"
#include "diversity_auto.h"
#include "message.h"
#include "new_menu.h"
#include "rade_launch.h"
#include "rade_native.h"
#include "rade_native_menu.h"
#include "ext.h"
#include "radio.h"
#include "transmitter.h"
#include <wdsp.h>
#include "receiver.h"
#include "mode.h"
#include "version.h"
#include "vfo.h"

static GtkWidget *dialog = NULL;
static GtkWidget *status_label = NULL;
static GtkWidget *edge_label = NULL;
static GtkWidget *call_e = NULL;
static GtkWidget *call_label = NULL;
static GtkWidget *recv_label = NULL;
static guint calls_timer_id = 0;
#define RECENT_CALLS 6
static char recent[RECENT_CALLS][160];
static int n_recent = 0;
static GtkWidget *mic_scale = NULL, *mic_agc_b = NULL, *mic_bar = NULL, *mic_text = NULL;
static GtkWidget *note_label = NULL;     // messages that must stay: the reporter's start, failures
static GtkWidget *tx_b = NULL;          // "RADE TX" check box: process transmit audio as RADE
static GtkWidget *mox_b = NULL;         // the transmit button
static GtkWidget *tx_label = NULL;
static GtkWidget *wave_b[3] = {NULL, NULL, NULL};
static GtkWidget *inv_b = NULL;
static int want_tx = 1;                 // RADE transmit follows the waveform unless switched off
static int updating = 0;                // ignore our own changes to the widgets
static int tx_applied = 0;
static GtkWidget *capture_b = NULL;
static GtkWidget *capture_label = NULL;
static guint timer_id = 0;
static guint event_timer_id = 0;
static char last_state[1024];


//
// Where the dial is (with CTUN, the dial is the CTUN frequency), and which side of it the SSB energy is.
//
static long long rade_dial(void) {
  return vfo[0].ctun ? vfo[0].ctun_frequency : vfo[0].frequency;
}

static int vfo_mode_sign(void) {
  switch (vfo[0].mode) {
  case modeUSB:
  case modeDIGU:
    return 1;
  case modeLSB:
  case modeDIGL:
    return -1;
  default:
    return 0;
  }
}

static const char *agc_name(int a);

// the last call heard in an end-of-over frame, for the Reporter (seq counts them)
static int heard_seq = 0;
static char heard_call[16] = "";
static float heard_snr = 0.0f;
static float heard_foff = 0.0f;
// the last few calls, newest last, so that two arriving close together are both told to the Reporter
#define HEARD_KEEP 5
static struct { int seq; char call[16]; float snr; double t; } heard_hist[HEARD_KEEP];
static int heard_n = 0;
// transmissions: how many have started, and when the last did, so that a short one is not missed
static int tx_count = 0;
static double tx_last_t = 0.0;
static double heard_t = 0.0;

#define CAPTURE_DIR "rade-captures"



//
// ---- callsigns ----
//
// Decoded from the end-of-over frame (V1) and logged, with what the radio
// was doing, to rade-captures/rade-calls.csv.
//
static void show_recent(void) {
  if (recv_label == NULL) { return; }
  char text[RECENT_CALLS * 164 + 32];
  text[0] = 0;
  if (n_recent == 0) { snprintf(text, sizeof text, "none yet"); }
  for (int i = 0; i < n_recent; i++) {
    strcat(text, recent[i]);
    strcat(text, "\n");
  }
  gtk_label_set_text(GTK_LABEL(recv_label), text);
}

static void log_call(const rade_native_call_t *c) {
  const RECEIVER *rx = receiver[0];
  mkdir(CAPTURE_DIR, 0755);
  const char *path = CAPTURE_DIR "/rade-calls.csv";
  int fresh = access(path, F_OK) != 0;
  FILE *f = fopen(path, "a");
  if (!f) { return; }
  if (fresh) {
    fputs("utc,local,callsign,waveform,dial_hz,mode,effective_invert,snr_db,foff_hz,"
          "rx_filter_low,rx_filter_high,agc,volume_db,adc,attenuation,preamp,capture\n", f);
  }
  time_t t = (time_t)c->t_unix;
  char utc[40], loc[40];
  strftime(utc, sizeof utc, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
  strftime(loc, sizeof loc, "%Y-%m-%d %H:%M:%S", localtime(&t));
  int inv = rade_native_get_invert();
  fprintf(f, "%s,%s,%s,V%d,%lld,%s,%d,%.1f,%.1f,%d,%d,%s,%.1f,%d,%d,%d,%s\n", utc, loc, c->call,
          c->waveform, rade_dial(), mode_string[vfo[0].mode], inv, c->snr_db, c->foff_hz,
          rx->filter_low, rx->filter_high, agc_name(rx->agc), rx->volume, rx->adc,
          adc[rx->adc].attenuation, adc[rx->adc].preamp, rade_native_capture_stem());
  fclose(f);
  snprintf(heard_call, sizeof heard_call, "%s", c->call);
  heard_snr = c->snr_db;
  heard_foff = c->foff_hz;
  heard_t = c->t_unix;
  heard_seq++;
  if (heard_n == HEARD_KEEP) { memmove(&heard_hist[0], &heard_hist[1], sizeof heard_hist[0] * (HEARD_KEEP - 1)); heard_n--; }
  heard_hist[heard_n].seq = heard_seq;
  snprintf(heard_hist[heard_n].call, sizeof heard_hist[heard_n].call, "%s", c->call);
  heard_hist[heard_n].snr = c->snr_db;
  heard_hist[heard_n].t = c->t_unix;
  heard_n++;
  if (rade_native_capture_active()) {
    char line[200];
    snprintf(line, sizeof line, "call %s snr=%.1f foff=%.1f", c->call, c->snr_db, c->foff_hz);
    rade_native_capture_event(line);
  }
  // newest first for the display
  if (n_recent < RECENT_CALLS) { n_recent++; }
  memmove(recent[1], recent[0], sizeof recent[0] * (n_recent - 1));
  char hms[16];
  strftime(hms, sizeof hms, "%H:%M:%S", localtime(&t));
  snprintf(recent[0], sizeof recent[0], "%s  %-8s V%d  SNR %.1f dB  %+.1f Hz  %.4f MHz", hms, c->call,
           c->waveform, c->snr_db, c->foff_hz, rade_dial() / 1e6);
  t_print("%s: RADE call %s\n", __func__, recent[0]);
}

static int calls_cb(gpointer arg) {
  rade_native_call_t c;
  int any = 0;
  while (rade_native_call_pop(&c)) {
    log_call(&c);
    any = 1;
  }
  if (any) { show_recent(); }
  if (rade_native_waveform() == 0) {
    calls_timer_id = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void call_changed_cb(GtkWidget *widget, gpointer data) {
  static int busy = 0;
  if (busy || widget == NULL) { return; }
  rade_native_set_callsign(gtk_entry_get_text(GTK_ENTRY(widget)));
  const char *c = rade_native_get_callsign();
  if (strcmp(c, gtk_entry_get_text(GTK_ENTRY(widget))) != 0) {
    busy = 1;                                   // show what will really be sent
    gtk_entry_set_text(GTK_ENTRY(widget), c);
    busy = 0;
  }
  char text[160];
  if (c[0] == 0) {
    snprintf(text, sizeof text, "no callsign: none is sent");
  } else if (rade_native_waveform() == RADE_NATIVE_V2) {
    snprintf(text, sizeof text, "%s: V2 callsign text is not implemented yet, nothing is sent", c);
  } else {
    snprintf(text, sizeof text, "%s is sent in the end-of-over frame", c);
  }
  if (call_label != NULL) { gtk_label_set_text(GTK_LABEL(call_label), text); }
}


//
// ---- the reporter ----
//
// The RADE Reporter (tools/rade-reporter) is a separate program with its own
// window. piHPSDR does not own it: it starts it detached, in its own session,
// so that it carries on when piHPSDR is closed. What it needs to know is in
// $XDG_STATE_HOME/pihpsdr-rade/state.json (default ~/.local/state/...), which
// is rewritten when it changes and at least every 2 s, atomically, and once
// more with "running":0 when piHPSDR exits. A reporter that finds the file
// older than 8 s takes it that piHPSDR has gone.
//
static char state_file[512];
static guint state_timer_id = 0;

static void state_write(int running) {
  if (state_file[0] == 0) { return; }
  char tmp[600], text[2200], call[16], ver[64], hist[1000];
  rade_native_status_t st;
  rade_native_get_status(&st);
  hist[0] = 0;
  for (int i = 0; i < heard_n; i++) {
    char one[100];
    snprintf(one, sizeof one, "%s{\"seq\":%d,\"call\":\"%s\",\"snr\":%.1f,\"t\":%.0f}", i ? "," : "",
             heard_hist[i].seq, heard_hist[i].call, heard_hist[i].snr, heard_hist[i].t);
    strncat(hist, one, sizeof hist - strlen(hist) - 1);
  }
  snprintf(call, sizeof call, "%s", rade_native_get_callsign());
  snprintf(ver, sizeof ver, "%s", build_version);
  for (char *c = ver; *c; c++) { if (*c == '"' || *c == '\\' || (unsigned char)*c < 32) { *c = '_'; } }
  snprintf(text, sizeof text,
           "{\"running\":%d,\"updated\":%.3f,\"pid\":%d,\"waveform\":%d,\"dial_hz\":%lld,"
           "\"tx\":%d,\"call\":\"%s\",\"version\":\"%s\","
           "\"rx_sync\":%d,\"rx_snr\":%.1f,\"rx_foff\":%.1f,"
           "\"heard_seq\":%d,\"heard_call\":\"%s\",\"heard_snr\":%.1f,\"heard_foff\":%.1f,\"heard_t\":%.0f,"
           "\"tx_count\":%d,\"tx_last_t\":%.0f,\"centre_hz\":%lld,\"heard\":[%s]}\n",
           running, g_get_real_time() / 1e6, (int)getpid(), rade_native_waveform(),
           rade_dial(), radio_is_transmitting() ? 1 : 0, call, ver,
           st.sync ? 1 : 0, st.snr_db, st.foff_hz, heard_seq, heard_call, heard_snr, heard_foff, heard_t, tx_count, tx_last_t,
           rade_dial() + (rade_native_get_invert() ? -1LL : 1LL) * RADE_NATIVE_DIAL_OFFSET, hist);
  snprintf(tmp, sizeof tmp, "%s.tmp", state_file);
  FILE *f = fopen(tmp, "w");
  if (!f) { return; }
  fputs(text, f);
  fclose(f);
  rename(tmp, state_file);
}

static void state_exit(void) {
  state_write(0);
}

static void band_default_changed(void);

static int state_cb(gpointer arg) {
  static char last[200];
  static gint64 last_t = 0;
  static int prev_tx = 0;
  char now[200];
  static int hf_side = -1;
  if (rade_native_waveform() == RADE_NATIVE_V1) {
    long long d = rade_dial();
    int side = d < 9900000 ? 0 : d > 10100000 ? 1 : hf_side;
    if (hf_side < 0) {
      hf_side = side;
    } else if (side != hf_side) {
      hf_side = side;
      band_default_changed();
    }
  } else {
    hf_side = -1;
  }
  int tx_now = radio_is_transmitting() ? 1 : 0;
  if (tx_now && !prev_tx) {
    tx_count++;
    tx_last_t = g_get_real_time() / 1e6;
  }
  prev_tx = tx_now;
  snprintf(now, sizeof now, "%d %lld %d %s %d %d", rade_native_waveform(), rade_dial(),
           tx_now, rade_native_get_callsign(), heard_seq, tx_count);
  gint64 t = g_get_monotonic_time();
  if (strcmp(now, last) != 0 || t - last_t >= 2000000) {
    state_write(1);
    strcpy(last, now);
    last_t = t;
  }
  return G_SOURCE_CONTINUE;
}

void rade_native_state_start(void) {
  if (state_timer_id != 0) { return; }
  const char *base = g_getenv("XDG_STATE_HOME");
  char dir[480];
  if (base && base[0]) {
    snprintf(dir, sizeof dir, "%s/pihpsdr-rade", base);
  } else {
    snprintf(dir, sizeof dir, "%s/.local/state/pihpsdr-rade", g_get_home_dir());
  }
  g_mkdir_with_parents(dir, 0755);
  snprintf(state_file, sizeof state_file, "%s/state.json", dir);
  atexit(state_exit);
  state_write(1);
  state_timer_id = g_timeout_add(250, state_cb, NULL);
}

static void say(const char *msg) {
  if (note_label != NULL) { gtk_label_set_text(GTK_LABEL(note_label), msg); }
  t_print("%s: %s\n", __func__, msg);
}

static void reporter_done(int early, int status, void *data) {
  char msg[900];
  if (early) {
    snprintf(msg, sizeof msg, "The Reporter stopped at once (status %d). See %s", status,
             (const char *)data);
  } else {
    snprintf(msg, sizeof msg, "The Reporter was closed");
  }
  say(msg);
}

static void reporter_cb(GtkWidget *widget, gpointer data) {
  static char logfile[600];
  char exe[512] = "", path[1024];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n > 0) { exe[n] = 0; }
  char *dir = g_path_get_dirname(exe);
  const char *rel = "tools/rade-reporter/rade_reporter.py";
  const char *where[] = {".", dir, NULL};
  int found = 0;
  for (int i = 0; i < 2 && !found; i++) {
    snprintf(path, sizeof path, "%s/%s", where[i], rel);
    found = access(path, R_OK) == 0;
  }
  g_free(dir);
  char msg[900];
  if (!found) {
    snprintf(msg, sizeof msg, "Reporter not found (looked for %s next to piHPSDR and in the current directory)", rel);
    say(msg);
    return;
  }
  // next to the state file, whatever XDG_STATE_HOME says
  char *sdir = g_path_get_dirname(state_file[0] ? state_file : "/tmp/x");
  snprintf(logfile, sizeof logfile, "%s/reporter.log", sdir);
  g_free(sdir);
  char *argv[] = {"python3", path, NULL};
  char err[200];
  if (rade_launch(argv, logfile, 5, reporter_done, logfile, err, sizeof err) == 0) {
    snprintf(msg, sizeof msg, "Reporter starting (log: %s). It keeps running when piHPSDR is closed", logfile);
  } else {
    snprintf(msg, sizeof msg, "Could not start the reporter: %s", err);
  }
  say(msg);
}


//
// ---- mic level ----
//
static void trim_cb(GtkWidget *widget, gpointer data) {
  if (updating) { return; }
  rade_native_tx_set_trim_db(gtk_range_get_value(GTK_RANGE(widget)));
}

static void agc_cb(GtkWidget *widget, gpointer data) {
  if (updating) { return; }
  rade_native_tx_set_agc(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}

//
// The bar runs from 20 dB under the ideal to 10 dB over it, so the ideal is
// two thirds of the way along; "about right" is 3 dB either side of it.
//
static void show_mic(void) {
  if (mic_bar == NULL) { return; }
  char text[200];
  if (!rade_native_tx_enabled()) {
    gtk_level_bar_set_value(GTK_LEVEL_BAR(mic_bar), 0.0);
    gtk_label_set_text(GTK_LABEL(mic_text), "RADE TX is off: the mic is not measured");
    return;
  }
  rade_native_txlevel_t l;
  rade_native_tx_level(&l);
  double d = l.valid ? l.delta_db : l.momentary - RADE_NATIVE_TARGET_LUFS;
  double f = (d + 20.0) / 30.0;
  gtk_level_bar_set_value(GTK_LEVEL_BAR(mic_bar), f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f);
  if (!l.valid) {
    snprintf(text, sizeof text, "Mic: speak normally for a few seconds to measure (now %.0f LUFS, peak %.0f dBFS)",
             l.momentary, l.peak_dbfs);
  } else {
    const char *verdict;
    char v[48];
    if (l.delta_db < -RADE_NATIVE_RIGHT_DB) {
      snprintf(v, sizeof v, "%.1f dB TOO LOW", -l.delta_db);
      verdict = v;
    } else if (l.delta_db > RADE_NATIVE_RIGHT_DB) {
      snprintf(v, sizeof v, "%.1f dB TOO HIGH", l.delta_db);
      verdict = v;
    } else {
      verdict = "about right";
    }
    snprintf(text, sizeof text, "Mic %.1f LUFS (ideal %.0f): %s   peak %.0f dBFS%s", l.loud_avg,
             RADE_NATIVE_TARGET_LUFS, verdict, l.peak_dbfs, l.limiting ? "   CLIPPING" : "");
    if (rade_native_tx_get_agc()) {
      char a[40];
      snprintf(a, sizeof a, "   AGC %+.1f dB", l.agc_gain_db);
      strncat(text, a, sizeof text - strlen(text) - 1);
    }
  }
  gtk_label_set_text(GTK_LABEL(mic_text), text);
}

//
// ---- capture ----
//
static const char *agc_name(int a) {
  static const char *n[] = {"off", "long", "slow", "medium", "fast", "custom", "fixed"};
  return a >= 0 && a < 7 ? n[a] : "?";
}

static const char *div_mode_name(int m) {
  static const char *n[] = {"manual", "auto-sum", "auto-null", "auto-best"};
  return m >= 0 && m < 4 ? n[m] : "?";
}

//
// The state that matters for judging a capture, as one line. The event
// log gets a line whenever this changes.
//
static void state_line(char *buf, size_t len) {
  const RECEIVER *rx = receiver[0];
  int w = rade_native_waveform();
  int inv = rade_native_get_invert();
  snprintf(buf, len,
           "f=%lld ctun=%d ctunf=%lld mode=%s filter=%d..%d rade=V%d inv=%d(user %d) "
           "agc=%s gain=%.0f hangthr=%.0f vol=%.1f nb=%d nr=%d snb=%d anf=%d "
           "adc=%d att=%d preamp=%d div=%d divmode=%s divref=%d divrun=%d",
           rade_dial(), vfo[0].ctun, vfo[0].ctun_frequency, mode_string[vfo[0].mode],
           rx->filter_low, rx->filter_high, w, inv, rade_native_user_invert(),
           agc_name(rx->agc), rx->agc_gain, rx->agc_hang_threshold, rx->volume,
           rx->nb, rx->nr, rx->snb, rx->anf, rx->adc, adc[rx->adc].attenuation,
           adc[rx->adc].preamp, diversity_enabled, div_mode_name(div_auto_mode),
           div_auto_ref, div_auto_running);
}

static void meta_text(char *buf, size_t len, const char *what) {
  const RECEIVER *rx = receiver[0];
  int w = rade_native_waveform();
  int inv = rade_native_get_invert();
  double lo, hi;
  rade_native_wdsp_edges(w, inv, 0.0, &lo, &hi);
  time_t t = time(NULL);
  char ts[64];
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S %Z", localtime(&t));
  snprintf(buf, len,
           "--- %s ---\n"
           "time: %s\n"
           "build: %s (%s)\n"
           "rade_waveform: V%d\n"
           "rade_user_invert: %d\n"
           "rade_effective_invert: %d\n"
           "rade_centre_hz: %.1f\n"
           "rade_passband_wdsp: %.0f..%.0f\n"
           "dial_hz: %lld\n"
           "ctun: %d  ctun_hz: %lld\n"
           "rit: %d %lld\n"
           "vfo_mode: %s\n"
           "rx_filter: %d..%d\n"
           "rx_sample_rate: %d\n"
           "rx_adc: %d  antenna: %d  attenuation: %d  preamp: %d  dither: %d  random: %d\n"
           "agc: %s  agc_gain: %.1f  agc_hang_threshold: %.1f  agc_hang: %.1f\n"
           "volume_db: %.1f\n"
           "nb: %d  nr: %d  snb: %d  anf: %d\n"
           "rade_tx: %d  tx_drive: %.1f  tx_puresignal: %d  tx_filter: %d..%d  mic_gain: %.1f\n"
           "diversity_enabled: %d\n"
           "div_mode: %s\n"
           "div_ref: %d  div_running: %d\n"
           "div_weight: cos %.4f sin %.4f  norm %.3f\n"
           "man_div: gain %.2f phase %.1f\n"
           "files: _tx.wav = float32 stereo, the WDSP TX output block (at the TX IQ rate), _txmic.wav = 16 kHz speech into the vocoder; _rx48.wav = float32 stereo 48 kHz, L=I R=Q as WDSP delivered it "
           "(WDSP sign convention, includes RX volume); _speech.wav = decoded speech as "
           "played; _stats.csv rx48_pos indexes into _rx48.wav\n",
           what, ts, build_version, build_commit, w, rade_native_user_invert(), inv,
           rade_native_centre(w), lo, hi, rade_dial(), vfo[0].ctun, vfo[0].ctun_frequency,
           vfo[0].rit_enabled, vfo[0].rit, mode_string[vfo[0].mode], rx->filter_low,
           rx->filter_high, rx->sample_rate, rx->adc, adc[rx->adc].antenna,
           adc[rx->adc].attenuation, adc[rx->adc].preamp, adc[rx->adc].dither,
           adc[rx->adc].random, agc_name(rx->agc), rx->agc_gain, rx->agc_hang_threshold,
           rx->agc_hang, rx->volume, rx->nb, rx->nr, rx->snb, rx->anf, rade_native_tx_enabled(),
           transmitter ? transmitter->drive : 0.0, transmitter ? transmitter->puresignal : 0,
           transmitter ? transmitter->filter_low : 0, transmitter ? transmitter->filter_high : 0,
           transmitter ? transmitter->mic_gain : 0.0, diversity_enabled,
           div_mode_name(div_auto_mode), div_auto_ref, div_auto_running, auto_div_cos,
           auto_div_sin, div_norm, man_div_gain, man_div_phase);
}

static int event_cb(gpointer arg) {
  if (!rade_native_capture_active()) {
    event_timer_id = 0;
    return G_SOURCE_REMOVE;
  }
  char now[1024];
  state_line(now, sizeof now);
  if (strcmp(now, last_state) != 0) {
    rade_native_capture_event(now);
    strcpy(last_state, now);
  }
  return G_SOURCE_CONTINUE;
}

static void capture_stop(void) {
  if (!rade_native_capture_active()) { return; }
  char meta[3072];
  meta_text(meta, sizeof meta, "end");
  rade_native_capture_stop(meta);
}

static void capture_start(void) {
  mkdir(CAPTURE_DIR, 0755);
  int w = rade_native_waveform();
  int inv = rade_native_get_invert();
  time_t t = time(NULL);
  char ts[32], stem[128], meta[3072];
  strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&t));
  snprintf(stem, sizeof stem, "%s_%lldkHz_RADE-V%d%s", ts, rade_dial() / 1000, w, inv ? "-inv" : "");
  meta_text(meta, sizeof meta, "start");
  if (rade_native_capture_start(CAPTURE_DIR, stem, meta) != 0) {
    t_print("%s: could not start capture in %s\n", __func__, CAPTURE_DIR);
    return;
  }
  state_line(last_state, sizeof last_state);
  rade_native_capture_event(last_state);
  if (event_timer_id == 0) { event_timer_id = g_timeout_add(500, event_cb, NULL); }
}

static void capture_cb(GtkWidget *widget, gpointer data) {
  if (updating) { return; }
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
    if (rade_native_waveform() == 0) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);
      return;
    }
    capture_start();
    if (!rade_native_capture_active()) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);
    }
  } else {
    capture_stop();
  }
}

static void cleanup(void) {
  if (timer_id != 0) {
    g_source_remove(timer_id);
    timer_id = 0;
  }
  recv_label = call_label = call_e = NULL;
  tx_b = mox_b = tx_label = inv_b = capture_b = capture_label = status_label = edge_label = NULL;
  wave_b[0] = wave_b[1] = wave_b[2] = NULL;
  mic_scale = mic_agc_b = mic_bar = mic_text = note_label = NULL;
  if (dialog != NULL) {
    //
    // This window is deliberately not "sub_menu": the main menu and the
    // other menus come and go (new_menu.c destroys sub_menu whenever one
    // is opened) but this one stays until it is closed.
    //
    GtkWidget *tmp = dialog;
    dialog = NULL;
    gtk_widget_destroy(tmp);
  }
}

static gboolean close_cb(void) {
  cleanup();
  return TRUE;
}

//
// Re-apply the receiver settings that depend on the RADE state: the
// passband and the panel's binaural flag.
//
static void apply(void) {
  rx_filter_changed(receiver[0]);
  rx_set_af_binaural(receiver[0]);
}

static void show_edges(void) {
  char text[128];
  int w = rade_native_waveform();
  if (w == 0) {
    snprintf(text, sizeof text, "RADE off");
  } else {
    int inv = rade_native_get_invert();
    double lo, hi;
    rade_native_wdsp_edges(w, inv, 0.0, &lo, &hi);
    snprintf(text, sizeof text, "Modem %s the dial frequency: %.0f to %.0f Hz (centre %.0f Hz)",
             inv ? "below" : "above", inv ? -hi : lo, inv ? -lo : hi, rade_native_centre(w));
  }
  gtk_label_set_text(GTK_LABEL(edge_label), text);
}

//
// The WDSP side of RADE transmit: the same "no speech processing" state the
// captured-data transmit uses (EQ, leveler, CFC, compressor, DEXP off, mic
// gain 1), and the TX panel passing I and Q instead of the mic sample. The
// bandpass follows through tx_set_filter(). Undone on the way out.
//
static void tx_apply(int on) {
  if (transmitter == NULL || on == tx_applied) { return; }
  if (on) {
    tx_xmit_captured_data_start(transmitter);
    SetTXAPHROTRun(transmitter->id, 0);          // the phase rotator too
    SetTXAPanelSelect(transmitter->id, 3);
  } else {
    SetTXAPanelSelect(transmitter->id, 2);
    tx_xmit_captured_data_end(transmitter);
    SetTXAPHROTRun(transmitter->id, transmitter->phrot_enable);
  }
  tx_applied = on;
  tx_set_filter(transmitter);
}

static int tx_ready(void) {
  return transmitter != NULL && rade_native_waveform() != 0 && rade_native_tx_enabled();
}

static void show_tx_state(void) {
  if (tx_label == NULL) { return; }
  const char *t;
  if (rade_native_waveform() == 0) {
    t = "Select V1 or V2";
  } else if (transmitter == NULL) {
    t = "No transmitter";
  } else if (transmitter->puresignal) {
    t = "RADE transmit is not available with PureSignal";
  } else if (!tx_ready()) {
    t = "TX is NOT RADE: the transmit button here is disabled, other TX buttons send ordinary speech";
  } else {
    t = rade_native_waveform() == RADE_NATIVE_V1 ? "TX: RADE V1" : "TX: RADE V2";
  }
  gtk_label_set_text(GTK_LABEL(tx_label), t);
}

//
// Switch RADE transmit on if it is wanted and possible (a waveform is set,
// there is a transmitter, no PureSignal).
//
static void tx_enable_now(void) {
  if (!want_tx || transmitter == NULL || transmitter->puresignal || rade_native_waveform() == 0) {
    show_tx_state();
    return;
  }
  if (!rade_native_tx_enabled()) { rade_native_tx_enable(1); }
  if (rade_native_tx_enabled()) { tx_apply(1); }
  show_tx_state();
}

static void tx_cb(GtkWidget *widget, gpointer data) {
  if (updating) { return; }
  want_tx = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if (want_tx) {
    tx_enable_now();
  } else {
    rade_native_tx_enable(0);
    tx_apply(0);
    show_tx_state();
  }
}

//
// Transmit, from the button or the space bar in this window. The same as the
// space bar in the main window, except that it refuses unless RADE transmit
// is on, so that it cannot send ordinary speech by mistake.
//
static void rade_ptt(void) {
  if (radio_is_transmitting()) {
    g_timeout_add(ptt_delay, ext_radio_set_mox, GINT_TO_POINTER(0));
  } else if (tx_ready()) {
    radio_set_mox(1);
  } else {
    show_tx_state();
  }
}

static void mox_cb(GtkWidget *widget, gpointer data) {
  if (updating) { return; }
  int want = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if (want != radio_is_transmitting()) { rade_ptt(); }
  // the real state is shown by update_cb() a moment later
}

static gboolean key_cb(GtkWidget *widget, GdkEventKey *event, gpointer data) {
  if (event->keyval == GDK_KEY_space) {
    rade_ptt();
    return TRUE;              // not typed into the callsign box
  }
  return FALSE;
}

//
// Change the waveform and/or the sideband without moving the energy. rade_native_plan() works out the
// sideband and the dial; the VFO goes to DIGU or DIGL while RADE is on, and back to USB or LSB (the same
// carrier, so the same place in the band) when RADE is switched off. The state is set before the mode and
// frequency change, because that recomputes the receive and transmit passbands.
//
static void retune(int new_wave) {
  int old_wave = rade_native_waveform();
  if (old_wave == 0 && new_wave == 0) { return; }
  rade_native_plan_t p = rade_native_plan(old_wave, rade_native_get_invert(), vfo_mode_sign(), rade_dial(),
                                          new_wave, rade_native_user_invert());
  rade_native_set_invert(p.invert);
  rade_native_configure(new_wave, rade_native_user_invert());
  int mode = new_wave ? (p.invert ? modeDIGL : modeDIGU) : (p.invert ? modeLSB : modeUSB);
  if (p.dial != rade_dial()) { vfo_id_set_frequency(0, p.dial); }
  if (vfo[0].mode != mode) { vfo_id_mode_changed(0, mode); }
}

//
// V1 is inverted below 10 MHz and not above. If the operator tunes across 10 MHz (by more than 100 kHz, so
// that the 3 kHz moves made here can never trigger it) the sideband follows the new band, and the dial is
// left where the operator put it.
//
static void band_default_changed(void) {
  int inv = rade_native_effective_invert(RADE_NATIVE_V1, rade_dial(), rade_native_user_invert());
  if (inv == rade_native_get_invert()) { return; }
  rade_native_set_invert(inv);
  vfo_id_mode_changed(0, inv ? modeDIGL : modeDIGU);
  apply();
  if (rade_native_tx_enabled() && transmitter != NULL) { tx_set_filter(transmitter); }
  show_edges();
}

static void waveform_cb(GtkWidget *widget, gpointer data) {
  if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) { return; }
  retune(GPOINTER_TO_INT(data));
  if (rade_native_waveform() != 0 && calls_timer_id == 0) {
    calls_timer_id = g_timeout_add(500, calls_cb, NULL);
  }
  call_changed_cb(call_e, NULL);              // the note depends on the waveform
  if (!rade_native_tx_enabled()) {
    tx_apply(0);                 // changing waveform switches the TX engine off ...
    tx_enable_now();             // ... and it is switched on again for the new one
  }
  updating = 1;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(capture_b), rade_native_capture_active());
  updating = 0;
  apply();
  show_edges();
}

static void invert_cb(GtkWidget *widget, gpointer data) {
  rade_native_configure(rade_native_waveform(), gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  retune(rade_native_waveform());       // the sideband flips and the dial moves 3 kHz, the energy stays
  apply();
  if (rade_native_tx_enabled() && transmitter != NULL) { tx_set_filter(transmitter); }
  show_edges();
}

static int update_cb(gpointer arg) {
  rade_native_status_t st;
  char text[200];
  rade_native_get_status(&st);
  if (st.waveform == 0) {
    snprintf(text, sizeof text, "-");
  } else {
    snprintf(text, sizeof text, "%s  SNR %.1f dB  offset %+.1f Hz  (underruns %lu, overruns %lu)",
             st.sync ? "SYNC" : "searching", st.snr_db, st.foff_hz, st.underruns, st.overruns);
  }
  gtk_label_set_text(GTK_LABEL(status_label), text);
  rade_native_capture_info(text, sizeof text);
  gtk_label_set_text(GTK_LABEL(capture_label), text);
  updating = 1;
  if (!rade_native_capture_active() &&
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(capture_b))) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(capture_b), FALSE);
  }
  int tx = radio_is_transmitting();
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mox_b), tx);
  gtk_button_set_label(GTK_BUTTON(mox_b), tx ? "TRANSMITTING - click or space to stop" : "Transmit (space bar)");
  // the waveform and the TX processing are not changed in mid-over
  for (int i = 0; i < 3; i++) { gtk_widget_set_sensitive(wave_b[i], !tx); }
  gtk_widget_set_sensitive(inv_b, !tx);
  gtk_widget_set_sensitive(tx_b, !tx);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tx_b), want_tx);
  updating = 0;
  show_tx_state();
  show_mic();
  return G_SOURCE_CONTINUE;
}

void rade_native_menu(GtkWidget *parent) {
  if (dialog != NULL) {
    gtk_window_present(GTK_WINDOW(dialog));
    return;
  }
  dialog = gtk_dialog_new();
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent));
  gtk_window_set_keep_above(GTK_WINDOW(dialog), TRUE);
  GtkWidget *headerbar = gtk_header_bar_new();
  gtk_window_set_titlebar(GTK_WINDOW(dialog), headerbar);
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(headerbar), TRUE);
  gtk_header_bar_set_title(GTK_HEADER_BAR(headerbar), "piHPSDR - RADE");
  g_signal_connect (dialog, "delete_event", G_CALLBACK (close_cb), NULL);
  g_signal_connect (dialog, "destroy", G_CALLBACK (close_cb), NULL);
  g_signal_connect (dialog, "key-press-event", G_CALLBACK (key_cb), NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing (GTK_GRID(grid), 10);
  gtk_grid_set_row_spacing (GTK_GRID(grid), 10);
  GtkWidget *close_b = gtk_button_new_with_label("Close");
  gtk_widget_set_name(close_b, "close_button");
  g_signal_connect (close_b, "button-press-event", G_CALLBACK(close_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), close_b, 0, 0, 1, 1);
  int w = rade_native_waveform();
  wave_b[0] = gtk_radio_button_new_with_label(NULL, "Off");
  wave_b[1] = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(wave_b[0]), "RADE V1");
  wave_b[2] = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(wave_b[0]), "RADE V2");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wave_b[w == RADE_NATIVE_V1 ? 1 : w == RADE_NATIVE_V2 ? 2 : 0]), TRUE);
  g_signal_connect(wave_b[0], "toggled", G_CALLBACK(waveform_cb), GINT_TO_POINTER(0));
  g_signal_connect(wave_b[1], "toggled", G_CALLBACK(waveform_cb), GINT_TO_POINTER(RADE_NATIVE_V1));
  g_signal_connect(wave_b[2], "toggled", G_CALLBACK(waveform_cb), GINT_TO_POINTER(RADE_NATIVE_V2));
  for (int i = 0; i < 3; i++) { gtk_grid_attach(GTK_GRID(grid), wave_b[i], 1 + i, 0, 1, 1); }
  inv_b = gtk_check_button_new_with_label("Invert");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inv_b), rade_native_user_invert());
  g_signal_connect(inv_b, "toggled", G_CALLBACK(invert_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), inv_b, 4, 0, 1, 1);
  GtkWidget *rep_b = gtk_button_new_with_label("Reporter");
  gtk_widget_set_tooltip_text(rep_b, "Opens the RADE Reporter in its own window. It is a separate program: "
                                     "it stays open when piHPSDR is closed, and starts with reporting off.");
  g_signal_connect(rep_b, "clicked", G_CALLBACK(reporter_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), rep_b, 5, 0, 1, 1);
  edge_label = gtk_label_new("");
  gtk_widget_set_halign(edge_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), edge_label, 0, 1, 5, 1);
  status_label = gtk_label_new("-");
  gtk_widget_set_halign(status_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), status_label, 0, 2, 5, 1);
  mox_b = gtk_toggle_button_new_with_label("Transmit (space bar)");
  g_signal_connect(mox_b, "toggled", G_CALLBACK(mox_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), mox_b, 0, 3, 3, 1);
  tx_b = gtk_check_button_new_with_label("RADE TX");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tx_b), want_tx);
  g_signal_connect(tx_b, "toggled", G_CALLBACK(tx_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), tx_b, 3, 3, 1, 1);
  tx_label = gtk_label_new("");
  gtk_widget_set_halign(tx_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), tx_label, 0, 4, 5, 1);
  GtkWidget *mic_l = gtk_label_new("Mic trim");
  gtk_widget_set_halign(mic_l, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), mic_l, 0, 5, 1, 1);
  mic_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -20.0, 20.0, 0.5);
  gtk_scale_set_value_pos(GTK_SCALE(mic_scale), GTK_POS_RIGHT);
  gtk_scale_add_mark(GTK_SCALE(mic_scale), 0.0, GTK_POS_BOTTOM, NULL);
  gtk_range_set_value(GTK_RANGE(mic_scale), rade_native_tx_get_trim_db());
  gtk_widget_set_hexpand(mic_scale, TRUE);
  gtk_widget_set_tooltip_text(mic_scale, "dB added to the mic before RADE. Set the mic itself in the system mixer or "
                                         "on the radio; use this to finish the job. (The Mic Gain slider has no effect on RADE.)");
  g_signal_connect(mic_scale, "value-changed", G_CALLBACK(trim_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), mic_scale, 1, 5, 3, 1);
  mic_agc_b = gtk_check_button_new_with_label("AGC");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mic_agc_b), rade_native_tx_get_agc());
  gtk_widget_set_tooltip_text(mic_agc_b, "Slowly moves the level to the ideal (about 1 dB a second, faster for a shout). "
                                         "It holds when you stop talking.");
  g_signal_connect(mic_agc_b, "toggled", G_CALLBACK(agc_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), mic_agc_b, 4, 5, 1, 1);
  mic_bar = gtk_level_bar_new_for_interval(0.0, 1.0);
  gtk_widget_set_name(mic_bar, "radelevel");
  gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(mic_bar), GTK_LEVEL_BAR_OFFSET_LOW, 17.0 / 30.0);    // 3 dB under the ideal
  gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(mic_bar), GTK_LEVEL_BAR_OFFSET_HIGH, 23.0 / 30.0);   // 3 dB over
  gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(mic_bar), GTK_LEVEL_BAR_OFFSET_FULL, 27.0 / 30.0);   // 7 dB over
  gtk_widget_set_size_request(mic_bar, -1, 14);
  {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "#radelevel block.filled { background-color: #2e9e4a; }"
        "#radelevel block.filled.low { background-color: #e08a00; }"
        "#radelevel block.filled.high { background-color: #e0a000; }"
        "#radelevel block.filled.full { background-color: #d03030; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
  }
  gtk_grid_attach(GTK_GRID(grid), mic_bar, 0, 6, 5, 1);
  mic_text = gtk_label_new("");
  gtk_widget_set_halign(mic_text, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), mic_text, 0, 7, 5, 1);
  capture_b = gtk_toggle_button_new_with_label("Capture");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(capture_b), rade_native_capture_active());
  g_signal_connect(capture_b, "toggled", G_CALLBACK(capture_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), capture_b, 0, 8, 1, 1);
  capture_label = gtk_label_new("not capturing");
  gtk_widget_set_halign(capture_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), capture_label, 1, 8, 4, 1);
  GtkWidget *call_l = gtk_label_new("My call");
  gtk_widget_set_halign(call_l, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), call_l, 0, 9, 1, 1);
  call_e = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(call_e), 7);
  gtk_entry_set_width_chars(GTK_ENTRY(call_e), 8);
  gtk_entry_set_text(GTK_ENTRY(call_e), rade_native_get_callsign());
  gtk_grid_attach(GTK_GRID(grid), call_e, 1, 9, 1, 1);
  call_label = gtk_label_new("");
  gtk_widget_set_halign(call_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), call_label, 2, 9, 3, 1);
  g_signal_connect(call_e, "changed", G_CALLBACK(call_changed_cb), NULL);
  call_changed_cb(call_e, NULL);
  GtkWidget *recv_l = gtk_label_new("Received calls");
  gtk_widget_set_halign(recv_l, GTK_ALIGN_END);
  gtk_widget_set_valign(recv_l, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), recv_l, 0, 10, 1, 1);
  recv_label = gtk_label_new("");
  gtk_widget_set_halign(recv_label, GTK_ALIGN_START);
  gtk_label_set_selectable(GTK_LABEL(recv_label), TRUE);
  gtk_grid_attach(GTK_GRID(grid), recv_label, 1, 10, 4, 1);
  show_recent();
  GtkWidget *hint = gtk_label_new("Receiver 0. The energy stays where it is: choosing V1 or V2, or Invert, moves the dial (3 kHz for a sideband change) and sets DIGU or DIGL; Off returns to USB or LSB on the same carrier. V1 is lower sideband below 10 MHz, V2 upper. This window stays open while you use other menus.");
  gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(hint), 60);
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), hint, 0, 11, 5, 1);
  note_label = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(note_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(note_label), 60);
  gtk_label_set_selectable(GTK_LABEL(note_label), TRUE);
  gtk_widget_set_halign(note_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), note_label, 0, 12, 6, 1);
  gtk_container_add(GTK_CONTAINER(content), grid);
  show_edges();
  show_tx_state();
  gtk_widget_show_all(dialog);
  gtk_widget_grab_focus(mox_b);                 // not the callsign box
  //
  // Park the window in the lower right corner of the main window, out of
  // the way of the menus, which open at the top. (A window manager that
  // does not allow programs to place windows, such as Wayland's, will
  // ignore this; the window can be dragged.)
  //
  gint x, y, mw, mh;
  GtkRequisition nat;
  gtk_window_get_position(GTK_WINDOW(parent), &x, &y);
  gtk_window_get_size(GTK_WINDOW(parent), &mw, &mh);
  gtk_widget_get_preferred_size(dialog, NULL, &nat);
  gtk_window_move(GTK_WINDOW(dialog), x + mw - nat.width - 10, y + mh - nat.height - 10);
  timer_id = g_timeout_add(250, update_cb, NULL);
}
