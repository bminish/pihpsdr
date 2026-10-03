# TCI Support in pihpsdr / deskHPSDR

This document describes the TCI (Transceiver Control Interface) server implemented
in this repository, and compares it command-by-command against the published
reference at:

> https://pure-editions.com/on7off/TCI-Remote/TCI_Protocol_Reference.html

It exists so that an agent (or developer) working on TCI in this codebase has a
single accurate reference for what is implemented, where, and how it diverges
from "standard" TCI as used by clients such as WSJT-X, MLDX, TCI:remote (ON7OFF),
logbook programs, etc.

Last reviewed against the code: 2026-10-03 (branch `feature/diversity-binaural-2`).
Everything below is from reading the source; behaviour marked *(code reading)* has
not been exercised against a client. Line numbers are deliberately avoided —
search for the function name.

Source files:
- [`src/tci.c`](../src/tci.c) — LWS WebSocket server, command parsing and dispatch, state reporting, CW macro handling.
- [`src/tci.h`](../src/tci.h) — public globals (`tci_enable`, `tci_port`, `tci_audio_rx_active`, `tci_audio_tx_active`) and entry points (`launch_tci`, `shutdown_tci`, `tci_send_chrono_frame`, `tci_audio_wakeup`).
- [`src/tci_audio.c`](../src/tci_audio.c) / [`src/tci_audio.h`](../src/tci_audio.h) — binary stream header/struct, RX and TX audio ring buffers, TX pre-buffering, chrono pacing.
- [`src/receiver.c`](../src/receiver.c) — RX audio tap (`tci_audio_rx_sample`), including the diversity ear-split onto stream 0.
- [`src/transmitter.c`](../src/transmitter.c) / [`src/client_thread.c`](../src/client_thread.c) — TX mic source override (`tci_get_next_mic_sample`) and the client/server equivalents.
- [`src/rigctl_menu.c`](../src/rigctl_menu.c) — GUI checkbox/spinbutton for enabling TCI and setting the port.
- [`src/rx_menu.c`](../src/rx_menu.c) — per-receiver "TCI volume" (`rx->tci_volume`).
- [`src/radio.c`](../src/radio.c) — `tci_enable` / `tci_port` persisted to/from the props file; TCI launched at start-up if enabled, shut down on exit.

Build: compiled only with `TCI=ON` in the Makefile (the default), which adds
`-DTCI`, `src/tci.o`, `src/tci_audio.o` and links libwebsockets.

---

## 1. Overview

| | This implementation | Published reference |
|---|---|---|
| Transport | WebSocket via libwebsockets | WebSocket |
| Default port | `tci_port = 40001` (`src/tci.c`) | 50001 |
| Subprotocols advertised | `"chat"`, `"superchat"`, `"tci"` (8192-byte RX buffer each) | not specified |
| Max simultaneous clients | `TCI_MAX_CLIENTS = 8` | not specified |
| Identity sent | `protocol:ExpertSDR3,2.0;` / `device:SunSDR2QRP;` | `protocol:2;` / `device:<name>;` |
| IQ streaming | Not implemented (no `iq_start`/`iq_stop` handlers, no IQ data path) | `iq_start`/`iq_stop`, frame type 0 |
| Audio streaming | RX and TX audio, 48 kHz, float32, 1024 samples per frame, stereo or mono RX | 48 kHz float32 stereo |

Enable/configure TCI via the CAT/TCI GUI menu (`rigctl_menu.c`), which toggles
`tci_enable` and sets `tci_port`, then calls `launch_tci()` / `shutdown_tci()`.
Changing the port while enabled restarts the server. If `launch_tci()` fails,
`tci_enable` is reset to 0. Settings persist via `GetPropI0`/`SetPropI0` in
`radio.c`.

`launch_tci()` refuses to start (returns -1) unless `sizeof(TCI_STREAM_HEADER) == 64`
and `sizeof(TCI_STREAM) == 16448`, which guarantees the audio payload starts at
byte 64 and is float-aligned.

---

## 2. Architecture

- **`CLIENT` struct** — one slot per connection (`tciclient[TCI_MAX_CLIENTS]`).
  Holds last-reported VFO-A/B frequency and mode, TX frequency, split and MOX
  (used to suppress redundant reports), TX-owner flag, last `trx` state, sensor
  opt-ins, per-client audio parameters (`audio_sample_rate`, `audio_channels`,
  `audio_sample_type`, `audio_samples`), per-receiver RX-audio flags,
  TX-audio flag, the LWS output queue, and the binary reassembly buffer.
- **Threads.**
  - A `GThread` (`tci_lws_server`) loops `lws_service()`. Before each service
    call it pulls RX audio from the rings (`tci_service_rx_audio`) and, if
    anything was queued, arms `lws_callback_on_writable` for clients with
    pending output. `lws_cancel_service()` wakes it whenever a frame is queued
    or the RX audio producer has accumulated 1024 samples (`tci_audio_wakeup`).
  - Incoming **text** frames are copied (truncated to 511 bytes) into a
    `PAYLOAD` and handed to the GTK main thread with `g_idle_add(tci_process_ws_payload)`;
    all command handlers therefore run on the GTK thread.
  - Incoming **binary** frames are reassembled and handled directly on the LWS thread.
  - `tci_reporter()` runs on the GTK main loop every 500 ms per client.
- **Command parsing** — `tci_parse_text` cuts the message at the **first** `;`,
  splits the command name at `:` and the arguments at `,` (up to
  `TCI_MAX_ARGS = 16`). Command names are lower-cased; arguments are not.
  The name is looked up in the static `tci_dispatch[]` table, which enforces
  min/max argument counts (`-1` = unlimited) before calling the handler.
  Unknown commands are ignored (logged only with `rigctl_debug`).
  - *(code reading)* Only the first command in a WebSocket text frame is
    processed. A client that packs several commands into one frame
    (`vfo:0,0,7074000;modulation:0,usb;`) loses everything after the first `;`.
- **Periodic state push** — `tci_reporter()` (500 ms) sends, only when changed
  since the last report: `tx_frequency`, `split_enable`, `trx`, `vfo` + `dds`
  (VFO-A; VFO-B), `modulation` + `rx_filter_band`. Every second (odd ticks),
  if the client opted in, it sends `rx_channel_sensors` + `rx_sensors`
  (RX sensors) and `tx_sensors` (TX sensors). Every 30 ticks (15 s) it queues
  a WebSocket PING control frame.
  - Nothing else is pushed when changed from the GUI or another source:
    lock, RIT/XIT, drive, tune, squelch, DSP toggles, volume, mute and AGC
    reach a client only in the init block, in reply to its own query/set, or
    when another TCI client sets them (where the handler broadcasts — see §4).
- **Output backpressure** — `tci_queue_frame` drops new **text** frames and
  `tci_queue_binary_frame` drops new **binary** frames once a client's
  `idle_queued` reaches 100. PING/PONG frames are not subject to the limit.
- **Binary reassembly** — `tci_handle_binary_lws` always collects fragments into
  `client->binary_rx_buf` (first allocation 8256 bytes, grown in 8192-byte
  steps) up to `TCI_BINARY_REASSEMBLY_MAX = 65536`; beyond that the partial
  frame is discarded. Because the buffer comes from `g_realloc`, the assembled
  frame is always suitably aligned for `float` access; there is no separate
  alignment check any more.
- **Exported but unused** — `tci_rx_filter_band_changed()` is non-static but has
  no caller and no prototype in `tci.h`. `tci_tx_chrono_timer_id` is never set
  (only cleared in `shutdown_tci`). `MAXDATASIZE` is unused.

---

## 3. Connection lifecycle / handshake

On `LWS_CALLBACK_ESTABLISHED`, `tci_init_client()` claims a free slot, sets
audio defaults (48000 Hz, 2 channels, float32, 1024 samples), increments
`cat_control`, starts the 500 ms reporter and requests a writable callback.
If all 8 slots are taken the connection is accepted at the WebSocket level
but gets no slot, so it is never served.

On the first `LWS_CALLBACK_SERVER_WRITEABLE`, `tci_send_initial_state()` queues
the init block, in this order:

```
protocol:ExpertSDR3,2.0;
device:SunSDR2QRP;
receive_only:<true if no transmitter>;
trx_count:1;
channels_count:2;
vfo_limits:<radio->frequency_min>,<radio->frequency_max>;
if_limits:<-RX0 sample_rate/2>,<+RX0 sample_rate/2>;
modulations_list:LSB,USB,DSB,CW,FMN,AM,DIGU,SPEC,DIGL,SAM,DRM;
dds:0,<VFO-A freq>;
if:0,0,0;  if:0,1,0;
vfo:0,0,<VFO-A freq>;  vfo:0,1,<VFO-B freq>;
modulation:0,<mode>;
rx_filter_band:0,<lo>,<hi>;
[if receivers > 1:
 dds:1,<VFO-B freq>;  if:1,0,0;  if:1,1,0;
 vfo:1,0,<VFO-B freq>;  vfo:1,1,<VFO-B freq>;
 modulation:1,<mode>;  rx_filter_band:1,<lo>,<hi>;]
rx_enable:0,true;  [rx_enable:1,true; if receivers > 1]
lock:0,<bool>;  vfo_lock:0,0,<bool>;  vfo_lock:0,1,<bool>;
sql_enable:0,<bool>;  sql_level:0,<dB>;
rx_anf_enable:0,<bool>;  rx_apf_enable:0,<bool>;
rx_nb_enable:0,<bool>;   rx_nf_enable:0,true;
rx_bin_enable:0,<bool>;  rx_nr_enable:0,<bool>;
rit_enable:0,<bool>;     rit_offset:0,<hz>;
xit_enable:0,<bool>;     xit_offset:0,<hz>;
tune_drive:0,<pct>;
tx_enable:0,<bool>;
split_enable:0,<bool>;
trx:0,<bool>;
tune:0,<bool>;
mute:<bool>;
rx_mute:0,<bool>;
volume:<dB>;
rx_volume:0,0,<dB>;  rx_volume:0,1,<dB>;
agc_gain:0,<dB>;
agc_mode:0,<off|fast|normal>;
[if receivers > 1: sql_enable … agc_mode for receiver 1, plus rit_enable/rit_offset:1]
cw_macros_speed:<wpm>;
cw_macros_delay:<ms>;
cw_keyer_speed:<wpm>;
audio_stream_sample_type: float32;
audio_stream_channels: 2
audio_stream_samples: 1024
audio_samplerate:48000;
tx_stream_audio_buffering:50;
ready;
start;
```

Notes:
- Each line is queued as its own WebSocket text frame.
- *(code reading)* Three of the audio lines are malformed: `audio_stream_sample_type: float32;`
  has a space after the colon, and `audio_stream_channels: 2` / `audio_stream_samples: 1024`
  have the space **and no terminating `;`**. A client that concatenates frames
  and splits on `;` will see them run into the next command.
- `vfo:0,1` and `vfo:1,*` all report VFO-B (see `vfo` in §4).
- RIT/XIT offsets are reported as `0` while RIT/XIT is disabled.

**Differences from the reference init block:**
- No `iq_samplerate:` line (no IQ subsystem).
- No `tx_profiles_ex:` line — no TX-profile concept.
- `ready` is sent with a `;` (the reference sends it without), followed by `start;`.
  The reference defines `start` as a separate "radio hardware running" notification, so sending it here is consistent.
- `protocol:ExpertSDR3,2.0;` / `device:SunSDR2QRP;` impersonate an Expert
  Electronics radio, deliberately, so that clients built for SunSDR accept the server.
- The reference expects `audio_samplerate` before `ready`; here it comes after a
  number of state lines, also before `ready`.

**Disconnect.** Socket close (`LWS_CALLBACK_CLOSED`) on a client that is still
`running`: removes any pending MOX-clear timer, drops MOX (via
`ext_radio_set_mox(0)`) if this client's last `trx` was `true`, releases TX
ownership, stops the reporter, frees the queue and reassembly buffer, recomputes
the global audio flags and decrements `cat_control`.

*(code reading)* That cleanup is skipped if `running` was already 0 when the
close arrives — the callback returns early for non-running slots. `running` is
cleared beforehand by `stop` (§4) and by a failed `lws_write`. Consequences:
`cat_control` is not decremented, the queue and reassembly buffer leak until
the slot is reused, and after a write failure while transmitting, TX ownership
is released but **MOX is not dropped**.

---

## 4. Full command reference (as implemented)

Command names are matched case-insensitively. Booleans: `tci_bool` accepts
`true` (any case) or anything starting with `1` as true; everything else is false.
"Args" is the `(min,max)` count enforced by `tci_dispatch[]`.

"Reply" describes what the server sends on a **set**:
*broadcast* = new value sent to all running clients; *echo* = new value sent to
the requesting client only; *none* = no direct reply (the reporter may pick
up the change within 500 ms where noted). A **get** (value argument omitted)
always replies to the requesting client only.

### VFO / frequency

| Command | Args | Handler | Notes |
|---|---|---|---|
| `vfo` | 2,3 | `tci_cmd_vfo` | `vfo:<rx>,<ch>[,<hz>];` `vfo:0,0` is VFO-A; **every other combination** (`0,1`, `1,0`, `1,1`) is VFO-B, so logbooks using either RX0/ch1 or RX1/ch0 as "the other VFO" both work. Reported frequency is the CTUN frequency when CTUN is on. Get for `rx=1` with one receiver returns nothing. Set: broadcast `vfo`. |
| `dds` | 1,2 | `tci_cmd_dds` | `dds:<rx>[,<hz>];` Treated as a synonym for VFO-A (rx 0) / VFO-B (rx 1); reports the VFO (or CTUN) frequency, **not** the panadapter centre. Set: broadcast `dds`. |
| `if` | — | (send only) | Always `if:<rx>,<ch>,0;` in the init block; no IF-offset model. Not in the dispatch table. |
| `lock` | 1,2 | `tci_cmd_lock` | `lock:<rx>[,<bool>];` Maps to the single global `locked`. If the value changed: broadcast `lock:0` and `vfo_lock:0,0`/`0,1` to all; otherwise echo. |
| `vfo_lock` | 1,3 | `tci_cmd_vfo_lock` | `vfo_lock:<rx>,<ch>,<bool>;` or `vfo_lock:<rx>,<bool>;` — same global `locked`; channel is accepted but not independent. |
| `split_enable` | 1,2 | `tci_cmd_split_enable` | Only `trx=0`. Reports true if the TX VFO is VFO-B. Set calls `radio_set_split()`; reply: none (reporter pushes within 500 ms). |
| `trx_count` | 0,0 | `tci_cmd_trx_count` | Always `trx_count:1;`. |
| `tx_frequency` | — | (send only) | `tx_frequency:<hz>;` pushed by the reporter on change (`vfo_get_tx_freq`). Not in the reference. |

### Modulation / filters

| Command | Args | Handler | Notes |
|---|---|---|---|
| `modulation` | 1,2 | `tci_cmd_modulation` → `tci_set_mode` | Accepted: `lsb,usb,dsb,cw,cwl,cwu,fmn,fm,am,digu,spec,digl,sam,drm`. `cw` sets CWU. Reported as `LSB,USB,DSB,CW,FM,AM,DIGU,SPEC,DIGL,SAM,DRM` — CWL/CWU both report `CW`, FM reports `FM` although `modulations_list` says `FMN`. Unknown mode: current mode echoed. Set: broadcast `modulation` + `rx_filter_band`. |
| `rx_filter_band` | 1,3 | `tci_cmd_rx_filter_band` | Get reports the current mode's `filters[mode][filter]` edges. Set (`<rx>,<lo>,<hi>`) calls `filter_edges_changed()` and broadcasts the requested edges. |
| `digl_offset` / `digu_offset` | 0,1 | stubs | Always reply `digl_offset:0;` / `digu_offset:0;`. Not in the reference. |

### RIT / XIT

| Command | Args | Handler | Notes |
|---|---|---|---|
| `rit_enable` | 1,2 | `tci_cmd_rit_enable` | Per receiver (`vfo_id_rit_onoff`). Set reply: none. |
| `rit_offset` | 1,2 | `tci_cmd_rit_offset` | Hz, per receiver (`vfo_id_rit_value`). Set: echo. Get reports 0 while RIT is off. |
| `xit_enable` | 1,2 | `tci_cmd_xit_enable` | Only `trx=0`; applies to the current TX VFO (`vfo_xit_onoff`). Set reply: none. |
| `xit_offset` | 1,2 | `tci_cmd_xit_offset` | Only `trx=0` (`vfo_xit_value`). Set: echo. Get reports 0 while XIT is off. |

### DSP toggles (receiver_id 0 or 1, must exist)

| Command | Args | Handler | Notes |
|---|---|---|---|
| `rx_anf_enable` | 1,2 | `tci_cmd_rx_anf_enable` | `receiver->anf` + `rx_set_noise()`. Set reply: none. |
| `rx_apf_enable` | 1,2 | `tci_cmd_rx_apf_enable` | Writes `vfo[].cwAudioPeakFilter`; the handler does not call anything to apply it. Get reports `false` unless the mode is CWL/CWU. Set reply: none. |
| `rx_nb_enable` | 1,2 | `tci_cmd_rx_nb_enable` | **Set** writes `receiver->nb` (0/1 = off / NB "ANB") + `rx_set_noise()`. **Get** reports `receiver->snb` (the *spectral* noise blanker) — a different control, so a set is not reflected by a subsequent get. Get also reports `false` in DIGL/DIGU. Set reply: none. |
| `rx_nf_enable` | 1,2 | `tci_cmd_rx_nf_enable` | No state behind it. Set echoes the requested value; get always reports `true`. |
| `rx_bin_enable` | 1,2 | `tci_cmd_rx_bin_enable` | Writes `receiver->binaural` and echoes. The handler does not call `rx_set_af_binaural()`, so *(code reading)* WDSP is not updated until something else applies it. |
| `rx_nr_enable` | 1,2 | `tci_cmd_rx_nr_enable` | True sets `receiver->nr = 4` (NR4, a non-standard WDSP extension); false sets 0; then `rx_set_noise()`. Get reports `false` unless the mode is USB/LSB/CWU/CWL/AM/SAM (`tci_rx_nr_default_for_mode`). Set reply: none. |

### AGC / squelch

| Command | Args | Handler | Notes |
|---|---|---|---|
| `agc_gain` | 1,2 | `tci_cmd_agc_gain` | dB, clamped to [-20, 120], `radio_set_agc_gain()`. Set: broadcast. |
| `agc_mode` | 1,2 | `tci_cmd_agc_mode` | `off`/`fast`/`normal` ↔ `AGC_OFF`/`AGC_FAST`/`AGC_MEDIUM`; any other AGC setting (e.g. slow, long) reports `normal`. Unknown string ignored. Set: broadcast. |
| `sql_enable` | 1,2 | `tci_cmd_sql_enable` | `radio_set_squelch_enable()`. Set: echo. |
| `sql_level` | 1,2 | `tci_cmd_sql_level` | TCI dB ↔ pihpsdr slider 0…100: `dB = slider × 1.4 − 140`, clamped to [-140, 0]; inverse `slider = (dB + 140) × 0.71429`. Set applies via `radio_set_squelch()` and then replies with the read-back level (echo). |

### PTT / TX control

| Command | Args | Handler | Notes |
|---|---|---|---|
| `trx` | 0,∞ | `tci_cmd_trx` | `trx:0,<bool>[,tci];` PTT, single-owner (§6). `trx;` or `trx:0;` queries. With `tci` as the third arg, the client's TCI TX audio is enabled and the server sends `audio_samplerate:48000;`, `audio_stream_sample_type:float32;`, `audio_stream_channels:2;`, `audio_stream_samples:1024;`, `tx_stream_audio_buffering:50;`, `audio_start:0;` before keying. Owner's set: `radio_set_mox(1)` and echo `trx`. Release is deferred by a timer: 50 ms if TCI TX audio was on, else 0 ms; the timer drops MOX, clears TX audio, echoes `trx` and resets the TX ring. |
| `tune` | 0,∞ | `tci_cmd_tune` | `tune:0,<bool>;` Same ownership rule as `trx`. Owner's set calls `radio_set_tune()` with no reply; tune state is not pushed by the reporter (MOX is). Non-owner gets `tune` + `trx` echoed. |
| `drive` | 0,2 | `tci_cmd_drive` | `drive:0,<0-100>;` `radio_set_drive()`. Set: broadcast. `drive;` queries. |
| `tune_drive` | 1,2 | `tci_cmd_tune_drive` | Sets `transmitter->tune_drive` **and clears `tune_use_drive`**. Get reports the live drive when `tune_use_drive` is set. Set: broadcast. |
| `tx_enable` | — | (send only) | `tx_enable:0,<true if a transmitter exists>;` in the init block. |
| `rx_smeter` | 1,3 | `tci_cmd_rx_smeter` | Query only. **Replies `rx_sensors:<rx>,<dBm>.0;`**, not `rx_smeter`. In the reference `rx_smeter` is server→client. |
| `rx_sensors_enable` | 1,2 | `tci_cmd_rx_sensors_enable` | `rx_sensors_enable:<bool>;` — the **first** argument is taken as the bool. The reference form `rx_sensors_enable:0,true;` therefore reads `0` and *(code reading)* disables. When on: every second, `rx_channel_sensors:<rx>,0,<dBm>.0;` and `rx_channel_sensors:<rx>,1,<dBm>.0;` (same value) for rx 0 and 1, then `rx_sensors:<rx>,<dBm>.0;` for each receiver. dBm is integer-truncated and formatted with a literal `.0` to avoid locale commas. |
| `tx_sensors_enable` | 1,2 | `tci_cmd_tx_sensors_enable` | First argument is the bool (matches the reference's `tx_sensors_enable:true;`). When on: every second, only while transmitting and `fwd > 0.01`, `tx_sensors:0,<mic>,<fwd>,<fwd>,<swr>;` — mic is `micpeak`, and both power fields carry **forward power** (the reference's 4th field is reverse power). One decimal, locale-independent. |

### Audio control

| Command | Args | Handler | Notes |
|---|---|---|---|
| `mute` | 0,1 | `tci_cmd_mute` | `mute:<bool>;` sets `active_receiver->mute_radio`. Set: broadcast. The reference form is `mute:<trx>`. |
| `rx_mute` | 1,2 | `tci_cmd_rx_mute` | Get reports `receiver[rx]->mute_radio`. *(code reading)* Set **only broadcasts** the requested state; it never writes `mute_radio`, so it has no effect on audio. |
| `volume` | 0,1 | `tci_cmd_volume` | dB, clamped [-40, 0]. Get reports `active_receiver->volume`; **set always applies to RX0** (`radio_set_af_gain(0, …)`). Set: echo. |
| `rx_volume` | 2,3 | `tci_cmd_rx_volume` | `rx_volume:<rx>,<ch>,<dB>;` clamped [-40, 0], `radio_set_af_gain(rx)`. Channel 0/1 accepted and echoed; both map to the same AF gain. Set: echo. |
| `mon_volume` / `mon_enable` | 0,1 | stubs | Always reply `mon_volume:-60;` / `mon_enable:false;`. |
| `audio_samplerate` | 1,1 | `tci_cmd_audio_samplerate` | Stores the client's rate and echoes it. Any value other than 48000 makes later `audio_start` silently fail. |
| `audio_stream_sample_type` | 1,1 | `tci_cmd_audio_stream_sample_type` | Substring match: `float32`→3, `int32`→2, `int24`→1, `int16`→0 (unrecognised keeps the previous value). No reply. Only float32 lets `audio_start` succeed. |
| `audio_stream_channels` | 1,1 | `tci_cmd_audio_stream_channels` | `1` → mono RX audio (left channel only); anything else → 2. No reply. Affects RX only; TX is always parsed as stereo. |
| `audio_stream_samples` | 1,1 | `tci_cmd_audio_stream_samples` | Stores the value and replies **`audio_samples:<n>;`** (not `audio_stream_samples`). Any value other than 1024 makes later `audio_start` silently fail. |
| `audio_start` | 1,1 | `tci_cmd_audio_start` | `audio_start:<rx>;` Enables RX audio for that receiver and replies `audio_start:<rx>;` (WSJT-X and TCI:remote expect it). Silently refused if the client's rate ≠ 48000, samples ≠ 1024 or type ≠ float32. |
| `audio_stop` | 1,1 | `tci_cmd_audio_stop` | Disables RX audio for that receiver and replies **`audio_off:<rx>;`**. Ignored if not started. |
| `tx_stream_audio_buffering` | — | (send only) | Server sends `tx_stream_audio_buffering:50;` in the init block and on `trx:…,tci`. Not in the dispatch table — a client sending it is ignored. |

### IQ

| Command | Args | Handler | Notes |
|---|---|---|---|
| `iq_samplerate` | 1,1 | `tci_cmd_iq_samplerate` | Echoes the rate if it is 48000/96000/192000/384000, otherwise no reply. Changes nothing. |
| `iq_start` / `iq_stop` | — | — | Not in the dispatch table. No IQ data path exists. |

### Spotting

`spot`, `spot_delete`, `spot_clear` are not in the dispatch table and are ignored.

### CW (pihpsdr-specific — not in the reference)

CW text goes into the rigctl CW buffer (`rigctl_queue_cw_string`/`_char`), the
same path as CAT CW; the rigctl CW thread keys MOX itself.

| Command | Args | Handler | Notes |
|---|---|---|---|
| `cw_macros` | 2,∞ | `tci_cmd_cw_macros` | `cw_macros:<trx>,<text>;` — `trx` is ignored; arguments 2…n are re-joined with `,`. Text passes through `tci_cw_decode_text`: `^`→`:`, `~`→`,`, `*`→`;`, `\|abc\|`→`[.abc]` (characters joined), `>`→`[+` (speed +25 %) and `<`→`[-` (speed −25 %) until the next `>`/`<`/`\|` or end of text. These bracket sequences are interpreted by the rigctl CW sender. |
| `cw_macros_stop` | 0,0 | `tci_cmd_cw_macros_stop` | `rigctl_purge_cw()` (discards unsent characters) and resets `cw_msg` state. |
| `cw_msg` | 1,4 | `tci_cmd_cw_msg` | `cw_msg:<id>,<prefix>,<call>[$N],<suffix>;` (`id` ignored, `_` = empty part) or `cw_msg:<call>;` to correct the call of an active message. See note below. |
| `cw_terminal` | 1,1 | `tci_cmd_cw_terminal` | Echoes `cw_terminal:true;`/`false;` per the argument. Nothing behind it. |
| `cw_macros_speed` / `cw_keyer_speed` | 0,1 | get/set | Both read and write the same `cw_keyer_speed`, clamped 1…100 WPM. Reply: echo of the respective command. |
| `cw_macros_delay` | 0,1 | get/set | Stores 0…5000 ms in `tci_cw_macros_delay_ms` and echoes. *(code reading)* The value is not used anywhere. |
| `cw_macros_speed_up` / `cw_macros_speed_down` | 1,1 | set | Change `cw_keyer_speed` by \|n\| WPM (clamped 1…100); reply `cw_macros_speed:<wpm>;`. |

*(code reading)* **`cw_msg` sequencing is incomplete.** The prefix → callsign
(×N) → suffix sequence and the `callsign_send:<call>;` broadcast are driven by
`tci_cw_msg_queue_next()`, which queues **one** character per call and is
called from exactly one place: once, from `tci_cmd_cw_msg`, and only when the
prefix is empty. Nothing calls it as the keyer drains. As written, a message
with a prefix sends only the prefix; without a prefix it sends only the first
callsign character. The suffix and `callsign_send` are never reached.

### Misc

| Command | Args | Handler | Notes |
|---|---|---|---|
| `stop` | 0,0 | `tci_cmd_stop` | Clears sensor opt-ins, queues `stop;`, sets `running = 0`, releases TX ownership and, if this client last keyed, calls `radio_set_mox(0)`. With `running` cleared, the next writable callback returns -1 and LWS **closes the connection**. *(code reading)* the queued `stop;` is likely never written, and the close then skips the per-slot cleanup (§3). In the reference, `stop` is a server→client "radio in standby" notice. |

On `shutdown_tci()` every connected client is queued `stop;` and given a close
timeout; the server waits up to ~500 ms for clients to go before joining the
LWS thread and destroying the context.

---

## 5. Binary audio streaming protocol

### Header — `TCI_STREAM_HEADER` (`src/tci_audio.h`)

64 bytes, all `uint32_t`, written in host byte order (the code notes it will not
work on big-endian CPUs):

| Offset | Field | Meaning here |
|---|---|---|
| 0 | `receiver` | Receiver index (0 or 1); 0 for chrono frames |
| 4 | `sample_rate` | Always `48000` (`TCI_AUDIO_SAMPLE_RATE`) |
| 8 | `format` | Always `3` = float32 (`TCI_AUDIO_SAMPLE_TYPE`) |
| 12 | `codec` | 0 |
| 16 | `crc` | 0 |
| 20 | `length` | Number of **float values** (see below) |
| 24 | `type` | `1` RX audio, `2` TX audio, `3` TX chrono |
| 28 | `channels` | `2`, or `1` for a mono RX stream |
| 32–63 | `reserv[8]` | 0 |

`TCI_STREAM` is the header plus `float audio[4096]` (16448 bytes in total).

Compared with the reference header: offsets 4, 20, 24 and the 64-byte payload
start match. The reference calls offset 20 `sample_count` "number of sample
pairs"; this server writes `2 × frames` for RX (and also for mono, where only
`frames` floats follow), and reads TX `length` as the total float count. Type 3
is the reference's "timing" frame. Offsets 8–19 and 28 are
implementation-specific in the reference.

### RX audio (server → client)

- **Tap.** `rx_process_buffer` (`receiver.c`) calls `tci_audio_rx_sample()`
  when any client has RX audio on (`tci_audio_rx_active`). The tap is before
  mute, mute-while-TX and the stereo/headphone routing, and is scaled by
  `tciscale = 10^(-(volume - tci_volume)/20)`, so the TCI level is independent
  of the AF-gain slider and set by the per-receiver **TCI volume** in the RX
  menu (default −20 dB). In client/server mode the client feeds the rings from
  `client_thread.c`.
- **Diversity ear split.** When `div_split_active()`, RX0's pass stores its
  folded-to-mono ear and RX1's pass emits the pair as **stream 0** (left = RX0
  ear × left balance, right = RX1 ear × right balance), both scaled by RX0's
  TCI gain law. Stream 1 receives nothing in that mode. Blocks of mismatched
  size (first block after engaging, around sample-rate changes) are skipped.
- **Ring.** One stereo ring per receiver, `TCI_RX_AUDIO_RING_FRAMES = 32768`
  frames (~0.68 s). When full, new samples are dropped. The producer wakes the
  LWS thread every 1024 samples.
- **Framing.** Each `lws_service` pass, `tci_audio_get_frame()` takes **up to**
  1024 frames (whatever is available) per enabled receiver and per client.
  Frames can therefore be shorter than 1024. Stereo payload is interleaved
  L/R; mono sends left only. Max frame: 64 + 1024 × 2 × 4 = 8256 bytes.
  - *(code reading)* The ring has one read pointer per receiver, not per
    client. With two clients streaming the same receiver, each takes alternate
    chunks rather than both getting the full stream.

### TX audio (client → server)

- Type `2`. Handled by `tci_handle_binary()` only if this client has
  `tx_audio_enabled` (set by `trx:0,true,tci`); otherwise dropped.
- `tci_audio_handle_tx_frame()` requires `length ≥ 2` and
  `frame size ≥ 64 + 4 × length`, treats the payload as stereo, and keeps the
  **left** channel in a mono ring of `TCI_TX_AUDIO_RING_FRAMES = 65536`
  samples (~1.37 s); overflow is dropped.
- `tci_get_next_mic_sample()` replaces the mic sample in `transmitter.c` (and
  `client_thread.c` for client/server) whenever `tci_audio_tx_active`. It
  pre-buffers 4096 samples (~85 ms) before playing, returns silence while
  pre-buffering, and on underrun goes back to pre-buffering (logged as
  "underrun").
- The ring is reset on `trx:…,tci` and when the deferred MOX-clear fires.
- WSJT-X has been seen sending 8256-byte frames (64 + 8192), i.e. 2048 floats
  per frame (comment in `tci_handle_binary_lws`).

### TX chrono (server → client)

- Type `3`, **header only** (64 bytes), `length = TCI_TX_AUDIO_CHRONO_LENGTH = 2048`
  (1024 × 2), `channels = 2`.
- Sent once per 1024 mic samples consumed by `tci_get_next_mic_sample()`, so it
  paces the client at the rate the transmitter actually takes audio.
- Goes to the first running client that has both `tx_audio_enabled` and
  `tx_owner`; at most one client gets it.

### Fragmentation

Inbound binary messages are reassembled from LWS fragments in
`tci_handle_binary_lws()` (always — the code comment notes that binary frames
were never seen arriving in one piece), up to 64 KiB, before
`tci_handle_binary()` is called. Text messages are not reassembled.

---

## 6. TX ownership model

`tci_transmitter_owned` (global) and `client->tx_owner` (per client) ensure only
one client can key the transmitter.

- The first client to send `trx:0,true` or `tune:0,true` while there is no
  owner becomes the owner.
- A non-owner's `trx`/`tune` set is ignored; it gets the current `trx` (and
  for `tune`, `tune`) state back.
- **Ownership is sticky.** `trx:0,false` drops MOX but does **not** release
  ownership. It is released only by `stop`, by socket close, or by a failed
  write. A second client therefore cannot key up until the first one
  disconnects — even if the first one is idle.
- On close of a running client that last keyed, MOX is dropped via
  `ext_radio_set_mox(0)`. See §3 for the paths where this does not happen.
- **No PTT watchdog.** The reference expects TX to be shut down if the
  controlling client goes away without releasing PTT. Here that relies on
  LWS noticing the close; a client that goes silent with the socket still
  open (e.g. a network partition before TCP gives up) leaves TX keyed. The
  server's 15 s WebSocket PING does not time anything out.

---

## 7. Compliance / comparison matrix

Legend: ✅ matches the reference · ⚠️ implemented but deviates · ❌ not implemented · ➕ extension (no reference equivalent)

| Reference command | Status | Notes |
|---|---|---|
| `protocol` / `device` | ⚠️ | Impersonates `ExpertSDR3,2.0` / `SunSDR2QRP`. |
| `receive_only` / `trx_count` / `channels_count` | ✅ | Init block. `trx_count` is always 1. |
| `vfo_limits` / `if_limits` | ✅ | `tci_send_limits()`. |
| `modulations_list` | ⚠️ | `LSB,USB,DSB,CW,FMN,AM,DIGU,SPEC,DIGL,SAM,DRM` — single `CW` instead of `CWL`/`CWU`, `FMN` instead of `NFM`, and FM is then reported as `FM`. |
| `iq_samplerate` (init) | ❌ | Not sent. Command echoes a valid rate but does nothing. |
| `audio_samplerate` (init) | ✅ | Sent as `48000`. |
| `tx_profiles_ex` / `tx_profile_ex` | ❌ | No TX profiles. |
| `ready` | ⚠️ | Sent with a trailing `;`, followed by `start;`. |
| `start` / `stop` (server→client) | ⚠️ | `start;` after `ready`; `stop;` on shutdown. The server also accepts `stop` from a client and closes that connection. |
| `iq_start` / `iq_stop` | ❌ | Not in the dispatch table. |
| `audio_start` / `audio_stop` | ⚠️ | Work per receiver; `audio_start` is echoed, `audio_stop` answers `audio_off:<rx>;`. Silently refused unless 48000 / 1024 / float32. |
| `audio_stream_sample_type` / `_channels` / `_samples` / `audio_samplerate` | ⚠️ | Stored per client. Only float32, 48000 and 1024 samples are usable; mono is supported for RX. `audio_stream_samples` is answered as `audio_samples:`. Init-block versions are malformed (§3). |
| `tx_stream_audio_buffering` | ⚠️ | Server sends `50`; ignored if sent by a client. |
| `rx_sensors_enable` | ⚠️ | Takes the first argument as the bool, so the reference form `rx_sensors_enable:0,true;` disables. |
| `tx_sensors_enable` | ✅ | |
| `rx_smeter` | ⚠️ | Never sent as such. Level goes out as `rx_sensors:<rx>,<dBm>;` and `rx_channel_sensors:<rx>,<ch>,<dBm>;`; a client `rx_smeter` query is answered with `rx_sensors`. |
| `tx_sensors` | ⚠️ | Fields are mic, fwd, **fwd**, swr — reverse power is replaced by forward power. |
| `tx_swr` | ❌ | SWR only inside `tx_sensors`. |
| `vfo` | ✅ | VFO-A at `0,0`, VFO-B for every other pair. |
| `dds` | ⚠️ | Reports/sets the VFO frequency, not the panadapter centre. |
| `if` | ⚠️ | Always 0, init block only, not settable. |
| `vfo_lock` / `lock` | ⚠️ | One global lock behind both. |
| `split_enable` | ✅ | |
| `vfo_swap_ex` | ❌ | |
| `modulation` | ⚠️ | See `modulations_list`. |
| `rx_filter_band` | ✅ | Get and set. |
| `tx_filter_band_ex` | ❌ | |
| `rit_enable` / `rit_offset` / `xit_enable` / `xit_offset` | ✅ | |
| `rx_nb_enable` | ⚠️ | Set drives NB (`nb`), get reports SNB (`snb`). |
| `rx_nb2_enable` / `rx_nb_enable_ex` | ❌ | |
| `rx_bin_enable` | ⚠️ | Stored and echoed; not applied to WDSP by the handler. |
| `rx_anf_enable` | ✅ | |
| `rx_nr_enable` | ⚠️ | On = NR4. |
| `rx_nr_enable_ex` | ❌ | |
| `agc_auto_ex` | ❌ | |
| `agc_gain` | ✅ | Clamped -20…120 dB. |
| `sql_enable` / `sql_level` | ✅ | Level in dB, -140…0, mapped from the 0…100 slider. |
| `trx` | ⚠️ | Single sticky owner (§6); optional `tci` audio source. |
| `tune` | ⚠️ | Same ownership; tune state not pushed. |
| `drive` / `tune_drive` | ✅ | 0–100. `tune_drive` set turns off "tune uses drive". |
| `mute` | ⚠️ | `mute:<bool>;` on the active receiver, bidirectional; the reference has `mute:<trx>`. |
| `rx_volume` | ✅ | Both channels share one gain. |
| `mon_enable` / `mon_volume` | ❌ | Stubs answering `false` / `-60`. |
| `tx_antenna` / `rx_antenna` | ❌ | |
| `rx_enable` | ❌ | Only sent (`true`) in the init block; not settable. |
| `run_cat_ex` | ❌ | |
| `line_in` | ❌ | |
| `keepalive` | ⚠️ | Not in the dispatch table; ignored harmlessly. The server sends its own WebSocket PING every 15 s instead. |
| PTT watchdog | ❌ | §6. |

### Not in the reference (➕)

| Command | Purpose |
|---|---|
| `tx_frequency` | Pushed TX frequency. |
| `rx_sensors` / `rx_channel_sensors` | S-meter push (see `rx_sensors_enable`). |
| `tx_enable` | Transmitter present (init block). |
| `audio_off` / `audio_samples` | Reply names used for `audio_stop` / `audio_stream_samples`. |
| `cw_macros`, `cw_macros_stop`, `cw_msg`, `callsign_send` | CW text and callsign-aware messages (`cw_msg` incomplete, §4). |
| `cw_terminal` | Echo only. |
| `cw_macros_speed`, `cw_keyer_speed`, `cw_macros_speed_up/down` | Keyer speed, one shared value. |
| `cw_macros_delay` | Stored, unused. |
| `rx_apf_enable`, `rx_nf_enable` | APF stored but not applied; NF has no state. |
| `digl_offset` / `digu_offset` | Always 0. |

---

## 8. Constants reference

From `src/tci.c`:

| Constant | Value | Purpose |
|---|---|---|
| `MAXDATASIZE` | 1024 | Unused |
| `MAXMSGSIZE` | 512 | Text message buffer (incoming text truncated to 511 bytes) |
| `TCI_MAX_ARGS` | 16 | Max comma-separated args per command |
| `TCI_BINARY_REASSEMBLY_MAX` | 65536 | Cap on a reassembled binary message |
| `TCI_MAX_CLIENTS` | 8 | Max simultaneous clients |
| (literal) | 100 | Per-client output queue limit (`idle_queued`) |
| (literal) | 500 ms / 30 ticks | Reporter period / PING interval (15 s) |

From `src/tci_audio.h`:

| Constant | Value | Purpose |
|---|---|---|
| `TCI_RX_AUDIO_MAX_RECEIVERS` | 2 | RX audio streams |
| `TCI_AUDIO_SAMPLE_RATE` | 48000 | Only supported rate |
| `TCI_AUDIO_SAMPLES` | 1024 | Samples per frame (RX max, chrono period, required `audio_stream_samples`) |
| `TCI_AUDIO_SAMPLE_TYPE` | 3 | float32, header `format` |
| `TCI_STREAM_RX_AUDIO` / `_TX_AUDIO` / `_TX_CHRONO` | 1 / 2 / 3 | Frame types |
| `TCI_TX_AUDIO_CHRONO_LENGTH` | 2048 | `length` in chrono frames |

From `src/tci_audio.c`:

| Constant | Value | Purpose |
|---|---|---|
| `TCI_RX_AUDIO_RING_FRAMES` | 32768 | RX ring per receiver (~0.68 s stereo) |
| `TCI_TX_AUDIO_RING_FRAMES` | 65536 | TX mono ring (~1.37 s) |
| `tci_tx_prebuffer_frames` | 4096 | TX pre-buffer before playout (~85 ms) |

---

## 9. Notable gaps and suspected bugs

All from code reading; none of these has been reproduced with a client yet.

1. **TX can stay keyed.** No PTT watchdog (§6), and after a failed `lws_write`
   while transmitting, ownership is released but MOX is not dropped (§3).
2. **Sticky TX ownership.** `trx:0,false` does not release ownership, so a
   second client cannot transmit until the first disconnects (§6).
3. **One command per text frame.** Everything after the first `;` is lost (§2).
4. **Close cleanup skipped** when `running` is already 0 (after `stop` or a
   write error): `cat_control` stays raised, memory is held (§3).
5. **`cw_msg` does not complete** — only the prefix, or one callsign
   character, is sent; suffix and `callsign_send` are never reached (§4).
6. **Set/get mismatches:** `rx_nb_enable` (NB vs SNB), `volume` (sets RX0,
   reports the active receiver), `rx_mute` (set has no effect), `rx_nf_enable`
   (always true), `rx_apf_enable`/`rx_bin_enable` (stored but not applied by
   the handler).
7. **Wire-format slips:** malformed init-block audio lines, `audio_samples:`
   and `audio_off:` reply names, `tx_sensors` reverse-power field, `length`
   counting floats rather than pairs, `rx_sensors_enable:0,true` disabling.
8. **Shared RX ring read pointer** — two clients on the same receiver split
   the stream between them (§5).
9. **Not implemented:** IQ streaming, TX profiles, antennas, `vfo_swap_ex`,
   `rx_enable`, `run_cat_ex`, spots, monitor.
10. **No change notification from the GUI** for anything other than
    frequency, mode/filter, split, MOX and TX frequency (§2).
