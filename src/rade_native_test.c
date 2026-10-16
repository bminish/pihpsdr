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

#include <errno.h>
#include <glib.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "rade_native_test.h"

static rade_test_exec_fn exec_fn;
static char sock_path[256];

const char *rade_test_socket_path(void) { return sock_path; }

static double now_unix(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int rade_test_arg(const char *args, const char *key, char *val, size_t n) {
  size_t kl = strlen(key);
  for (const char *p = args; p && *p;) {
    while (*p == ' ') { p++; }
    if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
      p += kl + 1;
      size_t l = strcspn(p, " ");
      if (l >= n) { l = n - 1; }
      memcpy(val, p, l);
      val[l] = 0;
      return 1;
    }
    p += strcspn(p, " ");
  }
  return 0;
}

//
// WAV
//
int rade_test_load_wav(const char *path, double gain_db, double max_s, float **out, long *n, char *err, size_t errn) {
  FILE *f = fopen(path, "rb");
  if (!f) { snprintf(err, errn, "cannot open %s", path); return -1; }
  unsigned char h[12];
  int fmt = 0, ch = 0, bits = 0, rate = 0;
  long data_len = -1, data_pos = 0;
  if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
    fclose(f); snprintf(err, errn, "not a WAV file"); return -1;
  }
  unsigned char ck[8];
  while (fread(ck, 1, 8, f) == 8) {
    uint32_t len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
    if (!memcmp(ck, "fmt ", 4)) {
      unsigned char b[16] = {0};
      if (len < 16 || fread(b, 1, 16, f) != 16) { fclose(f); snprintf(err, errn, "bad fmt chunk"); return -1; }
      fmt = b[0] | b[1] << 8; ch = b[2] | b[3] << 8; rate = b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24; bits = b[14] | b[15] << 8;
      fseek(f, (long)len - 16 + (len & 1), SEEK_CUR);
    } else if (!memcmp(ck, "data", 4)) {
      data_len = len; data_pos = ftell(f);
      break;
    } else {
      fseek(f, (long)len + (len & 1), SEEK_CUR);
    }
  }
  if (data_len < 0 || ch != 1 || rate != 48000 || !((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32))) {
    fclose(f);
    snprintf(err, errn, "need 48 kHz mono, 16-bit PCM or 32-bit float (got fmt %d, %d ch, %d Hz, %d bit)", fmt, ch, rate, bits);
    return -1;
  }
  long bytes = bits / 8;
  long total = data_len / bytes;
  if (total > (long)(max_s * 48000)) { fclose(f); snprintf(err, errn, "longer than %.0f s", max_s); return -1; }
  float *buf = malloc(sizeof(float) * (total > 0 ? total : 1));
  if (!buf) { fclose(f); snprintf(err, errn, "out of memory"); return -1; }
  fseek(f, data_pos, SEEK_SET);
  double g = pow(10.0, gain_db / 20.0);
  for (long i = 0; i < total; i++) {
    if (bits == 16) {
      int16_t v;
      if (fread(&v, 2, 1, f) != 1) { total = i; break; }
      buf[i] = (float)(v / 32768.0 * g);
    } else {
      float v;
      if (fread(&v, 4, 1, f) != 1) { total = i; break; }
      buf[i] = (float)(v * g);
    }
    if (buf[i] > 1.0f) { buf[i] = 1.0f; } else if (buf[i] < -1.0f) { buf[i] = -1.0f; }
  }
  fclose(f);
  *out = buf; *n = total;
  return 0;
}

//
// A request run on the GTK main thread. The socket thread waits for it; if it gives up, the main thread
// frees the job when it gets to it.
//
typedef struct {
  char verb[32], args[1024], reply[1024];
  int rc, done, abandoned;
  GMutex m;
  GCond c;
} job_t;

static gboolean job_cb(gpointer p) {
  job_t *j = p;
  int rc = exec_fn(j->verb, j->args, j->reply, sizeof j->reply);
  g_mutex_lock(&j->m);
  j->rc = rc;
  j->done = 1;
  int drop = j->abandoned;
  g_cond_signal(&j->c);
  g_mutex_unlock(&j->m);
  if (drop) { g_mutex_clear(&j->m); g_cond_clear(&j->c); g_free(j); }
  return G_SOURCE_REMOVE;
}

static int run(const char *verb, const char *args, char *reply, size_t n) {
  job_t *j = g_new0(job_t, 1);
  g_mutex_init(&j->m); g_cond_init(&j->c);
  snprintf(j->verb, sizeof j->verb, "%s", verb);
  snprintf(j->args, sizeof j->args, "%s", args ? args : "");
  g_idle_add(job_cb, j);
  gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  g_mutex_lock(&j->m);
  while (!j->done) {
    if (!g_cond_wait_until(&j->c, &j->m, end)) { break; }
  }
  int ok = j->done;
  if (ok) {
    snprintf(reply, n, "%s", j->reply);
    int rc = j->rc;
    g_mutex_unlock(&j->m);
    g_mutex_clear(&j->m); g_cond_clear(&j->c); g_free(j);
    return rc;
  }
  j->abandoned = 1;
  g_mutex_unlock(&j->m);
  snprintf(reply, n, "piHPSDR did not answer in 10 s");
  return -1;
}

static int client_gone(int fd) {
  char c;
  ssize_t r = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
  return r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

static void say(int fd, int rc, const char *text) {
  char out[1400];
  int l = snprintf(out, sizeof out, "%s%s%s\n", rc == 0 ? "ok" : "err", *text ? " " : "", text);
  if (send(fd, out, l, MSG_NOSIGNAL) < 0) { /* the client has gone: not a reason to die of SIGPIPE */ }
}

// value of key in a reply, as a double (0 if absent)
static double rnum(const char *reply, const char *key) {
  char v[64];
  return rade_test_arg(reply, key, v, sizeof v) ? atof(v) : 0.0;
}

//
// play: inject the file, key, wait for the last sample, unkey, wait for the release, and say when each was.
//
static void play(int fd, const char *args) {
  char reply[1024], v[64];
  double max_s = 90.0;
  if (rade_test_arg(args, "max_s", v, sizeof v)) { max_s = atof(v); }
  int rc = run("inject", args, reply, sizeof reply);
  if (rc != 0) { say(fd, rc, reply); return; }
  double dur = rnum(reply, "duration_s");
  double t_call = now_unix();
  rc = run("key", "", reply, sizeof reply);
  if (rc != 0) { run("abort", "", reply, sizeof reply); say(fd, rc, reply); return; }
  double t_keyed = 0.0, t_seen = 0.0;
  int why = 0;                                 // 0 done, 1 timeout, 2 client gone, 3 transmitter dropped
  double limit = dur + 10.0 < max_s ? dur + 10.0 : max_s;
  while (1) {
    usleep(50000);
    rc = run("poll", "", reply, sizeof reply);
    if (rc != 0) { why = 3; break; }
    int tx = (int)rnum(reply, "tx"), inj = (int)rnum(reply, "inj");
    if (tx && t_keyed == 0.0) { t_keyed = now_unix(); }
    if (t_keyed != 0.0 && !tx) { why = 3; break; }          // keyed and then not: the operator, a fault, or PTT lost
    if (inj == 2) { break; }
    if (now_unix() - t_call > limit) { why = 1; break; }
    if (client_gone(fd)) { why = 2; break; }
  }
  double t_unkey = now_unix();
  run(why == 0 ? "unkey" : "abort", "", reply, sizeof reply);      // anything but a clean finish also stops the injection
  double t_rel = 0.0;
  for (int i = 0; i < 100; i++) {              // up to 5 s for the end-of-over hold
    usleep(50000);
    if (run("poll", "", reply, sizeof reply) == 0 && !(int)rnum(reply, "tx")) { t_rel = now_unix(); break; }
  }
  t_seen = t_keyed;
  run("poll", "", reply, sizeof reply);
  char out[1000];
  snprintf(out, sizeof out, "done=%s duration_s=%.3f t_call=%.6f t_keyed=%.6f t_inject_start=%.6f t_inject_end=%.6f "
                            "t_unkey=%.6f t_released=%.6f consumed=%.3f ev_key=%.6f ev_end_req=%.6f ev_eoo_gen=%.6f "
                            "ev_eoo_out=%.6f ev_done=%.6f ev_unkey=%.6f ev_out0=%.6f t_mox_on=%.6f t_mox_off=%.6f",
           why == 0 ? "complete" : why == 1 ? "timeout" : why == 2 ? "client_gone" : "tx_dropped", dur, t_call, t_seen,
           rnum(reply, "t0"), rnum(reply, "t1"), t_unkey, t_rel, rnum(reply, "inj_frac"),
           rnum(reply, "ev_key"), rnum(reply, "ev_end_req"), rnum(reply, "ev_eoo_gen"), rnum(reply, "ev_eoo_out"),
           rnum(reply, "ev_done"), rnum(reply, "ev_unkey"), rnum(reply, "ev_out0"), rnum(reply, "t_mox_on"), rnum(reply, "t_mox_off"));
  say(fd, why == 0 && t_rel > 0 ? 0 : -1, out);
}

static void serve(int fd) {
  char buf[1100];
  size_t have = 0;
  while (1) {
    ssize_t r = read(fd, buf + have, sizeof buf - 1 - have);
    if (r <= 0) { break; }
    have += r;
    buf[have] = 0;
    char *nl;
    while ((nl = memchr(buf, '\n', have)) != NULL) {
      *nl = 0;
      char *line = buf;
      while (*line == ' ') { line++; }
      char verb[32] = "", *args = line;
      size_t vl = strcspn(line, " ");
      snprintf(verb, sizeof verb, "%.*s", (int)(vl < sizeof verb ? vl : sizeof verb - 1), line);
      args = line + vl;
      while (*args == ' ') { args++; }
      char reply[1024];
      if (!strcmp(verb, "play")) {
        play(fd, args);
      } else if (!*verb) {
        say(fd, -1, "empty request");
      } else {
        int rc = run(verb, args, reply, sizeof reply);
        say(fd, rc, reply);
      }
      size_t used = (size_t)(nl - buf) + 1;
      memmove(buf, buf + used, have - used);
      have -= used;
      buf[have] = 0;
    }
    if (have >= sizeof buf - 1) { have = 0; }
  }
  // (a client that goes away during a play is noticed in play(), which releases the transmitter)
  close(fd);
}

static void *serve_thread(void *arg) {
  serve((int)(intptr_t)arg);
  return NULL;
}

static void *server(void *arg) {
  int ls = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ls < 0) { return NULL; }
  struct sockaddr_un a;
  memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  if (strlen(sock_path) >= sizeof a.sun_path) { close(ls); return NULL; }      // too long for a socket address
  memcpy(a.sun_path, sock_path, strlen(sock_path) + 1);
  unlink(sock_path);
  mode_t old = umask(0177);
  int ok = bind(ls, (struct sockaddr *)&a, sizeof a) == 0 && listen(ls, 2) == 0;
  umask(old);
  if (!ok) { close(ls); return NULL; }
  while (1) {
    int fd = accept(ls, NULL, NULL);
    if (fd < 0) { if (errno == EINTR) { continue; } break; }
    pthread_t t;                      // a client each, so that status can be read while another plays
    if (pthread_create(&t, NULL, serve_thread, (void *)(intptr_t)fd) == 0) { pthread_detach(t); } else { close(fd); }
  }
  close(ls);
  return NULL;
}

void rade_test_start(rade_test_exec_fn fn) {
  static int started = 0;
  if (started) { return; }
  started = 1;
  exec_fn = fn;
  const char *base = getenv("XDG_STATE_HOME");
  char dir[190];
  if (base && *base) { snprintf(dir, sizeof dir, "%s/pihpsdr-rade", base); }
  else { snprintf(dir, sizeof dir, "%s/.local/state/pihpsdr-rade", getenv("HOME") ? getenv("HOME") : "."); }
  g_mkdir_with_parents(dir, 0755);
  snprintf(sock_path, sizeof sock_path, "%s/control.sock", dir);
  pthread_t t;
  if (pthread_create(&t, NULL, server, NULL) == 0) { pthread_detach(t); }
}
