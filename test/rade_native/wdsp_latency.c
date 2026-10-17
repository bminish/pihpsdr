/*
 * wdsp_latency: delay and CPU cost of one WDSP channel as a function of dsp_size, the way piHPSDR opens it
 * (in_size 1024, 2048-tap filters, linear phase, DIGU, 1000..2000 Hz; TX: dsp 96 kHz, out 192 kHz, CFIR, ALC).
 *
 *   gcc -O2 -pthread -o wdsp_latency wdsp_latency.c ../../wdsp/libwdsp.a ../../libspecbleach/libspecbleach.a \
 *       `pkg-config --libs fftw3 fftw3f` -lm
 *   ./wdsp_latency rx|tx(0|1) DSP_SIZE BP1(0|1) [x]     -> peak delay of a 1500 Hz Hann burst; with a 4th argument, CPU per second
 *
 * The delay is where the envelope of the burst peaks, so it includes the channel's input and output buffering.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
int WDSPwisdom(char*);
void OpenChannel(int,int,int,int,int,int,int,int,double,double,double,double,int);
int SetChannelState(int,int,int);
void fexchange0(int,double*,double*,int*);
void SetRXAMode(int,int); void RXASetNC(int,int); void RXASetMP(int,int); void RXASetPassband(int,double,double);
void SetRXAAGCFixed(int,double); void SetRXAAGCMode(int,int); void SetRXAPanelRun(int,int); void SetRXAPanelSelect(int,int);
void SetRXAPanelBinaural(int,int); void SetRXABandpassRun(int,int); void SetRXAPanelGain1(int,double);
void SetTXAMode(int,int); void TXASetNC(int,int); void TXASetMP(int,int); void SetTXABandpassWindow(int,int);
void SetTXABandpassFreqs(int,double,double); void SetTXACFIRRun(int,int); void SetTXAPanelSelect(int,int); void SetTXAALCSt(int,int);
void SetTXAPanelRun(int,int); void SetTXAPanelGain1(int,double);
#define IN 1024
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+1e-9*t.tv_nsec;}
int main(int argc,char**argv){
  int tx=atoi(argv[1]), dsp=atoi(argv[2]), bp1=argc>3?atoi(argv[3]):0;
  WDSPwisdom("/home/bminish/sdr/bm-pihpsdr/");
  int ch=tx?1:0, outrate=tx?192000:48000, oscale=outrate/48000;
  int nblk=argc>4?2400:120; int imp=40*IN;
  double *in=calloc(2*IN,8), *out=calloc(2*IN*oscale,8);
  if(!tx){
    OpenChannel(0,IN,dsp,48000,48000,48000,0,1,0.0,0.025,0.0,0.010,1);
    SetRXAMode(0,7); RXASetNC(0,2048); RXASetMP(0,0); SetRXABandpassRun(0,bp1);
    RXASetPassband(0,1000,2000); SetRXAPanelRun(0,1); SetRXAPanelSelect(0,3); SetRXAPanelBinaural(0,1);
    SetRXAAGCFixed(0,0.0); SetRXAAGCMode(0,0);
  } else {
    OpenChannel(1,IN,dsp,48000,96000,192000,1,0,0.0,0.025,0.0,0.010,1);
    TXASetMP(1,0); TXASetNC(1,2048); SetTXABandpassWindow(1,1); SetTXAMode(1,7);
    SetTXABandpassFreqs(1,1000,2000); SetTXACFIRRun(1,1); SetTXAPanelRun(1,1); SetTXAPanelSelect(1,3); SetTXAPanelGain1(1,1.0); SetTXAALCSt(1,1);
    SetChannelState(1,1,0);
  }
  double best=0; long bi=-1; srand(1);
  double t0=now();
  for(int b=0;b<nblk;b++){
    for(int i=0;i<IN;i++){ long n=(long)b*IN+i; double v=0;
      if(argc>4){ v=0.05*((rand()/(double)RAND_MAX)-0.5); in[2*i]=v; in[2*i+1]=v*0.3; }
      else { double d=(double)(n-imp)/(0.1*48000.0); double w=fabs(d)<1?0.5*(1+cos(M_PI*d)):0; double ph=2*M_PI*1500.0*n/48000.0; in[2*i]=0.3*w*cos(ph); in[2*i+1]=-0.3*w*sin(ph); } }
    int err=0; if(getenv("DBG")){double mi=0;for(int i=0;i<2*IN;i++)if(fabs(in[2*i<2*IN?i:0])>mi)mi=fabs(in[i]);printf("blk %d in-max %g\n",b,mi);} fexchange0(ch,in,out,&err); if(b==nblk-1&&getenv("DBG")){double s=0;for(int i=0;i<IN*oscale;i++)s+=out[2*i]*out[2*i];printf("err=%d last-block energy %g\n",err,s);}
    if(getenv("DBG")){double mo=0;for(int i=0;i<2*IN*oscale;i++)if(fabs(out[i])>mo)mo=fabs(out[i]);printf("blk %d out-max %g\n",b,mo);} if(argc<=4) for(int i=0;i<IN*oscale;i++){ double m=hypot(out[2*i],out[2*i+1]);  if(m>best){best=m;bi=(long)b*IN*oscale+i;} }
  }
  double dt=now()-t0;
  if(argc<=4) printf("%s dsp_size=%4d bp1=%d: peak delay %.1f ms (peak %.3f)\n", tx?"TX":"RX", dsp, bp1, (bi-(double)imp*oscale)/(outrate/1000.0), best);
  else printf("%s dsp_size=%4d bp1=%d: %.3f s of CPU per 1 s of signal\n", tx?"TX":"RX", dsp, bp1, dt/(nblk*IN/48000.0));
  return 0; }
