# Feature Request: Binaural Dual-ADC Presentation via the Diversity Path

**Status:** Proposed  
**Target Subsystem:** Diversity Engine (`src/diversity_auto.c`, `src/diversity_menu.c`), Receiver Engine (`src/receiver.c`), Protocol Ingestion (`src/new_protocol.c`, `src/old_protocol.c`)  
**Companion Documents:** [`docs/diversity.md`](diversity.md), [`docs/diversity-guide.md`](diversity-guide.md), [`docs/diversity-auto-phasing.md`](diversity-auto-phasing.md)

---

## 1. Summary & Motivation

In dual-ADC SDR architectures (e.g., Angelia, Orion, Orion 2, Saturn, Hermes-Lite 2 with dual-ADC companion), operators often deploy two distinct receive antennas—such as a horizontal dipole and a vertical loop, or spaced beverage antennas.

piHPSDR's existing diversity implementation operates exclusively at the RF/IQ pre-detection stage:
$$z = z_0 + w \cdot z_1$$
where raw IQ samples from ADC1 ($z_0$) and ADC2 ($z_1$) are combined with a complex weight $w = |w|e^{j\phi}$ to produce a **single, combined mono IQ stream** fed to `receiver[0]`.

While RF vector combining is effective for nulling localized interference or maximizing array SNR, it discards spatial information. Human binaural hearing is an exceptionally powerful biological noise filter (the "cocktail party effect"): when two antenna channels are presented separately to the left and right ears, the brain's auditory cortex uses interaural time differences (ITD) and interaural level differences (ILD) to separate wanted signals from ambient noise and competing QRM far more flexibly than a single scalar RF null.

### Objectives
1. Provide a dedicated **"Binaural / Ear-Split"** mode in piHPSDR that presents **ADC1 demodulated audio in the Left ear** and **ADC2 demodulated audio in the Right ear**.
2. Leverage the **Diversity hardware pipeline** (rather than independent software VFOs) to guarantee zero-drift, sample-locked DDC coherence across the two ears.
3. Automatically link receive settings (frequency, mode, filter edges, squelch, mute, dither, random) while keeping **SAM sidebands (LSB/USB) strictly independent**.
4. Support both **local soundcard sinks** (ALSA, PulseAudio, PipeWire) and the **hardware radio headphone jack** (`new_protocol_audio_samples` / `old_protocol_audio_samples`).
5. (Optional extension) Support **Phase-Steered Binaural Diversity**, allowing the existing diversity gain/phase controls to steer the perceived acoustic stereo image across the listener's head.

---

## 2. Why the Diversity Path is the Optimal Foundation

There are two conceivable ways to implement dual-ADC ear splitting in piHPSDR:

### Approach A: Dual Independent Receivers (Software VFO Slaving)
- Set `receivers = 2`, assign RX1 to ADC1 and RX2 to ADC2.
- Write software hooks in `vfo.c`, `rx_menu.c`, and `radio.c` to slave VFO B to VFO A on every knob tick, step change, and mode switch.
- **Drawbacks:** DDCs in Protocol 2 run on separate paths (DDC2 and DDC3); any sample rate difference or software delay can introduce phase drift or buffer misalignments. Requires intercepting multiple VFO and CAT pathways.

### Approach B: The Diversity Ingestion Pipeline (Recommended)
- Utilize the existing FPGA diversity mode (`diversity_enabled = 1`).
- In Protocol 2 (`src/new_protocol.c:L1425-L1444`), diversity sets DDC0's sync map byte 1363 to `0x02` (`sync DDC1 to DDC0`).
- **Hardware-Enforced Coherence:**
  - The FPGA locks DDC1 to DDC0 and delivers **interleaved sample pairs** ($I_0, Q_0, I_1, Q_1$) in the exact same UDP payload.
  - **Zero relative sample delay and zero relative clock drift.**
  - High-priority packets automatically write VFO A's frequency into both DDC0 and DDC1 (`high_priority_buffer_to_radio[13..16] = [9..12]`). Tuning VFO A automatically retunes both ADCs in hardware without software polling.
  - Sample rate, filter board selection, dither, and random are already synchronized in hardware.
  - Step attenuators are already managed by the Diversity menu (tied by default, or untied via **ATT** with gain-ratio compensation).
  - Panadapter zoom and pan are already synchronized between RX1 and RX2 (`src/radio.c:L2116-L2137`).

The Diversity path already solves every synchronization problem at the FPGA firmware level; it only requires altering how the received pairs are routed to the demodulators and audio sinks.

---

## 3. Operational Modes

Inside the Diversity pipeline, four presentation modes become possible:

```
                  ┌────────────────────────────────────────────────────────┐
                  │                FPGA Synced DDC Stream                  │
                  │              (i0, q0) = ADC1, (i1, q1) = ADC2          │
                  └───────────┬────────────────────────────────┬───────────┘
                              │                                │
                     ┌────────┴────────┐              ┌────────┴────────┐
                     │   ADC1 Stream   │              │   ADC2 Stream   │
                     └────────┬────────┘              └────────┬────────┘
                              │                                │
                 ┌────────────┴────────────┐      ┌────────────┴────────────┐
                 │       Receiver 0        │      │       Receiver 1        │
                 │   Demodulation (WDSP)   │      │   Demodulation (WDSP)   │
                 │    Filter, AGC, Audio   │      │    Filter, AGC, Audio   │
                 └────────────┬────────────┘      └────────────┬────────────┘
                              │                                │
                              ▼                                ▼
                         Left Ear                          Right Ear
                     (ADC1 Audio)                      (ADC2 Audio)
```

### Mode 1: Pure Dual-ADC Split (Raw ADC1-L / Raw ADC2-R)
- **Signal routing:**
  - `receiver[0]` (Left) is fed raw $(i_0, q_0)$.
  - `receiver[1]` (Right) is fed raw $(i_1, q_1)$.
- **Behavior:** Complete isolation between antennas. Left ear hears Antenna 1, Right ear hears Antenna 2.

### Mode 2: Phase-Steered Binaural Diversity (Acoustic Beamforming)
- **Signal routing:**
  - `receiver[0]` (Left) is fed raw $(i_0, q_0)$.
  - `receiver[1]` (Right) is fed phase-rotated and gain-scaled ADC2:
    $$i_1' = (div\_cos \cdot i_1 - div\_sin \cdot q_1) \cdot div\_norm$$
    $$q_1' = (div\_sin \cdot i_1 + div\_cos \cdot q_1) \cdot div\_norm$$
- **Behavior:** By turning the Diversity **Phase** slider ($\pm 180^\circ$) or running **Auto-Phasing**, the operator rotates the RF phase difference between ears. Because human hearing resolves direction via interaural phase differences, rotating $\phi$ causes incoming RF arrivals to pan spatially across the headphone stereo field. An interfering signal can be rotated to appear far to the left while the desired station remains centered.

### Mode 3: Nulled vs. Reference Comparison
- **Signal routing:**
  - `receiver[0]` (Left) is fed the combined diversity sum ($z_0 + w \cdot z_1$).
  - `receiver[1]` (Right) is fed raw ADC2 reference ($z_1$).
- **Behavior:** Allows instant acoustic comparison between the diversity-nulled output and the raw antenna signal.

### Mode 4: Sum in Left Ear, Difference in Right Ear ($\Sigma / \Delta$)
- **Signal routing:**
  - `receiver[0]` (Left) is fed the **Sum** ($\Sigma$):
    $$z_\Sigma = z_0 + w \cdot z_1$$
    $$i_\Sigma = (i_0 + (div\_cos \cdot i_1 - div\_sin \cdot q_1)) \cdot div\_norm$$
    $$q_\Sigma = (q_0 + (div\_sin \cdot i_1 + div\_cos \cdot q_1)) \cdot div\_norm$$
  - `receiver[1]` (Right) is fed the **Difference** ($\Delta$):
    $$z_\Delta = z_0 - w \cdot z_1 = z_0 + (-w) \cdot z_1$$
    $$i_\Delta = (i_0 - (div\_cos \cdot i_1 - div\_sin \cdot q_1)) \cdot div\_norm$$
    $$q_\Delta = (q_0 - (div\_sin \cdot i_1 + div\_cos \cdot q_1)) \cdot div\_norm$$
- **Behavior & Operational Significance:**
  - This is the classic Sum/Difference ($\Sigma / \Delta$) interferometry / array processing configuration adapted for binaural listening.
  - When $w$ is aligned to peak the desired on-axis signal (via Diversity controls or Auto-Phasing Sum objective):
    - **Left Ear ($\Sigma$):** Receives the reinforced constructive interference pattern (+3 dB to +6 dB array gain, maximized SNR).
    - **Right Ear ($\Delta$):** Receives the destructive interference cancellation pattern (a deep spatial null on the on-axis signal, stripping away the loud carrier/voice and exposing off-axis noise, low-level multi-path flutter, or co-channel interference).
  - **Connection to Invert:** Subtracting $w \cdot z_1$ is mathematically identical to rotating the diversity weight by $180^\circ$ ($w \cdot e^{j\pi}$), which is what `diversity_auto_invert()` does in the auto-phasing engine. In Mode 4, instead of manually toggling Invert on and off to hear the null, the listener hears both the **peaked sum** in the left ear and the **inverted null** in the right ear concurrently.
  - **Acoustic Null Tuning:** Provides immediate acoustic feedback when hand-tuning the phase/gain sliders or observing auto-phasing convergence: as the null sharpens, the target signal reaches maximum strength in the left ear while completely extinguishing in the right ear.

---

## 4. Parameter Synchronization Specification

When Binaural Diversity is active:

| Parameter | Slaved to RX1? | Details |
|---|---|---|
| **VFO Frequency** | **Yes (Hardware)** | Handled automatically by FPGA DDC sync; VFO A tunes both ADCs. |
| **Sample Rate** | **Yes (Hardware)** | Both DDCs share `receiver[0]->sample_rate`. |
| **Demodulation Mode** | **Yes (Software)** | RX2 automatically adopts RX1's mode (USB, LSB, CW, AM, SAM, FM). |
| **Filter Passband** | **Yes (Software)** | RX2 adopts RX1's `filter_low`, `filter_high`, and filter preset. |
| **Squelch** | **Yes (Software)** | RX2 adopts RX1's `squelch_enable` and `squelch` threshold. |
| **Mute / Mute When Not Active** | **Yes (Software)** | Both `receiver[0]->mute_when_not_active` and `receiver[1]->mute_when_not_active` are set to `0` so neither ear cuts out when switching active focus. |
| **Dither & Random** | **Yes (Hardware)** | Protocol packets copy ADC0 settings to ADC1. |
| **Attenuators** | **Flexible** | Uses Diversity **ATT** logic: tied by default, or split with independent dB adjustments. |
| **SAM Sidebands** | **NO (Independent)** | **`sam_sb_mode` remains independent.** In SAM mode, the operator can configure RX1 to LSB and RX2 to USB, providing synchronous AM stereo / independent sideband reception. |
| **AF Gain / Volume** | **Independent** | Allows balancing headphone volume between dissimilar antennas. |

---

## 5. Audio Sink Pipeline Design

### 5.1 Local Soundcard Output (`src/audio.c`, `pulseaudio.c`, `pipewire.c`)
- `receiver[0]->audio_channel` is set to `LEFT`.
- `receiver[1]->audio_channel` is set to `RIGHT`.
- In `rx_process_buffer()`:
  - RX0 writes $(L_0, 0.0)$ to its audio handle.
  - RX1 writes $(0.0, R_1)$ to its audio handle.
- With modern audio backends (PulseAudio, PipeWire, ALSA `dmix`), both streams are mixed into the selected output card, delivering clean stereo separation.

### 5.2 Hardware Radio Headphone Jack (`old_protocol.c`, `new_protocol.c`)
Currently, `src/receiver.c:L1218` contains a restriction:
```c
if (rx == active_receiver) {
  switch (protocol) {
  case ORIGINAL_PROTOCOL: old_protocol_audio_samples(left_sample, right_sample); break;
  case NEW_PROTOCOL:      new_protocol_audio_samples(left_sample, right_sample); break;
  }
}
```
Because of `rx == active_receiver`, background receivers are never sent to the radio's onboard DAC.

**Proposed Solution:**
In `rx_process_buffer()`:
Because RX0 and RX1 process buffers sequentially in the same RX thread:
1. When `rx->id == 0` and Binaural Diversity is active, store `left_sample` into a static array `binaural_left_buffer[i]`.
2. When `rx->id == 1` and Binaural Diversity is active, combine `binaural_left_buffer[i]` and `right_sample` into a single stereo pair and dispatch to `new_protocol_audio_samples(binaural_left_buffer[i], right_sample)`.
3. This enables full binaural dual-ADC listening directly from the radio's physical front-panel headphone jack.

---

## 6. Detailed Implementation Blueprint

### 6.1 State Definitions (`src/radio.h`, `src/radio.c`)
Introduce global state flags:
```c
extern int div_binaural;          // 0 = standard RF sum, 1 = binaural split
extern int div_binaural_steered;  // 0 = raw ADC1/ADC2, 1 = phase-steered ADC2
```
Save and restore `div_binaural` and `div_binaural_steered` in property routines (`radio_save_state` / `radio_restore_state`).

### 6.2 Diversity Menu Integration (`src/diversity_menu.c`)
In `diversity_menu()`, add controls to the top bar beside `Div` and `ATT`:
```c
GtkWidget *binaural_b = gtk_check_button_new_with_label("Binaural");
gtk_widget_set_tooltip_text(binaural_b,
                            "Present ADC1 in Left ear and ADC2 in Right ear.\n"
                            "Uses coherent DDC streams to provide binaural spatial reception.");
gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(binaural_b), div_binaural);
gtk_box_pack_start(GTK_BOX(topbox), binaural_b, FALSE, FALSE, 0);
g_signal_connect(binaural_b, "toggled", G_CALLBACK(binaural_cb), NULL);
```

When `div_binaural` is toggled:
1. Automatically call `radio_change_receivers(2)` if `receivers < 2`.
2. Set `receiver[0]->audio_channel = LEFT;` and `receiver[1]->audio_channel = RIGHT;`.
3. Set `receiver[0]->mute_when_not_active = 0;` and `receiver[1]->mute_when_not_active = 0;`.
4. Call `rx_sync_diversity_binaural()` to synchronize mode, filter, and squelch from RX1 to RX2.

### 6.3 Combiner Modifications (`src/receiver.c`)
Modify `rx_add_div_iq_samples()`:
```c
void rx_add_div_iq_samples(RECEIVER *rx, double i0, double q0, double i1, double q1) {
  ASSERT_SERVER();

  if (div_auto_running) {
    diversity_auto_sample(i0, q0, i1, q1);
  }

  if (div_binaural && receivers > 1) {
    switch (div_binaural_mode) {
    case DIV_BIN_RAW: // Mode 1: Pure ADC1 (Left) / Pure ADC2 (Right)
      rx_add_iq_samples(receiver[0], i0, q0);
      rx_add_iq_samples(receiver[1], i1, q1);
      break;

    case DIV_BIN_STEERED: { // Mode 2: ADC1 (Left) / Phase-Steered ADC2 (Right)
      double i1_rot = (div_cos * i1 - div_sin * q1) * div_norm;
      double q1_rot = (div_sin * i1 + div_cos * q1) * div_norm;
      rx_add_iq_samples(receiver[0], i0, q0);
      rx_add_iq_samples(receiver[1], i1_rot, q1_rot);
      break;
    }

    case DIV_BIN_REF: { // Mode 3: Combined (Left) / ADC2 Reference (Right)
      double i_sum = (i0 + (div_cos * i1 - div_sin * q1)) * div_norm;
      double q_sum = (q0 + (div_sin * i1 + div_cos * q1)) * div_norm;
      rx_add_iq_samples(receiver[0], i_sum, q_sum);
      rx_add_iq_samples(receiver[1], i1, q1);
      break;
    }

    case DIV_BIN_SUM_DIFF: { // Mode 4: Sum in Left (Sigma) / Difference in Right (Delta)
      double i_w = (div_cos * i1 - div_sin * q1) * div_norm;
      double q_w = (div_sin * i1 + div_cos * q1) * div_norm;
      rx_add_iq_samples(receiver[0], (i0 + i_w), (q0 + q_w)); // Sum (constructive)
      rx_add_iq_samples(receiver[1], (i0 - i_w), (q0 - q_w)); // Difference (destructive null)
      break;
    }
    }
  } else {
    // Standard Diversity RF vector combination
    double i_sample = (i0 + (div_cos * i1 - div_sin * q1)) * div_norm;
    double q_sample = (q0 + (div_sin * i1 + div_cos * q1)) * div_norm;
    rx_add_iq_samples(rx, i_sample, q_sample);
  }
}
```

In `src/new_protocol.c:L2193` and `src/old_protocol.c:L1376`:
```c
// Guard auxiliary feed to avoid duplicating samples when div_binaural is active
if (!div_binaural && receivers > 1 && (receiver[0]->sample_rate == receiver[1]->sample_rate)) {
  rx_add_iq_samples(receiver[1], leftsampledouble1, rightsampledouble1);
}
```

### 6.4 Synchronization Hook (`src/receiver.c`)
Add synchronization helper `rx_sync_diversity_binaural()`:
```c
void rx_sync_diversity_binaural(void) {
  if (!div_binaural || receivers < 2 || !receiver[0] || !receiver[1]) { return; }
  
  RECEIVER *rx0 = receiver[0];
  RECEIVER *rx1 = receiver[1];

  // Sync Mode
  if (vfo[VFO_B].mode != vfo[VFO_A].mode) {
    vfo[VFO_B].mode = vfo[VFO_A].mode;
    profiles_load_rxtx_profile(rx1);
    rx_mode_changed(rx1);
  }

  // Sync Filter
  if (vfo[VFO_B].filter != vfo[VFO_A].filter ||
      rx1->filter_low != rx0->filter_low ||
      rx1->filter_high != rx0->filter_high) {
    vfo[VFO_B].filter = vfo[VFO_A].filter;
    rx1->filter_low = rx0->filter_low;
    rx1->filter_high = rx0->filter_high;
    rx_filter_changed(rx1);
  }

  // Sync Squelch
  if (rx1->squelch_enable != rx0->squelch_enable || rx1->squelch != rx0->squelch) {
    rx1->squelch_enable = rx0->squelch_enable;
    rx1->squelch = rx0->squelch;
    rx_set_squelch(rx1);
  }

  // Preserve SAM sideband independence:
  // rx0->sam_sb_mode and rx1->sam_sb_mode are intentionally NOT modified.
}
```

Trigger `rx_sync_diversity_binaural()` in `rx_mode_changed()` and `rx_filter_changed()` whenever `rx->id == 0`.

---

## 7. Verification & Acceptance Criteria

1. **Audio Routing Verification**:
   - Disconnecting Antenna 2 results in audio only in the Left ear.
   - Disconnecting Antenna 1 results in audio only in the Right ear.
2. **Frequency Lock**:
   - Tuning VFO A with the main knob, mouse drag, or CAT command shifts the received frequency in both ears synchronously with zero beats or pitch differences.
3. **SAM Sideband Independence**:
   - On an AM carrier with Binaural mode engaged:
     - Setting RX1 to LSB produces lower-sideband audio in the Left ear.
     - Setting RX2 to USB produces upper-sideband audio in the Right ear.
4. **Hardware Headphone Jack Operation**:
   - Stereo separation is cleanly audible on the physical radio headphone socket without requiring a PC soundcard.
5. **Phase-Steered Operation (Mode 2)**:
   - Moving the Diversity Phase slider shifts the acoustic center of an on-air signal across the stereo soundstage without changing the overall signal amplitude.
6. **Sum / Difference Operation (Mode 4)**:
   - When configured for Sum/Difference, tuning a carrier to zero beat and nulling it with phase/gain controls causes the signal to peak strongly in the Left ear ($\Sigma$) while completely canceling out to noise in the Right ear ($\Delta$).
7. **Graceful Fallback**:
   - Unchecking **Binaural** returns piHPSDR to standard RF vector combining with no audio artifacts or panadapter desynchronization.

