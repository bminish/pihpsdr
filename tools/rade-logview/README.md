# RADE log viewer

Standalone viewer for the parameter logs piHPSDR writes (`rade-logs/<run>/`, or the `<run>.tar.gz` made when a run stops).
Needs Python 3 with tkinter, numpy and matplotlib (TkAgg); no piHPSDR running.

    python3 tools/rade-logview/rade_logview.py [run-directory | run.tar.gz ...]

- **Open…** any file in a run directory, or a `.tar.gz`; several runs can be loaded and are drawn together (one colour each) on a UTC axis.
- **Parameters**: each ticked one gets its own panel (SNR mean and min, offset, the V2 statistics, TX loudness, peak, leveller gain, limiting, output fill). Breaks longer than 1.5 s are drawn as breaks.
- **Marks**: heard overs (blue shade) and transmitted overs (red shade), key down/up, end-of-over and calls, dial/mode/setting changes.
- **Zoom / pan**: the matplotlib toolbar (magnifier, cross, home/back). *Fit all* shows every run.
- **Measure**: tick it, click A then B: dt, dy and the mean/min/max of every plotted series between them go to the text area. Esc clears.
- **Overs list**: choose one to zoom to it; the text area then gives its static information - run, UTC times, dial, mode, waveform, Invert, the nominal energy centre (dial +/- 1500 Hz), the decoder's offset, SNR statistics, end-of-over, call.
  Choosing a run in the run list shows its `meta.txt` and a summary (dials heard, calls, event counts).

The old single-file `*_RADE.csv` logs are converted by `convert_old_logs.py` (end-of-over flag and calls were never recorded in them and stay blank). Test: `python3 test/rade_logview/test_logview.py` (window part is withdrawn, nothing appears).
