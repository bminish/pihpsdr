# piHPSDR, `devtest/everything`: the "everything" branch

> **This is not a standard piHPSDR build.** It is the currently active development branch of the
> [bminish/pihpsdr](https://github.com/bminish/pihpsdr) fork of [dl1ycf/pihpsdr](https://github.com/dl1ycf/pihpsdr), carrying all
> our experimental work at once. It is rebased and rewritten at will (`devtest/` is not offered to anyone; see
> [docs/branches.md](docs/branches.md)). For something stable, use `TEST` (diversity) or the `feature/` and `fix/` branches listed there.

## What is in it

`TEST` (upstream's `TEST` branch plus our auto-diversity series) with, on top:

| Feature | What | Where it is written up |
|---|---|---|
| **Auto diversity** | The diversity engine (`diversity_auto.c`): references, weights, slew, noise floor, Best/Null/Sum. Most of it is already in upstream `TEST`. | [docs/changes.md](docs/changes.md) (the register and the way in), [docs/changes/accepted.md](docs/archive/diversity/changes/accepted.md), [docs/test-findings.md](docs/test-findings.md) |
| **Binaural diversity** | The ear split (a left and a right ear from the two antennas), the paired AGC, Balance ATT, the ear recorder. | [docs/binaural/README.md](docs/binaural/README.md) |
| **RADE V1 and V2** | Native FreeDV RADE receive and transmit in I/Q through `librade`, its own window, the Reporter and the log viewer; plus a RADE V1/V2 correlator reference for diversity (LC-071 to LC-076). | [docs/rade/README.md](docs/rade/README.md), [docs/diversity-radeV2-cp.md](docs/diversity-radeV2-cp.md), [docs/changes/lc-rade.md](docs/changes/lc-rade.md) |
| **CW fixes** | A local CW side tone synthesised in the PipeWire output callback (off by default, LC-064), two keyer fixes for lost taps (LC-065, LC-066), a CW filter drift fix, and the `cwlat` measurement tools. | [docs/cw.md](docs/cw.md) (index and open items), [docs/binaural/bug-cw-filter-edge-drift.md](docs/binaural/bug-cw-filter-edge-drift.md) |

Binaural and RADE interact: entering RADE switches the ear split off, and with RADE on the diversity reference follows the waveform.
The full branch list, what each is for and the number clashes between them: [docs/branches.md](docs/branches.md). All documents: [docs/README.md](docs/README.md).

## Build

Linux (Raspberry Pi or desktop). This branch has not been tried on MacOS. Install piHPSDR's own dependencies first, as in the
[piHPSDR Manual](#manual); the steps below add what this branch needs.

**Without RADE** this is an ordinary piHPSDR build (`make`), and nothing RADE is compiled. `RADE_NATIVE` is **off by default**, so for RADE you must do
steps 1 to 3 below. Without them you get a working piHPSDR with the diversity, binaural and CW work but no RADE.

1. **Tools for librade.** `git`, `cmake` (3.16 or newer), a C compiler, and network access (the script clones `github.com/freedv/rade_c`,
   and CMake downloads the Opus source it embeds). `librade` is **not part of this repository**.
2. **Fetch and build librade** (several minutes; it follows rade_c's HEAD, `RADE_REV=<commit|tag|branch>` pins a revision):
   ```
   tools/rade/build_rade.sh
   ```
   This leaves `third_party/rade_c-src` and `third_party/rade_c-build` (both untracked) and prints the librade commit it built
   (also in `third_party/rade_c-build/librade.rev`). Run it again to update librade, then `make clean` and rebuild piHPSDR.
   The RADE V2 comparison in `docs/diversity-radeV2-cp.md` was measured at rade_c `cc17222`;
   `RADE_REV=cc17222 tools/rade/build_rade.sh` reproduces that build.
3. **Build piHPSDR with RADE switched on**, either on the command line or once in `make.config.pihpsdr` (not tracked):
   ```
   make RADE_NATIVE=ON
   ```
   ```
   # make.config.pihpsdr
   RADE_NATIVE=ON
   ```
   If step 2 has not been done, `make` stops with "RADE_NATIVE=ON needs librade: run tools/rade/build_rade.sh first".
   After a librade update, or when switching `RADE_NATIVE` on or off, run `make clean` first.
4. **Check it is in.** The RADE button appears in the menu, and the binary links `librade`: `ldd ./pihpsdr | grep rade`.
5. **Optional.** The **Reporter** (button in the RADE window) is a separate Python program, `python3 tools/rade-reporter/rade_reporter.py`;
   it needs PyGObject (GTK 3) and `websocket-client`. The synthesised CW side tone is the "Synth sidetone" box in the CW menu and needs
   `AUDIO=PIPEWIRE` (the default on Linux).

RADE V2 is a development waveform and librade itself says on-air use is premature.

Other switches this branch has: `make DIVCAP=1` adds the diversity capture recorder and the ear recorder (ours, never going upstream).

## Manual

piHPSDR is compiled from the sources; consult the Manual on how to install the dependencies on your machine.

- v2.5: https://github.com/dl1ycf/pihpsdr/releases/download/v2.5/piHPSDR-Manual-v2.5.pdf
- v2.6: https://github.com/dl1ycf/pihpsdr/releases/download/v2.5/piHPSDR-Manual-v2.6.pdf
- v3.0: https://github.com/dl1ycf/pihpsdr/releases/download/v2.5/piHPSDR-Manual-v3.0.pdf
- current master, updated from time to time: https://github.com/dl1ycf/pihpsdr/releases/download/v2.5/piHPSDR-Manual.pdf

## piHPSDR itself

SDR host program supporting both the old (P1) and new (P2) HPSDR protocols, as well as the SoapySDR framework.
It runs on Linux (Raspberry Pi, desktop or laptop) and MacOS (Intel or Apple Silicon, using Homebrew or MacPorts).
Upstream features include the PipeWire audio module (the default on Linux), selectable colour themes, TCI with TCI-Audio,
DX cluster support, client/server operation with compression, CW audio peak filters, RX and TX profile slots, a manual multi-notch
filter, NR3/NR4 noise reduction, a configurable slider and toolbar area, CFC and DEXP in the TX chain, and audio recording and playback.
