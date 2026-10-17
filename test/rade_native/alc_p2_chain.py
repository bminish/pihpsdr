"""Where the ~1 dB between the ALC ceiling and the P2 192 kHz output comes from: the modem through TXA at level 0.357 with ALC on,
stage by stage (48 kHz, 96 kHz, 96->192 kHz, +CFIR)."""
import sys, numpy as np
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t11b_chain as T, filter_lab as L
from scipy.signal import resample_poly
x8, f = L.modem("v1", 40)
x48 = np.conj(resample_poly(x8, 6, 1)) * 0.357
for label, extra in (("48k->48k", []), ("96k dsp -> 96k out", ["--dsp","96000","--out","96000"]), ("96k dsp -> 192k out, no CFIR", ["--dsp","96000","--out","192000"]), ("96k -> 192k + CFIR", ["--dsp","96000","--out","192000","--cfir"])):
    for alc in (1,):
        y = T.probe("tx", x48.astype(np.complex64), ["--low","675","--high","2275","--neutral","--alc",str(alc)] + extra)
        act = np.abs(y) > 1e-4
        print("%-32s alc=%d peak %.3f (%.2f dBFS) rms %.3f" % (label, alc, np.abs(y).max(), 20*np.log10(np.abs(y).max()), np.sqrt(np.mean(np.abs(y[act])**2))))
