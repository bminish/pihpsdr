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

#include <gtk/gtk.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "atomic.h"
#include "ear_record.h"
#include "message.h"

//
// Single producer (the receive thread), single consumer (the writer
// thread), so the rings need no lock: each side only advances its own
// index, behind a barrier.
//
#define FRAME_RING  (1u << 19)          // frames, about 11 s at 48 kHz
#define EVENT_RING  (1u << 14)          // CSV rows

struct ear_event {
  uint64_t frame;      // frames written before this block
  uint8_t  who, mode;
  int8_t   paired;
  int16_t  rx1_cnt;
  uint16_t nsamp;
  float    bal_l, bal_r, af_db;
};

volatile int ear_record_active = 0;

static float            *fring = NULL;          // 2 floats per frame
static struct ear_event *ering = NULL;
static volatile uint32_t f_in = 0, f_out = 0, e_in = 0, e_out = 0;
static volatile uint64_t f_total = 0;           // frames accepted
static volatile uint32_t f_lost = 0, e_lost = 0;
static uint64_t          clipped = 0;           // samples held at full scale
static volatile int      w_run = 0;
static GThread          *writer = NULL;
static FILE             *wav = NULL, *csv = NULL;
static uint64_t          written = 0;          // frames in the file
static int               owner_tag = EAR_REC_NONE;
static char              wav_name[512];

static void put32(FILE *f, uint32_t v) {
  unsigned char b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };
  fwrite(b, 1, 4, f);
}

//
// PCM, 16 bits, 2 channels, 48 kHz. Written with zero sizes at the start
// and patched at the end.
//
// 16 bits is ample: the first recording (CW, AGC on) spanned about 30 dB,
// -46 to -15 dBFS, and 16-bit quantisation sits at -95 dBFS. Float's only
// gain, headroom above full scale, was never used.
//
static void wav_header(FILE *f, uint64_t frames) {
  const uint64_t bytes = frames * 4u;
  const uint32_t data  = bytes > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)bytes;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36u + data);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put32(f, 1u | (2u << 16));          // format 1 (PCM), 2 channels
  put32(f, 48000);
  put32(f, 48000u * 4u);              // byte rate
  put32(f, 4u | (16u << 16));         // block align 4, 16 bits
  fwrite("data", 1, 4, f);
  put32(f, data);
}

static void drain(void) {
  uint32_t in = f_in;
  MEMORY_BARRIER;

  while (f_out != in) {
    const uint32_t o = f_out;
    const uint32_t n = (in > o) ? in - o : FRAME_RING - o;
    int16_t pcm[2 * 1024];

    for (uint32_t k = 0; k < 2 * n; k += 2 * 1024) {
      const uint32_t m = (2 * n - k < 2 * 1024) ? 2 * n - k : 2 * 1024;

      for (uint32_t j = 0; j < m; j++) {
        double v = fring[2 * o + k + j] * 32768.0;

        if (v > 32767.0) { v = 32767.0; clipped++; }
        else if (v < -32768.0) { v = -32768.0; clipped++; }

        pcm[j] = (int16_t)lrint(v);
      }

      fwrite(pcm, sizeof(int16_t), m, wav);
    }

    written += n;
    MEMORY_BARRIER;
    f_out = (o + n) & (FRAME_RING - 1);
  }

  in = e_in;
  MEMORY_BARRIER;

  while (e_out != in) {
    const struct ear_event *e = &ering[e_out];

    if (csv != NULL) {
      fprintf(csv, "%llu,%u,%u,%d,%d,%u,%.4f,%.4f,%.1f\n", (unsigned long long)e->frame,
              e->who, e->mode, e->paired, e->rx1_cnt, e->nsamp, e->bal_l, e->bal_r, e->af_db);
    }

    MEMORY_BARRIER;
    e_out = (e_out + 1) & (EVENT_RING - 1);
  }
}

static gpointer writer_thread(gpointer data) {
  (void)data;

  while (w_run) {
    drain();
    g_usleep(50000);
  }

  drain();
  return NULL;
}

void ear_record_put(double left, double right) {
  if (!ear_record_active) { return; }

  const uint32_t i = f_in;
  const uint32_t n = (i + 1) & (FRAME_RING - 1);

  if (n == f_out) { f_lost++; return; }

  fring[2 * i]     = (float)left;
  fring[2 * i + 1] = (float)right;
  MEMORY_BARRIER;
  f_in = n;
  f_total++;
}

void ear_record_block(int who, int mode, int paired, int rx1_cnt, int nsamp,
                      double bal_l, double bal_r, double af_db) {
  if (!ear_record_active) { return; }

  const uint32_t i = e_in;
  const uint32_t n = (i + 1) & (EVENT_RING - 1);

  if (n == e_out) { e_lost++; return; }

  struct ear_event *e = &ering[i];
  e->frame   = f_total;
  e->who     = (uint8_t)who;
  e->mode    = (uint8_t)mode;
  e->paired  = (int8_t)paired;
  e->rx1_cnt = (int16_t)rx1_cnt;
  e->nsamp   = (uint16_t)nsamp;
  e->bal_l   = (float)bal_l;
  e->bal_r   = (float)bal_r;
  e->af_db   = (float)af_db;
  MEMORY_BARRIER;
  e_in = n;
}

int ear_record_owner(void) {
  return owner_tag;
}

int ear_record_start(const char *wav_path, const char *csv_path, int owner) {
  if (owner_tag != EAR_REC_NONE) { return 0; }

  if (fring == NULL) {
    fring = g_new(float, 2 * (size_t)FRAME_RING);
    ering = g_new(struct ear_event, EVENT_RING);
  }

  wav = fopen(wav_path, "wb");

  if (wav == NULL) {
    t_perror("ear_record_start:fopen");
    return 0;
  }

  wav_header(wav, 0);
  csv = NULL;

  if (csv_path != NULL) {
    csv = fopen(csv_path, "w");

    if (csv == NULL) {
      t_perror("ear_record_start:fopen csv");
    } else {
      fprintf(csv, "frame,who,mode,paired,rx1_cnt,nsamp,bal_l,bal_r,af_db\n");
    }
  }

  f_in = f_out = e_in = e_out = 0;
  f_total = 0;
  f_lost = e_lost = 0;
  written = 0;
  clipped = 0;
  g_strlcpy(wav_name, wav_path, sizeof(wav_name));
  owner_tag = owner;
  w_run = 1;
  writer = g_thread_new("ear_record", writer_thread, NULL);
  MEMORY_BARRIER;
  ear_record_active = 1;
  t_print("%s: %s%s%s\n", __func__, wav_path, csv ? " + " : "", csv ? csv_path : "");
  return 1;
}

void ear_record_stop(int owner) {
  if (owner_tag == EAR_REC_NONE || owner != owner_tag) { return; }

  ear_record_active = 0;
  //
  // A pass already inside put or block finishes into the ring; the writer
  // drains whatever is there after it is told to stop.
  //
  g_usleep(20000);
  w_run = 0;
  g_thread_join(writer);
  writer = NULL;
  fseek(wav, 0, SEEK_SET);
  wav_header(wav, written);
  fclose(wav);
  wav = NULL;

  if (csv != NULL) {
    fclose(csv);
    csv = NULL;
  }

  t_print("%s: %s %.1f s, %llu samples clipped%s\n", __func__, wav_name, written / 48000.0,
          (unsigned long long)clipped,
          (f_lost || e_lost) ? " (frames lost: writer fell behind)" : "");
  owner_tag = EAR_REC_NONE;
}

void ear_record_status(char *buf, size_t len) {
  if (owner_tag == EAR_REC_NONE) {
    if (len) { buf[0] = '\0'; }

    return;
  }

  snprintf(buf, len, "%.1f s", f_total / 48000.0);
}
