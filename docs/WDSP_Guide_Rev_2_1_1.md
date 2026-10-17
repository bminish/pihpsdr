# WDSP Guide — Revision 2.1.1 (2026)
**Author:** Warren C. Pratt, NR0V  
**Copyright:** © 2016 – 2026 Warren C. Pratt, NR0V  
**Format:** Machine-Parseable & Human-Verifiable Markdown Reference Manual

---

## 1. Document Overview & Agentic Quick Reference

This document is the authoritative technical reference manual for the **WDSP** (Warren Pratt Digital Signal Processing) library, Revision 2.1.1 (October 2026). It is engineered for two primary audiences:
1. **AI / Autonomous Coding Agents:** Every function prototype, argument type, valid range, return code, enumeration value, buffer format, thread-safety rule, and pipeline dependency is explicitly specified for unambiguous code generation and parameter validation.
2. **Human Systems Engineers & Operators:** Full prose explanations, architectural trade-offs, operational best practices, ASCII signal-flow block diagrams, and mathematical derivations are retained to verify accuracy and completeness against the original PDF manual.

### Library Architecture Summary
WDSP organizes signal processing into a four-tier hierarchy:
```
+-----------------------------------------------------------------------------------+
| WDSP Top-Level Environment                                                        |
|  - FFTW Wisdom Management (WDSPwisdom)                                            |
|  - Out-of-Channel Utility Blocks (Wideband Blankers, Resamplers, Diversity, etc.)  |
|  - Frequency-Domain Display Units (Panadapters, Waterfalls, Stitching)            |
+-----------------------------------------------------------------------------------+
                                   |
         +-------------------------+-------------------------+
         |                                                   |
+-----------------------------------+   +-----------------------------------+
| Channel [channel 0..31]           |   | Channel [channel 1..31]           |
|  - Independent processing thread  |   |  - Independent processing thread  |
|  - Asynchronous ring buffers      |   |  - Asynchronous ring buffers      |
|  - Power-of-2 sample rate changes |   |  - Power-of-2 sample rate changes |
|  - Slew-up / slew-down ramps      |   |  - Slew-up / slew-down ramps      |
+-----------------------------------+   +-----------------------------------+
                 |                                       |
+-----------------------------------+   +-----------------------------------+
| Unit: RXA (Type 0) Receiver       |   | Unit: TXA (Type 1) Transmitter    |
|  - Input Shifter & Decimator      |   |  - Input Mic Conditioning         |
|  - Dual Bandpass Filters (Linear/ |   |  - Phase Rotator (Auto FC)        |
|    Min Phase, Notches)            |   |  - Noise Gate / DEXP Expander     |
|  - Demodulators (SSB, AM, SAM,    |   |  - TX Equalizer (NURBS / Spline)  |
|    FM, WBFM Stereo, Digital)      |   |  - Audio Leveler (AGC)            |
|  - WCPAGC (Zero-Overshoot, Hang)  |   |  - Continuous Freq Compressor(CFC)|
|  - Noise Reduction (LMS NR,       |   |  - Martinez Speech Processor      |
|    Spectral NR2, Deep NNR)        |   |  - CESSB Overshoot Controller     |
|  - Meters (Input, S, AGC)         |   |  - Look-Ahead ALC (Zero-Overshoot)|
|  - Audio Output Patch Panel       |   |  - PureSignal 3.0 Predistortion   |
+-----------------------------------+   +-----------------------------------+
```

---

## 2. WDSP Organization

A **Channel** is an independent container for a single **Unit**:
- Provides input and output buffering allowing the unit to execute asynchronously from external hardware and operating system threads.
- Owns a dedicated OS thread for all DSP processing computations.
- Maintains independent buffer sizes, sample rates, and slew state.
- Multiple channels run in parallel, series, or independently (e.g., dual receivers, sub-receivers, transmitter).

A **Unit** is a major operational entity:
- Predefined types:
  - `Type 0`: **RXA** (Receiver Unit)
  - `Type 1`: **TXA** (Transmitter Unit)
- Houses modular DSP **Blocks** and internal buffers.

A **Block** is an individual signal processing algorithm:
- May have multiple instances inside a unit (e.g., Bandpass Filter 0, 1, 2).
- Can be shared or reused across units.

---

## 3. Operation of the Channel

### 3.1 Instantiation and Initialization
A channel is instantiated via `OpenChannel(...)`:

```c
void OpenChannel (
    int channel, 
    int in_size, 
    int dsp_size, 
    int input_samplerate, 
    int dsp_rate, 
    int output_samplerate, 
    int type, 
    int state, 
    double tdelayup, 
    double tslewup, 
    double tdelaydown, 
    double tslewdown, 
    int bfo
);
```

#### Parameter Reference
| Parameter | Type | Valid Range / Format | Description |
| :--- | :--- | :--- | :--- |
| `channel` | `int` | `0` to `31` (expandable) | Unique channel identifier. |
| `in_size` | `int` | Power-of-two (e.g., `64`..`2048`) | Count of complex samples provided to input per exchange call. |
| `dsp_size` | `int` | Power-of-two (e.g., `64`..`1024`) | Internal DSP buffer size passed through unit blocks. Determines latency & FFT size. |
| `input_samplerate` | `int` | Samples / sec (e.g., `48000`, `192000`, `384000`, up to `6144000`) | Input sample rate from hardware ADC/DDC. |
| `dsp_rate` | `int` | Power-of-two ratio relative to in/out | Rate used internally for DSP filtering/processing (`48000` for RX non-FM, `192000` for FM/WBFM, `96000` for TXA). |
| `output_samplerate` | `int` | Power-of-two ratio relative to in | Rate of samples output to DAC/audio soundcard. |
| `type` | `int` | `0` = RXA, `1` = TXA | Unit housed in this channel. |
| `state` | `int` | `0` = OFF, `1` = ON | Initial operational state. If `1`, starts and slews up when data arrives. |
| `tdelayup` | `double` | Seconds (e.g., `0.010`) | Delay before starting up-slew ramp. |
| `tslewup` | `double` | Seconds (e.g., `0.025`) | Raised-cosine up-slew duration. |
| `tdelaydown` | `double` | Seconds (e.g., `0.000`) | Delay before starting down-slew ramp. |
| `tslewdown` | `double` | Seconds (e.g., `0.010`) | Raised-cosine down-slew duration. |
| `bfo` | `int` | `0` or `1` | Block For Output: if non-zero, exchange calls block until output is ready. |

---

### 3.2 Setting Channel State
Controls channel activation, shutdown, and smooth T/R switching:

```c
int SetChannelState (int channel, int state, int dmode);
```
- **Returns:** Prior channel state (`0` or `1`).
- `channel`: Channel ID.
- `state`: `0` = OFF, `1` = ON.
- `dmode`:
  - `0`: Asynchronous return. Returns immediately; shutdown/slew continues in the background.
  - `1`: Synchronous return. Blocks until down-slew, flush, and zeroing are complete.

#### State Transition Execution Patterns
1. **Receive-to-Transmit Transition (Single Receiver):**
   ```c
   // 1. Synchronously shut down receiver
   SetChannelState(0, 0, 1);
   // 2. Hardware T/R switch (mute RX ADC, activate TX PA)
   switch_hardware_to_tx();
   // 3. Asynchronously turn on transmitter
   SetChannelState(5, 1, 0);
   ```
2. **Parallel Shutdown for Multiple Receivers (Fast T/R):**
   ```c
   // 1. Initiate parallel down-slew for all RX channels
   SetChannelState(0, 0, 0);
   SetChannelState(1, 0, 0);
   // 2. Wait for each down-slew to finish (parallel completion)
   SetChannelState(0, 0, 1);
   SetChannelState(1, 0, 1);
   // 3. Switch hardware and enable TX
   switch_hardware_to_tx();
   SetChannelState(5, 1, 0);
   ```

> [!IMPORTANT]
> **Data Flow Requirement During State Changes:**  
> Hardware data flow MUST NOT be stopped before a down-slew completes. Without continuous sample delivery to `fexchange`, the channel cannot compute the down-slew or flush buffers.
> 
> **Timeout Behavior:**  
> If dataflow halts while `dmode == 1`, a built-in safety timeout expires after ~100–200 ms (100 iterations of 1 ms), abandoning the flush. To prevent unneeded delays, always maintain sample delivery across state transitions.

---

### 3.3 Dynamic Channel Configuration API
Primary parameters can be reconfigured dynamically on the control thread:

```c
void SetDSPBuffsize (int channel, int dsp_size);
void SetDSPSamplerate (int channel, int dsp_rate);
void SetInputSamplerate (int channel, int in_rate);
void SetInputBuffsize (int channel, int in_size);
void SetOutputSamplerate (int channel, int out_rate);
void SetAllRates (int channel, int in_rate, int dsp_rate, int out_rate);
void SetChannelTDelayUp (int channel, double time);
void SetChannelTSlewUp (int channel, double time);
void SetChannelTDelayDown (int channel, double time);
void SetChannelTSlewDown (int channel, double time);
void SetType (int channel, int type);
```

> [!NOTE]
> - `SetDSPBuffsize` and `SetDSPSamplerate` can be called while data is flowing; they internally mute, flush, reconfigure, and slew up smoothly.
> - `SetInputSamplerate`, `SetInputBuffsize`, `SetOutputSamplerate`, and `SetAllRates` require setting `state = 0` (with `dmode = 1`) before invocation.
> - `SetType` destroys the existing unit and instantiates a new unit with default settings.

---

### 3.4 Data Exchange Protocols
WDSP operates on a **time-slice exchange model**: input samples representing time $\Delta t$ are consumed, and output samples representing that same $\Delta t$ are returned.

#### Preferred Protocol: Interleaved Complex Double (`fexchange0`)
```c
void fexchange0 (int channel, double* in, double* out, int* error);
```
- `in`: Buffer of interleaved `{double I, double Q}` pairs (size = `2 * in_size` doubles).
- `out`: Buffer of interleaved `{double I, double Q}` pairs (size = `2 * out_size` doubles).
- `error`: Non-zero if CPU starves or underflow occurs (zeros returned in `out`).
- *Note:* In-place processing is fully supported (`in == out`).

#### Split Real Float Protocol (`fexchange2`)
```c
void fexchange2 (int channel, INREAL* Iin, INREAL* Qin, OUTREAL* Iout, OUTREAL* Qout, int* error);
```
- Uses separate arrays for I and Q components (typically defined as `float` in `comm.h`). Requires internal conversion overhead.

---

## 4. WDSP Wisdom Calculations

Many blocks employ the **FFTW** (Fastest Fourier Transform in the West) library. A wisdom file pre-computes optimal FFT execution plans for the host CPU:

```c
int WDSPwisdom (char* directory);
char* wisdom_get_status (void);
```
- `WDSPwisdom`:
  - Returns `0`: Pre-existing wisdom file successfully loaded.
  - Returns `1`: Wisdom file was newly computed and saved.
  - Returns `-1`: Error (NULL path or path length > 1023 characters).
- Call once during host application initialization before opening channels.
- `wisdom_get_status()` returns a human-readable progress string during initial generation.

---

## 5. The RXA Receiver Unit

### 5.1 Operating Parameters & Signal Levels
- **Recommended DSP Rates:**
  - Non-FM Modes (SSB, CW, AM, Digital): `dsp_rate = 48000`
  - FM & WBFM Broadcast Modes: `dsp_rate = 192000`
- **Buffer Size Trade-Offs:**
  - `dsp_size = 64`: Minimum latency (~1.33 ms at 48 kHz).
  - `dsp_size = 1024`: Minimum CPU overhead.
  - Recommended general range: `64` to `1024`.
- **Nominal Signal Levels:**
  - Input scaling convention: Signal magnitude $\sqrt{I^2 + Q^2} = 1.0 \equiv 0\text{ dBm}$.
  - Meters, Squelch thresholds, and AGC assume this scaling.
  - Maximum AGC input scaling override:
    ```c
    void SetRXAAGCMaxInputLevel (int channel, double level); // default = 1.0
    ```

---

### 5.2 RXA Processing Architecture & Signal Paths

```
Raw IQ In [in_rate]
       |
+--------------------------+
| 5.3.1 Frequency Shifter  | (Local Oscillator rotation before decimation)
+--------------------------+
       |
+--------------------------+
| 5.3.2 Half-Band Decimator| (Resamples in_rate -> dsp_rate, up to 6144 kHz in)
+--------------------------+
       |
+--------------------------+
| 5.3.3 Signal PreGen      | (Optional test source: Tone, Noise, Sweep)
+--------------------------+
       |
       +---> [ Tap: Input Peak & Average Meters ]
       |
       +---------------------------------------------+ (Wideband Copy for CW NNR & SNB)
       |                                             |
+------------------------------------+               |
| 5.3.5 Notched Bandpass Filter      |               |
|  - Main passband filtering         |               |
|  - Up to 1024 fixed notches        |               |
+------------------------------------+               |
       |                                             |
       +---> [ Tap: Post-filter Display Sender ]     |
       +---> [ Tap: S-Meter (Peak & Average) ]       |
       +---> [ Tap: AM Squelch Trigger ]             |
       |                                             |
+------------------------------------+               |
| 5.3.8 AM/SAM Demodulator           |               |
|  - Envelope or Synchronous AM      |               |
|  - Carrier Fade Leveling           |               |
|  - Sideband selection (DSB/LSB/USB)|               |
+------------------------------------+               |
       |                                             |
+------------------------------------+               |
| 5.3.10 / 5.3.11 FM / WBFM Demod    |               |
|  - Narrowband FM (De-emphasis, PLL)|               |
|  - WBFM Stereo (192 kHz)           |               |
+------------------------------------+               |
       |                                             |
+------------------------------------+               |
| 5.3.13 Spectral Noise Blanker (SNB)| <-------------+ (Uses wideband copy in SSB)
|  - LPC forward prediction          |
+------------------------------------+
       |
+------------------------------------+
| 5.3.14 Equalizer (Pre-AGC Position)|
+------------------------------------+
       |
+------------------------------------+
| Pre-AGC Noise Reduction Options:   |
|  - 5.3.17 Automatic Notch (ANF)    |
|  - 5.3.18 LMS Noise Reduction (NR) |
|  - 5.3.19 Spectral NR (NR2)        |
|  - 5.3.20 CW Neural NR (NNR) <-----+ (Runs on 200-3000 Hz training band)
+------------------------------------+
       |
+------------------------------------+
| 5.3.21 Bandpass Filter 2           | (Restores operator passband after CW NNR)
+------------------------------------+
       |
+------------------------------------+
| 5.3.16 AGC (Automatic Gain Control)|
|  - WCPAGC Zero-Overshoot           |
|  - Dual-Decay & Hang Engine        |
+------------------------------------+
       |
       +---> [ Tap: AGC Meter (Gain, Peak, Average) ]
       |
+------------------------------------+
| Post-AGC Processing Options:       |
|  - Post-AGC Equalizer              |
|  - Post-AGC ANF / NR / NR2         |
|  - 5.3.20 Voice Neural NR (NNR)    |
|  - 5.3.23 AM Carrier Block         |
|  - 5.3.24 CW Audio Peaking (APF)   |
|  - 5.3.25 Dolly RTTY Dual Filter   |
+------------------------------------+
       |
+------------------------------------+
| 5.3.26 Audio Output Patch Panel    |
|  - Volume, Balance, Binaural, Pan  |
+------------------------------------+
       |
+------------------------------------+
| 5.3.2 Output Audio Resampler       | (dsp_rate -> out_rate)
+------------------------------------+
       |
Audio Out [out_rate]
```

---

### 5.3 Detailed RXA Block Specifications & APIs

#### 5.3.1 Frequency Shifter
Complex local oscillator multiplication ($e^{j 2 \pi f_{\text{shift}} t}$) performing frequency translation before decimation. Used for click-tune and fine IF offsets.
```c
void SetRXAShiftRun (int channel, int run);     // run: 0 = OFF, 1 = ON [default = 1]
void SetRXAShiftFreq (int channel, double fshift);// fshift: Offset frequency in Hz [default = 0.0]
```

#### 5.3.2 Resamplers (Input Half-Band Decimator & Output Resampler)
- **Input Decimator:** High-performance multi-stage half-band filter. Handles input sample rates up to 6144 kHz with superior anti-aliasing and alias rejection.
- Automatically configured by channel sample rate parameters. No external API calls required.

#### 5.3.3 Test Signal Generator (PreGen)
Substitutes incoming hardware samples with synthetically generated test waveforms:
```c
void SetRXAPreGenRun (int channel, int run);       // 0 = OFF, 1 = ON [default = 0]
void SetRXAPreGenMode (int channel, int mode);     // 0 = Tone, 2 = Noise, 3 = Sweep, 99 = Silence [default = 2]
void SetRXAPreGenToneMag (int channel, double mag);// Peak amplitude [0.0..1.0, default = 1.0]
void SetRXAPreGenToneFreq (int channel, double freq);// Tone frequency in Hz [default = 1000.0]
void SetRXAPreGenNoiseMag (int channel, double mag);// Noise amplitude [0.0..1.0, default = 1.0]
void SetRXAPreGenSweepMag (int channel, double mag);// Sweep amplitude [0.0..1.0, default = 1.0]
void SetRXAPreGenSweepFreq (int channel, double freq1, double freq2); // Start & Stop Hz [defaults: -20000.0, +20000.0]
void SetRXAPreGenSweepRate (int channel, double rate); // Sweep rate in Hz/sec [default = 4000.0]
```

#### 5.3.4 Input, S, and AGC Meters
Retrieves calibrated signal and gain levels:
```c
double GetRXAMeter (int channel, int mt);
```
- **Returns:** Level in dBm ($0.0\text{ dBm} \equiv 1.0$ peak magnitude), except `AGC_gain` which returns dB.
- Meter IDs (`mt`):
  - `0`: `S_peak` (post-bandpass peak)
  - `1`: `S_average` (post-bandpass smoothed average)
  - `2`: `Input_peak` (raw input peak)
  - `3`: `Input_average` (raw input average)
  - `4`: `AGC_gain` (current gain reduction in dB)
  - `5`: `AGC_peak` (post-AGC peak)
  - `6`: `AGC_average` (post-AGC average)

#### 5.3.5 Notched Bandpass Filter
Dual-purpose brickwall FIR filter performing both primary bandpass filtering and arbitrary notch filtering:
```c
void RXANBPSetRun (int channel, int run);          // 0 = OFF, 1 = ON [default = 1]
void RXANBPSetWindow (int channel, int wintype);   // 0 = 4-term Blackman-Harris, 1 = 7-term Blackman-Harris [default = 0]
void RXANBPSetNotchesRun (int channel, int run);   // 0 = Notches OFF, 1 = Notches ON [default = 0]
void RXANBPSetAutoIncrease (int channel, int autoincr); // Automatically expand notch width for >100dB depth [default = 1]
void RXANBPGetMinNotchWidth (int channel, double* minwidth); // Query minimum notch width in Hz
void RXANBPSetTuneFrequency (int channel, double tunefreq);   // Radio center RF frequency in Hz
void RXANBPSetShiftFrequency (int channel, double shift);      // Radio IF shift in Hz
```

##### Notch Database Management (Up to 1024 Notches)
```c
void RXANBPGetNumNotches (int channel, int* nnotches);
int RXANBPAddNotch (int channel, int notch, double fcenter, double fwidth, int active);
int RXANBPGetNotch (int channel, int notch, double* fcenter, double* fwidth, int* active);
int RXANBPDeleteNotch (int channel, int notch);
int RXANBPEditNotch (int channel, int notch, double fcenter, double fwidth, int active);
```
- Return values: `0` on success, `-1` on error.
- Notches are indexed consecutively starting from `0`. Deleting a notch slides down higher indices.
- `fcenter` is specified in absolute RF Hz. WDSP compares `fcenter` against `tunefreq` + `shift` to place notches in the passband.

#### 5.3.6 Post-Filter Display Sender
Streams complex samples immediately after the main bandpass filter into an independent Display Unit:
```c
void SetRXASpectrum (int channel, int flag, int disp, int ss, int LO);
```
- `flag`: `0` = OFF, `1` = ON.
- `disp`: Destination Display Unit ID.
- `ss`: Sub-span index (usually `0`).
- `LO`: LO position index (usually `0`).

#### 5.3.7 AM Squelch
Volume-based squelch with raised-cosine transitions and SNR-dependent tail length:
```c
void SetRXAAMSQRun (int channel, int run);                // 0 = OFF, 1 = ON
void SetRXAAMSQThreshold (int channel, double threshold); // Threshold in negative dBm [default = -40.0]
void SetRXAAMSQMaxTail (int channel, double tail);        // Maximum tail duration in seconds [default = 1.50]
```

#### 5.3.8 AM / Synchronous AM (SAM) Demodulator
Envelope detector or PLL-locked synchronous AM carrier detector:
```c
void SetRXAAMDSBMode (int channel, int sbmode);    // 0 = Both sidebands, 1 = LSB only, 2 = USB only [default = 0]
void SetRXAAMDFadeLevel (int channel, int levelfade); // 0 = OFF, 1 = ON (replaces carrier during selective fades) [default = 1]
```

#### 5.3.9 Advanced Voice Squelch (SSQL)
Speech-presence detection distinguishing human phonemes from white and atmospheric noise:
```c
void SetRXASSQLRun (int channel, int run);                // 0 = OFF, 1 = ON
void SetRXASSQLThreshold (int channel, double threshold); // Normalized threshold [0.0..1.0, typical = 0.16]
void SetRXASSQLTauMute (int channel, double tau_mute);    // Muting time-constant [seconds, range 0.1..2.0]
void SetRXASSQLTauUnMute (int channel, double tau_unmute);// Unmuting time-constant [seconds, range 0.1..1.0]
```

#### 5.3.10 FM Broadcast Stereo Demodulator
Wideband FM stereo decoder for broadcast signals (88–108 MHz):
- **Mandatory Requirements:** `SetRXAMode(channel, 12)`, `dsp_rate = 192000`, `input_samplerate >= 192000`.
- Bandpass filters are bypassed internally to preserve full 180 kHz MPX bandwidth.
```c
void SetRXAWBFMdmph (int channel, int dmph_run, int dmph_continent);
// dmph_run: 1 = De-emphasis ON, 0 = OFF [default = 1]
// dmph_continent: 0 = North America (75 us), 1 = Europe (50 us) [default = 0]
int GetRXAWBFMStereoIndicator (int channel); // Returns 1 if 19 kHz pilot / stereo detected, 0 otherwise
```

#### 5.3.11 Narrowband FM Demodulator & 5.3.12 FM Squelch
Standard communications FM (300–3000 Hz audio) with 6 dB/octave de-emphasis:
```c
void SetRXAFMDeviation (int channel, double deviation);   // Peak deviation in Hz [e.g., 5000.0]
void SetRXACTCSSRun (int channel, int run);               // CTCSS sub-audible tone notch filter (0 = OFF, 1 = ON)
void SetRXACTCSSFreq (int channel, double freq);          // CTCSS tone frequency in Hz [e.g., 100.0]
void SetRXAFMLimRun (int channel, int run);               // Audio volume limiter (0 = OFF, 1 = ON)
void SetRXAFMLimGain (int channel, double gaindB);        // Limiter gain in dB [range 0.0..30.0]
void SetRXAFMAFFilter (int channel, double low, double high); // Post-demod audio bandpass in Hz [e.g., 300.0, 3000.0]
void SetRXAFMSQRun (int channel, int run);                // FM squelch (0 = OFF, 1 = ON)
void SetRXAFMSQThreshold (int channel, double threshold); // Squelch threshold [1.0 to 0.01]:
                                                          // threshold = pow(10.0, -2.0 * slider / 100.0)
```

#### 5.3.13 Spectral Noise Blanker (SNB)
Linear Predictive Coding (LPC) impulse blanker:
- Models speech formants via forward prediction: $\hat{y}(m) = \sum a_k y(m - k)$.
- Compares error $e(m) = y(m) - \hat{y}(m)$ against threshold to detect impulse bursts.
- Replaces corrupted samples by solving linear equations derived from coefficients $a_k$.
```c
void SetRXASNBARun (int channel, int run); // 0 = OFF, 1 = ON [default = 0]
```
> [!NOTE]
> SNB is automatically bypassed when Neural Noise Reduction (NNR) CW models are active.

#### 5.3.14 & 5.3.15 RX Equalizer (Continuous-Gain & Higher-Degree Splines)
Interpolates continuous gain across arbitrary frequency points:
```c
void SetRXAEQRun (int channel, int run);                   // 0 = OFF, 1 = ON [default = 0]
void SetRXAEQWintype (int channel, int wintype);           // 0 = Blackman-Harris 4, 1 = BH7, 2 = Kaiser [default = 2]
void SetRXAEQCtfmode (int channel, int mode);              // 0 = 4th-power rolloff, 1 = flat outside band [default = 0]
void SetRXAEQProfile (int channel, int nfreqs, double* F, double* G);
```
- `F`: Array of frequencies in Hz. `F[0]` is UNUSED. Points stored in `F[1]` through `F[nfreqs]`.
- `G`: Array of gains in dB. `G[0]` is PREAMP gain applied to all frequencies. Points in `G[1]` through `G[nfreqs]`.
- Higher-Degree NURBS Curve Control:
  ```c
  void SetRXAEQCurve (int channel, int degree, int r, int umethod);
  // degree: 0 = legacy linear, 1 = linear with draw, 2..16 = B-spline degree
  // r: 0 = unweighted B-spline, 1 = rational NURBS with weights
  // umethod: 0 = chordal based on CP positions, 1 = uniform knots
  void SetRXAEQWeights (int channel, int ncp, double* weights); // 0.0 < weights[i] < 100.0
  void GetRXAEQDraw (int channel, double* X, double* Y);        // Returns 1024 polyline (Hz, dB) points
  ```

#### 5.3.16 Automatic Gain Control (WCPAGC)
Full-featured DSP AGC engineered with zero overshoot, dual-decay impulse immunity, and hang:
- **Zero Overshoot:** Internal lookahead delay prevents DAC clipping and audio thumps.
- **Dual Decay:** Differentiates speech decays from impulse key-clicks; key-clicks decay quickly without deafening receiver.
- **Hang Architecture:** Freezes decay gain during conversational pauses.

```c
void SetRXAAGCMode (int channel, int mode);
// mode: 0 = OFF (fixed gain), 1 = LONG, 2 = SLOW, 3 = MED, 4 = FAST, 5 = CUSTOM [default = 3]

void SetRXAAGCAttack (int channel, int attack);     // Attack time constant in ms [default = 1]
void SetRXAAGCDecay (int channel, int decay);       // Decay time constant in ms [default = 250]
void SetRXAAGCHang (int channel, int hang);         // Hang hold time in ms [default = 250]
void SetRXAAGCSlope (int channel, int slope);       // 10 * Slope in dB [default = 35 => 3.5 dB]
void SetRXAAGCFixed (int channel, double fixed_agc);// Fixed gain in dB when mode == 0 [default = 60.0]
void SetRXAAGCTop (int channel, double max_agc);    // Maximum AGC gain (Threshold) in dB [-20..+120 dB, default = 80]
void GetRXAAGCTop (int channel, double* max_agc);

// Panadapter Threshold Alignment (calibrates AGC line above noise floor)
void SetRXAAGCThresh (int channel, double thresh, double size, double rate);
void GetRXAAGCThresh (int channel, double* thresh, double size, double rate);

// Hang Level on Panadapter / Slider
void SetRXAAGCHangLevel (int channel, double hanglevel);   // Bandscope dBm level
void GetRXAAGCHangLevel (int channel, double* hanglevel);
void SetRXAAGCHangThreshold (int channel, int hangthreshold); // Slider value [0..100]
void GetRXAAGCHangThreshold (int channel, int* hangthreshold);
```

#### 5.3.17 Automatic Notch Filter (ANF)
Normalized Variable-Leak LMS adaptive filter that tracks and notches out carriers and heterodynes:
```c
void SetRXAANFRun (int channel, int run);                  // 0 = OFF, 1 = ON [default = 0]
void SetRXAANFTaps (int channel, int taps);                // Number of FIR taps [default = 64]
void SetRXAANFDelay (int channel, int delay);              // Delay samples [default = 16]
void SetRXAANFGain (int channel, double gain);             // LMS adaptation gain [1e-6..1e-3, default = 1e-4]
void SetRXAANFLeakage (int channel, double leakage);       // Variable leakage factor [0.001..1.0, default = 0.1]
void SetRXAANFVals (int channel, int taps, int delay, double gain, double leakage);
void SetRXAANFPosition (int channel, int position);        // 0 = Pre-AGC, 1 = Post-AGC [default = 0]
```

#### 5.3.18 LMS Noise Reduction (NR)
Variable-Leak LMS adaptive FIR filter configured for correlation enhancement (noise rejection):
```c
void SetRXAANRRun (int channel, int run);                  // 0 = OFF, 1 = ON [default = 0]
void SetRXAANRTaps (int channel, int taps);                // Filter taps [default = 64]
void SetRXAANRDelay (int channel, int delay);              // Decorrelation delay samples [default = 16]
void SetRXAANRGain (int channel, double gain);             // Adaptation step size [default = 1e-4]
void SetRXAANRLeakage (int channel, double leakage);       // Leak rate [default = 0.1]
void SetRXAANRVals (int channel, int taps, int delay, double gain, double leakage);
void SetRXAANRPosition (int channel, int position);        // 0 = Pre-AGC, 1 = Post-AGC [default = 0]
```

#### 5.3.19 Spectral Noise Reduction (NR2)
Frequency-domain Short-Time Spectral Amplitude (STSA) estimator based on Ephraim-Malah MMSE:
```c
void SetRXAEMNRRun (int channel, int run);                 // 0 = OFF, 1 = ON [default = 0]
void SetRXAEMNRgainMethod (int channel, int method);
// method: 0 = Gaussian linear, 1 = Gaussian log, 2 = Gamma distribution [default], 3 = Trained neural algorithm
void SetRXAEMNRnpeMethod (int channel, int method);
// method: 0 = OSMS (Optimal Smoothing Min Statistics), 1 = MMSE, 2 = NSTAT (Non-stationary) [default = 0]
void SetRXAEMNRPosition (int channel, int position);       // 0 = Pre-AGC, 1 = Post-AGC [default = 0]
void SetRXAEMNRtrainZetaThresh (int channel, double thresh);// Trained mode SNR threshold [-5.0..+5.0, default = -0.5]
void SetRXAEMNRtrainT2 (int channel, double t2);           // Trained mode weak-signal threshold [0.02..0.30, default = 0.20]

// Psychoacoustic Post-Processing & Artifact Removal
void SetRXAEMNRaeRun (int channel, int run);               // Artifact-Elimination Post-Filter (AEPF) [default = 1 (ALWAYS ON)]
void SetRXAEMNRpost2Run (int channel, int run);            // Psychoacoustic comfort noise injection [default = 0]
void SetRXAEMNRpost2Nlevel (int channel, double nlevel);   // Injection level [0.0..100.0, default = 15.0]
void SetRXAEMNRpost2Factor (int channel, double factor);   // Noise mix: 0.0 = original background, 100.0 = shaped white noise [default = 15.0]
void SetRXAEMNRpost2Rate (int channel, double tc);         // White noise syllable fade time-constant [seconds, default = 5.0]
void SetRXAEMNRpost2Taper (int channel, int taper);        // High-frequency rolloff taper [0..100, default = 12 for SSB]
```

#### 5.3.20 Deep Neural Network Noise Reduction (NNR)
State-of-the-art Deep Filtering neural network trained specifically on real-world off-air HF signals and amateur SSB/CW transmissions:
- Evaluates complex coefficients across 257 frequency bins and multiple frames (16 kHz internal rate, 512-point FFT). Corrects both magnitude and phase.
- Built-in models:
  - **Slot 0:** Voice Standard (261,834 parameters, 5 taps, 1 lookahead, 51.17 ms latency, ~10% single-thread CPU).
  - **Slot 1:** Voice Premium (584,746 parameters, 5 taps, 1 lookahead, 51.17 ms latency, +4.9 dB SDR, ~32% CPU).
  - **Slot 2:** CW Standard (266,328 parameters, 12 taps, 62.5 fps, 51.17 ms latency, ~11% CPU).
  - **Slot 3:** CW Premium (274,032 parameters, 24 taps, 125.0 fps, 43.17 ms latency, ~25% CPU).

```c
void SetRXANNRRun (int channel, int setit);              // 0 = OFF, 1 = ON [default = 0]
void SetRXANNRMaskFloor (int channel, double floor_db);   // Floor limit [-10.0 to -50.0 dB, default = -25.0 dB]
int SetRXANNRModel (int channel, int slot);               // Select slot 0..3. Returns active slot.
int GetRXANNRModel (int channel);                         // Query active model slot
void SetNNRModelPathSlot (int slot, const char* path);    // Custom model binary path override
```

> [!IMPORTANT]
> **CW Model Architecture & Second Bandpass Filter:**  
> When a CW model (slot 2 or 3) is selected in sideband modes (LSB, USB, CWL, CWU, DIGL, DIGU), WDSP automatically creates a wide 200–3000 Hz "training band" copy taken ahead of the Notched Bandpass Filter and feeds it to NNR ahead of the AGC. The operator's actual listening passband is then applied by **Bandpass Filter 2** (5.3.21) before reaching the AGC.
> Consoles MUST use `RXASetPassband(...)` collective so both filters receive the passband. Setting only `RXANBPSetFreqs` will narrow the S-meter but not the audible output!

#### 5.3.21 Bandpass Filter 2
Secondary FIR bandpass filter used for clean audio shaping in AM/SAM and whenever NNR CW models or post-AGC noise blocks are active:
```c
void SetRXABandpassWindow (int channel, int wintype); // 0 = BH4, 1 = BH7 [default = 1]
```

#### 5.3.22 Scope / Phase Display Sender
Siphons post-processing time-domain samples for oscilloscopes and phase monitors:
```c
void RXAGetaSipF (int channel, float* out, int size);  // Returns I-only float samples (size <= 4096)
void RXAGetaSipF1 (int channel, float* out, int size); // Returns interleaved {I, Q} float samples (size <= 4096)
```

#### 5.3.23 AM Carrier Block
High-pass DC notch filter removing carrier from demodulated AM audio prior to soundcard output (useful for recording clean audio for retransmission):
```c
void SetRXACBLRun (int channel, int run); // 0 = OFF, 1 = ON [default = 0]
```

#### 5.3.24 CW Audio Peaking Filter (APF)
Specialized resonant peaking filter centered on the CW sidetone:
```c
void SetRXASPCWRun (int channel, int run);                // 0 = OFF, 1 = ON [default = 0]
void SetRXASPCWSelection (int channel, int selection);
// selection: 0 = Double-Pole (high-Q FIR), 1 = Matched (max SNR), 2 = Gaussian (zero ringing), 3 = Bi-Quad (legacy IIR)
void SetRXASPCWFreq (int channel, double freq);           // Center pitch in Hz [default = 600.0]
void SetRXASPCWBandwidth (int channel, double bw);        // -6dB bandwidth in Hz [default = 100.0, typical 20..150]
void SetRXASPCWGain (int channel, double gain);           // Linear multiplier gain [default = 2.0]
```

#### 5.3.25 Dolly RTTY Peaking Filter
Multi-peak filter bank tuned for dual-tone FSK/RTTY demodulation:
```c
void SetRXAmpeakRun (int channel, int run);               // 0 = OFF, 1 = ON [default = 0]
void SetRXAmpeakNpeaks (int channel, int npeaks);         // Number of peaks [default = 2]
void SetRXAmpeakFilEnable (int channel, int fil, int enable); // Enable peak [fil = 0 or 1]
void SetRXAmpeakFilFreq (int channel, int fil, double freq);  // Defaults: fil 0 = 2125.0 Hz, fil 1 = 2295.0 Hz
void SetRXAmpeakFilBw (int channel, int fil, double bw);      // Bandwidth in Hz [default = 75.0]
void SetRXAmpeakFilGain (int channel, int fil, double gain);  // Linear gain [default = 1.0]
```

#### 5.3.26 Audio Output Patch Panel
Configures routing, channel balance, panning, and mono/binaural mapping:
```c
void SetRXAPanelRun (int channel, int run);               // 0 = OFF, 1 = ON [default = 1]
void SetRXAPanelSelect (int channel, int select);         // 0 = Mute, 1 = Q only, 2 = I only, 3 = Both I and Q [default = 3]
void SetRXAPanelGain1 (int channel, double gain);         // Master linear gain applied to both channels [default = 4.0]
void SetRXAPanelGain2 (int channel, double gainI, double gainQ); // Individual linear gains [default = 1.0, 1.0]
void SetRXAPanelPan (int channel, double pan);            // 0.0 = full left (I), 0.5 = center, 1.0 = full right (Q)
void SetRXAPanelCopy (int channel, int copy);             // 0 = none, 1 = copy I->Q, 2 = copy Q->I, 3 = swap I and Q
void SetRXAPanelBinaural (int channel, int bin);          // 0 = monaural (copy I->Q), 1 = binaural stereo
```

#### 5.3.27 RXA Collectives & Unified Controls (Recommended API)
Unified configuration routines that simultaneously configure all interdependent blocks:
```c
void SetRXAMode (int channel, int mode);
// Mode assignments:
// 0 = LSB, 1 = USB, 2 = DSB, 3 = CWL, 4 = CWU, 5 = FM, 6 = AM, 7 = DIGU, 
// 8 = SPEC, 9 = DIGL, 10 = SAM, 11 = DRM, 12 = Broadcast_FM_Stereo

void RXASetPassband (int channel, double f_low, double f_high);
// Sets passband limits across Notched Bandpass Filter, secondary Bandpass Filter 2, and S-meter.
// Mandatory for CW NNR models!

void RXASetNC (int channel, int nc);
// Sets impulse response length across all RX filters. Must be power of two >= dsp_size (recommended >= 2048).

void RXASetMP (int channel, int mp);
// mp: 0 = Linear Phase, 1 = Low Latency (Minimum Phase).
// Recommended: Minimum phase for SSB/CW; Linear phase for AM/SAM.
```

---

## 6. The TXA Transmitter Unit

### 6.1 Channel Parameters & Signal Characteristics
- **Recommended DSP Rate:** `dsp_rate = 96000` for all transmission modes.
- **Buffer Sizes:** `dsp_size` typically `64` (minimum latency) to `1024` (low CPU load).
- **Default Slew Timings:**
  - `tdelayup = 0.010` s, `tslewup = 0.025` s
  - `tdelaydown = 0.000` s, `tslewdown = 0.010` s
- **Signal Scaling:**
  - Audio Mic Input: Peak magnitude $\times$ Mic Gain $\approx 1.0 \equiv 0\text{ dBV}$.
  - RF Output: Interleaved complex $\{I, Q\}$ baseband signal with peak magnitude $\le 1.0$.

---

### 6.2 TXA Signal Flow Architecture

```
Mic Audio In (I or Q selected in PatchPanel)
       |
+------------------------------------+
| 6.3.1 Input Resampler              | (in_rate -> dsp_rate 96 kHz)
+------------------------------------+
       |
+------------------------------------+
| 6.3.2 PreGen Signal Generator      | (Optional test source: Noise, Tone, Sweep)
+------------------------------------+
       |
+------------------------------------+
| 6.3.3 PatchPanel                   | (Selects I or Q mic channel, applies Mic Gain)
+------------------------------------+
       |
       +---> [ Tap: Mic Meter (Peak & Average) ]
       |
+------------------------------------+
| 6.3.10 Phase Rotator               | (4 all-pass stages, minimizes speech asymmetry)
+------------------------------------+
       |
+------------------------------------+
| 6.3.5 Noise Gate                   | (Raised-cosine muting during pauses)
+------------------------------------+
       |
+------------------------------------+
| 6.3.6 Transmit Equalizer           | (Continuous-gain minimum-phase FIR)
+------------------------------------+
       |
       +---> [ Tap: Equalizer Meter ]
       |
+------------------------------------+
| 6.3.8 FM Pre-emphasis (Pos 0)      | [Optional position: before Leveler/CFC]
+------------------------------------+
       |
+------------------------------------+
| 6.3.9 Audio Leveler                | (Slow AGC: tau=500ms, max gain +5dB)
+------------------------------------+
       |
       +---> [ Tap: Leveler Meter & Gain ]
       |
+------------------------------------+
| 6.3.11 Continuous Freq Compressor  | (WOLA per-bin compression & Post-EQ)
+------------------------------------+
       |
       +---> [ Tap: CFC Meter & Gain ]
       |
+------------------------------------+
| 6.3.12 Bandpass Filter 0           | (Converts audio to analytic complex IQ)
+------------------------------------+
       |
+------------------------------------+
| 6.3.13 Speech Processor (Martinez) | (Baseband RF-style clipper)
+------------------------------------+
       |
       +---> [ Tap: Compressor Meter ]
       |
+------------------------------------+
| 6.3.12 Bandpass Filter 1           | (Removes clipper harmonics)
+------------------------------------+
       |
+------------------------------------+
| 6.3.14 CESSB Overshoot Control     | (Sliding lookahead envelope controller)
+------------------------------------+
       |
+------------------------------------+
| 6.3.12 Bandpass Filter 2           | (Final overshoot constraint filter)
+------------------------------------+
       |
+------------------------------------+
| 6.3.8 FM Pre-emphasis (Pos 1)      | [Default position: after preprocessing]
+------------------------------------+
       |
+------------------------------------+
| 6.3.15 Look-Ahead ALC              | (Rigid ceiling: tau=10ms, ZERO overshoot)
+------------------------------------+
       |
       +---> [ Tap: ALC Meter & Gain ]
       |
+------------------------------------+
| 6.3.16 AM / 6.3.17 FM Modulator    | (Injects carrier & CTCSS tones)
+------------------------------------+
       |
+------------------------------------+
| 6.3.2 PostGen Signal Generator     | (Tune tone, Two-tone test, Swept carrier)
+------------------------------------+
       |
+------------------------------------+
| 6.3.18 Up-Slew Controller          | (Smooth carrier ramp on T/R start)
+------------------------------------+
       |
+------------------------------------+
| 6.3.19 Siphon                      | (Taps raw/spec samples before distortion)
+------------------------------------+
       |
       +---> [ Tap: Output Meter ]
       |
+------------------------------------+
| 6.3.20 PureSignal 3.0 DPD          | (NURBS adaptive predistortion for PA)
+------------------------------------+
       |
+------------------------------------+
| 6.3.21 CFIR Filter                 | (CIC droop compensation for FPGA DUC)
+------------------------------------+
       |
+------------------------------------+
| 6.3.1 Output Resampler             | (dsp_rate -> out_rate)
+------------------------------------+
       |
Transmitter Complex IQ Out [out_rate]
```

---

### 6.3 Detailed TXA Block Specifications & APIs

#### 6.3.1 Resamplers
Converts `input_samplerate` to `dsp_rate` (96 kHz), and `dsp_rate` to `output_samplerate`. Fully automated based on channel parameters.

#### 6.3.2 PreGen & PostGen Signal Generators
- **PreGen (Microphone substitution):**
  ```c
  void SetTXAPreGenRun (int channel, int run);       // 0 = OFF, 1 = ON [default = 0]
  void SetTXAPreGenMode (int channel, int mode);     // 0=Tone, 2=Noise, 3=Sweep, 4=Sawtooth, 5=Triangle, 6=Pulse, 99=Silence
  void SetTXAPreGenToneMag (int channel, double mag);
  void SetTXAPreGenToneFreq (int channel, double freq);
  void SetTXAPreGenNoiseMag (int channel, double mag);
  void SetTXAPreGenSweepMag (int channel, double mag);
  void SetTXAPreGenSweepFreq (int channel, double freq1, double freq2);
  void SetTXAPreGenSweepRate (int channel, double rate);
  void SetTXAPreGenPulseMag (int channel, double mag);
  void SetTXAPreGenPulseFreq (int channel, double freq);       // Pulse recurrence rate in Hz [default = 0.25]
  void SetTXAPreGenPulseDutyCycle (int channel, double dc);     // 0.0 < dc < 1.0 [default = 0.25]
  void SetTXAPreGenPulseToneFreq (int channel, double freq);   // Burst tone Hz [default = 1000.0]
  void SetTXAPreGenPulseTransition (int channel, double transtime); // Raised-cosine edge time [default = 0.002 s]
  ```
- **PostGen (RF carrier, Tune, and Two-Tone Test):**
  ```c
  void SetTXAPostGenRun (int channel, int run);      // 0 = OFF, 1 = ON [default = 0]
  void SetTXAPostGenMode (int channel, int mode);    // 0=Tone, 1=Two-Tone, 3=Sweep, 6=Pulsed Tone, 7=Pulsed Two-Tone, 99=Silence
  void SetTXAPostGenToneMag (int channel, double mag);
  void SetTXAPostGenToneFreq (int channel, double freq);
  void SetTXAPostGenTTMag (int channel, double mag1, double mag2); // mag1 + mag2 <= 1.0 [default = 0.5, 0.5]
  void SetTXAPostGenTTFreq (int channel, double freq1, double freq2); // [default = 900.0, 1700.0 Hz]
  void SetTXAPostGenSweepMag (int channel, double mag);
  void SetTXAPostGenSweepFreq (int channel, double freq1, double freq2);
  void SetTXAPostGenSweepRate (int channel, double rate);
  void SetTXAPostGenPulseMag (int channel, double mag);
  void SetTXAPostGenPulseFreq (int channel, double freq);
  void SetTXAPostGenPulseDutyCycle (int channel, double dc);
  void SetTXAPostGenPulseToneFreq (int channel, double freq);
  void SetTXAPostGenPulseTransition (int channel, double transtime);
  void SetTXAPostGenPulseIQout (int channel, int IQout); // 0 = audio I-only, 1 = complex IQ [default = 0, use 1 for TX!]
  void SetTXAPostGenTTPulseMag (int channel, double mag1, double mag2);
  void SetTXAPostGenTTPulseFreq (int channel, double freq);
  void SetTXAPostGenTTPulseDutyCycle (int channel, double dc);
  void SetTXAPostGenTTPulseToneFreq (int channel, double freq1, double freq2);
  void SetTXAPostGenTTPulseTransition (int channel, double transtime);
  void SetTXAPostGenTTPulseIQout (int channel, int IQout);
  ```

#### 6.3.3 PatchPanel
Routes audio from physical soundcard buffers and establishes microphone gain:
```c
void SetTXAPanelRun (int channel, int run);       // 0 = OFF, 1 = ON [default = 1]
void SetTXAPanelSelect (int channel, int select); // 1 = Q channel, 2 = I channel [default = 2]
void SetTXAPanelGain1 (int channel, double gain); // Linear mic gain multiplier [default = 1.0]
```

#### 6.3.4 TX Meters
Monitors signal levels across every stage in the transmitter chain:
```c
double GetTXAMeter (int channel, int mt);
```
- **Returns:** Level in dBV ($0.0\text{ dBV} \equiv 1.0$ peak magnitude), except `Leveler_Gain`, `CFC_Gain`, and `ALC_Gain` which return gain in dB.
- Meter IDs (`mt`):
  - `0`: `MIC_Peak`, `1`: `MIC_Average`
  - `2`: `EQ_Peak`, `3`: `EQ_Average`
  - `4`: `Leveler_Peak`, `5`: `Leveler_Average`, `6`: `Leveler_Gain`
  - `7`: `CFC_Peak`, `8`: `CFC_AV`, `9`: `CFC_Gain`
  - `10`: `Compressor_Peak`, `11`: `Compressor_Average`
  - `12`: `ALC_Peak`, `13`: `ALC_Average`, `14`: `ALC_Gain`
  - `15`: `Output_Peak`, `16`: `Output_Average`

#### 6.3.5 Noise Gate
Attenuates background studio noise and blower noise during pauses in speech:
```c
void SetTXAAMSQRun (int channel, int run);                 // 0 = OFF, 1 = ON [default = 0]
void SetTXAAMSQMutedGain (int channel, double dBlevel);    // Attenuation when muted [negative dB, default = -13.98 dB]
void SetTXAAMSQThreshold (int channel, double threshold);  // Un-mute threshold [dB, default = -13.98 dB]
```

#### 6.3.6 & 6.3.7 Transmit Equalizer
Continuous-gain minimum-phase FIR equalizer:
```c
void SetTXAEQRun (int channel, int run);                   // 0 = OFF, 1 = ON [default = 0]
void SetTXAEQWintype (int channel, int wintype);           // 0=BH4, 1=BH7, 2=Kaiser [default = 2]
void SetTXAEQCtfmode (int channel, int mode);              // 0=4th-power rolloff, 1=flat outside band [default = 0]
void SetTXAEQProfile (int channel, int nfreqs, double* F, double* G);
void SetTXAEQCurve (int channel, int degree, int r, int umethod);
void SetTXAEQWeights (int channel, int ncp, double* weights);
void GetTXAEQDraw (int channel, double* X, double* Y);     // 1024 polyline (Hz, dB) points
```

#### 6.3.8 FM Pre-emphasis
Applies standard +6 dB/octave (+20 dB/decade) pre-emphasis curve between 300 and 3000 Hz:
```c
void SetTXAFMEmphPosition (int channel, int position);
// position: 0 = Before Leveler/CFC/ALC; 1 = After Leveler/CFC/ALC [default = 1]
```

#### 6.3.9 Audio Leveler
Slow-acting syllable AGC normalizing vocal dynamics prior to compression:
- Attack: $1\text{ ms}$ exponential time constant.
- Decay: $500\text{ ms}$ slow time constant.
- Maximum Gain: $+5.0\text{ dB}$ (prevents boosting ambient background noise).
```c
void SetTXALevelerSt (int channel, int state);        // 0 = OFF, 1 = ON [default = 0]
void SetTXALevelerAttack (int channel, int attack);   // ms [default = 1]
void SetTXALevelerDecay (int channel, int decay);     // ms [default = 500]
void SetTXALevelerTop (int channel, double maxgain);  // Max gain in dB [default = 5.0]
```

#### 6.3.10 Phase Rotator
All-pass filter cascade (4 stages) that alters differential phase relationships across frequencies to make speech waveforms symmetrical:
- Eliminates voice asymmetry without altering frequency spectrum.
- Increases average transmitted power by 2–15% without increasing peak DAC levels.
```c
void SetTXAPHROTRun (int channel, int run);               // 0 = OFF, 1 = ON [default = 0]
void SetTXAPHROTCorner (int channel, double frequency);   // All-pass 90-degree corner Hz [50..2000 Hz, default = 338.0]
void SetTXAPHROTNstages (int channel, int nstages);       // Cascaded stages [default = 4]
void SetTXAPHROTReverse (int channel, int reverse);       // 1 = Invert polarity (180 deg) before filter [default = 0]

// Automatic Corner Frequency Optimizer
void SetTXAPHROTAutoMode (int channel, int autoMode);     // 1 = Enable A/B optimizer search, 0 = Disable [default = 0]
void SetTXAPHROTAutoReset (int channel);                  // Reset search to 338.0 Hz default

// Asymmetry Telemetry Display
void GetTXAPHROTAsymmetry (
    int channel, 
    double* in_pos, double* in_neg, double* in_ratio, 
    double* out_pos, double* out_neg, double* out_ratio, 
    double* current_fc, double* auto_step
);
// in_ratio / out_ratio = |pos - neg| / (pos + neg). 0.0 = perfect symmetry, 1.0 = total asymmetry.
// auto_step = search step size in Hz; -1.0 when locked/converged; 0.0 when auto mode is off.
```

#### 6.3.11 Continuous Frequency Compressor (CFC) & Post-EQ
Frequency-domain Weighted OverLap-Add (WOLA) multiband processor:
- Independent compression profile ($G$) and Post-EQ profile ($E$) across FFT bins.
- Inherently phase-neutral: modifies only bin magnitudes; bin phase is untouched.
```c
void SetTXACFCOMPRun (int channel, int run);              // 0 = OFF, 1 = ON [default = 0]
void SetTXACFCOMPGprofile (int channel, int nfreqs, double* F, double* G); // Independent Compressor curve
void SetTXACFCOMPEprofile (int channel, int nfreqs, double* F, double* E); // Independent Post-EQ curve
void SetTXACFCOMPprofile (int channel, int nfreqs, double* F, double* G, double* E); // Legacy shared-frequencies call

void SetTXACFCOMPPrecomp (int channel, double precomp);   // Broadband offset compression in dB [default = 0.0]
void SetTXACFCOMPPeqRun (int channel, int run);           // Post-EQ enable (0 = OFF, 1 = ON) [default = 0]
void SetTXACFCOMPPrePeq (int channel, double prepeq);     // Post-EQ broadband pre-gain in dB [default = 0.0]

// Higher-Degree NURBS Spline Control
void SetTXACFCOMPCompCurve (int channel, int deg, int r, int umethod);
void SetTXACFCOMPCompWeights (int channel, int nfreq, double* weights);
void GetTXACFCOMPCompDraw (int channel, double* X, double* Y); // 1024 polyline points

void SetTXACFCOMPPeqCurve (int channel, int deg, int r, int umethod);
void SetTXACFCOMPPeqWeights (int channel, int nfreq, double* weights);
void GetTXACFCOMPPeqDraw (int channel, double* X, double* Y);

// Display Compression Depth
void GetTXACFCOMPDisplayCompression (int channel, double* comp_values, int* ready);
// Returns exactly 1025 decibels values from DC to fs/2. ready is 1 if updated data available.
```

#### 6.3.12 Bandpass Filters (Filter 0, 1, 2)
Three brickwall FIR filters in the transmit chain:
- **Filter 0:** Always active. Shapes audio passband and synthesizes analytic complex IQ.
- **Filter 1:** Automatically enabled when Speech Processor is active to remove clipping splatter.
- **Filter 2:** Automatically enabled when CESSB is active to clean up envelope peaks.
```c
void SetTXABandpassFreqs (int channel, double f_low, double f_high);
// For SSB: specify single-sided passband (e.g., LSB: -3000 to -200 Hz; USB: +200 to +3000 Hz)
// For AM/FM/DSB: specify symmetric passband (e.g., -5000 to +5000 Hz)
void SetTXABandpassWindow (int channel, int wintype); // 0 = BH4, 1 = BH7 [default = 0]
```

#### 6.3.13 Speech Processor (Baseband RF-Equivalent Clipper)
Implements Peter Martinez (G3PLX) baseband RF clipping algorithm:
- Envelope magnitude: $\text{mag} = \sqrt{I^2 + Q^2}$.
- If $\text{gain} \times \text{mag} > 1.0$: $I_{\text{out}} = I / \text{mag}$, $Q_{\text{out}} = 0$.
- Pure sine wave produces a pure sine wave with zero harmonic distortion.
```c
void SetTXACompressorRun (int channel, int run);     // 0 = OFF, 1 = ON [default = 0]
void SetTXACompressorGain (int channel, double gain);// Compression gain in dB [default = 3.0 dB]
```

#### 6.3.14 CESSB Overshoot Control
Controlled Envelope Single Sideband (Dave Hershberger, W9GR):
- Sliding-window lookahead peak limiter suppressing FIR filter overshoots.
- Recovers 2–3 dB average transmitted power without splatter.
- **Mandatory Requirements:** Speech Processor (6.3.13) MUST be active! Filters automatically switch to Linear Phase.
```c
void SetTXAosctrlRun (int channel, int run); // 0 = OFF, 1 = ON [default = 0]
```

#### 6.3.15 Look-Ahead Automatic Level Control (ALC)
Zero-overshoot protective peak limiter preventing DAC integer overflow and broadband splatter:
- Employs a 6 ms ring-buffer lookahead delay. Gain reduces smoothly *before* transients reach the output.
- Attack: $1\text{ ms}$, Decay: $10\text{ ms}$. Maximum Gain: $0.0\text{ dB}$ (never boosts).
- Must NEVER be disabled during normal radio transmission.
```c
void SetTXAALCSt (int channel, int state);         // 0 = OFF, 1 = ON [default = 1 (ALWAYS ON)]
void SetTXAALCAttack (int channel, int attack);    // ms [default = 1]
void SetTXAALCDecay (int channel, int decay);      // ms [default = 10]
void SetTXAALCMaxGain (int channel, double maxgain);// Max gain in dB [default = 0.0 dB]
```

#### 6.3.16 AM & 6.3.17 FM Modulators
- **AM Modulator:**
  ```c
  void SetTXAAMCarrierLevel (int channel, double c_level);
  // c_level: 0.0 = DSB (no carrier), 0.5 = 100% modulation, >0.5 = carrier reduced [default = 0.5]
  ```
- **FM Modulator:**
  ```c
  void SetTXAFMDeviation (int channel, double deviation); // Peak deviation in Hz [default = 5000.0]
  void SetTXACTCSSRun (int channel, int run);             // 0 = tone OFF, 1 = tone ON [default = 1]
  void SetTXACTCSSFreq (int channel, double freq);        // CTCSS frequency in Hz [default = 100.0]
  void SetTXAFMAFFilter (int channel, double low, double high); // Pre-mod audio bandpass in Hz [e.g., 300, 3000]
  ```

#### 6.3.18 Up-Slew Controller
Raised-cosine soft turn-on ramp applied specifically to internally generated carriers (AM/FM/TUNE):
```c
void SetTXAuSlewTime (int channel, double time); // Up-slew duration in seconds (should be < channel tslewup)
```

#### 6.3.19 Siphon
Taps uncorrupted transmit samples before PureSignal predistortion and CFIR filter for local display:
```c
void TXASetSipMode (int channel, int mode);      // 0 = buffered for polling, 1 = stream to display unit
void TXASetSipDisplay (int channel, int disp);   // Target display unit ID
void TXAGetaSipF (int channel, float* out, int size);  // Returns raw I-only samples (size <= 4096)
void TXAGetaSipF1 (int channel, float* out, int size); // Returns raw complex {I, Q} samples (size <= 4096)
void TXAGetSpecF1 (int channel, float* out);           // Returns 4096 magnitude points in dBV
void TXASetSipSpecmode (int channel, int mode);  // 0 = FFTW swapped halves, 1 = FFTW mirrored halves [default = 1]
```

#### 6.3.20 PureSignal 3.0 Adaptive Predistortion
Major architectural rewrite linearizing Power Amplifiers in software:
- Compares synchronized transmit drive samples (`tx`) and receiver PA feedback samples (`rx`).
- Models AM-AM and AM-PM distortion curves via **NURBS** and **LOWESS** regression, then compiles into fast splines.
- Native resilience against PA overdrive, low-level feedback noise, and network packet gaps.
- Corrects both adjacent-channel IMD splatter and hidden in-band distortion.

```c
// Real-time Synchronized Sample Delivery
void pscc (int channel, int size, double* tx, double* rx);
void SetPSFeedbackRate (int channel, int rate); // Feedback sample rate (samples/sec)

// State-Machine Control
void SetPSControl (int channel, int reset, int mancal, int automode, int turnon);
// (1, 0, 0, 0): Reset and turn OFF PureSignal
// (0, 1, 0, 0): Single calibration run (static correction)
// (0, 0, 1, 0): Automatic continuous adaptive calibration loop
// (0, 0, 0, 1): Turn ON and hold current loaded correction

void SetPSRunCal (int channel, int run);         // 0 = Pause calibration, 1 = Enable [default = 1]
void SetPSLoopDelay (int channel, double delay); // Delay between calibrations in seconds to save CPU [default = 0.0]
void SetPSMox (int channel, int mox);            // 0 = RX, 1 = TX (set mox=0 BEFORE setting channel state=0!)
void SetPSMoxDelay (int channel, double delay);  // Warmup delay before calibrating [default = 0.1 s]
double SetPSTXDelay (int channel, double delay); // Hardware PA delay compensation in seconds. Returns actual delay.
void SetPSHWPeak (int channel, double peak);     // Expected DUC peak magnitude [default = 0.4072 for openHPSDR]
void GetPSHWPeak (int channel, double* peak);
void GetPSMaxTX (int channel, double* maxtx);    // Query peak measured transmit level

// Telemetry & Diagnostic Info
void GetPSInfo (int channel, int* info);
// info array (16 ints):
// info[0]: rx_scale curve status
// info[1]: Magnitude curve status
// info[2]: cos(phase) curve status
// info[3]: sin(phase) curve status
// info[4]: Amplifier feedback level
// info[5]: Completed calibration count
// info[6]: Solution check flag: 2 indicates SEVERE PA OVERDRIVE!
// info[7]: Attempted calibration count
// info[14]: 1 if correction is actively applied
// info[15]: State-machine state ID

// AmpView Calibration Display Vectors
void GetPSDisp (
    int channel, 
    double* x, double* ym, double* yc, double* ys, 
    double* xm_cor, double* ym_cor, double* xa_cor, double* ya_cor, 
    int* nsamps_out, int* cpts_out, double* phs_ref_deg_out
);

// File Persistence
void PSSaveCorr (int channel, char* filename);
void PSRestoreCorr (int channel, char* filename);
```

#### 6.3.21 CFIR Filter
Compensates for passband droop of Cascaded Integrator-Comb (CIC) interpolation filters in FPGA DUC firmware:
```c
void SetTXACFIRRun (int channel, int run); // 0 = OFF, 1 = ON [default = 0 (enabled in openHPSDR New Protocol)]
```

#### 6.3.22 TXA Collectives & Unified Controls (Recommended API)
```c
void SetTXAMode (int channel, int mode);
// Mode assignments:
// 0 = LSB, 1 = USB, 2 = DSB, 5 = FM, 6 = AM, 7 = DIGU, 8 = SPEC, 9 = DIGL, 12 = AM_LSB, 13 = AM_USB

void TXASetNC (int channel, int nc); // Set impulse length across all TX filters (>= 2048 recommended)
void TXASetMP (int channel, int mp); // 0 = Linear Phase, 1 = Low Latency (Minimum Phase)
```

---

## 7. Panadapter & Other Frequency-Domain Displays

Independent display engines converting wideband IQ streams into rendered pixel arrays:

### 7.1 Lifecycle Management
```c
void XCreateAnalyzer (int disp, int* success, int m_size, int m_LO, int m_stitch, char* app_data_path);
// disp: Display ID [0..63]
// success: Returns 0 on successful creation
// m_size: Maximum FFT size (power of two up to 262144)
// m_LO: Number of LO positions for spur removal (1 for normal radio, >1 for Spectrum Analyzer)
// m_stitch: Number of DDC sub-spans to stitch (e.g., 1..8)
// app_data_path: Unused (pass NULL)

void DestroyAnalyzer (int disp);
```

### 7.2 Configuration API (`SetAnalyzer`)
```c
void SetAnalyzer (
    int disp, int n_pixout, int n_fft, int typ, int* flp, 
    int sz, int bf_sz, int win_type, double pi, int ovrlp, 
    int clp, double fscLin, double fscHin, int n_pix, int n_stch, 
    int calset, double fmin, double fmax, int max_w
);
```
- `n_pixout`: Pixel outputs generated per frame (e.g., `1` for panadapter only, `2` for panadapter + waterfall with independent averaging).
- `typ`: `0` = Real input (I only), `1` = Complex $\{I, Q\}$ input.
- `sz`: Active FFT size (power of two).
- `bf_sz`: Input chunk size per exchange.
- `win_type`:
  - `0`: Rectangular
  - `1`: 4-term Blackman-Harris
  - `2`: Hann
  - `3`: Flat-top (best amplitude accuracy)
  - `4`: Hamming
  - `5`: Kaiser (`pi = 14.0` recommended)
  - `6`: 7-term Blackman-Harris
- `ovrlp`: Overlap sample count to decouple display frame rate from sample rate:
  $$\text{overlap} = \max\left(0, \left\lceil \text{fft\_size} - \frac{\text{sample\_rate}}{\text{frame\_rate}} \right\rceil\right)$$
- `clp`: Symmetrical edge bins clipped to hide anti-aliasing filter rolloff (typically $1.7\%$ of `fft_size`).
- `fscLin`, `fscHin`: Fractional bins clipped on low and high edges to implement Pan and Zoom.
- `n_pix`: Width of rendered display in horizontal pixels.
- `n_stch`: Active sub-spans stitched into a single panoramic spectrum.
- `max_w`: Display buffer depth: $\text{fft\_size} + \min(0.1 \times \text{rate}, 0.1 \times \text{fft\_size} \times \text{frame\_rate})$.

#### Detector & Averaging Options
```c
void SetDisplayDetectorMode (int disp, int pixout, int mode);
// mode: 0 = Positive Peak, 1 = Rosenfell, 2 = Average, 3 = Sample, 4 = RMS

void SetDisplayAverageMode (int disp, int pixout, int mode);
// mode: -1 = Peak-Hold, 0 = None, 1 = Weighted Linear, 2 = Time-Window Linear, 3 = Weighted Log (aesthetic)

void SetDisplayNumAverage (int disp, int pixout, int num);       // Frame count for time-window averaging
void SetDisplayAvBackmult (int disp, int pixout, double mult);    // Weight factor: new = mult*old + (1-mult)*sample
void SetDisplaySampleRate (int disp, int rate);
void SetDisplayNormOneHz (int disp, int pixout, int norm);       // 1 = Normalize to 1 Hz equivalent noise bandwidth
void ResetPixelBuffers (int disp);                               // Flushes display pipeline
```

### 7.3 Data Ingestion & Pixel Retrieval
- **Separate IQ Buffers:**
  ```c
  void Spectrum (int disp, int ss, int LO, dINREAL* pI, dINREAL* pQ);
  ```
- **Interleaved IQ Buffers:**
  ```c
  void Spectrum2 (int run, int disp, int ss, int LO, dINREAL* pbuff); // dINREAL = float or double
  void Spectrum0 (int run, int disp, int ss, int LO, double* pbuff);  // Explicit double
  ```
- **Pixel Retrieval:**
  ```c
  void GetPixels (int disp, int pixout, dOUTREAL* pix, int* flag);
  // pix: Buffer receiving n_pix values in dBm.
  // flag: Set to 1 if a fresh frame was rendered, 0 if unchanged.
  ```

### 7.4 Peak Detection Within Passband
```c
void SetupDetectMaxBin (int run, int disp, int ss, int LO, double rate, double fLow, double fHigh, double tau, int frame_rate);
double GetDetectMaxBin (int disp); // Returns maximum FFT bin power in dB within [fLow, fHigh]
```

---

## 8. Blocks Used Outside Channels

### 8.1 Preemptive Wideband Noise Blanker (NB / ANB)
Power-ratio threshold blanker zeroing impulse samples with raised-cosine tapers:
```c
// Identifier-Based API (id = 0..31)
void create_anbEXT (int id, int run, int buffsize, double samplerate, double tau, double hangtime, double advtime, double backtau, double threshold);
void destroy_anbEXT (int id);
void flush_anbEXT (int id);
void xanbEXT (int id, double* in, double* out); // In-place supported (in == out)

void SetEXTANBRun (int id, int run);            // 0 = OFF, 1 = ON
void SetEXTANBSamplerate (int id, int rate);
void SetEXTANBBuffsize (int id, int size);
void SetEXTANBTau (int id, double tau);         // Slew edge time [e.g., 0.0001 s]
void SetEXTANBHangtime (int id, double time);   // Hang time [e.g., 0.0001 s]
void SetEXTANBAdvtime (int id, double time);    // Lookahead advance time [e.g., 0.0001 s]
void SetEXTANBBacktau (int id, double tau);     // Wideband baseline averaging constant [e.g., 0.05 s]
void SetEXTANBThreshold (int id, double thresh);// Trigger ratio over baseline [range 15.0..500.0]
```

### 8.2 Interpolating Wideband Noise Blanker (NB2 / NOB)
Advanced blanker that interpolates through corrupted samples:
- Modes (`mode`):
  - `0`: Zero mode (replaces with 0.0)
  - `1`: Sample-Hold (holds last uncorrupted sample)
  - `2`: Mean-Hold (averages start and end samples)
  - `3`: Hold-Sample (holds sample at end of impulse)
  - `4`: Linear Interpolation across the impulse duration
```c
void create_nobEXT (int id, int run, int mode, int buffsize, double samplerate, double slewtime, double hangtime, double advtime, double backtau, double threshold);
void destroy_nobEXT (int id);
void flush_nobEXT (int id);
void xnobEXT (int id, double* in, double* out);

void SetEXTNOBRun (int id, int run);
void SetEXTNOBMode (int id, int mode);
void SetEXTNOBSamplerate (int id, int rate);
void SetEXTNOBBuffsize (int id, int size);
void SetEXTNOBTau (int id, double tau);
void SetEXTNOBHangtime (int id, double time);
void SetEXTNOBAdvtime (int id, double time);
void SetEXTNOBBacktau (int id, double tau);
void SetEXTNOBThreshold (int id, double thresh);
```

### 8.3 Standalone Rational Resamplers
- **Complex Double-Precision Resampler:**
  ```c
  void* create_resampleV (int in_rate, int out_rate);
  void destroy_resampleV (void* ptr);
  void xresampleV (double* in, double* out, int size, int* outsamps, void* ptr);
  ```
- **Audio Single-Precision Float Resampler:**
  ```c
  void* create_resampleFV (int in_rate, int out_rate);
  void destroy_resampleFV (void* ptr);
  void xresampleFV (float* in, float* out, int size, int* outsamps, void* ptr);
  ```

### 8.4 Variable Ratio Resampler (`varsamp`)
High-precision non-integer fractional resampler accommodating drifting clock domains:
- Resampling ratio = $\text{var} \times (\text{out\_rate} / \text{in\_rate})$, where `var` is typically $\approx 1.0$.
```c
void* create_varsampV (int in_rate, int out_rate, int R); // R = polyphase density factor (1024 recommended)
void destroy_varsampV (void* ptr);
void xvarsampV (double* input, double* output, int numsamps, double var, int* outsamps, void* ptr);
```

### 8.5 Sample-Rate Matcher (`rmatch`)
Autonomous closed-loop rate matching engine:
- Combines ring buffering, PID feedback, feed-forward rate tracking, and `varsamp`.
- Underflow/Overflow Protection: Slews down smoothly and inserts zeros or blends audio without audible clicks.
```c
void* create_rmatchV (int in_size, int out_size, int nom_inrate, int nom_outrate, int ringsize, double var);
void destroy_rmatchV (void* a);
void xrmatchIN (void* b, double* in);   // Ingests samples from input clock domain
void xrmatchOUT (void* b, double* out); // Delivers matched samples to output clock domain

void getRMatchDiags (void* b, int* underflows, int* overflows, double* var, int* ringsize, int* nring);
void resetRMatchDiags (void* b);
void forceRMatchVar (void* b, int force, double fvar);
```

### 8.6 Diversity Reception Mixer
Phase and gain alignment combining inputs from multiple synchronized ADCs/antennas to steer directional nulls:
$$\begin{aligned}
I_{\text{mod}} &= I \cdot I_{\text{rotate}} - Q \cdot Q_{\text{rotate}} \\
Q_{\text{mod}} &= Q \cdot I_{\text{rotate}} + I \cdot Q_{\text{rotate}}
\end{aligned}$$
```c
void create_divEXT (int id, int run, int nr, int size); // id = 0 or 1; nr <= 8 streams
void destroy_divEXT (int id);
void flush_divEXT (int id);
void xdivEXT (int id, int size, double** in, double* out);

void SetEXTDIVRun (int id, int run);
void SetEXTDIVBuffsize (int id, int size);
void SetEXTDIVNr (int id, int nr);
void SetEXTDIVOutput (int id, int output); // if output == nr: mixes all; otherwise routes stream[output]
void SetEXTDIVRotate (int id, int nr, double* Irotate, double* Qrotate);
```

### 8.7 Envelope Elimination and Restoration (EER / Envelope Tracking)
Splits transmitter complex IQ into constant-magnitude phase drive and envelope amplitude signals:
- `amiq` modes:
  - `0`: Constant magnitude phase-only IQ ($\text{mag} \equiv \text{pgain}$)
  - `1`: Full amplitude and phase IQ ($\text{drive} \times \text{pgain}$)
  - `2`: Envelope-only magnitude sent on I channel ($I = \text{mag} \times \text{pgain}, Q = 0$)
```c
void create_eerEXT (int id, int run, int size, int rate, double mgain, double pgain, int rundelays, double mdelay, double pdelay, int amiq);
void pSetEERRun (EER a, int run);
void pSetEERSize (EER a, int size);
void pSetEERSamplerate (EER a, int rate);
void pSetEERMgain (EER a, double gain);
void pSetEERPgain (EER a, double gain);
void pSetEERRunDelays (EER a, int run);
void pSetEERMdelay (EER a, double delay); // Delay in microseconds
void pSetEERPdelay (EER a, double delay);
void pSetEERAMIQ (EER a, int amiq);
```

### 8.8 Integrated Downward Expander (DEXP) & VOX
Unified 5-state expander machine (`LOW`, `ATTACK`, `HIGH`, `HOLD`, `DECAY`) with side-channel trigger filtering and lookahead:
```c
void create_dexp (
    int id, int run_dexp, int size, double* in, double* out, 
    int rate, double dettau, double tattack, double tdecay, double thold, 
    double exp_ratio, double hyst_ratio, double attack_thresh, 
    int nc, int wtype, double lowcut, double highcut, 
    int run_filt, int run_vox, int run_audelay, double audelay, 
    void (__stdcall *pushvox)(int id, int active), 
    int antivox_run, int antivox_size, int antivox_rate, double antivox_gain, double antivox_tau
);
void destroy_dexp (int id);
void flush_dexp (int id);
void xdexp (int id);

void SetDEXPRun (int id, int run);
void SetDEXPSize (int id, int size);
void SetDEXPIOBuffers (int id, double* in, double* out);
void SetDEXPRate (int id, double rate);
void SetDEXPDetectorTau (int id, double tau);       // Smoothing time-constant [default = 0.020 s]
void SetDEXPAttackTime (int id, double time);       // Raised-cosine attack [default = 0.005 s]
void SetDEXPReleaseTime (int id, double time);      // Raised-cosine release [default = 0.100 s]
void SetDEXPHoldTime (int id, double time);         // Hold timer [default = 0.250 s]
void SetDEXPExpansionRatio (int id, double ratio);  // Linear ratio [default = 3.16 (10 dB)]
void SetDEXPHysteresisRatio (int id, double ratio); // Hold threshold ratio [default = 0.794 (2 dB)]
void SetDEXPAttackThreshold (int id, double thresh);// Trigger threshold level
void SetDEXPRunSideChannelFilter (int id, int run); // 0 = OFF, 1 = ON
void SetDEXPLowCut (int id, double lowcut);         // Trigger low cut Hz [default = 500.0]
void SetDEXPHighCut (int id, double highcut);       // Trigger high cut Hz [default = 1500.0]
void SetDEXPRunVox (int id, int run);               // Enable VOX callback
void SetDEXPRunAudioDelay (int id, int run);        // Enable lookahead delay
void SetDEXPAudioDelay (int id, double delay);      // Lookahead duration [default = 0.060 s]
void GetDEXPPeakSignal (int id, double* peak);

// Anti-VOX (Speaker Cancellation)
void SetAntiVOXRun (int id, int run);
void SetAntiVOXSize (int id, int size);
void SetAntiVOXRate (int id, double rate);
void SetAntiVOXGain (int id, double gain);
void SetAntiVOXDetectorTau (int id, double tau);
void SendAntiVOXData (int id, int nsamples, double* data);
```

### 8.9 Bandpass Filter Characterization Utility (BFCU)
Generates frequency response curves of brickwall FIR filters for GUI display:
```c
int create_bfcu (int id, int min_size, int max_size, double rate, double corner, int points);
void destroy_bfcu (int id);
void getFilterCorners (int id, int* lower_index, int* upper_index);
void getFilterCurve (int id, int size, int w_type, int index_low, int index_high, double* segment);
```

### 8.10 Filter Impulse Response Cache
Caches computed filter impulse responses to eliminate recalculation pauses during bandwidth or mode changes:
```c
void init_impulse_cache (int use);            // Initialize caching system
void use_impulse_cache (int use);             // 1 = Enable cache, 0 = Bypass
void destroy_impulse_cache (void);
int save_impulse_cache (const char* path);    // Save to disk file. Returns 0 on success.
int read_impulse_cache (const char* path);    // Restore from disk file. Returns 0 on success.
```

---

## 9. WDSP Version Information

```c
int GetWDSPVersion (void);
```
- **Returns:** Version integer computed as $\text{Version} \times 100$.
- *Examples:*
  - Version `1.14` returns `114`.
  - Version `2.1.1` returns `211`.

---

## 10. Appendix A: Higher-Degree Curve Interpolation (NURBS / B-Splines)

Several WDSP processing blocks support higher-degree B-spline and Non-Uniform Rational B-Spline (NURBS) curve interpolation between specified control points. These blocks include:
- Transmit Equalizer (`SetTXABPEQ...`)
- Receive Equalizer (`SetRXABPEQ...`)
- Continuous Frequency Compressor (CFC) (`SetTXACFCOMP...`)
- Post-CFC Equalizer (`SetTXACFCOMPPeq...`)

This curve system enables smooth, arbitrary frequency-magnitude sculpting without the stepped, constrained behavior of traditional octave or 1/3-octave graphic equalizers.

### 10.1 Control Points & Local Support
A control point is defined as a coordinate pair $(f, y)$ where $f$ is frequency in Hertz and $y$ is amplitude in dB (gain for equalizers; compression depth for compressors).

- **Linear Interpolation (`degree = 1`):** Straight line segments connect adjacent control points.
- **Higher-Degree Interpolation (`degree >= 2`):** The curve passes through the endpoints but is drawn toward intermediate control points via "gravitational pull" without necessarily intersecting them.
- **Minimum Points Constraint:** The number of control points $N_{\text{cp}}$ must satisfy:
  $$N_{\text{cp}} \ge \text{degree} + 1$$
  *(For example, cubic spline `degree = 3` requires at least 4 control points).*
- **Local Support Property:** Unlike simple natural cubic splines (where moving a single point alters the curve across the entire spectrum), NURBS curves exhibit local support. Perturbing a control point only modifies the curve within adjacent knot spans. Continuity and smoothness ($C^1$ and $C^2$) are strictly preserved through the knot boundaries.

### 10.2 Polynomial Degree
The `degree` parameter defines the polynomial order of each piecewise segment:
- **`degree = 0`:** Legacy piecewise-linear algorithm. Does not generate graphical draw coordinates.
- **`degree = 1`:** B-spline linear interpolation with graphical polyline draw output.
- **`degree = 3` (Cubic):** The recommended standard. Provides smooth, natural spectral transitions without excessive deviation from intermediate control points.
- **`degree = 5, 7, \dots, 16`:** Higher-order degrees produce progressively smoother curves but increase deviation from intermediate points. In practice, `degree = 7` is the maximum recommended.

### 10.3 Weights
Each control point $i$ may be assigned a rational weight $w_i$ satisfying:
$$0.0 < w_i < 100.0$$
- Increasing $w_i$ increases the attraction of the curve toward point $i$.
- If all weights are identical, the rational basis functions reduce to standard non-rational B-splines.
- **UI Interaction Pattern:** Mouse scroll-wheel hovering over a control point adjusts its weight interactively. Updates to the DSP filter pipeline should be debounced until mouse activity pauses to prevent audio clicks.

### 10.4 Knot Placement (`umethod`)
The `umethod` parameter determines how internal knot vectors are distributed across the frequency axis:
- **`umethod = 0` (Recommended):** Knot positions are derived directly from control point frequencies ($x$-coordinates). Preserves visual correspondence between control point handles and the resulting curve shape.
- **`umethod = 1`:** Uniformly distributed knot vector.

### 10.5 Graphical Polyline Draw Output
When `degree >= 1`, the functions `GetTXABPEQDraw`, `GetRXABPEQDraw`, `GetTXACFCOMPCompDraw`, and `GetTXACFCOMPPeqDraw` return exactly 1024 $(X, Y)$ coordinate pairs:
- $X$ values represent frequencies in Hertz (not normalized) and are **non-uniformly spaced**.
- $Y$ values represent gain or compression depth in dB.
- **UI Requirement:** The GUI must render these points as a continuous **polyline** ($X_i, Y_i \to X_{i+1}, Y_{i+1}$), never as an equispaced array.

### 10.6 User Interface Implementation Guidance
1. **Control Point Manipulation:** Drag control points to adjust $(f, \text{dB})$. Defer real-time DSP recalculation until the mouse button is released to prevent buffer underruns and audio clicks.
2. **Adding Points:** Double-clicking on the curve adds a new control point at that frequency and gain.
3. **Weight Visualization:** Render a concentric halo or ring around each control point sized proportionally to its weight $w_i$.
4. **Degree Selector:** Provide discrete options for Linear (1), Cubic (3), and Quintic (5). Set Cubic (3) as the factory default.

---

## 11. Appendix B: Phase Response in Audio Filters

### 11.1 Fundamental Concepts
Any filter that modifies the amplitude spectrum of a signal inherently impacts the timing relationships between frequency components. Amplitude response and phase response are mathematically coupled via Kramers-Kronig / Hilbert transform relations.

#### 11.1.1 Group Delay
Group delay ($\tau_g$) is the negative derivative of phase with respect to angular frequency:
$$\tau_g(\omega) = -\frac{d\phi(\omega)}{d\omega}$$
- **Flat Group Delay:** All frequencies experience identical time delay. Signal waveform shape and peak-to-average envelope ratios are preserved.
- **Dispersive Group Delay:** Different frequencies arrive at different times, smearing fast transients (speech consonants, CW keying edges, digital symbols).

#### 11.1.2 Linear Phase vs. Minimum Phase (Low Latency)
- **Linear Phase FIR Filters:** Have symmetric impulse responses ($h[n] = h[N-1-n]$). Group delay is perfectly constant across all frequencies:
  $$\tau_g = \frac{N-1}{2 \cdot f_s}$$
  *Tradeoff:* Introduces substantial fixed pipeline latency.
  - **RXA Latency ($f_s = 48\text{ kHz}$):**
    $$\Delta t_{\text{lin}} \approx \frac{\text{Filter\_Size}}{96}\text{ ms} \quad (\text{e.g., Size } 4096 = 42.67\text{ ms})$$
  - **TXA Latency ($f_s = 96\text{ kHz}$):**
    $$\Delta t_{\text{lin}} \approx \frac{\text{Filter\_Size}}{192}\text{ ms} \quad (\text{e.g., Size } 4096 = 21.33\text{ ms}; \text{ Size } 16384 = 85.33\text{ ms})$$
- **Minimum Phase (Low-Latency) FIR Filters:** All poles and zeros are located strictly inside the unit circle. Achieves the specified amplitude response with the minimum possible phase shift and minimum group delay:
  *Tradeoff:* Group delay is non-flat, exhibiting localized peaks near steep transition skirts. Pipeline latency is essentially zero.

#### 11.1.3 Psychoacoustic Audibility Thresholds
- **Lipshitz, Pocock, and Vanderkooy (1982):** Demonstrated that phase distortion alone—without magnitude modification—is generally undetectable by human listeners on broadband signals.
- **Blauert and Laws (1978):** Established human audibility thresholds for group delay distortion:
  - $\sim 1.6\text{ ms}$ at $1\text{ kHz}$
  - Higher tolerance at frequencies $> 2\text{ kHz}$
  - Stricter sensitivity below $500\text{ Hz}$
- Group delay distortion is primarily audible on sharp transients (speech plosives, CW dit onsets). For continuous voice and steady-state signals, it is imperceptible.

---

### 11.2 Mode-by-Mode Analysis

#### 11.2.1 SSB Voice
SSB signals occupy $100\text{--}3000\text{ Hz}$. Filter skirts lie near active speech energy. However, group delay variations in WDSP minimum-phase filters remain well below the Blauert & Laws threshold. Intelligibility is governed by amplitude response (French & Steinberg, 1947).
- **Recommendation:** **Minimum Phase** is the practical default (zero latency). Linear Phase may be used if latency is not a concern.

#### 11.2.2 AM Reception vs. Transmission
- **AM Transmit:** The carrier is centered within the passband by design. Upper and lower sidebands experience identical group delay profiles. Minimum phase introduces zero sideband asymmetry. Either mode is acceptable.
- **AM Receive (Critical Difference):** Physical oscillator drift, tuning errors, and Doppler shifts ensure that a received carrier is never perfectly centered in the passband. In a minimum phase filter, the upper and lower sidebands experience unequal group delay. When recombined in the demodulator, this phase mismatch generates severe harmonic and intermodulation distortion.
  > [!IMPORTANT]
  > Synchronous AM (SAM) demodulation **cannot** correct sideband group delay asymmetry. SAM tracks the carrier phase at $\omega_c$, but cannot undo differential delay applied to sideband pairs prior to demodulation.
- **Recommendation:** **Linear Phase is mandatory for AM reception.**

#### 11.2.3 CW Reception
- **Main Bandpass Filter:** CW tones are centered in the passband far from transition edges. Group delay distortion at the center frequency is negligible. Minimum phase is recommended.
- **Audio Peak Filter (APF):** Ultra-narrow resonant bandpass ($10\text{--}150\text{ Hz}$). Minimum phase produces a large group delay peak at resonance. At keying speeds above $35\text{--}40\text{ WPM}$, linear phase APF provides slightly cleaner, more crisp dit transitions.

#### 11.2.4 Digital Modes (PSK, FT8, JS8Call)
Demodulators for HF digital modes are designed to tolerate severe ionospheric multipath and dispersion, which exceed filter phase distortion by orders of magnitude.
- **Recommendation:** **Minimum Phase** (zero latency penalty).

---

### 11.3 Phase Response in the TX Audio Console

#### 11.3.1 Transmit Equalizer
To provide fine low-frequency resolution, the TX Equalizer employs FIR filter sizes up to $N = 16,384$ ($85\text{ ms}$ latency at $96\text{ kHz}$). Linear phase would introduce unacceptable delay in sidetone monitoring and fast break-in operation. The TX Equalizer uses **Minimum Phase**; on-air propagation paths (multipath, fading) completely swamp any microscopic phase differences.

#### 11.3.2 Continuous Frequency Compressor (CFC) & Post-EQ
The CFC and Post-EQ operate in the frequency domain via Weighted OverLap-Add (WOLA) Short-Time Fourier Transform (STFT) processing.
- Gain adjustments modify only bin magnitudes: $|X(k)| \leftarrow G(k)|X(k)|$.
- Bin phase angles $\angle X(k)$ are preserved completely unaltered (Ephraim & Malah, 1984).
- **Result:** The CFC and Post-EQ are **strictly phase-neutral**. They introduce zero phase distortion across all compression depths.

---

### 11.4 Phase Mode Summary Matrix

| Processing Block / Mode | Linear Phase Characteristics | Minimum Phase Characteristics | Recommended Setting |
| :--- | :--- | :--- | :--- |
| **SSB Receive** | Flat group delay; $20\text{--}85\text{ ms}$ latency | Zero latency; imperceptible phase deviation | **Minimum Phase** (Low Latency) |
| **AM Transmit** | Identical sideband delay | Symmetrical when carrier centered | **Either** |
| **AM Receive (Envelope & SAM)** | Preserves sideband symmetry across carrier offsets | Asymmetric sideband group delay causes severe distortion | **Linear Phase (Mandatory)** |
| **CW Main Filter** | No audible benefit at normal bandwidths | Clean passband center; zero latency | **Minimum Phase** |
| **CW APF (Narrow)** | Flat group delay through peak | Concentrated group delay at center frequency | **Linear Phase** for $>35\text{ WPM}$ |
| **Digital Modes** | No benefit | Zero latency | **Minimum Phase** |
| **TX Equalizer** | $85\text{ ms}$ latency at $N=16384$ | Zero latency; audibly identical over HF | **Minimum Phase** (Hardcoded) |
| **CFC & Post-EQ** | N/A (Frequency domain WOLA) | N/A (Frequency domain WOLA) | **Phase-Neutral by Design** |

---

### 11.5 Academic References
1. **Lipshitz, S. P., Pocock, M., & Vanderkooy, J.** (1982). *On the Audibility of Midrange Phase Distortion in Audio Systems*. Journal of the Audio Engineering Society, 30(9), 580–595.
2. **Blauert, J., & Laws, P.** (1978). *Group Delay Distortions in Electroacoustical Systems*. Journal of the Acoustical Society of America, 63(5), 1478–1483.
3. **Ephraim, Y., & Malah, D.** (1984). *Speech Enhancement Using a Minimum Mean-Square Error Short-Time Spectral Amplitude Estimator*. IEEE Transactions on Acoustics, Speech, and Signal Processing, 32(6), 1109–1121.
4. **French, N. R., & Steinberg, J. C.** (1947). *Factors Governing the Intelligibility of Speech Sounds*. Journal of the Acoustical Society of America, 19(1), 90–119.
5. **ITU-R BS.1116-3** (2015). *Methods for the Subjective Assessment of Small Impairments in Audio Systems*. International Telecommunication Union.

---

## 12. Appendix C: The Transmit Audio Chain — Technical Operation & Engineering Guidance

### Pipeline Architecture
```
[Microphone / CODEC / ASIO / WAV]
                │
                ▼
      [Phase Rotator (4-Stage All-Pass)]
                │
                ▼
[DEXP / VOX / Anti-VOX (5-State Machine)]
                │
                ▼
   [TX Equalizer (NURBS Minimum Phase FIR)]
                │
                ▼
     [Leveler (Syllabic Slow Lookahead AGC)]
                │
                ▼
  [Continuous Frequency Compressor (WOLA STFT)]
                │
                ▼
      [Post-CFC Equalizer (WOLA STFT)]
                │
                ▼
[Martinez Speech Processor (Analytic Envelope Limiter)]
                │
                ▼
 [Bandpass Filter 1 (Linear Phase Hilbert BPF)]
                │
                ▼
 [CESSB Overshoot Controller (Peak Stretcher)]
                │
                ▼
 [Bandpass Filter 2 (Linear Phase Cleanup BPF)]
                │
                ▼
   [Look-Ahead ALC (Fast Peak Limiter)]
                │
                ▼
[Up-Slew / Siphon / PureSignal 3.0 / CFIR]
                │
                ▼
       [To DUC / DAC Hardware]
```

---

### 12.1 Audio Input
Monitored via `TXAMIC`. The nominal operating level should peak near $0\text{ dBFS}$ ($1.0$ linear peak magnitude). Subsequent processing blocks assume normalized headroom.

---

### 12.2 Phase Rotator

#### Speech Asymmetry & SDR DAC Limits
Human voice waveforms are naturally asymmetric, often exhibiting positive-to-negative peak ratios exceeding $2:1$ ($20\text{--}40\%$ asymmetry).
- In vintage vacuum-tube plate-modulated transmitters, asymmetric positive audio peaks (>100%) were advantageous because they extracted additional RF power from the high-voltage supply.
- In modern SDR Direct Up-Conversion (DUC) transmitters, the signal feeds a high-speed DAC with a hard integer ceiling ($\pm 1.0$). When asymmetric speech hits the DAC ceiling on one polarity, the opposite polarity has unused headroom. Waveform asymmetry wastes available RF average power.

#### Filter Topology & Automatic Optimization
The Phase Rotator passes audio through a cascade of 4 first-order all-pass filter sections:
$$H(z) = \frac{a + z^{-1}}{1 + a z^{-1}}$$
- Amplitude response is flat across all frequencies ($0\text{ dB}$).
- Phase shifts from $0^\circ$ at DC, to $90^\circ$ at the corner frequency $f_c$, to $180^\circ$ at Nyquist.
- Frequencies above $f_c$ undergo differential phase rotation relative to lower frequencies, shifting harmonic phase alignments and restoring waveform envelope symmetry.
- **Performance:** Reduces peak-to-average power ratio (PAPR) and improves symmetry by $2\text{--}15$ percentage points.
- **Corner Frequency $f_c$:** Voice asymmetry shows a broad, shallow minimum between $200\text{--}400\text{ Hz}$.
- **Auto-Optimizer:** Evaluates candidate corner frequencies over $10\text{--}15\text{ s}$ windows using an A/B search algorithm. Reaches convergence within $3\text{--}5\text{ minutes}$ of active speech.
- **Digital Mode Caution:** Must be bypassed (`SetTXAPhaseRotRun(channel, 0)`) for phase-coherent digital modes.

---

### 12.3 Integrated Downward Expander (DEXP), VOX, and Anti-VOX

#### Five-State Expander State Machine
The DEXP block supersedes basic hard noise gates by eliminating gating chatter and consonant truncation.

```
          [Signal > Attack Thresh]
 LOW ─────────────────────────────────► ATTACK
  ▲                                       │
  │ [Decay timer                          │ [Ramp completes]
  │  expires]                             ▼
DECAY ◄──────────────────────────────── HIGH
  ▲     [Hold timer                       │
  │      expires]                         │ [Signal < Hold Thresh]
  └─────────────── HOLD ◄─────────────────┘
```

1. **`LOW` (Gate Closed):** Applies downward expansion gain:
   $$\text{low\_gain} = \frac{1}{\text{expansion\_ratio}} \quad (\text{Default } 10\text{ dB} \rightarrow \text{gain} \approx 0.316)$$
   Anti-VOX audio subtraction is calculated only in this state.
2. **`ATTACK` (Opening):** Smoothly transitions gain from `low_gain` to $1.0$ using a raised-cosine curve:
   $$g(t) = \text{low\_gain} + (1 - \text{low\_gain}) \cdot \frac{1}{2}\left[1 - \cos\left(\frac{\pi t}{T_{\text{att}}}\right)\right]$$
   Fires VOX PTT callback immediately.
3. **`HIGH` (Gate Open):** Unity gain ($1.0$). Transparent audio passthrough during active speech.
4. **`HOLD` (Sustain):** Triggered when signal drops below $\text{hold\_thresh} = \text{attack\_thresh} \times \text{hysteresis\_ratio}$ ($2\text{ dB}$ hysteresis window). Maintains unity gain for the duration of the hold timer ($250\text{ ms}$ default) to bridge conversational pauses.
5. **`DECAY` (Release):** Ramps gain from $1.0$ down to `low_gain` using a raised-cosine curve ($100\text{ ms}$ default) to prevent abrupt noise-floor cutoffs.

#### Key Engineering Features
- **Look-Ahead Audio Delay:** Stores audio in a $60\text{ ms}$ ring buffer while feeding an un-delayed signal to the detector. The gate opens before soft word-initial consonants arrive, preventing word-clipping.
- **Side-Channel Trigger BPF:** Isolates speech fundamentals ($500\text{--}1500\text{ Hz}$) for threshold detection, ignoring desk thumps and ambient hiss.
- **Detector Tau:** First-order averaging time constant ($20\text{ ms}$ default) prevents false triggering on brief acoustic clicks.
- **Anti-VOX Integration:** Smooths receiver audio and raises the trigger threshold in the `LOW` state, preventing speaker bleed from tripping VOX.

---

### 12.4 Transmit Equalizer
A minimum-phase FIR equalizer utilizing NURBS curve interpolation.
- **Sub-300 Hz Management:** Male voices carry dense energy at $100\text{--}300\text{ Hz}$. Rolling off low frequencies below $200\text{--}300\text{ Hz}$ prevents low-frequency room resonance from monopolizing downstream compressor headroom.
- **Presence Lift ($1\text{--}3\text{ kHz}$):** Boosting $2.0\text{--}2.5\text{ kHz}$ by $+3\text{ to }+6\text{ dB}$ sharpens consonant intelligibility on crowded bands.
- **High-Frequency Rolloff ($>3\text{ kHz}$):** Rolling off energy above $3\text{ kHz}$ confines speech energy to the standard SSB passband prior to compression.

---

### 12.5 Leveler (Slow-Acting Syllabic AGC)
Normalizes long-term speech dynamics to provide a consistent drive level to the CFC and Speech Processor.
- **Time Constants:** Attack $\tau_{\text{att}} = 1\text{ ms}$; Decay $\tau_{\text{dec}} = 500\text{ ms}$ (50 times slower than the ALC).
- **Gain Range:** Maximum boost limited to $+5\text{ dB}$; attenuates on loud passages.
- **True Lookahead:** Ring buffer architecture prevents onset overshoot.
- **Role:** Rides gain over conversational sentences without dynamic pumping or breathing artifacts.

---

### 12.6 Continuous Frequency Compressor (CFC) & Post-EQ

#### WOLA Architecture & Spectral Limiters
The CFC processes audio in the frequency domain using Weighted OverLap-Add (WOLA) STFT processing.
- For each frequency bin $k$, the programmed gain $G(k)$ from the NURBS compression profile is evaluated against the bin magnitude $|X(k)|$.
- If $G(k)|X(k)| > 1.0$, bin gain is scaled to normalize the bin output to exactly $1.0$.
- Phase angles $\angle X(k)$ are preserved completely unaltered.
- **No Pumping:** Low-frequency vowels do not depress high-frequency consonant gains.

#### Multiband Peak Reconstruction & Post-EQ Level Management
When multiple independent frequency bins are driven near unity simultaneously, their inverse FFT sum in the time domain creates peak overshoots exceeding the input level by $6\text{--}10\text{ dB}$.
- **Post-EQ Pre-Gain Compensation:** To maintain correct gain staging into the downstream ALC, aggressive Pre-Comp gain (e.g., $+16\text{ dB}$) must be balanced by a corresponding negative Post-EQ pre-gain (e.g., $-20\text{ dB}$).
- **Compression Display:** Monitors real-time per-bin gain reduction with peak-hold and decay.
- **Digital Mode Caution:** Disable CFC for digital modes to prevent inter-bin spectral intermodulation.

---

### 12.7 Martinez Baseband Speech Processor
An analytic baseband equivalent of the hardware RF speech clipper, based on Peter Martinez (G3PLX, RadCom 2010):
1. Compute instantaneous envelope magnitude:
   $$M = \sqrt{I^2 + Q^2}$$
2. If $G \cdot M > 1.0$ (above clipping threshold):
   $$I_{\text{out}} = \frac{I}{M}, \quad Q_{\text{out}} = 0$$
3. If $G \cdot M \le 1.0$ (below clipping threshold):
   $$I_{\text{out}} = G \cdot I, \quad Q_{\text{out}} = 0$$

- **Sine Wave Invariant:** For a pure tone $I = A\cos(\omega t), Q = A\sin(\omega t)$, the envelope $M = A$ is constant. Dividing $I$ by $M$ yields $\cos(\omega t)$, outputting a pristine sine wave with **zero harmonic distortion**.
- **Prerequisite:** Required to be active when using CESSB Overshoot Control.

---

### 12.8 Controlled Envelope Single Sideband (CESSB) Overshoot Control
Introduced to amateur radio by Dave Hershberger (W9GR, QEX):
- **Problem:** Steep bandpass filtering of heavily compressed speech causes transient envelope ringing that overshoots the clipping ceiling by several dB, forcing the transmitter drive down and reducing average power.
- **Mechanism:** Employs a sliding-window peak detector ($\sim 10$ samples at $96\text{ kHz}$) with lookahead ("peak stretching"). Envelope peaks exceeding unity scale down the local gain across the window. The signal is then cleaned up through a second linear-phase bandpass filter.
- **Power Gain:** Recovers $2\text{--}3\text{ dB}$ of average transmitted power without splatter.
- **Linear-Phase Requirement:** Accurate Hilbert envelope calculation ($M = \sqrt{I^2 + Q^2}$) requires strict $90^\circ$ quadrature alignment across the passband. Minimum-phase filters introduce frequency-dependent phase skew that corrupts envelope detection, causing severe algorithm distortion.

---

### 12.9 Look-Ahead ALC (DAC Protection)
The final safeguard before DAC interpolation and hardware transmission.
- **Look-Ahead Window:** Operates via a $6\text{ ms}$ circular buffer. Analyzes upcoming peak amplitudes and applies a smooth exponential gain reduction before the peak reaches the output.
- **Zero Overshoot:** Eliminates the transient attack spikes inherent in feedback-based ALCs.
- **Time Constants:** Attack $\tau_{\text{att}} = 1\text{ ms}$; Decay $\tau_{\text{dec}} = 10\text{ ms}$.
- **DAC Protection:** DUC transmitters operate DACs near full dynamic range. Any sample exceeding $0\text{ dBFS}$ causes catastrophic integer wrapping/clipping, spraying broadband splatter across megahertz of spectrum. The ALC must never be disabled.
- **Operational Rule:** The ALC is a safety limiter, not a compressor. Upstream levels should be adjusted so the ALC meter indicates $0\text{ dB}$ gain reduction during normal speech, touching $-1\text{ to }-2\text{ dB}$ only on occasional peaks.

---

### 12.10 PureSignal 3.0 Adaptive Predistortion & In-Band IMD Reduction
Power amplifier non-linearities create two distinct categories of Intermodulation Distortion (IMD):
1. **Adjacent-Channel Splatter (Out-of-Band):** Appears as spectral shoulders on panadapters and causes adjacent-channel interference.
2. **In-Band IMD (Hidden Distortion):** Human voice fundamentals ($100\text{--}300\text{ Hz}$) mix inside a nonlinear PA, producing 3rd- and 5th-order products that fall directly back inside the $300\text{--}3000\text{ Hz}$ passband ($40\text{--}60\text{ dB}$ below peak). This causes audible harshness, muddiness, and listener fatigue.

PureSignal predistortion samples the transmitted RF waveform, models the amplifier's non-linear transfer function, and applies the exact mathematical inverse distortion. Because it linearizes the PA transfer characteristic at the source, **it eliminates adjacent-channel splatter and suppresses in-band IMD simultaneously**, dramatically improving transmitted audio clarity.

---

## 13. Appendix D: Intermodulation Distortion (IMD) & PureSignal

### 13.1 Physics of IMD
No RF amplifier is perfectly linear. PA distortion is characterized by two transfer anomalies:
- **AM-to-AM Conversion:** Gain compression or expansion as a function of instantaneous drive envelope.
- **AM-to-PM Conversion:** Phase rotation as a function of instantaneous drive envelope.

When multi-frequency signals traverse this non-linear transfer function, sum and difference mixing products are generated:
$$f_{\text{IMD3}} = 2f_1 - f_2, \quad 2f_2 - f_1$$
$$f_{\text{IMD5}} = 3f_1 - 2f_2, \quad 3f_2 - 2f_1$$

---

### 13.2 Predistortion Fundamentals
PureSignal samples the PA output via an RF directional coupler and feedback receiver. It compares the sampled feedback signal $y(t)$ against the baseband reference $x(t)$ to derive the complex inverse transfer function:
$$x_{\text{predist}}(t) = x(t) \cdot G^{-1}(|x(t)|) \cdot e^{-j\Phi(|x(t)|)}$$
When $x_{\text{predist}}(t)$ is amplified by the PA, the amplifier's compression and phase shift cancel the predistortion, restoring linear RF output.

---

### 13.3 Measurement Techniques: Two-Tone vs. Noise Testing
- **Broadband Noise Testing (Representative of Speech):** Driving the transmitter with band-limited Gaussian noise exercises all amplitude levels randomly and unpredictably, matching the envelope statistics of human speech. Noise spectral regrowth directly reflects real-world on-air splatter.
- **Two-Tone Testing Pitfalls in Modern SDRs:**
  1. *Deterministic Envelope Periodicity:* Two equal tones produce an envelope that sweeps repetitively between zero and peak. The PA traverses the exact same trajectory millions of times, rewarding accidental curve optimizations at specific levels that do not benefit random speech.
  2. *Tone Spacing Sub-multiples:* If tone spacing $\Delta f$ is a simple integer sub-multiple of the sampling rate $f_s$, envelope samples cluster at discrete points rather than sweeping continuously across the correction table.
  3. *FFT Bin Quantization:* Small setup changes can shift narrow two-tone intermodulation spikes across FFT bin boundaries, producing measurement variances of several dB.
  > [!TIP]
  > When two-tone and broadband noise measurements disagree, the **noise test** is the authoritative metric for on-air spectral cleanliness.

---

### 13.4 PA Memory Effects
PA gain is not solely dependent on the instantaneous envelope level; it also depends on the recent history of the waveform. Thermal time constants, drain/collector power supply decoupling impedance, and semiconductor trap dynamics create **memory effects**.
- **Hysteresis Loop:** As the envelope rises toward a peak, the amplifier exhibits a different gain and phase shift than when the envelope falls through that same level.
- **Single-Valued Function Limit:** A standard correction lookup table provides one gain and phase correction per envelope amplitude. It cannot simultaneously correct two distinct states on a hysteresis loop. The correction curve tracks the centroid of the loop, leaving an irreducible residual distortion floor.

---

### 13.5 PureSignal 3.0 vs. PureSignal 2.0 Architectural Overhaul

| Feature | PureSignal 2.0 | PureSignal 3.0 |
| :--- | :--- | :--- |
| **Data Fitting Strategy** | Smoothed feedback data **before** curve fitting | Fits curves directly to **raw** feedback data; smooths finished correction curve |
| **Low-Drive Handling** | Forced curve through fixed origin, causing ripple artifacts at low amplitudes | Curve naturally extends to zero drive based on raw low-drive measurements |
| **Far-Out IMD Regrowth** | Pre-smoothing obscured fast non-linearities, limiting far-out IMD rejection | Captures fast non-linearities, achieving significantly deeper wideband IMD suppression |
| **Memory Effect Handling** | Averaged out memory hysteresis loops prematurely | Accurately models the true centroid of the memory hysteresis loop |
| **Calibration CPU Load** | High iteration count | Optimized data structures; significantly reduced CPU overhead |

#### The Two-Tone Paradox Explained
On a traditional two-tone test, PureSignal 3.0 may occasionally measure $1\text{ to }3\text{ dB}$ worse on close-in (3rd-order) products than PureSignal 2.0.
- **Why this occurs:** PS2's pre-smoothed curve introduced a subtle right-side inflection near maximum drive. On the rigid, periodic trajectory of a two-tone envelope, this inflection could fortuitously cancel 3rd-order products at that exact test frequency.
- **Reality:** Under broadband noise and real speech, PS3 matches PS2 on close-in products and outperforms PS2 by several dB on wideband spectral regrowth. PS3 provides a more accurate, truthful amplifier inversion.

---

### 13.6 Practical Operator Guidelines for Predistortion
1. **Feedback Signal Level:** Adjust feedback receiver attenuation so the feedback peak level sits between $-3\text{ and }-6\text{ dBFS}$ during maximum PA output. Avoid ADC clipping in the feedback receiver.
2. **Two-Tone Tone Spacing:** When running two-tone tests, select tone spacings that are not integer fractions of the DSP sample rate (e.g., use $700\text{ Hz}$ and $1900\text{ Hz}$ rather than $1000\text{ Hz}$ and $2000\text{ Hz}$).
3. **Memory Loops:** Do not be alarmed by scattered sample points or twin branches on the PureSignal calibration display; they represent real amplifier physical memory effects.
4. **Primary Reference:** Use broadband noise testing and spectrum analyzer panadapters to verify spectral purity.

---

## 14. Revision History (v0.10 to v2.1.1)

| Revision | Date | Summary of Changes |
| :--- | :--- | :--- |
| **0.10** | 2016-08-05 | Initial release of WDSP Architecture Guide; published for community comments. |
| **0.11** | 2016-08-24 | Added `TXASetSipSpecmode(...)` to API and documentation. Added notes on `SetPSControl()`. |
| **0.12** | 2016-11-28 | Added API function calls for PureSignal 2.0 adaptive predistortion. |
| **1.14** | 2017-03-17 | Added `GetWDSPVersion()`. Aligned document revision numbers with WDSP library version numbering. Documented envelope elimination and restoration (EER) calls. |
| **1.15** | 2017-05-06 | Added `AM_LSB` and `AM_USB` transmit modes. Introduced Continuous Frequency Compressor (CFC) and Phase Rotator audio processing blocks. |
| **1.16** | 2017-05-28 | Added CFC metering taps. Relocated Phase Rotator upstream in the transmit audio pipeline. |
| **1.17** | 2017-08-25 | Introduced Variable Ratio Resampler (`varsamp`) and Sample Rate Matcher (`rmatch`). |
| **1.18** | 2019-01-21 | Added `dexp`, the integrated downward expander and VOX system. |
| **1.19** | 2022-01-27 | Added CFC compression depth vs. frequency display API. Added RMS detector mode to panadapter/waterfall analyzer. Added 9 dynamic parameter tuning calls for `varsamp`. Enhanced `getRMatchDiags(...)` and `create_rmatchV(...)`. |
| **1.20** | 2022-02-15 | Non-Windows OS portability enhancements (`calculus.c`, `calculus.h`). High-zoom panadapter frequency resolution enhancements (`fscLin`, `fscHin` updated to `double`). |
| **1.21** | 2023-04-29 | Added FM demodulator volume limiter to stabilize weak-signal audio levels. Added Voice Squelch (AM/SSB). |
| **1.22** | 2023-05-20 | Performance and efficiency improvements: reduced system call frequency at the expense of slightly higher thread and memory allocations. |
| **1.23** | 2023-06-17 | Efficiency improvements in internal DSP buffer handling. |
| **1.24** | 2023-11-08 | Added carrier up-slew timing adjustments (`TXASetSlewTime`). Added FM transmit and receive bandwidth controls. Added display buffer clear API. Added $180^\circ$ microphone phase inversion to Phase Rotator. |
| **1.25** | 2024-07-05 | Added "Trained" mode for Spectral Noise Reduction (NR2) along with NSTAT Noise Power Estimation (NPE) method. |
| **1.26** | 2025-06-30 | Added Bandpass Filter Characterization Utility (BFCU). Added Filter Impulse Response Cache. Removed external file requirement (`zetaHat.bin`) for Trained NR2 mode. Added TX pulsed-tone and pulsed-two-tone modes. Added `SetupDetectMaxBin` peak detector. |
| **1.27** | 2025-09-19 | Clang / LLVM Static Analyzer compliance fixes for Linux builds. Added psychoacoustic post-processing for NR2. Removed faulty minimum-phase filter caching. |
| **1.28** | 2025-10-21 | Resolved and restored minimum-phase filter caching in impulse response cache. Added Gaussian filter impulse response generation. |
| **1.29** | 2026-01-16 | Added DoublePole, Matched, and Gaussian filter models to the CW Audio Peak Filter (APF). |
| **2.0.0** | 2026-07-01 | **Major Release: PureSignal 3.0** (new data structures, raw data fitting, robust algorithms). Free-curve NURBS/B-spline engine for EQ and CFC. Phase Rotator auto-calibration optimizer and asymmetry metering display. RX input decimator with high alias rejection (up to 6144 kHz input rate). Wideband FM Broadcast Stereo Demodulator. Added Appendices A, B, and C. FFTW wisdom recalculated. |
| **2.1.0** | 2026-09-04 | **Major Release: Deep Neural Network Noise Reduction (NNR).** PureSignal 3.0 algorithm tuning and calibration CPU cycle reduction. Filter generation efficiency optimizations. Introduced auto-generated `wdsp.h` header for exported C API. Added Appendix D (IMD and PureSignal). |
| **2.1.1** | 2026-10-15 | Added Standard and Premium CW models in NNR. Enhanced `SetChannelState(...)` dynamic transitions. Fixed bugs in `fmd.c` and `emph.c`. Portability fixes: `#include <limits.h>` in `wbfm.c`; `#include "wdsp_align.h"` in `extrapolate.c`, `nurbs_fit.c`, `nurbs_spline.c`; standardized C enums in `wdsp.h`. |


