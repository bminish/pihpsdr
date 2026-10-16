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
#include "rade_native_log.h"
#include "rade_native_test.h"
#include "vfo.h"

static GtkWidget *dialog = NULL;
static GtkWidget *status_label = NULL;
static GtkWidget *edge_label = NULL;
static GtkWidget *call_e = NULL;
static GtkWidget *call_label = NULL;
static GtkWidget *recv_label = NULL;
static guint calls_timer_id = 0;
#define RECENT_CALLS 4
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
static GtkWidget *drive_scale = NULL, *log_b = NULL, *log_label = NULL;
static GtkWidget *test_b = NULL, *test_label = NULL;
static int test_armed = 0;              // the operator has ticked Test harness: a script may key the transmitter
static int test_keyed = 0;              // ... and it has done so, and so is the one to release
static char test_stem[160] = "";
static double test_t_mox_on = 0.0, test_t_mox_off = 0.0;   // when the harness called radio_set_mox(1) / (0), unix seconds

static double test_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}
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

//
// Transmitting RADE: keyed, a RADE waveform on and its transmit engine in use, and not a TUNE or two-tone
// (which key the radio too but are not RADE). This, not radio_is_transmitting(), is what the Reporter is told:
// an SSB over, or a tune, in RADE mode is not reported as a RADE transmission.
//
static int rade_is_tx(void) {
  return radio_is_transmitting() && rade_native_waveform() != 0 && rade_native_tx_enabled() &&
         transmitter != NULL && !transmitter->tune && !transmitter->twotone;
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
static int heard_snr_valid = 0;
static float heard_foff = 0.0f;
// the last few calls, newest last, so that two arriving close together are both told to the Reporter
#define HEARD_KEEP 5
static struct { int seq; char call[16]; float snr; int snr_valid; double t; } heard_hist[HEARD_KEEP];
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
  for (int i = 0; i < RECENT_CALLS; i++) {            // always the same number of lines, so the window does not change size
    if (i < n_recent) { strcat(text, recent[i]); } else if (i == 0) { strcat(text, "none yet"); }
    if (i < RECENT_CALLS - 1) { strcat(text, "\n"); }
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
  char snr_s[16] = "";
  if (c->snr_valid) { snprintf(snr_s, sizeof snr_s, "%.1f", c->snr_db); }          // no lock, no SNR: blank, not 0
  fprintf(f, "%s,%s,%s,V%d,%lld,%s,%d,%s,%.1f,%d,%d,%s,%.1f,%d,%d,%d,%s\n", utc, loc, c->call,
          c->waveform, rade_dial(), mode_string[vfo[0].mode], inv, snr_s, c->foff_hz,
          rx->filter_low, rx->filter_high, agc_name(rx->agc), rx->volume, rx->adc,
          adc[rx->adc].attenuation, adc[rx->adc].preamp, rade_native_capture_stem());
  fclose(f);
  snprintf(heard_call, sizeof heard_call, "%s", c->call);
  heard_snr = c->snr_db;
  heard_snr_valid = c->snr_valid;
  heard_foff = c->foff_hz;
  heard_t = c->t_unix;
  heard_seq++;
  if (heard_n == HEARD_KEEP) { memmove(&heard_hist[0], &heard_hist[1], sizeof heard_hist[0] * (HEARD_KEEP - 1)); heard_n--; }
  heard_hist[heard_n].seq = heard_seq;
  snprintf(heard_hist[heard_n].call, sizeof heard_hist[heard_n].call, "%s", c->call);
  heard_hist[heard_n].snr = c->snr_db;
  heard_hist[heard_n].snr_valid = c->snr_valid;
  heard_hist[heard_n].t = c->t_unix;
  heard_n++;
  if (rade_log_active()) {
    char sn[16] = "none";
    if (c->snr_valid) { snprintf(sn, sizeof sn, "%.1f", c->snr_db); }
    rade_log_event("call %s snr=%s", c->call, sn);
  }
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
  char sn[24] = "    -   ";
  if (c->snr_valid) { snprintf(sn, sizeof sn, "%5.1f dB", c->snr_db); }
  snprintf(recent[0], sizeof recent[0], "%s  %-8s V%d  %s  %+6.1f Hz  %8.4f MHz", hms, c->call,
           c->waveform, sn, c->foff_hz, rade_dial() / 1e6);
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
  (void)c;                                      // no commentary on it: what is sent is the Call box
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
    char one[120], sn[16] = "null";
    if (heard_hist[i].snr_valid) { snprintf(sn, sizeof sn, "%.1f", heard_hist[i].snr); }
    snprintf(one, sizeof one, "%s{\"seq\":%d,\"call\":\"%s\",\"snr\":%s,\"t\":%.0f}", i ? "," : "",
             heard_hist[i].seq, heard_hist[i].call, sn, heard_hist[i].t);
    strncat(hist, one, sizeof hist - strlen(hist) - 1);
  }
  // SNR is told only while there is a lock (null otherwise: 0 is a real SNR); likewise for the call heard
  char rx_snr_s[16] = "null", heard_snr_s[16] = "null";
  if (st.sync) { snprintf(rx_snr_s, sizeof rx_snr_s, "%.1f", st.snr_db); }
  if (heard_snr_valid) { snprintf(heard_snr_s, sizeof heard_snr_s, "%.1f", heard_snr); }
  snprintf(call, sizeof call, "%s", rade_native_get_callsign());
  snprintf(ver, sizeof ver, "%s", build_version);
  for (char *c = ver; *c; c++) { if (*c == '"' || *c == '\\' || (unsigned char)*c < 32) { *c = '_'; } }
  snprintf(text, sizeof text,
           "{\"running\":%d,\"updated\":%.3f,\"pid\":%d,\"waveform\":%d,\"dial_hz\":%lld,"
           "\"tx\":%d,\"call\":\"%s\",\"version\":\"%s\","
           "\"rx_sync\":%d,\"rx_snr\":%s,\"rx_foff\":%.1f,"
           "\"heard_seq\":%d,\"heard_call\":\"%s\",\"heard_snr\":%s,\"heard_foff\":%.1f,\"heard_t\":%.0f,"
           "\"tx_count\":%d,\"tx_last_t\":%.0f,\"centre_hz\":%lld,\"heard\":[%s]}\n",
           running, g_get_real_time() / 1e6, (int)getpid(), rade_native_waveform(),
           rade_dial(), rade_is_tx() ? 1 : 0, call, ver,
           st.sync ? 1 : 0, rx_snr_s, st.foff_hz, heard_seq, heard_call, heard_snr_s, heard_foff, heard_t, tx_count, tx_last_t,
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

static int log_events_fresh = 0;        // set when logging starts: the next poll writes every setting as an event

static int state_cb(gpointer arg) {
  static char last[200];
  static gint64 last_t = 0;
  static int prev_tx = 0;
  char now[200];
  static int band_cls = -1;                       // 0 below 10 MHz, 1 60 m, 2 above: the default sideband changes between them
  if (rade_native_waveform() != 0) {
    long long d = rade_dial();
    int cls = rade_native_band_class(d);
    if (cls != 1 && band_cls >= 0 && band_cls != 1 && llabs(d - RADE_NATIVE_V1_INV_BELOW_HZ) < 100000) { cls = band_cls; }   // 100 kHz of hysteresis at 10 MHz
    if (band_cls < 0) {
      band_cls = cls;
    } else if (cls != band_cls) {
      band_cls = cls;
      band_default_changed();
    }
  } else {
    band_cls = -1;
  }
  int tx_now = rade_is_tx() ? 1 : 0;
  if (tx_now && !prev_tx) {
    tx_count++;
    tx_last_t = g_get_real_time() / 1e6;
  }
  prev_tx = tx_now;
  snprintf(now, sizeof now, "%d %lld %d %s %d %d", rade_native_waveform(), rade_dial(),
           tx_now, rade_native_get_callsign(), heard_seq, tx_count);
  if (rade_log_active()) {
    // the settings as events, each when it changes (and all of them once, at the start: log_events_reset())
    static long long p_dial = -1;
    static int p_wave = -1, p_inv = -1, p_txe = -1, p_agc = -1;
    static char p_mode[8] = "";
    static double p_drive = 1e9, p_trim = 1e9;
    if (log_events_fresh) { p_dial = -1; p_wave = p_inv = p_txe = p_agc = -1; p_mode[0] = 0; p_drive = p_trim = 1e9; log_events_fresh = 0; }
    long long d = rade_dial();
    const char *mo = mode_string[vfo[0].mode];
    if (rade_native_waveform() != p_wave) { p_wave = rade_native_waveform(); rade_log_event("wave %d", p_wave); }
    if (d != p_dial) { p_dial = d; rade_log_event("dial %lld", d); }
    if (strcmp(mo, p_mode) != 0) { snprintf(p_mode, sizeof p_mode, "%s", mo); rade_log_event("mode %s", mo); }
    if (rade_native_get_invert() != p_inv) { p_inv = rade_native_get_invert(); rade_log_event("invert %d", p_inv); }
    if (rade_native_tx_enabled() != p_txe) { p_txe = rade_native_tx_enabled(); rade_log_event("tx_enabled %d", p_txe); }
    if (rade_native_tx_get_drive_db() != p_drive) { p_drive = rade_native_tx_get_drive_db(); rade_log_event("drive %.1f", p_drive); }
    if (rade_native_tx_get_trim_db() != p_trim) { p_trim = rade_native_tx_get_trim_db(); rade_log_event("trim %.1f", p_trim); }
    if (rade_native_tx_get_agc() != p_agc) { p_agc = rade_native_tx_get_agc(); rade_log_event("agc %d", p_agc); }
    rade_log_ctx_t lc = { .dial_hz = d, .wave = rade_native_waveform(), .invert = rade_native_get_invert() };
    snprintf(lc.mode, sizeof lc.mode, "%s", mo);
    rade_log_context(&lc);
  }
  gint64 t = g_get_monotonic_time();
  if (strcmp(now, last) != 0 || t - last_t >= 2000000) {
    state_write(1);
    strcpy(last, now);
    last_t = t;
  }
  return G_SOURCE_CONTINUE;
}

static int test_exec(const char *verb, const char *args, char *reply, size_t n);

void rade_native_state_start(void) {
  if (state_timer_id != 0) { return; }
  rade_test_start(test_exec);
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

static void drive_cb(GtkWidget *widget, gpointer data) {
  if (updating) { return; }
  double db = gtk_range_get_value(GTK_RANGE(widget));
  rade_native_tx_set_drive_db(db);
}

//
// ---- parameter log ----
//
static void log_cb(GtkWidget *widget, gpointer data) {
  if (updating) { return; }
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
    int w = rade_native_waveform();
    char meta[512];
    snprintf(meta, sizeof meta,
             "program: piHPSDR %s\nwaveform: %s\ndial_hz: %lld\nmode: %s\ninvert: %d\ntrim_db: %.1f\nagc: %d\n"
             "drive_db: %.1f\nrade_tx: %d",
             build_version, w == RADE_NATIVE_V1 ? "V1" : w == RADE_NATIVE_V2 ? "V2" : "off", rade_dial(),
             mode_string[vfo[0].mode], rade_native_get_invert(), rade_native_tx_get_trim_db(),
             rade_native_tx_get_agc(), rade_native_tx_get_drive_db(), rade_native_tx_enabled());
    log_events_fresh = 1;                   // so the settings at the start are the first events
    if (rade_log_start(meta) != 0) {
      updating = 1;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);
      updating = 0;
      gtk_label_set_text(GTK_LABEL(log_label), "could not open the log file");
      return;
    }
    gtk_label_set_text(GTK_LABEL(log_label), rade_log_path());
  } else {
    rade_log_stop();
    gtk_label_set_text(GTK_LABEL(log_label), "not logging");
  }
}

static void agc_cb(GtkWidget *widget, gpointer data) {
  if (updating) { return; }
  rade_native_tx_set_agc(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}

//
// The bar runs from 20 dB under the ideal to 10 dB over it, so the ideal is
// two thirds of the way along; "about right" is 3 dB either side of it.
//
static gboolean pip_draw_cb(GtkWidget *w, cairo_t *cr, gpointer data) {
  double x = gtk_widget_get_allocated_width(w) * 2.0 / 3.0, h = gtk_widget_get_allocated_height(w);
  cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
  cairo_rectangle(cr, x - 2.0, 0.0, 4.0, h);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_rectangle(cr, x - 1.0, 0.0, 2.0, h);
  cairo_fill(cr);
  return FALSE;
}

static void show_mic(void) {
  if (mic_bar == NULL) { return; }
  //
  // One line of fixed width, the values side by side: verdict LOW / OK / HI / CLIP, loudness (LUFS), peak (dBFS before the
  // limiter) and the leveller's gain. With too little speech to measure the verdict is ---- and no number goes below -60: "--.-".
  //
  char text[120], lu[12] = "  --.-", pk[8] = " ---", ag[10] = "  off";
  const char *verdict = "----";
  rade_native_txlevel_t l;
  if (!rade_native_tx_enabled()) {
    gtk_level_bar_set_value(GTK_LEVEL_BAR(mic_bar), 0.0);
    snprintf(text, sizeof text, "OFF   %s LUFS  pk %s dBFS  AGC %s", lu, pk, ag);
    gtk_label_set_text(GTK_LABEL(mic_text), text);
    return;
  }
  rade_native_tx_level(&l);
  double lufs = l.valid ? l.loud_avg : l.momentary;
  double d = lufs - RADE_NATIVE_TARGET_LUFS;
  double f = (d + 20.0) / 30.0;
  gtk_level_bar_set_value(GTK_LEVEL_BAR(mic_bar), lufs < -60.0 ? 0.0 : f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f);
  if (lufs > -60.0) { snprintf(lu, sizeof lu, "%6.1f", lufs); }
  if (l.peak_dbfs > -60.0) { snprintf(pk, sizeof pk, "%4.0f", l.peak_dbfs); }
  if (rade_native_tx_get_agc()) { snprintf(ag, sizeof ag, "%+5.1f", l.agc_gain_db); }
  if (l.valid) {
    verdict = l.limiting ? "CLIP" : l.delta_db < -RADE_NATIVE_RIGHT_DB ? "LOW" : l.delta_db > RADE_NATIVE_RIGHT_DB ? "HI" : "OK";
  }
  snprintf(text, sizeof text, "%-5s %s LUFS  pk %s dBFS  AGC %s", verdict, lu, pk, ag);
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
  snprintf(test_stem, sizeof test_stem, "%s", stem);
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

static void test_disarm(void);
static void test_cb(GtkWidget *widget, gpointer data);

static void cleanup(void) {
  test_disarm();
  if (timer_id != 0) {
    g_source_remove(timer_id);
    timer_id = 0;
  }
  recv_label = call_label = call_e = NULL;
  tx_b = mox_b = tx_label = inv_b = capture_b = capture_label = status_label = edge_label = NULL;
  wave_b[0] = wave_b[1] = wave_b[2] = NULL;
  mic_scale = mic_agc_b = mic_bar = mic_text = note_label = NULL;
  drive_scale = log_b = log_label = test_b = test_label = NULL;
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

// A label of fixed width: the window does not grow or shrink when the text changes length (long text is cut with an ellipsis; the
// full text is the tooltip). mono: a fixed-pitch font so that numbers line up.
static void fixed_label(GtkWidget *l, int chars, int mono) {
  gtk_label_set_width_chars(GTK_LABEL(l), chars);
  gtk_label_set_max_width_chars(GTK_LABEL(l), chars);
  gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
  if (mono) {
    PangoAttrList *al = pango_attr_list_new();
    pango_attr_list_insert(al, pango_attr_family_new("monospace"));
    gtk_label_set_attributes(GTK_LABEL(l), al);
    pango_attr_list_unref(al);
  }
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
  int inv = rade_native_effective_invert(rade_native_waveform(), rade_dial(), rade_native_user_invert());
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
    // fixed width, the fields in the same places whatever they hold; with no lock the SNR and the offset are not measurements: dashes
    if (st.sync) {
      snprintf(text, sizeof text, "SYNC     %6.1f dB %+7.1f Hz  u%-4lu o%-4lu", st.snr_db, st.foff_hz, st.underruns, st.overruns);
    } else {
      snprintf(text, sizeof text, "NO SYNC     --.- dB    --.- Hz  u%-4lu o%-4lu", st.underruns, st.overruns);
    }
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
  fixed_label(edge_label, 64, 0);
  gtk_widget_set_tooltip_text(edge_label, "The energy stays where it is: choosing V1 or V2, or Invert, moves the dial (3 kHz for a sideband "
                                          "change) and sets DIGU or DIGL; Off returns to USB or LSB on the same carrier. V1 is lower sideband "
                                          "below 10 MHz, V2 upper; on 60 m both are upper. Invert reverses it.");
  gtk_grid_attach(GTK_GRID(grid), edge_label, 0, 1, 5, 1);
  status_label = gtk_label_new("-");
  gtk_widget_set_halign(status_label, GTK_ALIGN_START);
  fixed_label(status_label, 44, 1);
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
  fixed_label(tx_label, 64, 0);
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
  gtk_widget_set_tooltip_text(mic_agc_b, "Changes its gain only while you transmit and are speaking: pauses, hiss and the room never raise it. "
                                         "The gain is held through receive and from one over to the next. Moves slowly towards the ideal "
                                         "(0.2 to 0.5 dB a second; 2 dB a second down for a shout), adds at most 20 dB, may take off up to 30.");
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
  // a pip on the bar at the ideal level (-20 LUFS, two thirds along: the bar runs from 20 LU under it to 10 LU over)
  GtkWidget *ov = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(ov), mic_bar);
  GtkWidget *pip = gtk_drawing_area_new();
  gtk_widget_set_can_focus(pip, FALSE);
  g_signal_connect(pip, "draw", G_CALLBACK(pip_draw_cb), NULL);
  gtk_overlay_add_overlay(GTK_OVERLAY(ov), pip);
  gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(ov), pip, TRUE);
  gtk_grid_attach(GTK_GRID(grid), ov, 0, 6, 5, 1);
  mic_text = gtk_label_new("");
  gtk_widget_set_halign(mic_text, GTK_ALIGN_START);
  fixed_label(mic_text, 52, 1);
  gtk_grid_attach(GTK_GRID(grid), mic_text, 0, 7, 5, 1);
  capture_b = gtk_toggle_button_new_with_label("Capture");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(capture_b), rade_native_capture_active());
  g_signal_connect(capture_b, "toggled", G_CALLBACK(capture_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), capture_b, 0, 8, 1, 1);
  capture_label = gtk_label_new("not capturing");
  gtk_widget_set_halign(capture_label, GTK_ALIGN_START);
  fixed_label(capture_label, 52, 0);
  gtk_grid_attach(GTK_GRID(grid), capture_label, 1, 8, 4, 1);
  GtkWidget *call_l = gtk_label_new("Call");
  gtk_widget_set_halign(call_l, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), call_l, 0, 9, 1, 1);
  call_e = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(call_e), 7);
  gtk_entry_set_width_chars(GTK_ENTRY(call_e), 8);
  gtk_entry_set_text(GTK_ENTRY(call_e), rade_native_get_callsign());
  gtk_grid_attach(GTK_GRID(grid), call_e, 1, 9, 1, 1);
  g_signal_connect(call_e, "changed", G_CALLBACK(call_changed_cb), NULL);
  call_changed_cb(call_e, NULL);
  GtkWidget *recv_l = gtk_label_new("Received calls");
  gtk_widget_set_halign(recv_l, GTK_ALIGN_END);
  gtk_widget_set_valign(recv_l, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), recv_l, 0, 10, 1, 1);
  recv_label = gtk_label_new("");
  gtk_widget_set_halign(recv_label, GTK_ALIGN_START);
  fixed_label(recv_label, 62, 1);
  gtk_label_set_selectable(GTK_LABEL(recv_label), TRUE);
  gtk_grid_attach(GTK_GRID(grid), recv_label, 1, 10, 4, 1);
  show_recent();
  note_label = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(note_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(note_label), 60);
  gtk_label_set_selectable(GTK_LABEL(note_label), TRUE);
  gtk_widget_set_halign(note_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), note_label, 0, 12, 6, 1);
  gtk_grid_insert_row(GTK_GRID(grid), 8);
  gtk_grid_insert_row(GTK_GRID(grid), 8);
  GtkWidget *drive_l = gtk_label_new("Drive");
  gtk_widget_set_halign(drive_l, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), drive_l, 0, 8, 1, 1);
  drive_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -6.0, 6.0, 0.1);
  gtk_scale_set_value_pos(GTK_SCALE(drive_scale), GTK_POS_RIGHT);
  gtk_scale_add_mark(GTK_SCALE(drive_scale), 0.0, GTK_POS_BOTTOM, NULL);
  gtk_range_set_value(GTK_RANGE(drive_scale), rade_native_tx_get_drive_db());
  gtk_widget_set_hexpand(drive_scale, TRUE);
  gtk_widget_set_tooltip_text(drive_scale, "Level into the transmit chain, dB around the defined one (0: the modem peaks "
                                           "just below the ALC ceiling). Above 0 the ALC starts to compress the peaks.");
  g_signal_connect(drive_scale, "value-changed", G_CALLBACK(drive_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), drive_scale, 1, 8, 4, 1);
  log_b = gtk_toggle_button_new_with_label("Log");
  gtk_widget_set_tooltip_text(log_b, "Writes the decode parameters (sync, SNR, offset, librade statistics) and the "
                                     "transmit parameters (loudness, peak, leveller gain, drive, buffers) to a new "
                                     "file in rade-logs/ each time it is switched on. Written by a background thread; "
                                     "the audio never waits for it.");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(log_b), rade_log_active());
  g_signal_connect(log_b, "toggled", G_CALLBACK(log_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), log_b, 0, 9, 1, 1);
  log_label = gtk_label_new(rade_log_active() ? rade_log_path() : "not logging");
  gtk_widget_set_halign(log_label, GTK_ALIGN_START);
  fixed_label(log_label, 52, 0);
  gtk_label_set_selectable(GTK_LABEL(log_label), TRUE);
  gtk_grid_attach(GTK_GRID(grid), log_label, 1, 9, 4, 1);
  gtk_grid_insert_row(GTK_GRID(grid), 10);
  test_b = gtk_check_button_new_with_label("Test harness");
  gtk_widget_set_tooltip_text(test_b, "Lets a script (test/rade_native/rade_ctl.py) key the transmitter and send a file as the "
                                      "mic, for repeatable over-the-air tests. Off when piHPSDR starts and when this window closes. "
                                      "Limits: drive TEST_MAX_DRIVE_PCT, the 10 m band, 90 s an over. Untick to stop at once.");
  g_signal_connect(test_b, "toggled", G_CALLBACK(test_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), test_b, 0, 10, 1, 1);
  test_label = gtk_label_new("off: a script can read the status only");
  gtk_widget_set_halign(test_label, GTK_ALIGN_START);
  fixed_label(test_label, 52, 0);
  gtk_grid_attach(GTK_GRID(grid), test_label, 1, 10, 4, 1);
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


//////////////////////////////////////////////////////////////////////////////
//
// Test harness (see rade_native_test.h). Everything here runs on the GTK main thread.
//
//////////////////////////////////////////////////////////////////////////////

#define TEST_MAX_DRIVE_PCT   20            // the transmitter's drive setting, percent: more than this is refused (was 1)
#define TEST_MIN_HZ          28000000LL    // the 10 m band, and only that
#define TEST_MAX_HZ          29700000LL
#define TEST_MAX_OVER_S      90.0

static void test_release(void) {
  rade_native_tx_inject(NULL, 0);
  if (test_keyed) {
    test_keyed = 0;
    if (radio_is_transmitting()) { radio_set_mox(0); }
  }
}

static void test_disarm(void) {
  if (!test_armed && !test_keyed) { return; }
  test_release();
  test_armed = 0;
  if (test_b != NULL && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(test_b))) {
    updating = 1;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(test_b), FALSE);
    updating = 0;
  }
  if (test_label != NULL) { gtk_label_set_text(GTK_LABEL(test_label), "off: a script can read the status only"); }
}

static void test_cb(GtkWidget *widget, gpointer data) {
  if (updating) { return; }
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
    test_armed = 1;
    char text[300];
    snprintf(text, sizeof text, "ARMED: a script can key the transmitter (drive <= %d %%, 10 m, <= %.0f s). Untick to stop.",
             TEST_MAX_DRIVE_PCT, TEST_MAX_OVER_S);
    gtk_label_set_text(GTK_LABEL(test_label), text);
  } else {
    test_disarm();
  }
}

// why the transmitter may not be keyed by a script now, or NULL
static const char *test_refusal(void) {
  if (!test_armed) { return "the operator has not enabled the test harness (RADE window, Test harness)"; }
  if (rade_native_waveform() == 0) { return "RADE is off"; }
  if (transmitter == NULL) { return "no transmitter"; }
  if (transmitter->puresignal) { return "PureSignal is on"; }
  if (!tx_ready()) { return "RADE transmit is not on"; }
  if (transmitter->drive > TEST_MAX_DRIVE_PCT) { return "drive is above the test limit"; }
  long long d = rade_dial();
  if (d < TEST_MIN_HZ || d > TEST_MAX_HZ) { return "the dial is outside the 10 m band"; }
  if (transmitter->tune || transmitter->twotone) { return "tune or two-tone is on"; }
  return NULL;
}

static void test_status(char *r, size_t n) {
  double ev[7];
  rade_native_tx_times(ev);
  double t0 = 0, t1 = 0, fr = 0;
  int inj = rade_native_tx_inject_state(&t0, &t1, &fr);
  rade_native_status_t st;
  rade_native_get_status(&st);
  rade_native_txlevel_t lv;
  rade_native_tx_level(&lv);
  char why[120] = "";
  if (test_refusal() != NULL) {
    snprintf(why, sizeof why, "%s", test_refusal());
    for (char *c = why; *c; c++) { if (*c == ' ') { *c = '_'; } }
  }
  snprintf(r, n, "armed=%d wave=%d dial=%lld mode=%s invert=%d user_invert=%d drive_db=%.1f trim_db=%.1f agc=%d agc_db=%.2f "
                 "rade_tx=%d tx=%d tx_drive_pct=%d capture=%d log=%d sync=%d snr=%.1f foff=%.1f mic_lufs=%.1f mic_peak_dbfs=%.1f "
                 "inj=%d inj_frac=%.3f t0=%.6f t1=%.6f can_key=%d refusal=%s ev_key=%.6f ev_end_req=%.6f ev_eoo_gen=%.6f ev_eoo_out=%.6f "
                 "ev_done=%.6f ev_unkey=%.6f ev_out0=%.6f t_mox_on=%.6f t_mox_off=%.6f",
           test_armed, rade_native_waveform(), rade_dial(), mode_string[vfo[0].mode], rade_native_get_invert(),
           rade_native_user_invert(), rade_native_tx_get_drive_db(), rade_native_tx_get_trim_db(), rade_native_tx_get_agc(),
           lv.agc_gain_db, rade_native_tx_enabled(), radio_is_transmitting() ? 1 : 0,
           transmitter ? transmitter->drive : -1, rade_native_capture_active(), rade_log_active(), st.sync, st.snr_db,
           st.foff_hz, lv.valid ? lv.loud_avg : -99.0, lv.peak_dbfs, inj, fr, t0, t1, why[0] == 0,
           why[0] ? why : "none", ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], test_t_mox_on, test_t_mox_off);
}

static void wset(GtkWidget *w, int on) {
  if (w != NULL) { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), on ? TRUE : FALSE); }
}

// show a state in a button without running its callback (the harness has already done the work)
static void wshow(GtkWidget *w, int on) {
  if (w != NULL) { updating = 1; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), on ? TRUE : FALSE); updating = 0; }
}

static int test_exec(const char *verb, const char *args, char *reply, size_t n) {
  char v[300];
  reply[0] = 0;
  if (!strcmp(verb, "status") || !strcmp(verb, "poll")) { test_status(reply, n); return 0; }
  if (!strcmp(verb, "abort") || !strcmp(verb, "unkey")) {
    int was = test_keyed;
    if (!strcmp(verb, "abort")) { rade_native_tx_inject(NULL, 0); }
    if (test_keyed) {
      test_keyed = 0;
      if (radio_is_transmitting()) { test_t_mox_off = test_now(); radio_set_mox(0); }
    }
    snprintf(reply, n, "released=%d", was);
    return 0;
  }
  if (!test_armed) { snprintf(reply, n, "%s", test_refusal()); return -1; }
  if (!strcmp(verb, "set")) {
    if (rade_test_arg(args, "wave", v, sizeof v)) {
      int w = atoi(v);
      if (w < 0 || w > 2 || wave_b[w] == NULL) { snprintf(reply, n, "wave must be 0, 1 or 2 (and the RADE window open)"); return -1; }
      if (radio_is_transmitting()) { snprintf(reply, n, "not while transmitting"); return -1; }
      wset(wave_b[w], 1);
    }
    if (rade_test_arg(args, "invert", v, sizeof v)) { if (inv_b == NULL) { snprintf(reply, n, "window"); return -1; } wset(inv_b, atoi(v)); }
    if (rade_test_arg(args, "dial", v, sizeof v)) {
      long long d = atoll(v);
      if (d < TEST_MIN_HZ || d > TEST_MAX_HZ) { snprintf(reply, n, "dial outside the 10 m band"); return -1; }
      if (radio_is_transmitting()) { snprintf(reply, n, "not while transmitting"); return -1; }
      vfo_id_set_frequency(0, d);
    }
    if (rade_test_arg(args, "drive_db", v, sizeof v)) { double d = atof(v); rade_native_tx_set_drive_db(d); if (drive_scale) { updating = 1; gtk_range_set_value(GTK_RANGE(drive_scale), d < -6.0 ? -6.0 : d); updating = 0; } }
    if (rade_test_arg(args, "trim_db", v, sizeof v)) { double d = atof(v); rade_native_tx_set_trim_db(d); if (mic_scale) { updating = 1; gtk_range_set_value(GTK_RANGE(mic_scale), d); updating = 0; } }
    if (rade_test_arg(args, "agc", v, sizeof v)) { int a = atoi(v); rade_native_tx_set_agc(a); if (mic_agc_b) { updating = 1; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mic_agc_b), a); updating = 0; } }
    if (rade_test_arg(args, "tx", v, sizeof v)) { wset(tx_b, atoi(v)); }
    test_status(reply, n);
    return 0;
  }
  if (!strcmp(verb, "capture")) {
    if (!strncmp(args, "on", 2)) {
      if (rade_native_waveform() == 0) { snprintf(reply, n, "RADE is off"); return -1; }
      if (!rade_native_capture_active()) { capture_start(); }
      if (!rade_native_capture_active()) { snprintf(reply, n, "could not start the capture"); return -1; }
      wshow(capture_b, 1);                          // not wset(): that would run capture_cb and start it twice
      snprintf(reply, n, "stem=%s", rade_native_capture_stem());    // the path as piHPSDR sees it, rade-captures/<stem>
    } else {
      capture_stop();
      wshow(capture_b, 0);
      snprintf(reply, n, "stem=%s/%s", CAPTURE_DIR, test_stem);
    }
    return 0;
  }
  if (!strcmp(verb, "log")) {
    if (!strncmp(args, "on", 2)) {
      wset(log_b, 0); wset(log_b, 1);                          // a new file each time
      if (!rade_log_active()) { snprintf(reply, n, "could not start the log"); return -1; }
      snprintf(reply, n, "file=%s", rade_log_path());
    } else {
      wset(log_b, 0);
      snprintf(reply, n, "file=%s", rade_log_path());
    }
    return 0;
  }
  if (!strcmp(verb, "inject")) {
    char path[300], err[200], g[32];
    if (!rade_test_arg(args, "file", path, sizeof path)) { snprintf(reply, n, "file= is needed"); return -1; }
    double gain = rade_test_arg(args, "gain_db", g, sizeof g) ? atof(g) : 0.0;
    float *buf = NULL;
    long len = 0;
    if (rade_test_load_wav(path, gain, TEST_MAX_OVER_S, &buf, &len, err, sizeof err) != 0) { snprintf(reply, n, "%s", err); return -1; }
    int rc = rade_native_tx_inject(buf, len);
    free(buf);
    if (rc != 0) { snprintf(reply, n, "out of memory"); return -1; }
    snprintf(reply, n, "duration_s=%.3f samples=%ld", len / 48000.0, len);
    return 0;
  }
  if (!strcmp(verb, "key")) {
    const char *why = test_refusal();
    if (why) { snprintf(reply, n, "%s", why); return -1; }
    if (radio_is_transmitting()) { snprintf(reply, n, "already transmitting"); return -1; }
    if (rade_native_tx_inject_state(NULL, NULL, NULL) == 0) { snprintf(reply, n, "nothing injected: use inject first"); return -1; }
    test_keyed = 1;
    test_t_mox_on = test_now();
    test_t_mox_off = 0.0;
    radio_set_mox(1);
    snprintf(reply, n, "keyed=1");
    return 0;
  }
  snprintf(reply, n, "unknown request %s", verb);
  return -1;
}
