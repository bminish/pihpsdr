# RADE log viewer

Standalone viewer for the parameter logs piHPSDR writes (`rade-logs/<run>/`, or the `<run>.tar.gz` made when a run stops).
Needs Python 3 with tkinter, numpy and matplotlib (TkAgg); no piHPSDR running.

    python3 tools/rade-logview/rade_logview.py [run-directory | run.tar.gz | folder-of-runs ...]

- **Files…** (select several at once) any files in run directories and/or `.tar.gz`; **Folder…** a run directory, or a folder of runs (every run directory in it, and every `.tar.gz` whose directory is not there). The same applies to the command line. Runs are drawn together (one colour each) on a UTC axis.
- **Live runs**: a run opened from a directory whose `meta.txt` has no `ended_utc` yet (the log is still being written) is marked ● in the list. **Update live (F5)** reloads the live runs whose files have changed (a line being written is ignored until it is complete), keeps the zoom, and, with **Follow the newest data** ticked, moves the right edge to the newest data at the same width. **every N s** does this automatically (1-60 s, off by default). A run that has ended stops being live. A `.tar.gz` is never live.
- **Parameters**: each ticked one gets its own panel (SNR mean and min, offset, the V2 statistics, TX loudness, peak, leveller gain, limiting, output fill). Breaks longer than 1.5 s are drawn as breaks. There are no legend boxes over the data: a panel with two lines names them in its axis label (solid mean / dotted min) and each run's colour is the colour of its entry in the run list.
- **Marks**: heard overs (blue shade) and transmitted overs (red shade), key down/up, end-of-over and calls, dial/mode/setting changes.
- **Zoom / pan**: the matplotlib toolbar (magnifier, cross, home/back). *Fit all* shows every run.
- **Measure**: tick it, click A then B: dt, dy and the mean/min/max of every plotted series between them go to the text area. Esc clears.
- **Overs list**: choose one to zoom to it; the text area then gives its static information - run, UTC times, dial, mode, waveform, Invert, the nominal energy centre (dial +/- 1500 Hz), the decoder's offset, SNR statistics, end-of-over, call.
  Choosing a run in the run list shows its `meta.txt` and a summary (dials heard, calls, event counts).

The old single-file `*_RADE.csv` logs are converted by `convert_old_logs.py` (end-of-over flag and calls were never recorded in them and stay blank). Test: `python3 test/rade_logview/test_logview.py` (window part is withdrawn, nothing appears).
