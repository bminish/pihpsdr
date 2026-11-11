# piHPSDR - `feature/rade-native`

This branch of piHPSDR adds **FreeDV RADE V1 and V2** (digital voice over HF SSB) inside the program: receive and
transmit, a RADE window for waveform, callsign and levels, a separate Reporter, and diversity reception for RADE V2.
It is experimental and **not a standard piHPSDR build**. RADE V2 is a development waveform, and librade itself says
on-air use is premature.

RADE needs David Rowe's **librade** ([freedv/rade_c](https://github.com/freedv/rade_c)). It is not part of this
repository; a script fetches and builds it.

**Get and compile** (Linux; not tried on MacOS)

1. Install piHPSDR's usual dependencies (see the Manual), plus `git`, `cmake` (3.16 or newer) and a C compiler. The script
   clones rade_c and CMake downloads the Opus source it embeds, so it needs network access.
2. Build librade (several minutes):
   ```
   tools/rade/build_rade.sh
   ```
3. Build piHPSDR. RADE is on by default on this branch:
   ```
   make
   ```
   `make RADE_NATIVE=OFF` builds without RADE. If librade has not been built, `make` stops and tells you to run step 2.

**Update**

```
git pull
tools/rade/build_rade.sh      # fetches rade_c's latest and rebuilds librade
make clean
make
```

The script follows rade_c's latest commit and prints the one it built (also in `third_party/rade_c-build/librade.rev`).
`RADE_REV=<commit|tag|branch> tools/rade/build_rade.sh` builds a particular revision instead.

**Reporter (optional)**: opened from the RADE window, a separate program (`tools/rade-reporter/rade_reporter.py`) that
needs PyGObject (GTK 3) and `websocket-client`.

More: [docs/rade/README.md](docs/rade/README.md).

***

**SDR host program**,
supporting both the old (P1) and new (P2) HPSDR protocols, as well as the SoapySDR framework.
It runs on Linux (Raspberry Pi but also Desktop or Laptop computers running LINUX) and MacOS (Intel or AppleSilicon CPUs, using  "Homebrew" or "MacPorts").

**piHPSDR Manual (PDF file, about 300 pages) for release versions:**

**v2.5:** https://github.com/dl1ycf/pihpsdr/releases/download/v2.5/piHPSDR-Manual-v2.5.pdf

**v2.6:** https://github.com/dl1ycf/pihpsdr/releases/download/v2.5/piHPSDR-Manual-v2.6.pdf

**v3.0:** https://github.com/dl1ycf/pihpsdr/releases/download/v2.5/piHPSDR-Manual-v3.0.pdf

**A manual for the current master branch is updated from time to time here:**

https://github.com/dl1ycf/pihpsdr/releases/download/v2.5/piHPSDR-Manual.pdf

***
piHPSDR should be compiled from the sources, consult the Manual (**link given above**) on how to compile/install piHPSDR on your machine
***

Latest features:

- new PipeWire audio module for LINUX (now the default)
- Selectable color themes
- more complete TCI implementation including TCI-Audio
- DX cluster support
- Reduced client/server bandwidth due to data compression
- different selectable CW audio peak filters
- storing/restoring RX and TX profiles in individual (user-selectable) slots
- manual multi notch filter (in the FILTER menu)
- NR3/NR4 noise reduction models (RNNnoise and libspecbleach) fully integrated
- client/server model for remote operation (including transmitting in phone and CW)
- fully configurable Slider and Toolbar area
- added continuous frequency compressor (**CFC**) and downward expander (**DEXP**) to the TX chain
- audio recording (RX capture) and playback (TX)



