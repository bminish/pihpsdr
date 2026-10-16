// Interoperability of src/rade_v2_text.c (the V2 callsign we transmit) with freedv-gui's own V2 text code
// (src/pipeline/rade_text.cpp of its backend, built unmodified; FREEDV_SRC is its src/ directory).
//
//   interop                       A, B, C, D below
//   interop --iq in8k.f32 CALL    decode a file of 8 kHz complex baseband (interleaved float32 I,Q) with librade V2 and
//                                 freedv-gui's text receiver; the call it delivers must be CALL
//
// A  the symbol stream equals freedv-gui's rade_text_tx_next_symbol(), bit for bit, for 3 cycles, for many callsigns
// B  freedv-gui's receiver, fed our symbols (clean and with noise), delivers the callsign, as often as with its own stream
// C  the same through the real V2 modem (librade): our symbols into rade_tx_set_data_symbol(), the modem signal (clean and
//    with noise) through rade_rx(), rade_rx_get_data_symbol() into freedv-gui's receiver
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <random>

extern "C" {
#include "rade_api.h"
#include "rade_text.h"
#include "rade_v2_text.h"
#include "util/logging/ulog.h"
}

static int fails = 0;
static void check(bool ok, const char *what, const std::string &info = "") {
  printf("  %-4s %s %s\n", ok ? "ok" : "FAIL", what, info.c_str());
  if (!ok) { fails++; }
}

struct Got { std::string text; int n = 0; };
static void on_text(rade_text_t, const char *txt, int len, void *st) {
  Got *g = (Got *)st;
  g->text.assign(txt, len);
  g->n++;
}

// what freedv-gui puts on the air: the characters it keeps
static std::string normalise(const std::string &in) {
  std::string out;
  for (char c : in) {
    if (out.size() >= 32) { break; }
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '/') { out += c; }
    else if (c >= 'a' && c <= 'z') { out += (char)(c - 'a' + 'A'); }
  }
  return out;
}

static const char *CALLS[] = {"K1ABC", "VK3XYZ", "W1AW/P", "EI6IZ", "EI6IZ/P", "VE3/KG6AOV/MM", "ABCDEFGH", "ABCDEFGHIJKLMNOP",
                              "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", "k1abc", "K1-ABC.", "////////", "A"};

static void part_a() {
  printf("A  symbol stream equals freedv-gui's\n");
  for (const char *c : CALLS) {
    rade_text_t old = rade_text_create();
    rade_text_generate_tx_string(old, c, (int)strlen(c));
    rade_v2_text_t t;
    int blocks = rade_v2_text_init(&t, c);
    int total = 3 * blocks * 112 + 17;
    int bad = 0;
    for (int i = 0; i < total; i++) {
      if (rade_text_tx_next_symbol(old) != rade_v2_text_next(&t)) { bad++; }
    }
    std::string label = std::string("'") + c + "' -> '" + t.text + "', " + std::to_string(blocks) + " block(s), " + std::to_string(total) + " symbols";
    check(bad == 0 && std::string(t.text) == normalise(c), label.c_str(), bad ? "(" + std::to_string(bad) + " differ)" : "");
    rade_text_destroy(old);
  }
  rade_v2_text_t t;
  check(rade_v2_text_init(&t, "") == 0 && rade_v2_text_next(&t) == -1.0f && rade_v2_text_next(&t) == -1.0f, "no callsign: constant -1.0, nothing to decode");
  check(rade_v2_text_init(&t, "-.-") == 0, "only characters that are dropped: nothing to send");
  rade_v2_text_init(&t, "EI6IZ");
  for (int i = 0; i < 50; i++) { rade_v2_text_next(&t); }
  rade_v2_text_restart(&t);
  rade_v2_text_t u; rade_v2_text_init(&u, "EI6IZ");
  check(rade_v2_text_next(&t) == rade_v2_text_next(&u), "restart goes back to the start");
}

// feed `n` symbols (each + noise) into a freedv-gui receiver; returns the callsign delivered ("" if none) and when
template <class Src> static std::string rx_stream(Src next, int n, double sigma, unsigned seed, int *at = nullptr) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> nd(0.0f, 1.0f);
  rade_text_t rx = rade_text_create();
  Got g;
  rade_text_set_rx_callback(rx, on_text, &g);
  std::string r;
  for (int i = 0; i < n; i++) {
    rade_text_rx_symbol(rx, next() + (float)sigma * nd(rng));
    if (g.n) { r = g.text; if (at) { *at = i; } break; }
  }
  rade_text_destroy(rx);
  return r;
}

static void part_b() {
  printf("B  freedv-gui's receiver, our symbols\n");
  for (const char *c : {"EI6IZ", "VE3/KG6AOV/MM", "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"}) {
    std::string want = normalise(c);
    rade_v2_text_t t; rade_v2_text_init(&t, c);
    int at = -1;
    std::string got = rx_stream([&] { return rade_v2_text_next(&t); }, 112 * 4 * 6, 0.0, 1, &at);
    check(got == want, (std::string("clean: '") + c + "' delivered").c_str(), "after " + std::to_string(at) + " symbols (" + std::to_string(at * 0.04) + " s)");
  }
  // with noise: ours against its own stream, same noise, many trials
  for (double sigma : {0.8, 1.0}) {
    int ours = 0, theirs = 0, trials = 40;
    for (int k = 0; k < trials; k++) {
      rade_v2_text_t t; rade_v2_text_init(&t, "EI6IZ");
      rade_text_t old = rade_text_create();
      rade_text_generate_tx_string(old, "EI6IZ", 5);
      ours += rx_stream([&] { return rade_v2_text_next(&t); }, 112 * 6, sigma, 100 + k) == "EI6IZ";
      theirs += rx_stream([&] { return rade_text_tx_next_symbol(old); }, 112 * 6, sigma, 100 + k) == "EI6IZ";
      rade_text_destroy(old);
    }
    char buf[120];
    snprintf(buf, sizeof buf, "noise sigma %.1f (Es/N0 %.1f dB): decoded %d/%d with ours, %d/%d with freedv-gui's own transmitter", sigma,
             -20 * log10(sigma), ours, trials, theirs, trials);
    check(ours == theirs && ours > 0, buf);
  }
}

// librade V2 modem frames carrying the data symbols, then back
static std::string modem_run(const char *call, double snr_db, unsigned seed, int frames, bool restart_mid = false) {
  rade_initialize();
  struct rade *tx = rade_open("", RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0 | RADE_MODE_V2);
  struct rade *rx = rade_open("", RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0 | RADE_MODE_V2);
  if (!tx || !rx) { return "(rade_open failed)"; }
  int nf = rade_n_features_in_out(tx), nout = rade_n_tx_out(tx), nin_max = rade_nin_max(rx);
  rade_v2_text_t t; rade_v2_text_init(&t, call);
  std::vector<float> feat(nf, 0.0f);
  std::vector<RADE_COMP> buf((size_t)frames * nout + 4000);
  size_t pos = 0;
  for (int f = 0; f < frames; f++) {
    if (restart_mid && f == 61) { rade_v2_text_restart(&t); }
    rade_tx_set_data_symbol(tx, rade_v2_text_next(&t));
    pos += rade_tx(tx, buf.data() + pos, feat.data());
  }
  double p = 0;
  for (size_t i = 0; i < pos; i++) { p += buf[i].real * buf[i].real + buf[i].imag * buf[i].imag; }
  p /= pos;
  double sig = snr_db > 90 ? 0.0 : sqrt(p * (8000.0 / 3000.0) / pow(10.0, snr_db / 10.0) / 2.0);
  std::mt19937 rng(seed);
  std::normal_distribution<float> nd(0.0f, 1.0f);
  for (size_t i = 0; i < pos && sig > 0; i++) { buf[i].real += (float)sig * nd(rng); buf[i].imag += (float)sig * nd(rng); }
  rade_text_t rt = rade_text_create();
  Got g; rade_text_set_rx_callback(rt, on_text, &g);
  std::vector<float> fo(nf);
  std::vector<RADE_COMP> in(nin_max);
  size_t rp = 0;
  int eoo, was_sync = 0;
  while (rp + rade_nin(rx) <= pos) {
    int nin = rade_nin(rx);
    memcpy(in.data(), buf.data() + rp, sizeof(RADE_COMP) * nin);
    rp += nin;
    int n = rade_rx(rx, fo.data(), &eoo, NULL, in.data());
    int sync = rade_sync(rx);
    if (was_sync && !sync) { rade_text_rx_reset(rt); }
    was_sync = sync;
    if (n > 0) { rade_text_rx_symbol(rt, rade_rx_get_data_symbol(rx)); }
  }
  std::string r = g.n ? g.text : "";
  rade_text_destroy(rt); rade_close(tx); rade_close(rx);
  return r;
}

static void part_c() {
  printf("C  through the V2 modem (librade), freedv-gui's text receiver\n");
  for (const char *c : {"EI6IZ", "VE3/KG6AOV/MM"}) {
    check(modem_run(c, 100, 1, 700) == normalise(c), (std::string("clean: '") + c + "' decoded").c_str());
  }
  int ok = 0, n = 6;
  for (int k = 0; k < n; k++) { ok += modem_run("EI6IZ", 6.0, 7 + k, 900) == "EI6IZ"; }
  char buf[100]; snprintf(buf, sizeof buf, "6 dB SNR (3 kHz): decoded %d/%d runs of 36 s", ok, n);
  check(ok >= n - 1, buf);
  check(modem_run("EI6IZ", 100, 1, 700, true) == "EI6IZ", "a restart of the stream in mid over is decoded");
}


// D  our receiver (rade_v2_text_rx_*) against freedv-gui's, symbol by symbol on the same input: delivered at the same
//    symbol with the same text, over many calls, offsets into the cycle, noise levels, resets and pure noise
static void part_d() {
  printf("D  receiver: ours against freedv-gui's on identical symbols\n");
  int runs = 0, same = 0, delivered = 0;
  std::string firstdiff;
  const char *calls[] = {"EI6IZ", "W1AW/P", "VE3/KG6AOV/MM", "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456", "K1ABC", ""};
  for (const char *c : calls) {
    for (double sigma : {0.0, 0.6, 0.9, 1.1}) {
      for (int skip : {0, 37, 111, 200}) {
        for (int reset_at : {-1, 300}) {
          rade_v2_text_t t; rade_v2_text_init(&t, c);
          for (int i = 0; i < skip; i++) { rade_v2_text_next(&t); }
          std::mt19937 rng(1000 * runs + 7);
          std::normal_distribution<float> nd(0.0f, 1.0f);
          rade_text_t th = rade_text_create();
          Got g; rade_text_set_rx_callback(th, on_text, &g);
          rade_v2_text_rx_t us; rade_v2_text_rx_init(&us);
          bool ok = true;
          int n_th = 0;
          for (int i = 0; i < 112 * 8 && ok; i++) {
            if (i == reset_at) { rade_text_rx_reset(th); rade_v2_text_rx_reset(&us); }
            float sym = rade_v2_text_next(&t) + (float)sigma * nd(rng);
            int before = g.n;
            rade_text_rx_symbol(th, sym);
            char msg[RADE_V2_TEXT_MAXCHARS + 1];
            int got = rade_v2_text_rx_symbol(&us, sym, msg);
            int theirs = g.n != before;
            if (got != theirs || (got && g.text != msg)) {
              ok = false;
              char b[200]; snprintf(b, sizeof b, "call '%s' sigma %.1f skip %d reset %d symbol %d: ours %d '%s' theirs %d '%s'", c, sigma, skip, reset_at, i, got, got ? msg : "", theirs, theirs ? g.text.c_str() : "");
              if (firstdiff.empty()) { firstdiff = b; }
            }
            n_th += theirs;
          }
          delivered += n_th > 0;
          same += ok; runs++;
          rade_text_destroy(th);
        }
      }
    }
  }
  char b[160]; snprintf(b, sizeof b, "%d runs identical to freedv-gui's receiver of %d (%d of them delivered a call)", same, runs, delivered);
  check(same == runs, b, firstdiff);
  int nd_ok = 1;
  { rade_text_t th = rade_text_create(); Got g; rade_text_set_rx_callback(th, on_text, &g);
    rade_v2_text_rx_t us; rade_v2_text_rx_init(&us);
    std::mt19937 rng(5); std::normal_distribution<float> nd(0.0f, 1.0f);
    for (int i = 0; i < 112 * 200; i++) {
      float sym = nd(rng); char msg[40];
      int b4 = g.n; rade_text_rx_symbol(th, sym);
      if ((rade_v2_text_rx_symbol(&us, sym, msg) != 0) != (g.n != b4)) { nd_ok = 0; }
    }
    check(nd_ok, "900 s of pure noise: same (no) deliveries as freedv-gui's"); rade_text_destroy(th); }
}

static int decode_file(const char *path, const char *want) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return 2; }
  fseek(f, 0, SEEK_END); long n = ftell(f) / 8; fseek(f, 0, SEEK_SET);
  std::vector<RADE_COMP> buf(n);
  std::vector<float> raw(2 * n);
  if (fread(raw.data(), 4, 2 * n, f) != (size_t)(2 * n)) { return 2; }
  fclose(f);
  for (long i = 0; i < n; i++) { buf[i].real = raw[2 * i]; buf[i].imag = raw[2 * i + 1]; }
  rade_initialize();
  struct rade *rx = rade_open("", RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0 | RADE_MODE_V2);
  int nf = rade_n_features_in_out(rx), nin_max = rade_nin_max(rx);
  std::vector<float> fo(nf); std::vector<RADE_COMP> in(nin_max);
  rade_text_t rt = rade_text_create();
  Got g; rade_text_set_rx_callback(rt, on_text, &g);
  long rp = 0; int eoo, was = 0, syncs = 0, syms = 0;
  while (rp + rade_nin(rx) <= n) {
    int nin = rade_nin(rx);
    memcpy(in.data(), buf.data() + rp, sizeof(RADE_COMP) * nin); rp += nin;
    int k = rade_rx(rx, fo.data(), &eoo, NULL, in.data());
    int sync = rade_sync(rx);
    if (was && !sync) { rade_text_rx_reset(rt); }
    was = sync; syncs += sync;
    if (k > 0) { rade_text_rx_symbol(rt, rade_rx_get_data_symbol(rx)); syms++; }
  }
  printf("%s: %d symbols, delivered '%s' (%d times), wanted '%s'\n", path, syms, g.text.c_str(), g.n, want);
  return g.n && g.text == want ? 0 : 1;
}

int main(int argc, char **argv) {
  ulog_set_quiet(true);                    // the receiver logs every block it decodes
  if (argc == 4 && !strcmp(argv[1], "--iq")) { return decode_file(argv[2], argv[3]); }
  part_a(); part_b(); part_c(); part_d();
  printf(fails ? "FAILED %d\n" : "all passed\n", fails);
  return fails ? 1 : 0;
}
