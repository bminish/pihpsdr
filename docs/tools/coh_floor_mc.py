# Monte Carlo check of the Min coherence noise floor (LC-012, docs/changes.md).
# Two independent white noises through the 4-term Blackman-Harris FFT and the
# exponential average; counts how often they pass a gate set at the floor.
# Run: python3 docs/tools/coh_floor_mc.py   (numpy; a few minutes)

import numpy as np
rng=np.random.default_rng(1)
rho=[1.0,0.8160,0.4386,0.1500,0.0304]
def floor_n(nb,nblk,pfa=1e-3):
    den=nb+sum(2*(nb-k)*rho[k]**2 for k in range(1,5) if k<nb)
    n=max(2.0,nb*nb/den*nblk); return min(0.5,1-pfa**(1/(n-1)))
def floor_naive(nb,nblk,pfa=1e-3):   # feature-branch formula (independent bins), same PFA
    n=max(2.0,nb*nblk); return min(0.5,1-pfa**(1/(n-1)))
N=512; x=2*np.pi*np.arange(N)/N
w=0.35875-0.48829*np.cos(x)+0.14128*np.cos(2*x)-0.01168*np.cos(3*x)
def run(nb,alpha,blocks=60000,warm=None):
    lo=100; sl=slice(lo,lo+nb)
    xy=np.zeros(nb,complex); xx=np.zeros(nb); yy=np.zeros(nb)
    w1=w2=0.0; passes_new=passes_old=cnt=0; first=True
    for b in range(blocks):
        a=1.0 if first else alpha; first=False
        X=np.fft.fft(w*(rng.standard_normal(N)+1j*rng.standard_normal(N)))[sl]
        Y=np.fft.fft(w*(rng.standard_normal(N)+1j*rng.standard_normal(N)))[sl]
        xy+=a*(X*np.conj(Y)-xy); xx+=a*(abs(X)**2-xx); yy+=a*(abs(Y)**2-yy)
        w1=(1-a)*w1+a; w2=(1-a)**2*w2+a*a
        coh=abs(xy.sum())**2/(xx.sum()*yy.sum())
        nblk=w1*w1/w2
        passes_new+= coh>=floor_n(nb,nblk); passes_old+= coh>=floor_naive(nb,nblk); cnt+=1
        if b%500==499: first=True   # periodic reset: exercises the start-up blocks too
    return passes_new/cnt, passes_old/cnt
for nb,alpha,label in [(5,0.041,'Carrier 5 bins, 2 s @12 Hz'),(5,0.34,'Carrier 5 bins, 0.2 s'),
                       (17,0.34,'Digital 200 Hz occupied, 0.2 s'),(200,0.041,'Window 2.4 kHz, 2 s'),
                       (200,0.34,'Window 2.4 kHz, 0.2 s')]:
    n_new,n_old=run(nb,alpha,blocks=20000 if nb<50 else 6000)
    ss=(2-alpha)/alpha
    print(f"{label:34s} floor={100*floor_n(nb,ss):6.2f}%  noise pass rate: new {100*n_new:6.3f}%   independent-bin formula {100*n_old:6.3f}%")
