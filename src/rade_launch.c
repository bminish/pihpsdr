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


#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rade_launch.h"

typedef struct {
  rade_launch_done done;
  void *data;
  gint64 start;
  int grace_s;
} launch_t;

static char log_path[1024];

static void child_setup(gpointer data) {
  (void)data;
  setsid();                                   // own session: a closed terminal or window does not reach it
  int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) { close(fd); }
  }
}

static void child_exited(GPid pid, gint status, gpointer data) {
  launch_t *l = data;
  int early = (g_get_monotonic_time() - l->start) < (gint64)l->grace_s * 1000000;
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? -WTERMSIG(status) : status);
  g_spawn_close_pid(pid);
  if (l->done) { l->done(early, code, l->data); }
  g_free(l);
}

int rade_launch(char *const argv[], const char *logfile, int grace_s,
                rade_launch_done done, void *data, char *err, size_t errlen) {
  snprintf(log_path, sizeof log_path, "%s", logfile);
  char *dir = g_path_get_dirname(logfile);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  // a note in the log so that runs can be told apart
  FILE *f = fopen(log_path, "a");
  if (f) {
    GDateTime *now = g_date_time_new_now_local();
    char *ts = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    fprintf(f, "--- %s: %s\n", ts, argv[0]);
    g_free(ts);
    g_date_time_unref(now);
    fclose(f);
  }
  GPid pid;
  GError *gerr = NULL;
  if (!g_spawn_async(NULL, (gchar **)argv, NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDIN_FROM_DEV_NULL,
                     child_setup, NULL, &pid, &gerr)) {
    snprintf(err, errlen, "%s", gerr ? gerr->message : "spawn failed");
    if (gerr) { g_error_free(gerr); }
    return -1;
  }
  launch_t *l = g_new0(launch_t, 1);
  l->done = done;
  l->data = data;
  l->grace_s = grace_s;
  l->start = g_get_monotonic_time();
  g_child_watch_add(pid, child_exited, l);
  return 0;
}
