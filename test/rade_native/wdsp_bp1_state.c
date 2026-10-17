/*
 * wdsp_bp1_state: is the second RX band-pass (bp1) running in DIGU/DIGL, the way piHPSDR sets the channel up?
 * Same call order as receiver.c (OpenChannel, SetRXABandpassRun(1), the noise calls, then SetRXAMode).
 *
 *   gcc -O1 -pthread -I../../wdsp -I../../libspecbleach/include -o wdsp_bp1_state wdsp_bp1_state.c \
 *       ../../wdsp/libwdsp.a ../../libspecbleach/libspecbleach.a `pkg-config --libs fftw3 fftw3f` -lm
 */
#include "comm.h"
int WDSPwisdom(char*); void SetRXABandpassRun(int,int); void SetRXAEMNRRun(int,int); void SetRXASBNRRun(int,int);
int main(void){
  WDSPwisdom("/home/bminish/sdr/bm-pihpsdr/");
  OpenChannel(0,1024,2048,48000,48000,48000,0,1,0.0,0.025,0.0,0.010,1);
  SetRXABandpassRun(0,1);                       // receiver.c:959
  printf("after open+SetRXABandpassRun: bp1.run=%d\n", rxa[0].bp1.p->run);
  SetRXAANRRun(0,0); SetRXAEMNRRun(0,0); SetRXANNRRun(0,0); SetRXASBNRRun(0,0); // rx_set_noise
  printf("after rx_set_noise-style Run(0) calls: bp1.run=%d\n", rxa[0].bp1.p->run);
  SetRXAANFRun(0,0);
  printf("after ANF run 0: bp1.run=%d\n", rxa[0].bp1.p->run);
  SetRXAMode(0,7);
  printf("after SetRXAMode(DIGU): bp1.run=%d nbp0.run=%d\n", rxa[0].bp1.p->run, rxa[0].nbp0.p->run);
  return 0; }
