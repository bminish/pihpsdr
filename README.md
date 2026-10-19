<p align="center">
<img src="release/pihpsdr/piHPSDRv3_logo.png" width="300" height="300">
</p>

> # ⚠️ This is the `feature/rade-native` branch: it is **not a standard piHPSDR build**
>
> It adds native **FreeDV RADE V1 and V2** (receive and transmit, in I/Q, with its own RADE window and Reporter). That needs
> David Rowe's `librade`, which is **not part of this repository**: it has to be fetched and built first, and the normal
> build instructions in the Manual do not cover it. Without the steps below you get an ordinary piHPSDR with no RADE.
>
> **Build** (Linux; this branch has not been tried on MacOS):
>
> 1. Install what librade's build needs, as well as piHPSDR's own dependencies: `git`, `cmake` (3.16 or newer), a C compiler, and
>    network access (the script clones `github.com/freedv/rade_c` and CMake downloads the Opus source it embeds).
> 2. Fetch and build librade (several minutes; it follows rade_c's HEAD, `RADE_REV=<commit|tag|branch>` pins a revision):
>    ```
>    tools/rade/build_rade.sh
>    ```
>    This leaves `third_party/rade_c-src` and `third_party/rade_c-build` (both untracked). Run it again to update librade.
> 3. Build piHPSDR with RADE switched on, either on the command line or once in `make.config.pihpsdr`:
>    ```
>    make RADE_NATIVE=ON
>    ```
>    ```
>    # make.config.pihpsdr
>    RADE_NATIVE=ON
>    ```
>    `RADE_NATIVE` is off by default; with it off, nothing RADE is compiled. With it on, `make` stops with
>    "RADE_NATIVE=ON needs librade: run tools/rade/build_rade.sh first" if step 2 has not been done.
> 4. Optional: the **Reporter** (button in the RADE window) is a separate Python program,
>    `python3 tools/rade-reporter/rade_reporter.py`, and needs PyGObject (GTK 3) and `websocket-client`.
>
> What it does, the design and the test status: [docs/rade/README.md](docs/rade/README.md). RADE V2 is a development
> waveform and librade itself says on-air use is premature.

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



