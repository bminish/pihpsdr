/* Copyright (C)
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
// ===================================================================
//  DEVELOPMENT TOOL - NOT PART OF THE DIVERSITY FEATURE.
//  Compiled only under -DDIVERSITY_CAPTURE ("make DIVCAP=1").
//  To be deleted before upstream submission - see the header.
// ===================================================================
//
// Writes the auto-phasing analysis blocks to a file so that a real
// two-antenna signal can be replayed into the correlator offline.
//
// The one design constraint that matters: this must not slow the
// analysis thread down. A block that thread fails to process is a gap in
// the sample stream, and a gap forces rade_corr_reset() - so a capture
// that stalls on fwrite() destroys the lock it was recording. The
// analysis thread therefore only memcpy()s into a ring and signals; a
// writer thread does the I/O. If the ring fills, the block is dropped and
// counted, and the count goes in the trailer so a capture that perturbed
// its own subject can be recognised and discarded rather than believed.
//

#include <gtk/gtk.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atomic.h"
#include "diversity_capture.h"
#include "discovered.h"
#ifdef __APPLE__
  #include "MacOS.h"        // for apple_sem()
#endif
#include "message.h"
#include "radio.h"

//
// Slots in the ring. Sixteen blocks is about 1.4 s at any sample rate
// (the analysis block is ~85 ms at all of them), which is far more than
// a buffered write to a local disk needs and cheap enough not to think
// about: 1 MB at 48 kHz, 8 MB at 384 kHz. One slot is always the one
// being filled, so fifteen of the sixteen are usable.
//
#define DIVCAP_QUEUE 16

volatile int div_capture_active = 0;

static FILE      *fp = NULL;
static GThread   *writer = NULL;
static volatile atomic_int cap_run = 0;

//
// Raised once per block queued, and once by diversity_capture_stop() to
// wake the writer for the last time. Created once and never destroyed:
// the analysis thread posts it and is not joined here, exactly as in
// diversity_auto.c.
//
static int        cap_sem_created = 0;
#ifdef __APPLE__
  static sem_t   *cap_sem = NULL;
  #define CAP_SEM   cap_sem
#else
  static sem_t    cap_sem;
  #define CAP_SEM   (&cap_sem)
#endif

//
// A single-producer/single-consumer ring: the analysis thread fills at
// cap_head, the writer thread drains at cap_tail, and a memory barrier
// on each side is all the ordering it needs. See the ring in
// src/new_protocol.c, which this follows.
//
static struct divcap_block  slot_meta[DIVCAP_QUEUE];
static float               *slot_data[DIVCAP_QUEUE];
static volatile atomic_int cap_head = 0;      // next slot to fill
static volatile atomic_int cap_tail = 0;      // next slot to write

static int        cap_nfft = 0;
static size_t     cap_payload = 0;   // bytes of float payload per block
static guint32    cap_seq = 0;
static volatile guint32 cap_blocks = 0;    // blocks actually written
static volatile guint32 cap_skipped = 0;   // blocks lost to a full ring
static guint32    cap_max_blocks = 0;
static gint64     cap_t0 = 0;
static char       cap_path[512];

//
// Close the file and stamp the trailer. Called from the writer thread
// when the block budget is reached and from diversity_capture_stop()
// otherwise, so it has to be idempotent - hence the fp test.
//
// No lock, and none needed: the two callers cannot overlap, because
// diversity_capture_stop() joins the writer before it calls this.
//
static void divcap_close(void) {
  if (fp == NULL) { return; }

  struct divcap_trailer tr;
  memset(&tr, 0, sizeof(tr));
  tr.end_magic = DIVCAP_END_MAGIC;
  tr.blocks    = cap_blocks;
  tr.skipped   = cap_skipped;
  tr.duration  = (double)(g_get_monotonic_time() - cap_t0) / 1.0e6;
  fwrite(&tr, sizeof(tr), 1, fp);
  fclose(fp);
  fp = NULL;
  t_print("%s: %s: %u blocks, %u skipped, %.1f s\n", __func__,
          cap_path, cap_blocks, cap_skipped, tr.duration);
}

static gpointer divcap_writer_thread(gpointer data) {
  (void) data;
  t_print("%s: diversity capture writer running\n", __func__);

  for (;;) {
    sem_wait(CAP_SEM);

    //
    // Drain before quitting, so that a capture is complete on disk: each
    // queued block carries its own post, and the post from
    // diversity_capture_stop() gives one final wake that finds the ring
    // empty.
    //
    if (cap_tail == cap_head) {
      if (!cap_run) { break; }

      continue;                     // stale post from a previous capture
    }

    MEMORY_BARRIER;
    const int slot = cap_tail;
    //
    // The slot is not handed back until the write finishes, so the ring
    // holds it against the filler. That is the whole point of the ring:
    // the analysis thread finds it full and skips a block rather than
    // waiting behind the disk.
    //
    int ok = 1;

    if (fp != NULL) {
      if (fwrite(&slot_meta[slot], sizeof(struct divcap_block), 1, fp) != 1) { ok = 0; }

      if (ok && fwrite(slot_data[slot], 1, cap_payload, fp) != cap_payload) { ok = 0; }
    }

    if (fp != NULL) {
      if (!ok) {
        t_print("%s: write failed, stopping capture\n", __func__);
        div_capture_active = 0;
        divcap_close();
      } else {
        cap_blocks++;

        if (cap_max_blocks > 0 && cap_blocks >= cap_max_blocks) {
          //
          // The budget is up. Stop the feed and close the file here rather
          // than waiting to be told, so a capture is complete on disk even
          // if nobody is watching the menu.
          //
          div_capture_active = 0;
          divcap_close();
        }
      }
    }

    MEMORY_BARRIER;
    cap_tail = (cap_tail + 1) % DIVCAP_QUEUE;
  }

  t_print("%s: diversity capture writer stopped\n", __func__);
  return NULL;
}

static int divcap_env_int(const char *name, int dflt, int lo, int hi) {
  const char *s = g_getenv(name);

  if (s == NULL || *s == '\0') { return dflt; }

  int v = atoi(s);

  if (v < lo) { v = lo; }

  if (v > hi) { v = hi; }

  return v;
}

int diversity_capture_start(int sample_rate, int nfft) {
  if (div_capture_active || fp != NULL) { return 1; }

  if (sample_rate <= 0 || nfft <= 0) { return 0; }

  const char *dir  = g_getenv("PIHPSDR_DIVCAP_DIR");
  const char *note = g_getenv("PIHPSDR_DIVCAP_NOTE");
  const int   secs = divcap_env_int("PIHPSDR_DIVCAP_SECONDS", 60, 1, 3600);

  if (dir == NULL || *dir == '\0') { dir = "."; }

  {
    time_t     now = time(NULL);
    struct tm  tm;
    char       stamp[32];
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    snprintf(cap_path, sizeof(cap_path), "%s/divcap-%s.divc", dir, stamp);
  }

  cap_nfft    = nfft;
  cap_payload = (size_t)nfft * 4u * sizeof(float);   // two arms, I and Q

  for (int i = 0; i < DIVCAP_QUEUE; i++) {
    slot_data[i] = g_malloc(cap_payload);

    if (slot_data[i] == NULL) {
      for (int j = 0; j < i; j++) { g_free(slot_data[j]); slot_data[j] = NULL; }

      t_print("%s: cannot allocate %zu bytes x %d\n", __func__, cap_payload, DIVCAP_QUEUE);
      return 0;
    }
  }

  fp = fopen(cap_path, "wb");

  if (fp == NULL) {
    t_perror("diversity_capture_start:fopen");

    for (int i = 0; i < DIVCAP_QUEUE; i++) { g_free(slot_data[i]); slot_data[i] = NULL; }

    return 0;
  }

  struct divcap_header h;
  memset(&h, 0, sizeof(h));
  memcpy(h.magic, DIVCAP_MAGIC, 8);
  h.version     = DIVCAP_VERSION;
  h.sample_rate = (guint32)sample_rate;
  h.nfft        = (guint32)nfft;
  h.block_bytes = (guint32)cap_payload;
  h.t_start     = (guint64)time(NULL);
  //
  // Explicit precisions: DISCOVERED::name is 64 bytes and the environment
  // is unbounded, both wider than the fields they go into.
  //
  snprintf(h.radio, sizeof(h.radio), "%.*s", DIVCAP_RADIO_LEN - 1,
           (radio != NULL) ? radio->name : "unknown");
  snprintf(h.note, sizeof(h.note), "%.*s", DIVCAP_NOTE_LEN - 1,
           (note != NULL) ? note : "");

  if (fwrite(&h, sizeof(h), 1, fp) != 1) {
    t_perror("diversity_capture_start:fwrite");
    fclose(fp);
    fp = NULL;

    for (int i = 0; i < DIVCAP_QUEUE; i++) { g_free(slot_data[i]); slot_data[i] = NULL; }

    return 0;
  }

  //
  // Created once and never destroyed - see the declaration - then
  // drained, because a writer that exited on cap_run without consuming
  // the post that woke it leaves a count behind.
  //
  if (!cap_sem_created) {
#ifdef __APPLE__
    cap_sem = apple_sem(0);
#else
    (void)sem_init(&cap_sem, 0, 0);
#endif
    cap_sem_created = 1;
  }

  while (sem_trywait(CAP_SEM) == 0) { }

  cap_head = 0;
  cap_tail = 0;
  cap_seq = cap_blocks = cap_skipped = 0;
  cap_t0 = g_get_monotonic_time();
  //
  // The budget is in blocks because that is what the writer counts. One
  // block is nfft samples at the DDC rate whatever the rate is, so this
  // comes out as the wall time the operator asked for.
  //
  cap_max_blocks = (guint32)(((double)secs * (double)sample_rate) / (double)nfft);

  if (cap_max_blocks < 1) { cap_max_blocks = 1; }

  cap_run = 1;                    // before the thread, or it can exit at once
  writer = g_thread_new("divcap", divcap_writer_thread, NULL);
  div_capture_active = 1;
  t_print("%s: %s rate=%d nfft=%d limit=%us (%u blocks, %.0f MB)\n", __func__,
          cap_path, sample_rate, nfft, secs, cap_max_blocks,
          (double)cap_max_blocks * (double)(cap_payload + sizeof(struct divcap_block)) / 1.0e6);
  return 1;
}

void diversity_capture_stop(void) {
  if (writer == NULL && fp == NULL) { return; }

  div_capture_active = 0;
  cap_run = 0;

  if (cap_sem_created) {
    sem_post(CAP_SEM);
  }

  if (writer != NULL) {
    g_thread_join(writer);
    writer = NULL;
  }

  //
  // After the join, so this cannot race the writer's own call to it.
  //
  divcap_close();

  for (int i = 0; i < DIVCAP_QUEUE; i++) {
    g_free(slot_data[i]);
    slot_data[i] = NULL;
  }
}

void diversity_capture_block(const float *arm0, const float *arm1,
                             const struct divcap_block *meta) {
  if (!div_capture_active) { return; }

  const size_t half = (size_t)cap_nfft * 2u * sizeof(float);
  const int slot  = cap_head;
  const int nhead = (slot + 1) % DIVCAP_QUEUE;

  if (nhead == cap_tail) {
    //
    // The writer is behind. Drop this block rather than block the
    // analysis thread - see the note at the top - and count it so the
    // trailer can say the capture was lossy.
    //
    cap_skipped++;
    cap_seq++;
    return;
  }

  slot_meta[slot] = *meta;
  slot_meta[slot].rec_magic = DIVCAP_REC_MAGIC;
  slot_meta[slot].seq       = cap_seq++;
  memcpy(slot_data[slot],                    arm0, half);
  memcpy((char *)slot_data[slot] + half,     arm1, half);
  //
  // The slot is complete before the writer can be told it exists.
  //
  MEMORY_BARRIER;
  cap_head = nhead;
  sem_post(CAP_SEM);
}

void diversity_capture_status(char *buf, size_t len) {
  if (buf == NULL || len == 0) { return; }

  if (!div_capture_active && fp == NULL) {
    buf[0] = '\0';
    return;
  }

  const double mb = (double)cap_blocks *
                    (double)(cap_payload + sizeof(struct divcap_block)) / 1.0e6;

  if (cap_skipped > 0) {
    snprintf(buf, len, "rec %u/%u %.0fMB (%u lost)",
             cap_blocks, cap_max_blocks, mb, cap_skipped);
  } else {
    snprintf(buf, len, "rec %u/%u %.0fMB", cap_blocks, cap_max_blocks, mb);
  }
}
