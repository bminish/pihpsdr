/*
 * DEVELOPMENT TOOL. Not part of piHPSDR - see radev2_probe.h.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "radev2_probe.h"
#include "rade_v2_core.h"     /* rade_frame_sync(), exported by librade */

/* rade_rx_v2.c's detector, which is static there. */
#define V2_TSIG 0.38f
#define V2_TSIN 4.0f

int v2probe_open(struct v2probe *p, const char *name, int verbose, int agc) {
  memset(p, 0, sizeof(*p));
  p->name = name;
  int flags = RADE_MODE_V2 | RADE_USE_C_ENCODER | RADE_USE_C_DECODER;

  if (!verbose) { flags |= RADE_VERBOSE_0; }

  p->r = rade_open("", flags);

  if (p->r == NULL) {
    fprintf(stderr, "radev2: rade_open failed for %s\n", name);
    return 0;
  }

  rade_rx_set_agc(p->r, agc);
  p->cap = rade_nin_max(p->r) * 8;
  p->buf = malloc(sizeof(RADE_COMP) * (size_t)p->cap);
  p->features = malloc(sizeof(float) * (size_t)rade_n_features_in_out(p->r));
  p->symcap = 4096;
  p->sym = malloc(sizeof(struct v2sym) * (size_t)p->symcap);
  p->n_acq0 = p->r->rx_v2.n_acq;
  return p->buf != NULL && p->features != NULL && p->sym != NULL;
}

void v2probe_push(struct v2probe *p, float re, float im) {
  if (p->n >= p->cap) { v2probe_drain(p); }   /* leaves fewer than nin_max */

  p->buf[p->n].real = re;
  p->buf[p->n].imag = im;
  p->n++;
}

void v2probe_drain(struct v2probe *p) {
  for (;;) {
    const int nin = rade_nin(p->r);

    if (nin <= 0 || p->n < nin) { break; }

    int has_eoo = 0;
    const int got = rade_rx(p->r, p->features, &has_eoo, NULL, p->buf);
    const rade_rx_v2_state *rx = &p->r->rx_v2;
    p->consumed += nin;

    if (p->nsym >= p->symcap) {
      p->symcap *= 2;
      p->sym = realloc(p->sym, sizeof(struct v2sym) * (size_t)p->symcap);
    }

    struct v2sym *s = &p->sym[p->nsym++];
    s->t      = (double)p->consumed / RADE_MODEM_SAMPLE_RATE;
    s->sync   = (rx->state == RADE_RX_V2_SYNC);
    s->sig    = (rx->Ry_max > V2_TSIG) && (rx->Ry_max / (rx->Ry_min + 1e-12f) >= V2_TSIN);
    s->valid  = (got > 0);
    s->eoo    = has_eoo;
    s->ry_max = rx->Ry_max;
    s->snr    = rx->snr_est_dB;
    s->foff   = rx->freq_offset;
    s->data   = s->valid ? rade_rx_get_data_symbol(p->r) : 0.0f;
    /*
     * The receiver runs FrameSyncNet on every symbol's latents to choose
     * the frame parity, and keeps only a slow average of it. Run it again
     * on the latents that were just decoded: it is stateless, and it is
     * the receiver's own learned judgement of whether those latents look
     * like a RADE frame.
     */
    s->fsync  = s->valid ? rade_frame_sync(&rx->sync_model, rx->az_hat, 0) : 0.0f;

    if (has_eoo) { p->n_eoo++; }

    if (got > 0 && p->feat_out != NULL) {
      fwrite(p->features, sizeof(float), (size_t)got, p->feat_out);
    }

    if (p->csv_out != NULL) {
      fprintf(p->csv_out, "%.4f,%d,%d,%d,%d,%.4f,%.2f,%.2f,%.4f,%.4f\n",
              s->t, s->sync, s->sig, s->valid, s->eoo, s->ry_max, s->snr,
              s->foff, s->data, s->fsync);
    }

    memmove(p->buf, p->buf + nin, sizeof(RADE_COMP) * (size_t)(p->n - nin));
    p->n -= nin;
  }
}

int v2probe_acquisitions(const struct v2probe *p) {
  return p->r->rx_v2.n_acq - p->n_acq0;
}

static int cmp_float(const void *a, const void *b) {
  const float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

void v2probe_summary(const struct v2probe *p, struct v2summary *s) {
  memset(s, 0, sizeof(*s));
  s->nsym = p->nsym;
  s->seconds = (double)p->consumed / RADE_MODEM_SAMPLE_RATE;
  s->acq = v2probe_acquisitions(p);
  s->eoo = p->n_eoo;
  long nsync = 0, nsig = 0, nconf = 0;
  double ssig = 0.0, ssync = 0.0, sabs = 0.0, sfs = 0.0;
  float *dv = malloc(sizeof(float) * (size_t)(p->nsym + 1));

  for (long i = 0; i < p->nsym; i++) {
    const struct v2sym *y = &p->sym[i];

    if (y->sync) { nsync++; ssync += y->snr; }

    if (y->sig)  { nsig++;  ssig  += y->snr; }

    if (y->valid) {
      dv[s->frames] = fabsf(y->data);
      s->frames++;
      sabs += fabsf(y->data);
      sfs += y->fsync;

      if (fabsf(y->data) > 0.5f) { nconf++; }
    }
  }

  if (p->nsym > 0) {
    s->sync_frac = (double)nsync / p->nsym;
    s->sig_frac  = (double)nsig / p->nsym;
  }

  s->snr_sig   = nsig  ? ssig / nsig   : NAN;
  s->snr_sync  = nsync ? ssync / nsync : NAN;
  s->data_abs  = s->frames ? sabs / s->frames : NAN;
  s->data_conf = s->frames ? (double)nconf / s->frames : NAN;
  s->fsync     = s->frames ? sfs / s->frames : NAN;
  s->data_med  = NAN;

  if (s->frames > 0) {
    qsort(dv, (size_t)s->frames, sizeof(float), cmp_float);
    s->data_med = dv[s->frames / 2];
  }

  free(dv);
}

void v2probe_close(struct v2probe *p) {
  if (p->r != NULL) { rade_close(p->r); }

  free(p->buf);
  free(p->features);
  free(p->sym);
  memset(p, 0, sizeof(*p));
}
