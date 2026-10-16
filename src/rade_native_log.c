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

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rade_native_log.h"

#define LOG_DIR     "rade-logs"
#define RX_RING     4096
#define TX_RING     1024
#define EV_RING     256
#define EV_LEN      160

typedef struct { double t; rade_log_rx_t v; } rx_slot_t;
typedef struct { double t; rade_log_tx_t v; } tx_slot_t;
typedef struct { double t; char text[EV_LEN]; } ev_slot_t;

static rx_slot_t *rx_ring;
static tx_slot_t *tx_ring;
static ev_slot_t *ev_ring;
static atomic_ulong rx_w, rx_r, tx_w, tx_r, ev_w, ev_r;
static atomic_ulong dropped;
static atomic_int   on;
static pthread_t    thread;
static int          have_thread;
static FILE        *fp;
static double       t0;
static char         path[256];

static double now_unix(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void rade_log_rx(const rade_log_rx_t *r) {
  if (!atomic_load_explicit(&on, memory_order_acquire)) { return; }
  unsigned long w = atomic_load_explicit(&rx_w, memory_order_relaxed);
  if (w - atomic_load_explicit(&rx_r, memory_order_acquire) >= RX_RING) { atomic_fetch_add(&dropped, 1); return; }
  rx_slot_t *s = &rx_ring[w % RX_RING];
  s->t = now_unix();
  s->v = *r;
  atomic_store_explicit(&rx_w, w + 1, memory_order_release);
}

void rade_log_tx(const rade_log_tx_t *r) {
  if (!atomic_load_explicit(&on, memory_order_acquire)) { return; }
  unsigned long w = atomic_load_explicit(&tx_w, memory_order_relaxed);
  if (w - atomic_load_explicit(&tx_r, memory_order_acquire) >= TX_RING) { atomic_fetch_add(&dropped, 1); return; }
  tx_slot_t *s = &tx_ring[w % TX_RING];
  s->t = now_unix();
  s->v = *r;
  atomic_store_explicit(&tx_w, w + 1, memory_order_release);
}

void rade_log_event(const char *fmt, ...) {
  if (!atomic_load_explicit(&on, memory_order_acquire)) { return; }
  unsigned long w = atomic_load_explicit(&ev_w, memory_order_relaxed);
  if (w - atomic_load_explicit(&ev_r, memory_order_acquire) >= EV_RING) { atomic_fetch_add(&dropped, 1); return; }
  ev_slot_t *s = &ev_ring[w % EV_RING];
  s->t = now_unix();
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s->text, EV_LEN, fmt, ap);
  va_end(ap);
  atomic_store_explicit(&ev_w, w + 1, memory_order_release);
}

// one row, the fields not used by this kind left empty
typedef struct { double t; char line[400]; } row_t;

static void num(char *out, size_t n, double v, const char *f) {
  if (isnan(v)) { out[0] = 0; } else { snprintf(out, n, f, v); }
}

static int cmp_row(const void *a, const void *b) {
  double d = ((const row_t *)a)->t - ((const row_t *)b)->t;
  return d < 0 ? -1 : d > 0 ? 1 : 0;
}

// csv text is quoted if it has a comma or a quote
static void quote(char *out, size_t n, const char *s) {
  size_t k = 0;
  out[k++] = '"';
  for (; *s && k + 3 < n; s++) {
    if (*s == '"') { out[k++] = '"'; }
    out[k++] = *s;
  }
  out[k++] = '"';
  out[k] = 0;
}

//
// Rows are held back for 0.4 s after they are made before they are written, and written in time order,
// so a row from one ring cannot be overtaken by an older one still to be picked up from another.
//
static row_t *rows;
static size_t pending;

static void drain(int final) {
  const size_t cap = 2 * (RX_RING + TX_RING + EV_RING);
  if (!rows) { rows = malloc(sizeof(row_t) * cap); if (!rows) { return; } }
  size_t n = pending;
  unsigned long r, w;
  r = atomic_load_explicit(&rx_r, memory_order_relaxed);
  w = atomic_load_explicit(&rx_w, memory_order_acquire);
  for (; r != w && n < cap; r++) {
    const rx_slot_t *s = &rx_ring[r % RX_RING];
    const rade_log_rx_t *v = &s->v;
    rows[n].t = s->t;
    snprintf(rows[n].line, sizeof rows[n].line,
             "%.3f,%.3f,rx,%ld,%d,%.2f,%.2f,%d,%.4f,%.3f,%.4f,%.2f,%ld,%ld,%lu,%lu,,,,,,,,,,,,,",
             s->t, s->t - t0, v->rx_pos, v->sync, v->snr_db, v->foff_hz, v->nout, v->delta_hat, v->est_foff,
             v->gain, v->snr_est, v->in_fill, v->out_fill, v->underruns, v->overruns);
    n++;
  }
  atomic_store_explicit(&rx_r, r, memory_order_release);
  r = atomic_load_explicit(&tx_r, memory_order_relaxed);
  w = atomic_load_explicit(&tx_w, memory_order_acquire);
  for (; r != w && n < cap; r++) {
    const tx_slot_t *s = &tx_ring[r % TX_RING];
    const rade_log_tx_t *v = &s->v;
    char avg[32];
    num(avg, sizeof avg, v->loud_avg, "%.2f");
    rows[n].t = s->t;
    snprintf(rows[n].line, sizeof rows[n].line,
             "%.3f,%.3f,tx,,,,,,,,,,,,,,%d,%.1f,%.2f,%.1f,%.2f,%s,%.2f,%d,%lu,%ld,%ld,%d,",
             s->t, s->t - t0, v->tx, v->trim_db, v->agc_db, v->drive_db, v->loud_mom, avg, v->peak_dbfs,
             v->limiting, v->underruns, v->out_fill, v->pcm_fill, v->end_state);
    n++;
  }
  atomic_store_explicit(&tx_r, r, memory_order_release);
  r = atomic_load_explicit(&ev_r, memory_order_relaxed);
  w = atomic_load_explicit(&ev_w, memory_order_acquire);
  for (; r != w && n < cap; r++) {
    const ev_slot_t *s = &ev_ring[r % EV_RING];
    char q[2 * EV_LEN + 4];
    quote(q, sizeof q, s->text);
    rows[n].t = s->t;
    snprintf(rows[n].line, sizeof rows[n].line, "%.3f,%.3f,ev,,,,,,,,,,,,,,,,,,,,,,,,,,%s", s->t, s->t - t0, q);
    n++;
  }
  atomic_store_explicit(&ev_r, r, memory_order_release);
  if (n > 1) { qsort(rows, n, sizeof(row_t), cmp_row); }
  double cutoff = final ? 1e30 : now_unix() - 0.4;
  size_t i = 0;
  for (; i < n && rows[i].t <= cutoff; i++) { fputs(rows[i].line, fp); fputc('\n', fp); }
  if (i > 0) { fflush(fp); }
  memmove(rows, rows + i, (n - i) * sizeof(row_t));
  pending = n - i;
}

static void *writer(void *arg) {
  while (atomic_load_explicit(&on, memory_order_acquire)) {
    drain(0);
    usleep(200000);
  }
  drain(1);
  return NULL;
}

int rade_log_active(void) { return atomic_load(&on); }
const char *rade_log_path(void) { return path; }

int rade_log_start(const char *meta) {
  rade_log_stop();
  if (!rx_ring) {
    rx_ring = calloc(RX_RING, sizeof(rx_slot_t));
    tx_ring = calloc(TX_RING, sizeof(tx_slot_t));
    ev_ring = calloc(EV_RING, sizeof(ev_slot_t));
    if (!rx_ring || !tx_ring || !ev_ring) { return -1; }
  }
  mkdir(LOG_DIR, 0755);
  time_t tt = time(NULL);
  struct tm tm;
  localtime_r(&tt, &tm);
  char ts[32];
  strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
  snprintf(path, sizeof path, LOG_DIR "/%s_RADE.csv", ts);
  fp = fopen(path, "w");
  if (!fp) { path[0] = 0; return -1; }
  t0 = now_unix();
  fprintf(fp, "# RADE parameter log, started %s local time\n", ts);
  for (const char *p = meta ? meta : ""; *p;) {          // one # line per line of settings
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len > 0) { fprintf(fp, "# %.*s\n", (int)len, p); }
    p += len + (e ? 1 : 0);
  }
  fputs("t_unix,t,kind,rx_pos,sync,snr_db,foff_hz,nout,delta_hat,est_foff,gain,snr_est,in_fill,out_fill,"
        "rx_underruns,rx_overruns,tx,trim_db,agc_db,drive_db,loud_mom,loud_avg,peak_dbfs,limiting,"
        "tx_underruns,tx_out_fill,tx_pcm_fill,end_state,note\n", fp);
  fflush(fp);
  pending = 0;
  atomic_store(&rx_r, atomic_load(&rx_w));
  atomic_store(&tx_r, atomic_load(&tx_w));
  atomic_store(&ev_r, atomic_load(&ev_w));
  atomic_store(&dropped, 0);
  atomic_store_explicit(&on, 1, memory_order_release);
  have_thread = pthread_create(&thread, NULL, writer, NULL) == 0;
  if (!have_thread) {
    atomic_store(&on, 0);
    fclose(fp);
    fp = NULL;
    return -1;
  }
  return 0;
}

void rade_log_stop(void) {
  if (!atomic_load(&on)) { return; }
  atomic_store_explicit(&on, 0, memory_order_release);
  if (have_thread) { pthread_join(thread, NULL); have_thread = 0; }
  if (fp) {
    fprintf(fp, "# dropped rows: %lu\n", (unsigned long)atomic_load(&dropped));
    fclose(fp);
    fp = NULL;
  }
}
