# TCI Support in pihpsdr / deskHPSDR

This document describes the TCI (Transceiver Control Interface) server implemented
in this repository, and compares it command-by-command against the published
reference at:

> https://pure-editions.com/on7off/TCI-Remote/TCI_Protocol_Reference.html

It exists so that an agent (or developer) working on TCI in this codebase has a
single accurate reference for what is implemented, where, and how it diverges
from "standard" TCI as used by clients such as Expert Electronics SunSDR
software, MLDX, Logbook programs, etc.

Source files:
- [`src/tci.c`](../src/tci.c) — LWS WebSocket server, command dispatch, all state broadcast logic, CW macro engine.
- [`src/tci.h`](../src/tci.h) — public globals and the notification entry points called from the rest of the app (`tci_vfo_changed`, `tci_mode_changed`, etc.).
- [`src/tci_audio.c`](../src/tci_audio.c) / [`src/tci_audio.h`](../src/tci_audio.h) — binary audio frame format, ring buffers, RX/TX/chrono framing.
- [`src/rigctl_menu.c`](../src/rigctl_menu.c) — GUI checkbox/spinbutton for enabling TCI and setting the port.
- [`src/radio.c`](../src/radio.c) — `tci_enable` / `tci_port` persisted to/from the props file.

---

## 1. Overview

| | This implementation | Published spec |
|---|---|---|
| Transport | WebSocket via libwebsockets | WebSocket (RFC 6455) |
| Default port | `tci_port = 40001` (declared in `src/tci.h:25`, initialized in `src/tci.c:65`) | 50001 |
| Subprotocols advertised | `"chat"`, `"superchat"`, `"tci"` | not specified |
| Max simultaneous clients | `TCI_MAX_CLIENTS = 8` (`src/tci.c:56`) | unspecified, "multiple clients supported" |
| Device identity sent | `protocol:ExpertSDR3,2.0;` / `device:SunSDR2QRP;` | `protocol:2;` / implementation-chosen name |
| IQ streaming | Commands accepted but are stubs; no IQ binary data path exists | Required (`iq_start`/`iq_stop`, ≥96 kHz) |
| Audio streaming | Fully implemented, fixed 48 kHz / stereo / float32 / 512-sample frames | Fully specified, 48 kHz fixed |

Enable/configure TCI via the CAT/TCI GUI menu (`rigctl_menu.c`), which toggles
`tci_enable` and sets `tci_port`, then calls `launch_tci()` / `shutdown_tci()`
(`src/tci.c:202`, `src/tci.c:224`). Settings persist via `GetPropI0`/`SetPropI0`
for `tci_enable` and `tci_port` in `radio.c`.

---

## 2. Architecture

- **`CLIENT` struct** (`src/tci.c:96-123`) — one slot per connection (array
  `tciclient[TCI_MAX_CLIENTS]`), tracking last-reported VFO/mode/split/MOX
  state (used to suppress redundant broadcasts), TX ownership flag, per-receiver
  RX-audio-enabled flags + read cursors, TX-audio-enabled flag, and a binary
  reassembly buffer for fragmented WebSocket frames.
- **Command dispatch** — text commands are parsed into a `TCI_CMD`
  (cmd name + up to `TCI_MAX_ARGS=16` comma-separated args) and looked up in
  the static `tci_dispatch[]` table (`src/tci.c:3076`), which enforces
  min/max arg counts before calling the handler. Unknown commands are logged
  and ignored (matches spec's "silently ignore unknown commands").
- **Periodic state push** — `tci_reporter()` runs on a 500 ms GTK timer
  per client (`g_timeout_add(500, tci_reporter, ...)`) and re-sends VFO
  frequency/mode, split, MOX, TX frequency, and (if enabled) RX/TX sensor
  readings only when the value has changed since `last_*`. A counter rolls
  over every 30 ticks (~15 s) to send a binary PING.
- **Output backpressure** — both text (`tci_queue_frame`) and binary
  (`tci_queue_binary_frame`) queuing drop new frames once a client's
  `idle_queued` counter reaches 100, preventing unbounded memory growth if a
  client stalls.
- **Binary reassembly** — fragmented incoming binary WebSocket frames are
  accumulated in `client->binary_rx_buf` (starts at 8 KiB, doubles as needed)
  up to `TCI_BINARY_REASSEMBLY_MAX = 65536` bytes, after which the partial
  frame is discarded (`tci_handle_binary_lws`, `src/tci.c:521`).
- **Event loop** — a dedicated `GThread` runs `lws_service()` in a loop;
  `lws_cancel_service()` is used to wake the loop immediately whenever a
  frame is queued for send (text, binary, or new RX audio), avoiding
  fixed-latency polling for output.

---

## 3. Connection lifecycle / handshake

On `LWS_CALLBACK_ESTABLISHED` a free `CLIENT` slot is initialized and a
write-queued callback is armed. On the first writable callback,
`tci_send_initial_state()` (`src/tci.c:3395`) sends the full init block, in
this exact order:

```
protocol:ExpertSDR3,2.0;
device:SunSDR2QRP;
receive_only:<bool>;            (from can_transmit)
trx_count:<n>;
channels_count:2;
vfo_limits:<min>,<max>;
if_limits:<min>,<max>;
modulations_list:LSB,USB,DSB,CW,FMN,AM,DIGU,SPEC,DIGL,SAM,DRM;
dds:0,<freq>;
if:0,0,0;
if:0,1,0;
vfo:0,0,<freq>;
vfo:0,1,<freq>;
modulation:0,<mode>;
rx_filter_band:0,<lo>,<hi>;
[... repeated for VFO/receiver 1 if receivers > 1 ...]
rx_enable:0,true;  [and rx_enable:1,true; if receivers > 1]
lock:0,<bool>;  vfo_lock:0,0,<bool>;  vfo_lock:0,1,<bool>;
sql_enable:0,<bool>;  sql_level:0,<dB>;
rx_anf_enable:0,<bool>;  rx_apf_enable:0,<bool>;
rx_nb_enable:0,<bool>;   rx_nf_enable:0,<bool>;
rx_bin_enable:0,<bool>;  rx_nr_enable:0,<bool>;
rit_enable:0,<bool>;     rit_offset:0,<hz>;
xit_enable:0,<bool>;     xit_offset:0,<hz>;
tune_drive:0,<pct>;
tx_enable:0,<bool>;
split_enable:0,<bool>;
trx:0,<bool>;                    (MOX state)
tune:0,<bool>;
mute:<bool>;
rx_mute:0,<bool>;
volume:<dB>;
rx_volume:0,0,<dB>;  rx_volume:0,1,<dB>;
agc_gain:0,<dB>;
agc_mode:0,<off|fast|normal>;
[... sql_enable..agc_mode repeated for receiver 1 if receivers > 1 ...]
cw_macros_speed:<wpm>;
cw_macros_delay:<ms>;
cw_keyer_speed:<wpm>;
ready;
start;
```

**Differences from the spec's init block:**
- No `iq_samplerate:` line is sent (spec requires it, ≥96000).
- No `tx_profiles_ex:` line is sent — this implementation has no TX-profile concept.
- An extra trailing `start;` is sent after `ready` (spec only documents `ready` as the terminator).
- `protocol:ExpertSDR3,2.0;` / `device:SunSDR2QRP;` impersonate a specific
  Expert Electronics radio/software combo rather than emitting a
  generic/implementation-specific identity — done deliberately so unmodified
  TCI clients (e.g. SunSDR control apps) recognize the server.

---

## 4. Full command reference (as implemented)

All commands below are parsed case-insensitively. Booleans accept `true`/`false`/`1`/`0`.
Function names refer to handlers in `src/tci.c`; "Send fn" is the function used to
both answer a query and to broadcast state changes to all connected clients.

### VFO / frequency

| Command | Args (min,max) | Handler | Notes |
|---|---|---|---|
| `vfo` | 2,3 | `tci_cmd_vfo` → `tci_set_vfo`/`tci_send_vfo` | `vfo:<trx 0/1>,<channel 0=A/1=B>,<freq_hz>;` Get if freq omitted. Receiver-id gated by `receivers`. |
| `dds` | (S→C only) | `tci_send_dds` | Reports CTUN/VFO frequency for the panadapter; sent in init + on VFO change. |
| `lock` | 1,2 | `tci_cmd_lock` | `lock:<trx>,<bool>;` Maps to the global `locked` flag (VFO_A only). |
| `vfo_lock` | 1,3 | `tci_cmd_vfo_lock` | `vfo_lock:<trx>,<channel>,<bool>;` Also tied to the same global `locked` flag. |
| `split_enable` | 1,2 | `tci_cmd_split_enable` | True if TX VFO is VFO-B. |
| `trx_count` | 0,0 | `tci_cmd_trx_count` | Always reports `1` (single TRX). |

### Modulation / filters

| Command | Args | Handler | Notes |
|---|---|---|---|
| `modulation` | 1,2 | `tci_cmd_modulation` → `tci_set_mode` | Mode strings: `lsb,usb,dsb,cw,cwl,cwu,fmn,fm,am,digu,spec,digl,sam,drm`. Reports back as one of `LSB,USB,DSB,CW,FM,AM,DIGU,SPEC,DIGL,SAM,DRM` (note: CWL/CWU both report as `CW`, and `fm`/`fmn` are aliases reporting as `FM`). |
| `rx_filter_band` | 1,3 | `tci_cmd_rx_filter_band` | `rx_filter_band:<trx>,<lo>,<hi>;` Get reports the active mode's `FILTER` preset edges; **set** (3 args) calls `filter_edges_changed(receiver_id, low, high)` to actually reshape the filter, then broadcasts the new edges to all clients. |
| `digl_offset` / `digu_offset` | 0,1 / 0,1 | stub handlers | Always reply `digl_offset:0;` / `digu_offset:0;`. No real digital-mode offset support. |

### RIT / XIT

| Command | Args | Handler | Notes |
|---|---|---|---|
| `rit_enable` | 1,2 | `tci_cmd_rit_enable` | Per-receiver. |
| `rit_offset` | 1,2 | `tci_cmd_rit_offset` | Hz, per-receiver. |
| `xit_enable` | 1,2 | `tci_cmd_xit_enable` | TRX index must be `0`; applies to whichever VFO is currently the TX VFO. |
| `xit_offset` | 1,2 | `tci_cmd_xit_offset` | Same TX-VFO semantics as `xit_enable`. |

### DSP toggles (per receiver, receiver_id must be 0 or 1)

| Command | Handler | Notes |
|---|---|---|
| `rx_anf_enable` | `tci_cmd_rx_anf_enable` | Maps to `receiver->anf`. |
| `rx_apf_enable` | `tci_cmd_rx_apf_enable` | Maps to `vfo[].cwAudioPeakFilter`; effective state forced `false` unless mode is CW. |
| `rx_nb_enable` | `tci_cmd_rx_nb_enable` | Maps to `receiver->snb`; effective state forced `false` in DIGL/DIGU modes. |
| `rx_nf_enable` | `tci_cmd_rx_nf_enable` | Setter exists but getter always reports `true` (`tci_send_rx_nf_enable` hardcodes `"true"` — likely a stub/bug; the value-aware variant `tci_send_rx_nf_enable_value` is only used for the set-echo path). |
| `rx_bin_enable` | `tci_cmd_rx_bin_enable` | Maps to `receiver->binaural`. |
| `rx_nr_enable` | `tci_cmd_rx_nr_enable` | Maps to `receiver->nr` (on/off only — no algorithm selection). Effective state forced `false` for modes where NR has no default per `tci_rx_nr_default_for_mode`. |

### AGC / squelch

| Command | Handler | Notes |
|---|---|---|
| `agc_gain` | `tci_cmd_agc_gain` | dB, clamped to **[-20, 120]**. |
| `agc_mode` | `tci_cmd_agc_mode` | String `off`/`fast`/`normal` ↔ `AGC_OFF`/`AGC_FAST`/`AGC_MEDIUM`. No "auto vs manual gain" concept — see §8 gap on `agc_auto_ex`. |
| `sql_enable` | `tci_cmd_sql_enable` | Per-receiver. |
| `sql_level` | `tci_cmd_sql_level` | Reported value is dB derived from an internal 0–100 slider via `tci_sql_db_from_slider`: `dB = (slider% × 1.4) − 140`, clamped to **[-140, 0]**. |

### PTT / TX control

| Command | Handler | Notes |
|---|---|---|
| `trx` | `tci_cmd_trx` | `trx:<0>,<bool>[,<source>];` PTT. **Single-owner exclusivity** — see §6. Accepts an optional `source="tci"` argument to initialize TX-audio-over-TCI for that client. |
| `tune` | `tci_cmd_tune` | Tune/carrier mode; same single-owner exclusivity as `trx`. |
| `drive` | `tci_cmd_drive` | `drive:0,<0-100>;` |
| `tune_drive` | `tci_cmd_tune_drive` | `tune_drive:0,<0-100>;` Uses `transmitter->tune_drive` or live drive if `tune_use_drive` is set. |
| `rx_smeter` | `tci_cmd_rx_smeter` | Query-only; pushes current S-meter immediately as `rx_smeter:<rx>,<dBm>;` (also periodically via `tci_reporter` if `rx_sensors_enable` is on). |
| `rx_sensors_enable` | `tci_cmd_rx_sensors_enable` | Per-client opt-in flag (`client->rxsensor`) for periodic S-meter push. |
| `tx_sensors_enable` | `tci_cmd_tx_sensors_enable` | Per-client opt-in flag (`client->txsensor`) for periodic mic/RMS/peak/SWR push, sent only while actually transmitting and `fwd > 0.01`. |

### Audio control

| Command | Handler | Notes |
|---|---|---|
| `mute` | `tci_cmd_mute` | Global RX mute (`active_receiver->mute_radio`). |
| `rx_mute` | `tci_cmd_rx_mute` | Per-receiver mute. |
| `volume` | `tci_cmd_volume` | dB, clamped **[-40, 0]**. |
| `rx_volume` | `tci_cmd_rx_volume` | `rx_volume:<rx>,<channel 0/1>,<dB>;` clamped **[-40, 0]**. Channel is accepted/echoed but both channels map to the same `receiver->volume`. |
| `mon_volume` / `mon_enable` | stub handlers | Always reply `-60` / `false`; no real TX monitor/sidetone path is wired to these commands (monitor ring buffer exists in `tci_audio.c` but isn't driven by these two commands). |
| `audio_start` / `audio_stop` | `tci_cmd_audio_start` / `tci_cmd_audio_stop` | `audio_start:<receiver_id>;` enables/disables binary RX-audio streaming to that client for that receiver (`client->rx_audio_enabled[]`). |
| `audio_samplerate`, `audio_stream_sample_type`, `audio_stream_channels`, `audio_stream_samples` | corresponding stub handlers | Always answer fixed values `48000` / `float32`(format id `3`) / `2` / `512` — these are informational echoes, not configurable.|

### IQ streaming (stubs only)

| Command | Handler | Notes |
|---|---|---|
| `iq_start` / `iq_stop` | stub handlers | Accepted, parsed, **no-op** — no IQ binary data path exists anywhere in `tci.c`/`tci_audio.c`. |
| `iq_samplerate` | `tci_cmd_iq_samplerate` | Accepts/echoes a rate but does not actually change anything; no IQ frames are ever produced. |

### Spotting (stubs only)

| Command | Handler | Notes |
|---|---|---|
| `spot` / `spot_delete` / `spot_clear` | stub handlers | Parsed and accepted, no-op. No DX spot list is implemented. |

### CW macro / callsign extension (pihpsdr-specific — not in the spec)

| Command | Handler | Notes |
|---|---|---|
| `cw_macros` | `tci_cmd_cw_macros` | Sends a literal macro text string to the CW keyer (`rigctl_queue_cw_string`), with `^`/`~`/`*`/`|text|`/`>`/`<` escape decoding (`tci_cw_decode_text`, ~`src/tci.c:2734`). |
| `cw_macros_stop` | `tci_cmd_cw_macros_stop` | Stops keyer transmission, resets CW message state. |
| `cw_msg` | `tci_cmd_cw_msg` | Two forms: (1) `cw_msg:<callsign>;` mid-transmission callsign correction; (2) `cw_msg:<macro_id>,<prefix>,<callsign>[$repeat],<suffix>;` queues prefix → callsign (repeated `$N` times) → suffix, broadcasting `callsign_send:<callsign>;` to all clients when the callsign segment starts. |
| `cw_terminal` | stub handler | Always replies `true`/`false`; no real terminal echo implemented. |
| `cw_macros_speed` / `cw_keyer_speed` | get/set handlers | WPM, 1–100. |
| `cw_macros_delay` | get/set handler | Inter-macro delay in ms, 0–5000 (`tci_cw_macros_delay_ms`). |
| `cw_macros_speed_up` / `cw_macros_speed_down` | set handlers | Adjust speed by a step count argument — non-standard relative adjustment, no spec equivalent. |

### Misc

| Command | Handler | Notes |
|---|---|---|
| `stop` | `tci_cmd_stop` | Client signals shutdown intent: disables both sensor flags and releases TX ownership if held. Does not close the socket itself. |

---

## 5. Binary audio streaming protocol

### Header — `TCI_STREAM_HEADER` (`src/tci_audio.h:47`)

64 bytes, all fields `uint32_t` (native/little-endian on target platforms):

| Offset | Field | Meaning here |
|---|---|---|
| 0 | `receiver` | Receiver index (0 or 1) |
| 4 | `sample_rate` | Always `48000` (`TCI_AUDIO_SAMPLE_RATE`) |
| 8 | `format` | Always `3` (`TCI_AUDIO_FORMAT_FLOAT32`) |
| 12 | `codec` | Unused (always 0) |
| 16 | `crc` | Unused (always 0) |
| 20 | `length` | Sample count field — see per-frame-type notes below |
| 24 | `type` | `1` = RX audio, `2` = TX audio, `3` = TX chrono (see `TCI_STREAM_*` constants) |
| 28 | `channels` | Always `2` (`TCI_AUDIO_CHANNELS`) |
| 32–63 | `reserv[8]` | Reserved/zero |

This is positionally compatible with the spec's documented 64-byte header
(offsets 0/4/24/64-payload line up), though the spec's generic field names
(`flags`, `hdr[2..4]`) are repurposed here as `receiver`/`codec`/`crc`, and the
spec uses `sample_rate > 48000` to flag IQ frames — moot here since no IQ
frames are ever sent.

### RX audio frame (server → client)

- Type `1`, payload = `512` interleaved stereo float32 samples (`TCI_RX_AUDIO_FRAME_FRAMES`), i.e. **4096 payload bytes + 64-byte header = 4160 bytes** total (`TCI_AUDIO_RX_FRAME_MAX_BYTES`).
- Generated from a per-receiver ring buffer (`TCI_RX_AUDIO_RING_FRAMES = 48000`, 1 second deep) fed by `tci_audio_rx_sample()` from the receiver's audio pipeline.
- Pushed via `tci_service_rx_audio()` whenever data is available and the client has `audio_start`'d that receiver.

### TX audio frame (client → server)

- Type `2`. Received in `tci_handle_binary()` and routed to `tci_audio_handle_tx_frame()` only if `client->tx_audio_enabled` is set (set by sending `trx:0,true,tci`, i.e. claiming TX with TCI as the audio source).
- Payload is stereo float32 like RX frames; only the left channel is retained into a mono ring buffer (`TCI_TX_AUDIO_RING_FRAMES = 48000*4`, 4 seconds deep) consumed sample-by-sample by `tci_get_next_mic_sample()` as the transmit microphone source.

### TX chrono frame (server → client) — pihpsdr-specific, not in the spec

- Type `3`, **header only**, no payload. `length` field is set to `1024` (`TCI_TX_AUDIO_CHRONO_LENGTH = 512*2`) as a sentinel, not an actual sample count.
- Purpose: a timing pulse sent once per TX-audio frame period so the client paces its outgoing TX audio stream; queued from `tci_queue_tx_chrono_frame()`, driven by `tci_tx_chrono_loop()` called once per mic sample consumed.
- Sent only to the single client currently holding TX-audio ownership (one sender at a time).

### Fragmentation / reassembly / alignment

Inbound binary WebSocket frames may arrive fragmented at the LWS layer;
`tci_handle_binary_lws()` reassembles up to `TCI_BINARY_REASSEMBLY_MAX = 65536`
bytes before dispatching to `tci_handle_binary()`. The spec does not discuss
WebSocket-level fragmentation, since that's a transport detail outside the
"frame format" section, but a real client could split a binary message across
multiple WebSocket fragments depending on its library — this implementation
handles it, but only up to that 64 KiB cap.

Additionally, to prevent memory alignment faults when casting incoming binary data to float arrays (e.g. for TX audio processing), `tci_handle_binary_lws()` checks the float-alignment of the received buffer:
```c
aligned = ((uintptr_t) data & (alignof(float) - 1)) == 0;
```
If the buffer is not float-aligned, it is copied into `client->binary_rx_buf` (which is allocated with proper standard alignment) before being handled, ensuring safety.

---

## 6. TX ownership model

This implementation enforces **single TX ownership**: `tci_transmitter_owned`
(global) plus `client->tx_owner` (per-client) ensure only one connected
client can hold PTT/tune at a time.

- First client to send `trx:0,true` with no existing owner becomes the owner; `radio_set_mox(1)` is invoked.
- A `trx:0,true` from a non-owning client while someone else holds TX is **silently ignored** (current MOX state is echoed back instead of granting TX) — this is a deliberate deviation from the spec's implied "any client may set trx, server arbitrates/echoes" model.
- The owner releases TX with `trx:0,false`; ownership and `tx_audio_enabled` are also cleared automatically on `stop` or on socket close (`LWS_CALLBACK_CLOSED`).
- **Gap vs spec:** the spec requires a PTT watchdog (automatic TX shutdown after 60–120 s if the controlling client disconnects without releasing PTT). This implementation relies on `LWS_CALLBACK_CLOSED` releasing ownership on a clean disconnect, but there is **no timeout-based watchdog** for a client that goes silent without the socket closing (e.g. a network partition). This is a known gap worth hardening if used for unattended remote operation.

---

## 7. Compliance / comparison matrix

Legend: ✅ matches spec · ⚠️ implemented but deviates · ❌ not implemented · ➕ extension (no spec equivalent)

| Spec command | Status | Notes |
|---|---|---|
| `protocol` / `device` / `receive_only` / `trx_count` / `channels_count` | ✅ | Sent in init block. |
| `vfo_limits` / `if_limits` | ✅ | `tci_send_limits()`. |
| `modulations_list` | ⚠️ | Sent, but list differs from spec example (`LSB,USB,DSB,CW,FMN,AM,DIGU,SPEC,DIGL,SAM,DRM` vs spec's `LSB,USB,DSB,CWL,CWU,...`) — this app collapses CWL/CWU into one `CW`/`FMN` entry rather than exposing both directions and `NFM` naming. |
| `iq_samplerate` (init) | ❌ | Never sent; no IQ subsystem. |
| `audio_samplerate` (init) | ✅ | Sent fixed as `48000`. |
| `tx_profiles_ex` | ❌ | No TX profile concept exists. |
| `ready` | ✅ | Sent as terminator. |
| `iq_start` / `iq_stop` | ❌ | Parsed, no-op stubs; no IQ data ever flows. |
| `audio_start` / `audio_stop` | ✅ | Fully implemented per-receiver RX audio streaming. |
| `audio_stream_sample_type` / `_channels` / `_samples` / `audio_samplerate` | ⚠️ | Implemented as **read-only fixed-value echoes** (`float32`/`2`/`512`/`48000`); spec implies these can be client-negotiated, this server ignores any client-requested values. |
| `tx_stream_audio_buffering` | ❌ | Not in dispatch table at all; any client sending it gets "unknown command" handling. |
| `rx_sensors_enable` / `tx_sensors_enable` | ✅ | Implemented. |
| `vfo` / `dds` / `if` | ✅ | Implemented (`if` is always reported as `0`, no true IF-offset model). |
| `vfo_lock` / `lock` | ✅ | Implemented, though both ultimately gate off one shared `locked` global rather than independent per-VFO/per-channel locks. |
| `split_enable` | ✅ | Implemented. |
| `vfo_swap_ex` | ❌ | Not implemented. |
| `modulation` | ✅ | Implemented; see modulations_list note above for naming differences. |
| `rx_filter_band` | ✅ | Implemented — get returns the mode's preset edges, set actually reshapes the filter via `filter_edges_changed()` and broadcasts the change. |
| `tx_filter_band_ex` | ❌ | Not implemented. |
| `rit_enable` / `rit_offset` / `xit_enable` / `xit_offset` | ✅ | Implemented. |
| `rx_nb_enable` | ✅ | Implemented (NB1 only). |
| `rx_nb2_enable` | ❌ | Not implemented — only one noise blanker stage exposed. |
| `rx_bin_enable` / `rx_anf_enable` / `rx_nr_enable` | ⚠️ | Implemented as simple booleans; spec's `rx_nr_enable_ex` (algorithm-select NR1–NR4) has no equivalent — this app's NR is on/off only. |
| `rx_apf_enable` | ✅ | Implemented. |
| `rx_nf_enable` | ⚠️ | Setter accepts and echoes state, but getter is hardcoded to return "true"; does not toggle a real filter stage. |
| `rx_nr_enable_ex` | ❌ | Not implemented. |
| `agc_auto_ex` | ❌ | Not implemented — no auto/manual AGC distinction. |
| `agc_mode` | ✅ | Implemented (maps `off`/`fast`/`normal` ↔ `AGC_OFF`/`AGC_FAST`/`AGC_MEDIUM`). |
| `agc_gain` | ✅ | Implemented, range -20..120 dB (vs spec's open-ended "numeric"). |
| `sql_enable` / `sql_level` | ✅ | Implemented; `sql_level` units are dB derived from an internal 0–100 slider, range -140..0. |
| `trx` (PTT) | ⚠️ | Implemented but **single-owner exclusivity** (see §6) instead of spec's all-clients-may-set model. |
| `tune` | ⚠️ | Same single-owner exclusivity as `trx`. |
| `drive` / `tune_drive` | ✅ | Implemented, 0–100. |
| `mute` | ⚠️ | Spec defines `mute` as a one-way client→server command with a TRX arg; this app's `mute` is global (no TRX arg) and is bidirectional (also pushed by the server). |
| `rx_mute` | ✅ | Implemented. |
| `tx_antenna` / `rx_antenna` | ❌ | Not implemented. |
| `rx_enable` | ⚠️ | Only ever sent server→client during init (`rx_enable:0,true;`); not present in the dispatch table as a client-settable command. |
| `volume` | ✅ | Implemented. |
| `rx_volume` / `mon_enable` / `mon_volume` | ⚠️ | `rx_volume` implemented; `mon_enable`/`mon_volume` are stubs that always answer `false`/`-60` and don't control anything real. |
| `tx_profiles_ex` / `tx_profile_ex` | ❌ | No TX profile feature at all. |
| `rx_smeter` | ✅ | Implemented (query + periodic push). |
| `tx_sensors` | ✅ | Implemented (mic/RMS/peak/SWR, push only while transmitting). |
| `tx_swr` / `tx_forward_power` | ❌ | Not sent as separate commands; SWR/forward power are folded into `tx_sensors` only. |
| `rx_channel_sensors` | ❌ | Not implemented. |
| `run_cat_ex` | ❌ | Not implemented — no CAT pass-through over TCI. |
| `spot` / `spot_delete` / `spot_clear` | ❌ | Parsed, no-op stubs. |
| `keepalive` | ⚠️ | Not handled as a text command at all; this server instead relies on binary WebSocket PING/PONG frames (`opPING`/`opPONG`) sent every ~15 s by `tci_reporter`. A client sending the spec's literal `keepalive;` text command would hit "unknown command" handling (harmless, since unknown commands are ignored) rather than being recognized. |
| `stop` | ✅ | Implemented (clears client sensor flags and PTT ownership). |
| PTT watchdog | ❌ | Not implemented — see §6 gap note. |

### pihpsdr-only extensions (➕, no spec equivalent)

| Command | Purpose |
|---|---|
| `cw_macros` | Send literal text to the CW keyer with escape-decoded formatting. |
| `cw_macros_stop` | Stop keyer transmission. |
| `cw_msg` | Queue/correct a callsign-aware CW message (prefix/callsign×repeat/suffix), broadcasting `callsign_send:` notifications. |
| `cw_terminal` | Stub query, always `true`/`false`. |
| `cw_macros_speed` / `cw_keyer_speed` / `cw_macros_delay` | CW keyer speed/delay configuration over TCI. |
| `cw_macros_speed_up` / `cw_macros_speed_down` | Relative speed step adjustment. |
| `digl_offset` / `digu_offset` | Present as stubs (always reply `0`) — placeholders for a feature that isn't implemented, included presumably for client compatibility/probing rather than as a real extension. |

---

## 8. Constants reference

From `src/tci.c`:

| Constant | Value | Purpose |
|---|---|---|
| `MAXDATASIZE` | 1024 | (declared, effectively unused) |
| `MAXMSGSIZE` | 512 | Text message / response buffer size |
| `TCI_MAX_ARGS` | 16 | Max comma-separated args per command |
| `TCI_BINARY_REASSEMBLY_MAX` | 65536 | Cap on reassembled fragmented binary frame size |
| `TCI_MAX_CLIENTS` | 8 | Max simultaneous WebSocket clients |

From `src/tci_audio.h`:

| Constant | Value | Purpose |
|---|---|---|
| `TCI_RX_AUDIO_MAX_RECEIVERS` | 2 | Max concurrent RX audio streams |
| `TCI_RX_AUDIO_RING_FRAMES` | 48000 | RX ring buffer depth (1 s) |
| `TCI_RX_AUDIO_FRAME_FRAMES` | 512 | Samples per RX audio frame (~10.7 ms) |
| `TCI_AUDIO_SAMPLE_RATE` | 48000 | Fixed audio sample rate |
| `TCI_AUDIO_CHANNELS` | 2 | Stereo frame layout |
| `TCI_AUDIO_FORMAT_FLOAT32` | 3 | Sample format id used in header `format` field |
| `TCI_STREAM_RX_AUDIO` | 1 | Frame type: RX audio |
| `TCI_STREAM_TX_AUDIO` | 2 | Frame type: TX audio |
| `TCI_STREAM_TX_CHRONO` | 3 | Frame type: TX timing pulse |
| `TCI_TX_AUDIO_FRAME_FRAMES` | 512 | Samples per TX audio frame |
| `TCI_TX_AUDIO_CHRONO_LENGTH` | 1024 (512×2) | Sentinel `length` value in chrono frames |
| `TCI_AUDIO_MONITOR_RING_FRAMES` | 192000 (48000×4) | Monitor ring buffer depth (4 s) — buffer exists but isn't wired to `mon_enable`/`mon_volume` |
| `TCI_TX_AUDIO_RING_FRAMES` | 192000 (48000×4) | TX mic ring buffer depth (4 s) |

---

## 9. Summary of notable gaps (for anyone hardening this for unattended/multi-op use)

1. **No PTT watchdog** — a client that vanishes without closing its socket cleanly can leave TX latched. (§6)
2. **No real IQ streaming path** — `iq_start`/`iq_stop`/`iq_samplerate` are accepted but inert; any client expecting panadapter IQ data over TCI will get nothing.
3. **Single TX owner, not spec's shared-arbitration model** — fine for one human + one logging/digi-mode client, but a second client trying to key up will be silently ignored rather than getting a clear rejection or queued-access message.
4. **Several stub commands** (`mon_enable`, `mon_volume`, `cw_terminal`, `spot*`, `digl_offset`, `digu_offset`) reply with fixed canned values — a client probing capability via these may be misled into thinking a feature is live when it isn't.
5. **`rx_nf_enable` getter bug-smell** — always reports `true` regardless of actual state; only the setter path reports the real value.
