/*
 * rade_native_tx_live(): what the Reporter is told is a RADE transmission. TUNE, two-tone and noise reach the engine as
 * transmitting = 0 (transmitter.c); a TUNE's flag clears a moment before the key does, so a block or two of "keyed, not
 * tune" at either edge must never count.
 */
#include <stdio.h>
#include <string.h>
#include "rade_native.h"

static int fails = 0;
static void check(int ok, const char *what) { printf("  %-4s %s\n", ok ? "ok" : "FAIL", what); if (!ok) fails++; }
static void blocks(int n, int transmitting) {
  double buf[2048];
  for (int i = 0; i < n; i++) { memset(buf, 0, sizeof buf); rade_native_tx_block(buf, 1024, 0, transmitting); }
}

int main(void) {
  rade_native_configure(RADE_NATIVE_V2, 0);
  rade_native_tx_enable(1);
  check(!rade_native_tx_live(), "idle: not live");
  blocks(RADE_LIVE_SAMPLES / 1024 - 1, 1);
  check(!rade_native_tx_live(), "keyed for less than 170 ms: not live");
  blocks(1, 1);
  check(rade_native_tx_live(), "keyed for 170 ms: live");
  blocks(30, 1);
  check(rade_native_tx_live(), "and stays live through the over");
  blocks(1, 0);
  check(!rade_native_tx_live(), "unkeyed (or TUNE, two-tone, noise: the caller passes 0): not live at once");
  // the end of a TUNE: the tune flag clears while the radio is still keyed, for a few blocks
  blocks(40, 0);
  blocks(3, 1);
  check(!rade_native_tx_live(), "TUNE ending (3 blocks of keyed-but-not-tune): not live");
  blocks(1, 0);
  // a TUNE then an over
  blocks(RADE_LIVE_SAMPLES / 1024, 1);
  check(rade_native_tx_live(), "an over after it: live");
  rade_native_tx_enable(0);
  check(!rade_native_tx_live(), "engine off: not live");
  rade_native_configure(0, 0);
  printf(fails ? "FAILED %d\n" : "all passed\n", fails);
  return fails ? 1 : 0;
}
