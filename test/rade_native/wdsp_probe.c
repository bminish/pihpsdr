/*
 * wdsp_probe: push complex baseband through one vendored-WDSP channel
 * the way piHPSDR sets it up, and write what comes out.
 *
 *   wdsp_probe rx|tx [options] in.iq.f32 out.iq.f32
 *     --low HZ --high HZ   channel bandpass edges (negative = lower sideband)
 *     --wisdom DIR/        directory holding wdspWisdom01 (default ../../)
 *     --copy N             RX panel copy (0 keeps I and Q separate)
 *     --agc N              RX AGC mode (0 off with --fixed dB; 1 long 2 slow 3 med 4 fast; default 3)
 *     --top DB             AGC top (max) gain, default 80
 *     --neutral            TX: switch the speech stages off
 *     --alc 0|1            TX: ALC state (default 1)
 *     --gain DB            TX: panel gain
 *     --dsp HZ --out HZ    TX: WDSP rates (48000 each by default; piHPSDR P2: 96000 and 192000)
 *     --speech-stages      TX: the leveler, CFC and phase rotator as one SSB profile of ours sets them (see source)
 *     --cfir               TX: run the CFIR (P2 firmware compensation), as piHPSDR does in P2
 *
 * Files are interleaved float32 I,Q at 48 kHz. Input is padded with zeros
 * to a whole number of buffers; output has the same length.
 * Channel parameters follow receiver.c / transmitter.c; this is a probe,
 * not a copy of the engine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wdsp.h"

#define BUF 1024
#define RXA_DIGU_MODE 7
#define TXA_DIGU_MODE 7

int main(int argc, char **argv) {
  int nobp = 0, tx = 0, copy = 0, agc = -1, neutral = 0, alc = 1, i = 2;
  char *wis = "../../";
  int speech = 0;
  int dsp_rate = 48000, out_rate = 48000, cfir = 0;
  double top = 80.0, fixed = 0.0, low = 300, high = 3000, gain_db = 0;
  if (argc < 4) { fprintf(stderr, "usage: wdsp_probe rx|tx [opts] in out\n"); return 1; }
  tx = !strcmp(argv[1], "tx");
  for (; i < argc - 2; i++) {
    if (!strcmp(argv[i], "--wisdom")) wis = argv[++i];
    else if (!strcmp(argv[i], "--low")) low = atof(argv[++i]);
    else if (!strcmp(argv[i], "--high")) high = atof(argv[++i]);
    else if (!strcmp(argv[i], "--copy")) copy = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--nobp")) nobp = 1;
    else if (!strcmp(argv[i], "--dsp")) dsp_rate = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--out")) out_rate = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--cfir")) cfir = 1;
    else if (!strcmp(argv[i], "--speech-stages")) speech = 1;
    else if (!strcmp(argv[i], "--speech-stages-eq")) speech = 2;
    else if (!strcmp(argv[i], "--top")) top = atof(argv[++i]);
    else if (!strcmp(argv[i], "--fixed")) fixed = atof(argv[++i]);
    else if (!strcmp(argv[i], "--agc")) agc = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--neutral")) neutral = 1;
    else if (!strcmp(argv[i], "--alc")) alc = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--gain")) gain_db = atof(argv[++i]);
    else { fprintf(stderr, "bad option %s\n", argv[i]); return 1; }
  }
  FILE *fi = fopen(argv[argc - 2], "rb"), *fo = fopen(argv[argc - 1], "wb");
  if (!fi || !fo) { perror("open"); return 1; }
  fseek(fi, 0, SEEK_END); long n = ftell(fi) / 8; fseek(fi, 0, SEEK_SET);
  float *in = calloc((n / BUF + 1) * BUF * 2, sizeof(float));
  if (fread(in, 8, n, fi) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
  long nb = (n + BUF - 1) / BUF;

  WDSPwisdom(wis);   // re-uses wdspWisdom01 in that directory (trailing /)
  int ch = 0;
  if (!tx) {
    OpenChannel(ch, BUF, 2 * BUF, 48000, 48000, 48000, 0, 1, 0.0, 0.025, 0.0, 0.010, 1);
    SetRXAMode(ch, RXA_DIGU_MODE);
    SetRXABandpassRun(ch, nobp ? 0 : 1);
    RXASetPassband(ch, low, high);
    SetRXAPanelRun(ch, 1);
    SetRXAPanelSelect(ch, 3);
    SetRXAPanelCopy(ch, copy);
    // AGC as rx_set_agc() does it: 0 off (fixed gain --fixed dB), else a
    // timing set (1 long, 2 slow, 3 medium, 4 fast) with top gain --top dB
    if (agc == 0) { SetRXAAGCFixed(ch, fixed); SetRXAAGCMode(ch, 0); }
    else {
      static const int hang[] = {0, 2000, 1000, 0, 0}, decay[] = {0, 2000, 500, 250, 50};
      int a = agc < 0 ? 3 : agc;
      SetRXAAGCAttack(ch, 2); SetRXAAGCHang(ch, hang[a]); SetRXAAGCDecay(ch, decay[a]);
      SetRXAAGCHangThreshold(ch, 0); SetRXAAGCSlope(ch, 35);
      SetRXAAGCTop(ch, top); SetRXAAGCMode(ch, 5);
    }
  } else {
    OpenChannel(ch, BUF, 2 * BUF, 48000, dsp_rate, out_rate, 1, 0, 0.0, 0.025, 0.0, 0.010, 1);
    TXASetMP(ch, 0);
    SetTXABandpassWindow(ch, 1);
    SetTXABandpassRun(ch, nobp ? 0 : 1);
    SetTXAAMSQRun(ch, 0);
    SetTXAALCAttack(ch, 1);
    SetTXAALCDecay(ch, 10);
    SetTXAALCSt(ch, alc);
    SetTXAPreGenMode(ch, 0);
    SetTXAPreGenToneMag(ch, 0.0);
    SetTXAPreGenToneFreq(ch, 0.0);
    SetTXAPreGenRun(ch, 0);
    SetTXAPostGenRun(ch, 0);
    SetTXACFIRRun(ch, cfir);
    SetTXAPanelRun(ch, 1);
    SetTXAPanelSelect(ch, 3);
    SetTXAPanelGain1(ch, pow(10.0, gain_db / 20.0));
    if (neutral) {
      SetTXAEQRun(ch, 0);
      SetTXALevelerSt(ch, 0);
      SetTXACFCOMPRun(ch, 0);
      SetTXACompressorRun(ch, 0);
      SetTXAPHROTRun(ch, 0);
    }
    if (speech) {
      // what piHPSDR's SSB profile of this station applied through tx_set_compressor() / tx_set_phrot():
      // the leveler and the CFC with this profile (bands 1..10; band 0 is the pre-compression), and the phase rotator
      static double F[10] = {50, 100, 200, 500, 1000, 1500, 2000, 2500, 3000, 5000};
      static double G[10] = {6, 6, 6, 4, 0, 0, 2, 2, 0, 0};
      static double E[10] = {-6, -12, -11, -11, 0, 0, 2, 1, 0, 0};
      SetTXALevelerSt(ch, 1); SetTXALevelerAttack(ch, 1); SetTXALevelerDecay(ch, 500); SetTXALevelerTop(ch, 8.0);
      SetTXACFCOMPprofile(ch, 10, F, G, E);
      SetTXACFCOMPPrecomp(ch, 0.0); SetTXACFCOMPPrePeq(ch, 0.0);
      SetTXACFCOMPRun(ch, 1); SetTXACFCOMPPeqRun(ch, speech == 2 ? 1 : 0);
      SetTXAPHROTCorner(ch, 600.0); SetTXAPHROTNstages(ch, 2); SetTXAPHROTReverse(ch, 0); SetTXAPHROTRun(ch, 1);
    }
    SetTXAMode(ch, TXA_DIGU_MODE);
    SetTXABandpassFreqs(ch, low, high);
    SetChannelState(ch, 1, 0);
  }

  double *din = malloc(sizeof(double) * 2 * BUF), *dout = malloc(sizeof(double) * 2 * BUF * 4);
  for (long b = 0; b < nb; b++) {
    for (int k = 0; k < 2 * BUF; k++) din[k] = in[b * 2 * BUF + k];
    memset(dout, 0, sizeof(double) * 2 * BUF * 4);
    int err = 0;
    fexchange0(ch, din, dout, &err);
    if (err) fprintf(stderr, "fexchange0 block %ld err %d\n", b, err);
    int nout = tx ? BUF * (out_rate / 48000) : BUF;
    float *of = malloc(sizeof(float) * 2 * nout);
    for (int k = 0; k < 2 * nout; k++) of[k] = dout[k];
    fwrite(of, sizeof(float), 2 * nout, fo);
    free(of);
  }
  fclose(fo);
  CloseChannel(ch);
  return 0;
}
