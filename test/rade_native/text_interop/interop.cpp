// Interoperability of src/rade_eoo_text.c with freedv-gui's V1 end-of-over text
// (its rade_text.cpp and ldpc_*.cpp, built unmodified):
//   freedv-gui encode -> our decode, our encode -> freedv-gui decode,
// clean and with symbol noise. Build and run: make -C test/rade_native text_interop
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
extern "C" {
#include "rade_eoo_text.h"
}
#include "pipeline/rade_text.h"   // freedv-gui's

// freedv-gui's logger, not needed here
extern "C" void ulog_log(int, const char *, int, void *, const char *, ...) {}

static std::string got;
static void cb(rade_text_t, const char *t, int, void *) { got = t; }

static float gauss() {
  float u = (rand() + 1.0f) / (RAND_MAX + 2.0f), v = (rand() + 1.0f) / (RAND_MAX + 2.0f);
  return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v);
}

int main() {
  const char *calls[] = {"EI6IZ", "VK2DVZ", "W1AW/P", "KG5SWP", "A", "G4ABC/M", "OH2BCD"};
  const float sigmas[] = {0.0f, 0.25f, 0.45f};
  int bad = 0;
  rade_text_t rt = rade_text_create();
  rade_text_enable_stats_output(rt, 0);
  rade_text_set_rx_callback(rt, cb, nullptr);
  srand(1);
  for (float sg : sigmas) {
    int n = 0, ours_dec_theirs = 0, theirs_dec_ours = 0, tried = 0;
    for (const char *c : calls) {
      for (int rep = 0; rep < (sg == 0.0f ? 1 : 20); rep++) {
        float a[180], b[180];
        rade_text_generate_tx_string(rt, c, strlen(c), a, 180);   // theirs
        rade_eoo_text_encode(c, b, 180);                           // ours
        // identical on the wire?
        if (sg == 0.0f) {
          for (int i = 0; i < 180; i++) if (a[i] != b[i]) { printf("  encoder differs at %d for %s\n", i, c); bad++; break; }
        }
        for (int i = 0; i < 180; i++) { a[i] += sg * gauss(); b[i] += sg * gauss(); }
        char out[16];
        tried++;
        if (rade_eoo_text_decode(a, 90, out) && !strcmp(out, c)) theirs_dec_ours++;
        float b2[180]; memcpy(b2, b, sizeof b);
        got.clear();
        rade_text_rx(rt, b2, 90);
        if (got == c) ours_dec_theirs++;
        n++;
      }
    }
    printf("noise sigma %.2f: freedv-gui->ours %d/%d   ours->freedv-gui %d/%d\n", sg, theirs_dec_ours, tried, ours_dec_theirs, tried);
    if (sg == 0.0f && (theirs_dec_ours != tried || ours_dec_theirs != tried)) bad++;
  }
  printf(bad ? "FAIL\n" : "PASS\n");
  return bad != 0;
}
