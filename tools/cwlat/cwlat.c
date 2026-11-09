/*
 * cwlat - measure latency and jitter between MIDI paddle events and the piHPSDR
 *         CW sidetone, from OUTSIDE piHPSDR.
 *
 * What it does:
 *   1. Sends paddle / PTT note events into a (virtual) MIDI port through the ALSA
 *      sequencer. Each event is time-stamped with CLOCK_MONOTONIC immediately
 *      before it is written.
 *   2. Captures the monitor of the PipeWire sink piHPSDR plays its audio to. The
 *      time (CLOCK_MONOTONIC) at the start of every capture callback is recorded,
 *      so every audio sample gets a time on the SAME clock as the MIDI events.
 *   3. After the run, finds the sidetone (band-pass at the sidetone frequency),
 *      matches tone segments to key events and reports latency, jitter, element
 *      lengths, dropped events and double elements.
 *
 * Nothing is printed while the test runs, so the instrument does not disturb the
 * timing it measures.
 *
 * What it measures: MIDI write -> sink monitor, that is, up to the point where the
 * sidetone enters the sink. PipeWire adds one quantum of ring fill plus the sink's
 * headroom after that, and the USB transfer / DAC are not covered at all.
 *
 * Build: tools/cwlat/build.sh      Usage: cwlat --help
 */

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FS 48000.0

#define NOTE_CWR 20      // as in the piHPSDR MIDI table of this installation
#define NOTE_CWL 21
#define NOTE_PTT 22
#define NOTE_KEY 23      // external keyer, used by --test ext: map it to "CW Key (Keyer)" (CW_KEYER_KEYDOWN) in the MIDI menu
#define NOTE_EXTPTT 24   // ... and this one to "PTT (CW Keyer)" (CW_KEYER_PTT), the PTT of an external keyer (note 22 stays the footswitch PTT)

/* ------------------------------------------------------------------ utilities */

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static unsigned long long rng_state = 88172645463325252ULL;
static double urand(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return (double)(rng_state >> 11) / 9007199254740992.0;
}
static double uni(double a, double b) { return a + (b - a) * urand(); }

static volatile sig_atomic_t stop_flag = 0;
static void on_signal(int s) { (void) s; stop_flag = 1; }

/* ------------------------------------------------------------------- options */

static const char *opt_sink = NULL;
static const char *opt_dest = "32:0";
static const char *opt_test = NULL;
static const char *opt_label = "";
static const char *opt_csv = NULL;
static const char *opt_events = NULL;
static const char *opt_sinkwav = NULL;
static int opt_trials = 120;
static int opt_wpm = 22;
static int opt_freq = 550;
static int opt_chan = 0;
static int opt_ptt = 0;
static int opt_confirm = 0;
static int opt_selftest = 0;
static double opt_pause_frac = 0.2;
static double opt_cold_ms = 400.0;
static double opt_hang_from = 100.0, opt_hang_to = 1400.0, opt_hang_step = 50.0;   // hang test: grid of D
static unsigned long long opt_seed = 12345;

/* --------------------------------------------------------------- the schedule */

typedef struct {
  double t_plan;      // planned time, seconds after start
  double t_send;      // actual CLOCK_MONOTONIC when written (0 = not sent)
  int    note;
  int    on;
  int    trial;     // trial index, -1 for PTT
} EVENT;

typedef struct {
  int    kind;        // 0 straight, 1 dit, 2 dah, 3 squeeze, 4 hang (pairs of dit taps), 5 blip (pairs: blip or tap, then a probe tap)
  double hold;        // ms
  double gap;         // hang test: time between the keyer finishing the previous tap and this tap, ms (0 otherwise)
  int    ev_press;    // index of the first press event
  int    ev_release;  // index of the release event of the primary key
} TRIAL;

static EVENT *ev = NULL;
static int nev = 0, evcap = 0;
static TRIAL *tr = NULL;
static int ntr = 0;

static int cur_trial = -1;

static int add_event(double t, int note, int on) {
  if (nev >= evcap) {
    evcap = evcap ? 2 * evcap : 1024;
    ev = realloc(ev, evcap * sizeof(EVENT));
  }
  ev[nev].t_plan = t;
  ev[nev].t_send = 0.0;
  ev[nev].note = note;
  ev[nev].on = on;
  ev[nev].trial = cur_trial;
  return nev++;
}

static int cmp_ev(const void *a, const void *b) {
  double d = ((const EVENT *) a)->t_plan - ((const EVENT *) b)->t_plan;
  return (d > 0) - (d < 0);
}

static double build_schedule(void) {
  static const double tap_holds[] = {20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 140};
  static const double sq_holds[] = {40, 60, 80, 100, 140, 200};
  static const double short_holds[] = {5, 10, 15, 20, 25, 30, 35, 40, 50};
  static const double ext_holds[] = {40, 60, 80};          // ext: key hold, ms
  static const double ext_leads[] = {5, 30, 100};          // ext: key-down this long after PTT on (5: before TX is up)
  static const double ext_gaps[] = {120, 250, 380};        // ext: previous key-up to PTT on (the first wait is gap + lead, below 500 ms)
  int ext = !strcmp(opt_test, "ext");
  int kind = !strcmp(opt_test, "straight") || !strcmp(opt_test, "short") || ext ? 0 : !strcmp(opt_test, "dit") ? 1 : !strcmp(opt_test, "dah") ? 2 : !strcmp(opt_test, "hang") ? 4 : !strcmp(opt_test, "blip") ? 5 : 3;
  double press_prev = 0.0, pending_gap = 0.0;
  double t = 0.0;
  tr = calloc(opt_trials, sizeof(TRIAL));
  if (opt_ptt) {
    add_event(0.3, NOTE_PTT, 1);
    t = 1.5;
  } else {
    t = 0.8;
  }
  for (int i = 0; i < opt_trials; i++) {
    double hold, last;
    int e1, e2 = -1, e3 = -1;
    cur_trial = i;
    TRIAL *T = &tr[ntr++];
    T->kind = kind;
    switch (kind) {
    case 0:
      //
      // short: straight key holds of 5 to 50 ms, every one after a long pause, so that each lands in the
      // time the keyer needs to switch from RX to TX
      //
      hold = ext ? ext_holds[i % 3] : !strcmp(opt_test, "short") ? short_holds[(i * 4) % 9] : uni(30.0, 160.0);
      break;
    case 1:
    case 2:
      hold = tap_holds[(i * 7) % 12];
      break;
    case 4:
      hold = 40.0;
      break;
    case 5:
      //
      // pairs. Even trials: the first of four pair types is a real 60 ms tap (reference: starts TX, so its
      // probe is warm), the other three are blips of 1, 2 and 3 ms. Odd trials: the probe, a 60 ms tap.
      //
      hold = (i % 2) ? 60.0 : (double[]){60.0, 1.0, 2.0, 3.0}[(i / 2) % 4];
      break;
    default:
      hold = sq_holds[(i * 5) % 6];
      break;
    }
    T->hold = hold;
    if (ext) {
      //
      // external keyer: PTT on, the key down after a lead time, key up, PTT off 60 ms later. The PTT events belong to
      // no trial (the analysis looks at the key). The next trial comes ext_gaps ms after this key-up, so its first key-down
      // carries a wait of gap + lead (below 500 ms) in CW_KEYER_KEYDOWN: the case the side tone's idle state is for.
      //
      double lead = ext_leads[(i / 3) % 3];
      cur_trial = -1;
      add_event(t, NOTE_EXTPTT, 1);
      cur_trial = i;
      e1 = add_event(t + lead / 1000.0, NOTE_KEY, 1);
      e2 = add_event(t + (lead + hold) / 1000.0, NOTE_KEY, 0);
      cur_trial = -1;
      add_event(t + (lead + hold + 60.0) / 1000.0, NOTE_EXTPTT, 0);
      cur_trial = i;
      last = t + (lead + hold) / 1000.0;
      T->gap = ext_gaps[(i / 9) % 3];
      T->ev_press = e1;
      T->ev_release = e2;
    } else if (kind == 0 || kind == 2 || kind == 5) {
      e1 = add_event(t, NOTE_CWR, 1);
      e2 = add_event(t + hold / 1000.0, NOTE_CWR, 0);
      last = t + hold / 1000.0;
      T->ev_press = e1;
      T->ev_release = e2;
    } else if (kind == 1 || kind == 4) {
      e1 = add_event(t, NOTE_CWL, 1);
      e2 = add_event(t + hold / 1000.0, NOTE_CWL, 0);
      last = t + hold / 1000.0;
      T->ev_press = e1;
      T->ev_release = e2;
    } else {
      double d = uni(0.0, 0.010);
      e1 = add_event(t, NOTE_CWL, 1);
      e2 = add_event(t + d, NOTE_CWR, 1);
      e3 = add_event(t + hold / 1000.0, NOTE_CWL, 0);
      add_event(t + d + hold / 1000.0, NOTE_CWR, 0);
      last = t + d + hold / 1000.0;
      T->ev_press = e1;
      T->ev_release = e3;
    }
    //
    // gap to the next trial: usually short (keyer still hanging in TX), sometimes a long
    // pause after which break-in has to go to TX again
    //
    if (kind == 5) {
      //
      // blip test: the probe comes 150 ms after the press of the blip (inside the hang time if the blip started
      // TX, long after the cold start is over if it did not); after the probe a long pause, so that the next
      // pair starts from RX (the keyer's hang time may be as short as 300 ms)
      //
      if (i % 2 == 0) {
        press_prev = t;
        t = press_prev + 0.150;
      } else {
        t = last + 2.5;
      }
    } else if (kind == 4) {
      //
      // hang test: even trials follow a long pause (TX has dropped); odd trials come D ms after the keyer
      // has finished the preceding dit (press + dot + gap) - the keyer's hang time counts from there
      //
      double two_dots = 2.0 * 1.2 / opt_wpm;          // seconds: dot + inter-element gap
      if (i % 2 == 0) {
        press_prev = t;
        int nd_ = (int)((opt_hang_to - opt_hang_from) / opt_hang_step + 0.5) + 1;
        pending_gap = opt_hang_from + opt_hang_step * ((i / 2) % nd_);   // default 100 .. 1400 ms in steps of 50
        t = press_prev + two_dots + pending_gap / 1000.0;
      } else {
        T->gap = pending_gap;
        t = last + 2.5;
      }
    } else if (ext) {
      t = last + T->gap / 1000.0;
    } else {
      double settle = kind == 0 ? 0.05 : kind == 1 ? 0.25 : kind == 2 ? 0.35 : 0.80;
      double gap = settle + uni(0.0, 0.15);
      if (!opt_ptt && urand() < opt_pause_frac) { gap = uni(0.9, 1.6); }
      if (!strcmp(opt_test, "short")) { gap = uni(1.2, 1.8); }
      t = last + gap;
    }
  }
  cur_trial = -1;
  if (opt_ptt) { add_event(t + 0.8, NOTE_PTT, 0); }
  // events were added in time order per trial, except squeeze (d offsets): sort them
  // but trial indices must follow, so remember note/time and re-find afterwards
  for (int i = 0; i < ntr; i++) {
    // store planned times in the trial for re-linking after sorting
    (void) tr[i];
  }
  //
  // sort; fix trial references by planned time + note
  //
  double *press_t = malloc(ntr * sizeof(double)), *rel_t = malloc(ntr * sizeof(double));
  int *press_n = malloc(ntr * sizeof(int)), *rel_n = malloc(ntr * sizeof(int));
  for (int i = 0; i < ntr; i++) {
    press_t[i] = ev[tr[i].ev_press].t_plan;
    press_n[i] = ev[tr[i].ev_press].note;
    rel_t[i] = ev[tr[i].ev_release].t_plan;
    rel_n[i] = ev[tr[i].ev_release].note;
  }
  qsort(ev, nev, sizeof(EVENT), cmp_ev);
  for (int i = 0; i < ntr; i++) {
    for (int j = 0; j < nev; j++) {
      if (ev[j].t_plan == press_t[i] && ev[j].note == press_n[i] && ev[j].on) { tr[i].ev_press = j; }
      if (ev[j].t_plan == rel_t[i] && ev[j].note == rel_n[i] && !ev[j].on) { tr[i].ev_release = j; }
    }
  }
  free(press_t); free(rel_t); free(press_n); free(rel_n);
  return ev[nev - 1].t_plan;
}

/* ----------------------------------------------------------------- MIDI sender */

static snd_seq_t *seq = NULL;
static int seq_port, dst_client, dst_port;

static int midi_open(void) {
  if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { return -1; }
  snd_seq_set_client_name(seq, "cwlat");
  seq_port = snd_seq_create_simple_port(seq, "out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  if (seq_port < 0) { return -1; }
  snd_seq_addr_t a;
  if (snd_seq_parse_address(seq, &a, opt_dest) < 0) {
    fprintf(stderr, "cwlat: cannot parse MIDI destination '%s'\n", opt_dest);
    return -1;
  }
  dst_client = a.client;
  dst_port = a.port;
  //
  // VirMIDI only delivers to its rawmidi input when the sender is SUBSCRIBED to it;
  // events with just a destination address are accepted by the kernel but dropped.
  //
  int r = snd_seq_connect_to(seq, seq_port, dst_client, dst_port);
  if (r < 0) {
    fprintf(stderr, "cwlat: cannot connect to %s: %s\n", opt_dest, snd_strerror(r));
    return -1;
  }
  return 0;
}

static double midi_send(int note, int on) {
  snd_seq_event_t e;
  snd_seq_ev_clear(&e);
  snd_seq_ev_set_source(&e, seq_port);
  snd_seq_ev_set_subs(&e);
  snd_seq_ev_set_direct(&e);
  //
  // The virtual MIDI port turns sequencer events into a raw byte stream with RUNNING STATUS: a message with the
  // same status as the one before it is sent without its status byte. piHPSDR's parser does not support running
  // status and drops such a message. Two note-ons in a row (PTT and a paddle, or both paddles of a squeeze) or two
  // note-offs in a row (the end of a run, a PTT release) lost their second message: this was the "PTT stuck" and
  // "first tap after PTT lost" fault of the PTT runs, and the squeeze test never squeezed. A Program Change in
  // front of every note changes the status, so the note always carries its own (piHPSDR ignores Program Change).
  //
  snd_seq_ev_set_pgmchange(&e, opt_chan, 0);
  snd_seq_event_output_direct(seq, &e);
  if (on) {
    snd_seq_ev_set_noteon(&e, opt_chan, note, 100);
  } else {
    snd_seq_ev_set_noteoff(&e, opt_chan, note, 0);
  }
  double t = now();
  snd_seq_event_output_direct(seq, &e);
  return t;
}

static void release_all(void) {
  if (!seq) { return; }
  midi_send(NOTE_CWL, 0);
  midi_send(NOTE_CWR, 0);
  midi_send(NOTE_KEY, 0);
  midi_send(NOTE_EXTPTT, 0);
  midi_send(NOTE_PTT, 0);
}

/* ------------------------------------------------------------- PipeWire capture */

typedef struct { double t; size_t first; int n; } CB;
static CB *cbs = NULL;
static size_t ncb = 0, cbcap = 0;
static float *rec = NULL;
static size_t nrec = 0, reccap = 0;
static atomic_int cap_started = 0;
static atomic_int cap_overflow = 0;

static struct pw_thread_loop *pwloop;
static struct pw_context *pwctx;
static struct pw_core *pwcore;
static struct pw_stream *pwstream;

static void on_process(void *data) {
  (void) data;
  double t = now();
  struct pw_buffer *b = pw_stream_dequeue_buffer(pwstream);
  if (!b) { return; }
  struct spa_data *d = &b->buffer->datas[0];
  if (d->data != NULL && d->chunk != NULL) {
    const float *s = SPA_PTROFF(d->data, d->chunk->offset, float);
    int n = d->chunk->size / (2 * sizeof(float));
    if (nrec + n <= reccap && ncb < cbcap) {
      for (int i = 0; i < n; i++) { rec[nrec + i] = 0.5f * (s[2 * i] + s[2 * i + 1]); }
      cbs[ncb].t = t;
      cbs[ncb].first = nrec;
      cbs[ncb].n = n;
      ncb++;
      nrec += n;
      atomic_store(&cap_started, 1);
    } else {
      atomic_store(&cap_overflow, 1);
    }
  }
  pw_stream_queue_buffer(pwstream, b);
}

static int capture_start(double seconds) {
  reccap = (size_t)((seconds + 4.0) * FS);
  cbcap = reccap / 8 + 64;
  rec = malloc(reccap * sizeof(float));
  cbs = malloc(cbcap * sizeof(CB));
  if (!rec || !cbs) { return -1; }
  pw_init(NULL, NULL);
  pwloop = pw_thread_loop_new("cwlat", NULL);
  pwctx = pw_context_new(pw_thread_loop_get_loop(pwloop), NULL, 0);
  if (pw_thread_loop_start(pwloop) < 0) { return -1; }
  pw_thread_loop_lock(pwloop);
  pwcore = pw_context_connect(pwctx, NULL, 0);
  if (!pwcore) { pw_thread_loop_unlock(pwloop); return -1; }
  struct pw_properties *props = pw_properties_new(
                                  PW_KEY_MEDIA_TYPE, "Audio",
                                  PW_KEY_MEDIA_CATEGORY, "Capture",
                                  PW_KEY_MEDIA_ROLE, "DSP",
                                  PW_KEY_NODE_NAME, "cwlat",
                                  PW_KEY_TARGET_OBJECT, opt_sink,
                                  NULL);
  pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
  static const struct pw_stream_events events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
  };
  pwstream = pw_stream_new_simple(pw_thread_loop_get_loop(pwloop), "cwlat-capture", props, &events, NULL);
  if (!pwstream) { pw_thread_loop_unlock(pwloop); return -1; }
  struct spa_audio_info_raw info = {.format = SPA_AUDIO_FORMAT_F32, .rate = 48000, .channels = 2};
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod *params[1];
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
  int res = pw_stream_connect(pwstream, PW_DIRECTION_INPUT, PW_ID_ANY,
                              PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
                              params, 1);
  pw_thread_loop_unlock(pwloop);
  return res < 0 ? -1 : 0;
}

static void capture_stop(void) {
  if (!pwloop) { return; }
  pw_thread_loop_lock(pwloop);
  pw_stream_destroy(pwstream);
  pw_core_disconnect(pwcore);
  pw_thread_loop_unlock(pwloop);
  pw_thread_loop_stop(pwloop);
  pw_context_destroy(pwctx);
  pw_thread_loop_destroy(pwloop);
  pwloop = NULL;
}

/* -------------------------------------------------------------------- analysis */

typedef struct { double on10, on50, off50, off10; } SEG;
static SEG *seg = NULL;
static int nseg = 0;

static double t_of(double pos) {
  // sample position -> CLOCK_MONOTONIC seconds, through the capture callback table
  size_t lo = 0, hi = ncb;
  while (hi - lo > 1) {
    size_t mid = (lo + hi) / 2;
    if ((double) cbs[mid].first <= pos) { lo = mid; } else { hi = mid; }
  }
  return cbs[lo].t + (pos - (double) cbs[lo].first) / FS;
}

static int cmp_d(const void *a, const void *b) {
  double d = *(const double *) a - *(const double *) b;
  return (d > 0) - (d < 0);
}

static void find_segments(int *ok) {
  int W = (int) lround(FS / opt_freq);
  size_t N = nrec;
  double *I = malloc((N + 1) * sizeof(double)), *Q = malloc((N + 1) * sizeof(double));
  float *env = malloc(N * sizeof(float));
  double w = 2.0 * M_PI * opt_freq / FS;
  I[0] = Q[0] = 0.0;
  for (size_t i = 0; i < N; i++) {
    I[i + 1] = I[i] + rec[i] * cos(w * (double) i);
    Q[i + 1] = Q[i] + rec[i] * sin(w * (double) i);
  }
  for (size_t i = 0; i < N; i++) {
    long a = (long) i - W / 2, b = a + W;
    if (a < 0 || b > (long) N) { env[i] = 0.0f; continue; }
    double ii = I[b] - I[a], qq = Q[b] - Q[a];
    env[i] = (float)(2.0 * sqrt(ii * ii + qq * qq) / W);
  }
  // amplitude reference: 99.5 th percentile; noise floor: median (on a subsample)
  size_t step = N > 200000 ? N / 200000 : 1, m = 0;
  double *sub = malloc((N / step + 1) * sizeof(double));
  for (size_t i = 0; i < N; i += step) { sub[m++] = env[i]; }
  qsort(sub, m, sizeof(double), cmp_d);
  double P = sub[(size_t)(0.995 * (m - 1))], med = sub[m / 10];   // noise floor: 10th percentile (the median is tone when the duty cycle exceeds 50 %)
  free(sub);
  *ok = (P > 1e-5 && P > 8.0 * med);
  if (!*ok) {
    free(I); free(Q); free(env);
    fprintf(stderr, "cwlat: no sidetone found at %d Hz (peak %.3g, noise floor %.3g)\n", opt_freq, P, med);
    return;
  }
  double T50 = 0.5 * P, T10 = 0.1 * P;
  seg = malloc(100000 * sizeof(SEG));
  int on = 0;
  size_t i = (size_t) W;
  double on10 = 0, on50 = 0;
  for (; i < N - (size_t) W && nseg < 99990; i++) {
    if (!on && env[i] >= T50 && env[i - 1] < T50) {
      double f = (T50 - env[i - 1]) / (env[i] - env[i - 1]);
      on50 = (double)(i - 1) + f;
      size_t j = i;
      while (j > 1 && env[j] >= T10) { j--; }
      f = (T10 - env[j]) / (env[j + 1] - env[j] + 1e-30);
      on10 = (double) j + (f < 0 ? 0 : f > 1 ? 1 : f);
      on = 1;
    } else if (on && env[i] < T50 && env[i - 1] >= T50) {
      double f = (env[i - 1] - T50) / (env[i - 1] - env[i]);
      double off50 = (double)(i - 1) + f;
      size_t j = i;
      while (j < N - 1 && env[j] >= T10) { j++; }
      f = (env[j - 1] - T10) / (env[j - 1] - env[j] + 1e-30);
      double off10 = (double)(j - 1) + (f < 0 ? 0 : f > 1 ? 1 : f);
      if ((off50 - on50) / FS > 0.005) {
        seg[nseg].on10 = t_of(on10);
        seg[nseg].on50 = t_of(on50);
        seg[nseg].off50 = t_of(off50);
        seg[nseg].off10 = t_of(off10);
        nseg++;
      }
      on = 0;
      //
      // Resume 3 ms after the end of the tone, not where the level falls below 10 %: with receiver audio on
      // the sink the level may stay above 10 % into the next tone, which would then be skipped (seen on
      // 2026-11-09 at every cold start, with the AF gain at its minimum, which is not a mute).
      //
      i = (size_t) off50 + (size_t) (0.003 * FS);
      if (i < j && j - (size_t) off50 < (size_t) (0.003 * FS)) { i = j; }
    }
  }
  free(I); free(Q); free(env);
}

typedef struct {
  int    trial, kind, nel, dropped, cold;
  double hold, tpress, lat10, lat50, len[4], offlat, idle;
} RES;

static void stats_line(const char *name, double *v, int n) {
  if (n <= 0) { printf("  %-34s n=0\n", name); return; }
  qsort(v, n, sizeof(double), cmp_d);
  double sum = 0, sum2 = 0;
  for (int i = 0; i < n; i++) { sum += v[i]; sum2 += v[i] * v[i]; }
  double mean = sum / n, sd = sqrt(fmax(0.0, sum2 / n - mean * mean));
  printf("  %-34s n=%3d  mean %7.2f  sd %6.2f  min %7.2f  p5 %7.2f  p50 %7.2f  p95 %7.2f  max %7.2f\n",
         name, n, mean, sd, v[0], v[(int)(0.05 * (n - 1))], v[(n - 1) / 2], v[(int)(0.95 * (n - 1))], v[n - 1]);
}

static void analyse(void) {
  int ok;
  find_segments(&ok);
  if (!ok) { return; }
  RES *R = calloc(ntr, sizeof(RES));
  double dot = 1200.0 / opt_wpm;
  // quantum seen by the capture stream
  int cnt[8192] = {0}, best = 0;
  for (size_t k = 0; k < ncb; k++) { if (cbs[k].n > 0 && cbs[k].n < 8192) { cnt[cbs[k].n]++; } }
  for (int q = 1; q < 8192; q++) { if (cnt[q] > cnt[best]) { best = q; } }
  // match
  int s0 = 0;
  for (int i = 0; i < ntr; i++) {
    RES *r = &R[i];
    r->trial = i;
    r->kind = tr[i].kind;
    r->hold = tr[i].hold;
    double tp = ev[tr[i].ev_press].t_send;
    double tr_rel = ev[tr[i].ev_release].t_send;
    double tnext = i + 1 < ntr ? ev[tr[i + 1].ev_press].t_send : tp + 3.0;
    r->tpress = tp;
    if (tp == 0.0) { r->dropped = 1; continue; }   // never sent (interrupted)
    while (s0 < nseg && seg[s0].on50 < tp - 0.002) { s0++; }
    int k = s0;
    while (k < nseg && seg[k].on50 < tnext - 0.002 && r->nel < 4) {
      r->len[r->nel] = (seg[k].off50 - seg[k].on50) * 1000.0;
      if (r->nel == 0) {
        r->lat10 = (seg[k].on10 - tp) * 1000.0;
        r->lat50 = (seg[k].on50 - tp) * 1000.0;
        r->offlat = (seg[k].off50 - tr_rel) * 1000.0;
        r->idle = k > 0 ? (seg[k].on50 - seg[k - 1].off50) * 1000.0 : 1e9;
      }
      r->nel++;
      k++;
    }
    // elements beyond four are only counted
    while (k < nseg && seg[k].on50 < tnext - 0.002) { r->nel++; k++; }
    r->dropped = (r->nel == 0);
    r->cold = !r->dropped && r->idle > opt_cold_ms;
    s0 = s0 < nseg ? s0 : nseg;
  }
  // ---- report
  printf("\n==== cwlat result: %s  test=%s  wpm=%d  freq=%d Hz  ptt=%d ====\n", opt_label, opt_test, opt_wpm, opt_freq, opt_ptt);
  printf("capture callback size seen: %d frames (%.2f ms); %zu callbacks, %d tone segments, trials=%d\n",
         best, best / 48.0, ncb, nseg, ntr);
  int ndrop = 0;
  for (int i = 0; i < ntr; i++) { ndrop += R[i].dropped; }
  printf("dropped (no tone at all): %d of %d (%.1f %%)\n", ndrop, ntr, 100.0 * ndrop / ntr);
  double *v = malloc(ntr * sizeof(double));
  int n;
  const char *grp[3] = {"all", "warm (idle < cold-ms)", "cold (first after pause)"};
  for (int g = 0; g < 3; g++) {
    printf("-- %s\n", grp[g]);
    n = 0;
    for (int i = 0; i < ntr; i++) if (!R[i].dropped && (g == 0 || (g == 1 && !R[i].cold) || (g == 2 && R[i].cold))) { v[n++] = R[i].lat10; }
    stats_line("onset latency, 10% point (ms)", v, n);
    n = 0;
    for (int i = 0; i < ntr; i++) if (!R[i].dropped && (g == 0 || (g == 1 && !R[i].cold) || (g == 2 && R[i].cold))) { v[n++] = R[i].lat50; }
    stats_line("onset latency, 50% point (ms)", v, n);
    n = 0;
    for (int i = 0; i < ntr; i++) if (!R[i].dropped && (g == 0 || (g == 1 && !R[i].cold) || (g == 2 && R[i].cold))) {
        double ref = R[i].kind == 0 ? R[i].hold : R[i].kind == 2 ? 3.0 * dot : dot;
        if (R[i].kind == 3 || R[i].kind == 5) { continue; }
        v[n++] = R[i].len[0] - ref;
      }
    stats_line(R[0].kind == 0 ? "first tone length minus hold (ms)" : "first tone length minus nominal (ms)", v, n);
    if (R[0].kind == 0) {
      n = 0;
      for (int i = 0; i < ntr; i++) if (!R[i].dropped && (g == 0 || (g == 1 && !R[i].cold) || (g == 2 && R[i].cold))) { v[n++] = R[i].offlat; }
      stats_line("release -> tone off, 50% point (ms)", v, n);
    }
  }
  if (tr[0].kind != 0 && tr[0].kind != 4 && tr[0].kind != 5) {
    printf("-- elements produced per tap, by hold time (keyer repeat point)\n");
    printf("  hold ms | trials | 1 elem | 2 elem | 3+ elem | dropped | mean onset lat (ms)\n");
    double hs[16];
    int nh = 0;
    for (int i = 0; i < ntr; i++) {
      int f = 0;
      for (int j = 0; j < nh; j++) if (hs[j] == R[i].hold) { f = 1; }
      if (!f && nh < 16) { hs[nh++] = R[i].hold; }
    }
    qsort(hs, nh, sizeof(double), cmp_d);
    for (int j = 0; j < nh; j++) {
      int t = 0, e1 = 0, e2 = 0, e3 = 0, dr = 0;
      double sl = 0;
      int nl = 0;
      for (int i = 0; i < ntr; i++) {
        if (R[i].hold != hs[j]) { continue; }
        t++;
        if (R[i].dropped) { dr++; continue; }
        sl += R[i].lat10;
        nl++;
        if (R[i].nel == 1) { e1++; } else if (R[i].nel == 2) { e2++; } else { e3++; }
      }
      printf("  %7.0f | %6d | %6d | %6d | %7d | %7d | %8.2f\n", hs[j], t, e1, e2, e3, dr, nl ? sl / nl : 0.0);
    }
  }
  if (tr[0].kind == 5) {
    //
    // pair types: even trial 4k+0 and its probe. Hold 60 = real tap (starts TX): its probe is the warm reference and
    // the tap itself the cold reference. A blip is shorter than the transition into TX; if the keyer starts TX for it
    // anyway, its probe is warm, if it swallows it the probe is cold (about 20 ms later).
    //
    double hs[4] = {60.0, 1.0, 2.0, 3.0};
    double cold_sum = 0, warm_sum = 0; int cold_n = 0, warm_n = 0;
    for (int i = 0; i + 1 < ntr; i += 2) {
      if (tr[i].hold == 60.0) {
        if (!R[i].dropped) { cold_sum += R[i].lat50; cold_n++; }
        if (!R[i + 1].dropped) { warm_sum += R[i + 1].lat50; warm_n++; }
      }
    }
    double cold_ref = cold_n ? cold_sum / cold_n : 0.0, warm_ref = warm_n ? warm_sum / warm_n : 0.0;
    printf("-- blip test: probe tap 150 ms after the press of a first tap (sink latency, 50%% point)\n");
    printf("  reference: real tap, cold %.1f ms (n=%d); its probe, warm %.1f ms (n=%d); cold-like = probe later than warm+10 ms\n", cold_ref, cold_n, warm_ref, warm_n);
    printf("  first tap ms | pairs | tap produced tone | probe lat50 mean | min .. max | probe cold-like | probe dropped\n");
    for (int j = 0; j < 4; j++) {
      int np = 0, tone = 0, cl = 0, dr = 0, nl = 0; double sm = 0, mn = 1e9, mx = -1e9;
      for (int i = 0; i + 1 < ntr; i += 2) {
        if (tr[i].hold != hs[j]) { continue; }
        np++;
        if (!R[i].dropped) { tone++; }
        if (R[i + 1].dropped) { dr++; continue; }
        nl++; sm += R[i + 1].lat50; if (R[i + 1].lat50 < mn) { mn = R[i + 1].lat50; } if (R[i + 1].lat50 > mx) { mx = R[i + 1].lat50; }
        if (R[i + 1].lat50 > warm_ref + 10.0) { cl++; }
      }
      printf("  %12.0f | %5d | %17d | %16.1f | %6.1f .. %6.1f | %d of %d | %d\n", hs[j], np, tone, nl ? sm / nl : 0.0, nl ? mn : 0.0, nl ? mx : 0.0, cl, nl, dr);
    }
  }
  if (tr[0].kind == 4) {
    //
    // latency of the 2nd tap of each pair against D. The 1st tap of each pair follows a long pause and shows the
    // delay of a cold start. The effective hang time is the D at which the 2nd tap starts to show it too.
    //
    double dd[64]; int nd = 0;
    for (int i = 1; i < ntr; i += 2) {
      int f = 0;
      for (int j = 0; j < nd; j++) if (dd[j] == tr[i].gap) { f = 1; }
      if (!f && nd < 64) { dd[nd++] = tr[i].gap; }
    }
    qsort(dd, nd, sizeof(double), cmp_d);
    double cold_sum = 0; int cold_n = 0;
    for (int i = 0; i < ntr; i += 2) { if (!R[i].dropped) { cold_sum += R[i].lat50; cold_n++; } }
    double warm_sum = 0; int warm_n = 0;
    double warm_lim = dd[0] + 50.0 > 200.0 ? dd[0] + 50.0 : 200.0;   // the smallest D values are the warm reference
    for (int i = 1; i < ntr; i += 2) { if (!R[i].dropped && tr[i].gap <= warm_lim) { warm_sum += R[i].lat50; warm_n++; } }
    double cold_ref = cold_n ? cold_sum / cold_n : 0.0, warm_ref = warm_n ? warm_sum / warm_n : 0.0;
    printf("-- hang time: 2nd tap against D (D counts from the end of the previous element and its gap, in ms)\n");
    printf("  reference: first tap after a long pause (cold) %.1f ms (n=%d), 2nd tap with D<=%.0f ms (warm) %.1f ms (n=%d)\n", cold_ref, cold_n, warm_lim, warm_ref, warm_n);
    printf("  D ms | n | mean lat50 (ms) | min .. max | cold-like (lat > warm+10) | dropped\n");
    double est = -1.0;
    for (int j = 0; j < nd; j++) {
      int n_ = 0, cl = 0, dr = 0; double sm = 0, mn = 1e9, mx = -1e9;
      for (int i = 1; i < ntr; i += 2) {
        if (tr[i].gap != dd[j]) { continue; }
        if (R[i].dropped) { dr++; continue; }
        n_++; sm += R[i].lat50; if (R[i].lat50 < mn) { mn = R[i].lat50; } if (R[i].lat50 > mx) { mx = R[i].lat50; }
        if (R[i].lat50 > warm_ref + 10.0) { cl++; }
      }
      printf("  %5.0f | %d | %8.1f | %6.1f .. %6.1f | %d of %d | %d\n", dd[j], n_, n_ ? sm / n_ : 0.0, n_ ? mn : 0.0, n_ ? mx : 0.0, cl, n_, dr);
      if (est < 0 && n_ && 2 * cl >= n_) { est = dd[j]; }
    }
    if (est >= 0) { printf("  => the radio has dropped out of TX when the next tap comes %.0f ms after the keyer finished (effective hang)\n", est); }
    else { printf("  => no cold-like second taps up to the largest D: effective hang is longer than the range tested\n"); }
  }
  if (opt_csv) {
    FILE *f = fopen(opt_csv, "w");
    if (f) {
      fprintf(f, "label,test,ptt,trial,kind,hold_ms,lat10_ms,lat50_ms,nel,len0_ms,len1_ms,len2_ms,len3_ms,off_lat_ms,idle_ms,cold,dropped,gap_ms\n");
      for (int i = 0; i < ntr; i++) {
        fprintf(f, "%s,%s,%d,%d,%d,%.2f,%.3f,%.3f,%d,%.2f,%.2f,%.2f,%.2f,%.3f,%.1f,%d,%d,%.0f\n", opt_label, opt_test, opt_ptt, i, R[i].kind,
                R[i].hold, R[i].lat10, R[i].lat50, R[i].nel, R[i].len[0], R[i].len[1], R[i].len[2], R[i].len[3], R[i].offlat,
                R[i].idle > 1e8 ? -1.0 : R[i].idle, R[i].cold, R[i].dropped, tr[i].gap);
      }
      fclose(f);
      printf("per-trial results written to %s\n", opt_csv);
    }
  }
  printf("NOTE: measured MIDI write -> sink monitor. The 10%% / 50%% points lie 1.0 / 2.5 ms into the 5 ms tone ramp,\n"
         "      so the start of the ramp is about lat10 - 1.0 ms. Add the sink's reported latency (1 quantum + headroom)\n"
         "      for the ALSA input; USB transfer and DAC are not covered.\n");
  free(v); free(R);
}

/* -------------------------------------------------------------------- selftest */

static void selftest(void) {
  //
  // Synthetic run: tone follows each press after 6.0 ms + a little jitter, length = hold.
  // Checks the analysis against known numbers, no MIDI, no PipeWire.
  //
  double span = build_schedule();
  double t0 = 1000.0;
  for (int i = 0; i < nev; i++) { ev[i].t_send = t0 + ev[i].t_plan; }
  reccap = (size_t)((span + 6.0) * FS);
  cbcap = reccap / 64 + 64;
  rec = calloc(reccap, sizeof(float));
  cbs = malloc(cbcap * sizeof(CB));
  for (size_t k = 0; k * 64 < reccap && ncb < cbcap; k++) {
    cbs[ncb].t = t0 + (double)(k * 64) / FS + (urand() - 0.5) * 2e-5;
    cbs[ncb].first = k * 64;
    cbs[ncb].n = 64;
    ncb++;
  }
  nrec = reccap;
  const double lat = 0.0060;
  for (int i = 0; i < ntr; i++) {
    double s = (ev[tr[i].ev_press].t_plan + lat + (urand() - 0.5) * 0.001) * FS;
    double e = s + tr[i].hold * FS / 1000.0;
    if (tr[i].kind == 1 && tr[i].hold > 95.0) { /* a second dit */ }
    for (long n = (long) s; n < (long) e + 240; n++) {
      double a = n < s ? 0 : n < s + 240 ? 0.5 * (1 - cos(M_PI * (n - s) / 240.0)) : n < e ? 1.0 : n < e + 240 ? 0.5 * (1 + cos(M_PI * (n - e) / 240.0)) : 0.0;
      if (n >= 0 && (size_t) n < nrec) { rec[n] += (float)(0.1 * a * sin(2 * M_PI * opt_freq * n / FS)); }
    }
  }
  printf("selftest: synthetic latency 6.0 ms (+-0.5 ms), tone length = hold\n");
  analyse();
}


/* ------------------------------------------------------- wall-clock bookkeeping */

//
// UTC epoch seconds minus CLOCK_MONOTONIC seconds, taken from the closest of several
// back-to-back readings. Needed to compare the MIDI send times with external,
// GPS-time-stamped recordings (KiwiSDR). The system clock should be NTP-synchronised.
//
static double real_minus_mono(void) {
  double best = 1e9, off = 0.0;
  for (int i = 0; i < 9; i++) {
    struct timespec a, r, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    clock_gettime(CLOCK_REALTIME, &r);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double d = (b.tv_sec - a.tv_sec) + 1e-9 * (b.tv_nsec - a.tv_nsec);
    if (d < best) {
      best = d;
      off = (r.tv_sec + 1e-9 * r.tv_nsec) - ((a.tv_sec + 1e-9 * a.tv_nsec) + 0.5 * d);
    }
  }
  return off;
}

static double off_start = 0.0, off_end = 0.0;

static void write_events(void) {
  if (!opt_events) { return; }
  FILE *f = fopen(opt_events, "w");
  if (!f) { return; }
  fprintf(f, "# cwlat events: label=%s test=%s ptt=%d wpm=%d trials=%d\n", opt_label, opt_test, opt_ptt, opt_wpm, ntr);
  fprintf(f, "# utc_minus_mono_start=%.6f utc_minus_mono_end=%.6f (seconds; utc = mono + offset)\n", off_start, off_end);
  fprintf(f, "idx,trial,note,on,hold_ms,t_plan_s,t_send_mono_s,t_send_utc_s\n");
  for (int i = 0; i < nev; i++) {
    double hold = ev[i].trial >= 0 ? tr[ev[i].trial].hold : 0.0;
    // interpolate the clock offset over the run
    double a = ev[nev - 1].t_send > ev[0].t_send ? (ev[i].t_send - ev[0].t_send) / (ev[nev - 1].t_send - ev[0].t_send) : 0.0;
    double off = off_start + a * (off_end - off_start);
    fprintf(f, "%d,%d,%d,%d,%.1f,%.6f,%.6f,%.6f\n", i, ev[i].trial, ev[i].note, ev[i].on, hold, ev[i].t_plan,
            ev[i].t_send, ev[i].t_send > 0 ? ev[i].t_send + off : 0.0);
  }
  fclose(f);
}


static void write_sinkwav(void) {
  if (!opt_sinkwav || nrec == 0) { return; }
  FILE *f = fopen(opt_sinkwav, "wb");
  if (!f) { return; }
  // mono IEEE float WAV, 48 kHz (the capture is the mean of the two channels, as analysed)
  uint32_t datalen = (uint32_t)(nrec * sizeof(float)), riff = 36 + datalen, fmtlen = 16, rate = 48000, brate = 48000 * 4;
  uint16_t fmt = 3, ch = 1, align = 4, bits = 32;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtlen, 4, 1, f);
  fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&datalen, 4, 1, f);
  fwrite(rec, sizeof(float), nrec, f);
  fclose(f);
  char name[1100];
  snprintf(name, sizeof(name), "%s.cb.csv", opt_sinkwav);
  f = fopen(name, "w");
  if (!f) { return; }
  fprintf(f, "# first-sample index and CLOCK_MONOTONIC time of each capture callback; utc = mono + offset in the events file\n");
  fprintf(f, "t_mono_s,first,n\n");
  for (size_t k = 0; k < ncb; k++) { fprintf(f, "%.6f,%zu,%d\n", cbs[k].t, cbs[k].first, cbs[k].n); }
  fclose(f);
}

/* ------------------------------------------------------------------------ main */

static void usage(void) {
  printf("usage: cwlat --confirm-safe --sink NODE --test straight|short|dit|dah|squeeze|hang|blip|ext [options]\n"
         "  --sink NAME       PipeWire sink piHPSDR plays to (its monitor is captured)\n"
         "  --dest C:P        ALSA seq destination, default 32:0 (VirMIDI 4-0)\n"
         "  --test T          straight: CWR held; dit: CWL tap; dah: CWR tap; squeeze: both;\n"
         "                    hang: pairs of dit taps, 2nd tap D=100..1400 ms after the keyer finished the 1st (break-in hang time);\n"
         "                          --hang-from MS --hang-to MS --hang-step MS change the grid of D (default 100 1400 50, at most 64 values)\n"
         "                    short: straight key holds of 5-50 ms, each after a long pause (every one in the RX->TX switch)\n"
         "                    ext: an external keyer (needs note 23 mapped to \"CW Key (Keyer)\" and note 24 to \"PTT (CW Keyer)\"): PTT on, key down after a lead of 5/30/100 ms, hold 40/60/80 ms,\n"
         "                          key up, PTT off; the next trial 120/250/380 ms after the key-up (the first wait of the key-down is below 500 ms)\n"
         "                    blip: pairs (tap or 1-3 ms blip, then a probe tap 150 ms later); a probe that is warm shows TX was started\n"
         "  --trials N        default 120\n"
         "  --wpm N           keyer speed set in piHPSDR (for nominal lengths), default 22\n"
         "  --freq HZ         sidetone frequency, default 550\n"
         "  --chan N          MIDI channel 0-15, default 0\n"
         "  --ptt             send PTT (note 22) first and hold it for the whole run\n"
         "  --pause-frac F    fraction of long pauses (break-in), default 0.2\n"
         "  --cold-ms MS      idle time above which a first element counts as 'cold', default 400\n"
         "  --label TEXT      free text copied to the output\n"
         "  --csv FILE        per-trial results\n"
         "  --sink-wav FILE   the recorded sink monitor as a mono float32 WAV (48 kHz), with FILE.cb.csv (callback times)\n"
         "  --events FILE     every MIDI event with monotonic and UTC send times (for external captures)\n"
         "  --seed N          random seed\n"
         "  --selftest        analysis check on synthetic data (no radio, no MIDI, no PipeWire)\n"
         "  --confirm-safe    required for real runs: PA off, drive zero, operator present\n");
}

int main(int argc, char **argv) {
  static struct option lo[] = {
    {"sink", 1, 0, 's'}, {"dest", 1, 0, 'd'}, {"test", 1, 0, 't'}, {"trials", 1, 0, 'n'}, {"wpm", 1, 0, 'w'},
    {"freq", 1, 0, 'f'}, {"chan", 1, 0, 'c'}, {"ptt", 0, 0, 'p'}, {"pause-frac", 1, 0, 'P'}, {"cold-ms", 1, 0, 'C'}, {"hang-from", 1, 0, 'F'}, {"hang-to", 1, 0, 'G'}, {"hang-step", 1, 0, 'H'},
    {"label", 1, 0, 'l'}, {"csv", 1, 0, 'o'}, {"events", 1, 0, 'E'}, {"sink-wav", 1, 0, 'W'}, {"seed", 1, 0, 'S'}, {"selftest", 0, 0, 'T'}, {"confirm-safe", 0, 0, 'y'},
    {"help", 0, 0, 'h'}, {0, 0, 0, 0}
  };
  int c;
  while ((c = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
    switch (c) {
    case 's': opt_sink = optarg; break;
    case 'd': opt_dest = optarg; break;
    case 't': opt_test = optarg; break;
    case 'n': opt_trials = atoi(optarg); break;
    case 'w': opt_wpm = atoi(optarg); break;
    case 'f': opt_freq = atoi(optarg); break;
    case 'c': opt_chan = atoi(optarg); break;
    case 'p': opt_ptt = 1; break;
    case 'P': opt_pause_frac = atof(optarg); break;
    case 'C': opt_cold_ms = atof(optarg); break;
    case 'F': opt_hang_from = atof(optarg); break;
    case 'G': opt_hang_to = atof(optarg); break;
    case 'H': opt_hang_step = atof(optarg); break;
    case 'l': opt_label = optarg; break;
    case 'o': opt_csv = optarg; break;
    case 'E': opt_events = optarg; break;
    case 'W': opt_sinkwav = optarg; break;
    case 'S': opt_seed = strtoull(optarg, NULL, 10); break;
    case 'T': opt_selftest = 1; break;
    case 'y': opt_confirm = 1; break;
    default: usage(); return 1;
    }
  }
  rng_state ^= opt_seed * 0x9E3779B97F4A7C15ULL;
  if (!opt_test || (strcmp(opt_test, "straight") && strcmp(opt_test, "dit") && strcmp(opt_test, "dah") && strcmp(opt_test, "squeeze") && strcmp(opt_test, "hang") && strcmp(opt_test, "blip") && strcmp(opt_test, "short") && strcmp(opt_test, "ext"))) {
    usage();
    return 1;
  }
  if (opt_selftest) {
    selftest();
    return 0;
  }
  if (!opt_sink) { usage(); return 1; }
  if (!opt_confirm) {
    fprintf(stderr, "cwlat: refusing to key the radio without --confirm-safe (PA off, drive zero, operator present)\n");
    return 1;
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  struct sched_param sp = {.sched_priority = 40};
  if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
    fprintf(stderr, "cwlat: note: no real-time priority (sender timing may jitter by ~0.1 ms)\n");
  }
  double span = build_schedule();
  if (midi_open() < 0) { return 1; }
  if (capture_start(span + 3.0) < 0) {
    fprintf(stderr, "cwlat: cannot start capture of '%s'\n", opt_sink);
    return 1;
  }
  // wait for audio to flow
  double tw = now();
  while (!atomic_load(&cap_started) && now() - tw < 5.0) { usleep(10000); }
  if (!atomic_load(&cap_started)) {
    fprintf(stderr, "cwlat: no audio from the sink monitor within 5 s\n");
    release_all();
    capture_stop();
    return 1;
  }
  usleep(500000);
  fprintf(stderr, "cwlat: running %d trials (%.0f s)...\n", ntr, span);
  off_start = real_minus_mono();
  double start = now() + 0.2;
  for (int i = 0; i < nev && !stop_flag; i++) {
    double target = start + ev[i].t_plan;
    struct timespec ts;
    double early = target - 0.0003;
    ts.tv_sec = (time_t) early;
    ts.tv_nsec = (long)((early - (double) ts.tv_sec) * 1e9);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    while (now() < target && !stop_flag) { }
    ev[i].t_send = midi_send(ev[i].note, ev[i].on);
  }
  release_all();
  off_end = real_minus_mono();
  write_events();
  usleep(2000000);
  capture_stop();
  if (atomic_load(&cap_overflow)) { fprintf(stderr, "cwlat: warning: capture buffer overflow, run truncated\n"); }
  if (stop_flag) { fprintf(stderr, "cwlat: interrupted; analysing what was recorded\n"); }
  write_sinkwav();
  analyse();
  snd_seq_close(seq);
  return 0;
}
