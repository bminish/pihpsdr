/*
 * midisend - send repeated MIDI notes to a (virtual) MIDI port through the ALSA sequencer, the way cwlat does
 * (a subscribed port and note events). For teaching a note to piHPSDR's MIDI menu ("learn") or a manual check.
 *
 *   midisend [--dest 32:0] [--note 23] [--chan 0] [--hold-ms 1000] [--gap-ms 500] [--count N] [--stop-file FILE] [--off]
 *
 * Sends Note On (velocity 100, as cwlat), waits hold-ms, Note Off, waits gap-ms; N times (0: until SIGINT/SIGTERM or the stop
 * file appears). Always ends with a Note Off. --off sends only the Note Off (to release a stuck key or PTT). Build: gcc -O2 -Wall -o midisend midisend.c -lasound
 */
#include <alsa/asoundlib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop = 0;
static void on_sig(int s) { (void) s; stop = 1; }

// The virtual MIDI port uses running status, which piHPSDR's parser does not understand: a Program Change in front of every
// note makes the note carry its own status byte (see cwlat.c, midi_send()).
static void send_note(snd_seq_t *seq, snd_seq_event_t *e, int chan, int note, int on) {
  snd_seq_ev_set_pgmchange(e, chan, 0);
  snd_seq_event_output_direct(seq, e);
  if (on) { snd_seq_ev_set_noteon(e, chan, note, 100); } else { snd_seq_ev_set_noteoff(e, chan, note, 0); }
  snd_seq_event_output_direct(seq, e);
}

static void msleep(int ms) {
  struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
  const char *dest = "32:0", *stopfile = NULL;
  int note = 23, chan = 0, hold = 1000, gap = 500, count = 0, off_only = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dest") && i + 1 < argc) { dest = argv[++i]; }
    else if (!strcmp(argv[i], "--note") && i + 1 < argc) { note = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--chan") && i + 1 < argc) { chan = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--hold-ms") && i + 1 < argc) { hold = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--gap-ms") && i + 1 < argc) { gap = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--count") && i + 1 < argc) { count = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--stop-file") && i + 1 < argc) { stopfile = argv[++i]; }
    else if (!strcmp(argv[i], "--off")) { off_only = 1; }
    else { fprintf(stderr, "usage: midisend [--dest C:P] [--note N] [--chan C] [--hold-ms MS] [--gap-ms MS] [--count N] [--stop-file FILE]\n"); return 2; }
  }
  snd_seq_t *seq;
  if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { fprintf(stderr, "midisend: cannot open the sequencer\n"); return 1; }
  snd_seq_set_client_name(seq, "midisend");
  int port = snd_seq_create_simple_port(seq, "out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_MIDI_GENERIC);
  snd_seq_addr_t a;
  if (port < 0 || snd_seq_parse_address(seq, &a, dest) < 0 || snd_seq_connect_to(seq, port, a.client, a.port) < 0) {
    fprintf(stderr, "midisend: cannot connect to %s\n", dest);
    return 1;
  }
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
  for (int n = 0; !off_only && !stop && (count == 0 || n < count); n++) {
    snd_seq_event_t e;
    snd_seq_ev_clear(&e); snd_seq_ev_set_source(&e, port); snd_seq_ev_set_subs(&e); snd_seq_ev_set_direct(&e);
    send_note(seq, &e, chan, note, 1);
    msleep(hold);
    send_note(seq, &e, chan, note, 0);
    msleep(gap);
    if (stopfile && access(stopfile, F_OK) == 0) { stop = 1; }
  }
  snd_seq_event_t e;
  snd_seq_ev_clear(&e); snd_seq_ev_set_source(&e, port); snd_seq_ev_set_subs(&e); snd_seq_ev_set_direct(&e);
  send_note(seq, &e, chan, note, 0);
  snd_seq_close(seq);
  return 0;
}
