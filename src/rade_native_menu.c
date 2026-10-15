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
#include <sys/stat.h>
#include <time.h>

#include "agc.h"
#include "diversity_auto.h"
#include "message.h"
#include "new_menu.h"
#include "rade_native.h"
#include "rade_native_menu.h"
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
static GtkWidget *tx_b = NULL;
static int tx_applied = 0;
static GtkWidget *capture_b = NULL;
static GtkWidget *capture_label = NULL;
static guint timer_id = 0;
static guint event_timer_id = 0;
static char last_state[1024];

#define CAPTURE_DIR "rade-captures"


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
  int inv = rade_native_effective_invert(w, vfo[0].frequency, rade_native_user_invert());
  snprintf(buf, len,
           "f=%lld ctun=%d ctunf=%lld mode=%s filter=%d..%d rade=V%d inv=%d(user %d) "
           "agc=%s gain=%.0f hangthr=%.0f vol=%.1f nb=%d nr=%d snb=%d anf=%d "
           "adc=%d att=%d preamp=%d div=%d divmode=%s divref=%d divrun=%d",
           vfo[0].frequency, vfo[0].ctun, vfo[0].ctun_frequency, mode_string[vfo[0].mode],
           rx->filter_low, rx->filter_high, w, inv, rade_native_user_invert(),
           agc_name(rx->agc), rx->agc_gain, rx->agc_hang_threshold, rx->volume,
           rx->nb, rx->nr, rx->snb, rx->anf, rx->adc, adc[rx->adc].attenuation,
           adc[rx->adc].preamp, diversity_enabled, div_mode_name(div_auto_mode),
           div_auto_ref, div_auto_running);
}

static void meta_text(char *buf, size_t len, const char *what) {
  const RECEIVER *rx = receiver[0];
  int w = rade_native_waveform();
  int inv = rade_native_effective_invert(w, vfo[0].frequency, rade_native_user_invert());
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
           rade_native_centre(w), lo, hi, vfo[0].frequency, vfo[0].ctun, vfo[0].ctun_frequency,
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
  int inv = rade_native_effective_invert(w, vfo[0].frequency, rade_native_user_invert());
  time_t t = time(NULL);
  char ts[32], stem[128], meta[3072];
  strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&t));
  snprintf(stem, sizeof stem, "%s_%lldkHz_RADE-V%d%s", ts, vfo[0].frequency / 1000, w, inv ? "-inv" : "");
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
  if (dialog != NULL) {
    GtkWidget *tmp = dialog;
    dialog = NULL;
    gtk_widget_destroy(tmp);
    sub_menu = NULL;
    active_menu = NO_MENU;
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
    int inv = rade_native_effective_invert(w, vfo[0].frequency, rade_native_user_invert());
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
    SetTXAPanelSelect(transmitter->id, 3);
  } else {
    SetTXAPanelSelect(transmitter->id, 2);
    tx_xmit_captured_data_end(transmitter);
  }
  tx_applied = on;
  tx_set_filter(transmitter);
}

static void tx_cb(GtkWidget *widget, gpointer data) {
  int on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if (on) {
    if (transmitter == NULL || transmitter->puresignal || rade_native_waveform() == 0) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);
      return;
    }
    rade_native_tx_enable(1);
    if (!rade_native_tx_enabled()) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);
      return;
    }
    tx_apply(1);
  } else {
    rade_native_tx_enable(0);
    tx_apply(0);
  }
}

static void waveform_cb(GtkWidget *widget, gpointer data) {
  if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) { return; }
  rade_native_configure(GPOINTER_TO_INT(data), rade_native_user_invert());
  if (!rade_native_tx_enabled()) {
    tx_apply(0);                 // changing waveform switches the TX engine off
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tx_b), FALSE);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(capture_b), rade_native_capture_active());
  apply();
  show_edges();
}

static void invert_cb(GtkWidget *widget, gpointer data) {
  rade_native_configure(rade_native_waveform(), gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
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
  if (!rade_native_capture_active() &&
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(capture_b))) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(capture_b), FALSE);
  }
  return G_SOURCE_CONTINUE;
}

void rade_native_menu(GtkWidget *parent) {
  dialog = gtk_dialog_new();
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent));
  GtkWidget *headerbar = gtk_header_bar_new();
  gtk_window_set_titlebar(GTK_WINDOW(dialog), headerbar);
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(headerbar), TRUE);
  gtk_header_bar_set_title(GTK_HEADER_BAR(headerbar), "piHPSDR - RADE");
  g_signal_connect (dialog, "delete_event", G_CALLBACK (close_cb), NULL);
  g_signal_connect (dialog, "destroy", G_CALLBACK (close_cb), NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing (GTK_GRID(grid), 10);
  gtk_grid_set_row_spacing (GTK_GRID(grid), 10);
  GtkWidget *close_b = gtk_button_new_with_label("Close");
  gtk_widget_set_name(close_b, "close_button");
  g_signal_connect (close_b, "button-press-event", G_CALLBACK(close_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), close_b, 0, 0, 1, 1);
  int w = rade_native_waveform();
  GtkWidget *off_b = gtk_radio_button_new_with_label(NULL, "Off");
  GtkWidget *v1_b = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(off_b), "RADE V1");
  GtkWidget *v2_b = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(off_b), "RADE V2");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w == RADE_NATIVE_V1 ? v1_b : w == RADE_NATIVE_V2 ? v2_b : off_b), TRUE);
  g_signal_connect(off_b, "toggled", G_CALLBACK(waveform_cb), GINT_TO_POINTER(0));
  g_signal_connect(v1_b, "toggled", G_CALLBACK(waveform_cb), GINT_TO_POINTER(RADE_NATIVE_V1));
  g_signal_connect(v2_b, "toggled", G_CALLBACK(waveform_cb), GINT_TO_POINTER(RADE_NATIVE_V2));
  gtk_grid_attach(GTK_GRID(grid), off_b, 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), v1_b, 2, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), v2_b, 3, 0, 1, 1);
  GtkWidget *inv_b = gtk_check_button_new_with_label("Invert");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inv_b), rade_native_user_invert());
  g_signal_connect(inv_b, "toggled", G_CALLBACK(invert_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), inv_b, 4, 0, 1, 1);
  edge_label = gtk_label_new("");
  gtk_widget_set_halign(edge_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), edge_label, 0, 1, 5, 1);
  status_label = gtk_label_new("-");
  gtk_widget_set_halign(status_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), status_label, 0, 2, 5, 1);
  tx_b = gtk_toggle_button_new_with_label("Transmit RADE");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tx_b), rade_native_tx_enabled());
  g_signal_connect(tx_b, "toggled", G_CALLBACK(tx_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), tx_b, 0, 4, 2, 1);
  capture_b = gtk_toggle_button_new_with_label("Capture");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(capture_b), rade_native_capture_active());
  g_signal_connect(capture_b, "toggled", G_CALLBACK(capture_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), capture_b, 0, 3, 1, 1);
  capture_label = gtk_label_new("not capturing");
  gtk_widget_set_halign(capture_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), capture_label, 1, 3, 4, 1);
  GtkWidget *hint = gtk_label_new("Receiver 0. Use DIGU or USB; the passband is set by RADE. Invert flips the default: V1 below 10 MHz is inverted, V2 never.");
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), hint, 0, 5, 5, 1);
  gtk_container_add(GTK_CONTAINER(content), grid);
  sub_menu = dialog;
  show_edges();
  gtk_widget_show_all(dialog);
  timer_id = g_timeout_add(250, update_cb, NULL);
}
