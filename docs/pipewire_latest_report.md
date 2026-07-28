# Engineering Report: Clean PipeWire Backend Port for Latest pihpsdr

This document records the design, conflict resolutions, and verification of merging the native PipeWire audio backend (`feature/pipewire-native`) into the latest vanilla version of `pihpsdr` (at master commit `60c63428`).

---

## 1. Project Context & Objectives

- **Vanilla Latest Baseline**: Upstream `pihpsdr` master branch containing updates up to commit `60c63428` (including the major WDSP 2.00 release).
- **PipeWire Backend Branch**: `feature/pipewire-native` containing a native, low-latency, single-stream mixed PipeWire playback backend (`src/pipewire.c`) that replaces the legacy PulseAudio simple API wrapper.
- **Merge Goal**: Combine both cleanly so that the PipeWire backend compiles and runs successfully on top of the latest upstream version, preserving low-latency manual CW keying and internal sidetone mixing.

---

## 2. Merge Conflict Resolution in `src/transmitter.c`

The major update to WDSP 2.00 on the master branch introduced conflicts in `src/transmitter.c`. These were resolved as follows:

### Conflict 1: Sidetone Playout Condition in `client_sidetone_thread`
- **Master Branch**:
  ```c
  if (radio_is_transmitting() && (txmode == modeCWL || txmode == modeCWU)) {
  ```
- **PipeWire Branch**:
  ```c
  if ((radio_is_transmitting() || (!cw_breakin && (keydown || cw_ring_inpt != cw_ring_outpt))) && (txmode == modeCWL || txmode == modeCWU)) {
  ```
- **Resolution**: Kept the **PipeWire Branch** logic. This allows the sidetone thread to process and play sidetone samples immediately when the key is pressed (keydown) or while sidetone samples remain in the ring buffer, even if the radio has not transitioned to active RF transmission yet (when break-in is disabled).

### Conflict 2: Writing Sidetones to Local Audio in `client_sidetone_thread`
- **Master Branch**:
  ```c
  if (active_receiver->local_audio && !duplex) {
  ```
- **PipeWire Branch**:
  ```c
  if (active_receiver->local_audio) {
  ```
- **Resolution**: Kept the **PipeWire Branch** logic. In the single-stream mixed design, sidetone is mixed internally with receiver audio in the process callback. Therefore, the sidetone must be written to the local audio buffer unconditionally, even in duplex mode.

### Conflict 3: Playout Active Flag in `tx_add_mic_sample`
- **Master Branch**:
  ```c
  if (!duplex) { did_tx_audio = 1; }
  ```
- **PipeWire Branch**:
  ```c
  did_tx_audio = 1;
  ```
- **Resolution**: Kept the **PipeWire Branch** logic. We unconditionally set the playout active flag (`did_tx_audio = 1`) to ensure transmitter sidetone playout is processed during duplex or zero-beating.

---

## 3. Verification & Compilation

Compilation was successfully verified on the merged codebase:
1.  **Configuration**: Configured compile options via `make.config.pihpsdr`:
    ```makefile
    GPIO=OFF
    AUDIO=PIPEWIRE
    ```
    *(GPIO was disabled to prevent compilation failures on systems lacking Raspberry Pi-specific development libraries).*
2.  **Build Execution**:
    ```bash
    make AUDIO=PIPEWIRE clean
    make AUDIO=PIPEWIRE -j$(nproc)
    ```
    The build completed with no errors, producing the `pihpsdr` binary.

---

## 4. Key Warning: `bm-pihpsdr` Regression

During our initial repository analysis, we inspected the local `/home/bminish/sdr/bm-pihpsdr` repository and found that the merge commit `2c4e608a` **completely deleted `src/transmitter.c`** from the project's source tree. Since the `Makefile` still compiles `src/transmitter.c`, running a clean build in `bm-pihpsdr` will fail.

### Recommended Recovery Action
To fix `bm-pihpsdr`, do one of the following:
- Copy the cleanly merged version of `src/transmitter.c` we generated:
  ```bash
  cp docs/pipewire_latest.patch /path/to/bm-pihpsdr/
  # or copy the file directly:
  cp src/transmitter.c /home/bminish/sdr/bm-pihpsdr/src/transmitter.c
  ```
- Restoring it from git using:
  ```bash
  git checkout 60c634285e7f7ffc99ce67cbf0caaa79c25f6c53 -- src/transmitter.c
  ```
  And then re-applying the conflict resolutions described above.
