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

#include "message.h"
#include "new_menu.h"
#include "rade_native.h"
#include "rade_native_menu.h"
#include "radio.h"
#include "receiver.h"
#include "vfo.h"

static GtkWidget *dialog = NULL;
static GtkWidget *status_label = NULL;
static GtkWidget *edge_label = NULL;
static guint timer_id = 0;

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

static void waveform_cb(GtkWidget *widget, gpointer data) {
  if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) { return; }
  rade_native_configure(GPOINTER_TO_INT(data), rade_native_user_invert());
  apply();
  show_edges();
}

static void invert_cb(GtkWidget *widget, gpointer data) {
  rade_native_configure(rade_native_waveform(), gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  apply();
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
  GtkWidget *hint = gtk_label_new("Receiver 0. Use DIGU or USB; the passband is set by RADE. Invert flips the default: V1 below 10 MHz is inverted, V2 never.");
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), hint, 0, 3, 5, 1);
  gtk_container_add(GTK_CONTAINER(content), grid);
  sub_menu = dialog;
  show_edges();
  gtk_widget_show_all(dialog);
  timer_id = g_timeout_add(250, update_cb, NULL);
}
