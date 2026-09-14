# Protocol-2 PureSignal: which ADC does the feedback attenuator belong to?

**Not fixed. Noted for future work.** Found while looking for a code-side
explanation of a G2 transmit failure; it is *not* that explanation (see
Scope), but it is a real inconsistency and worth a patch on its own merits.

## The two sites

piHPSDR sets the TX-time step attenuators twice, in two packets. They
disagree on the PureSignal line.

`new_protocol_high_priority()`, `src/new_protocol.c:1219`:

```c
high_priority_buffer_to_radio[1443] = adc[0].attenuation;          // ADC0
high_priority_buffer_to_radio[1442] = ... adc[1].attenuation;      // ADC1
if (xmit && local_pa_enabled) {
  high_priority_buffer_to_radio[1442] = 31;
  high_priority_buffer_to_radio[1443] = 31;
}
if (xmit && transmitter->puresignal) {
  high_priority_buffer_to_radio[1442] = transmitter->attenuation;  // -> ADC1
}
```

`new_protocol_transmit_specific()`, `src/new_protocol.c:1346`:

```c
transmit_specific_buffer[59] = adc[0].attenuation;                 // ADC0
transmit_specific_buffer[58] = ... adc[1].attenuation;             // ADC1
if (!txband->disablePA && pa_enabled) {
  transmit_specific_buffer[58] = 31;   // ADC1
  transmit_specific_buffer[59] = 31;   // ADC0
}
if (transmitter->puresignal) {
  transmit_specific_buffer[59] = transmitter->attenuation;         // -> ADC0
}
```

One writes `transmitter->attenuation` to ADC0, the other to ADC1. The
comment above the high-priority block claims the two are "essentially
duplicated"; this is the line where they are not.

## Why both are wrong

Neither consults the setting that actually routes the feedback. The
feedback ADC is operator-selectable and is sent in the receive-specific
packet at `src/new_protocol.c:1414`:

```c
receive_specific_buffer[17] = receiver[PS_RX_FEEDBACK]->adc;
```

`PS_RX_FEEDBACK` is `RECEIVERS + 1` = 3 (`src/radio.c:1598`), so the value
lives in the props as `receiver.3.adc`. Both attenuator sites hardcode
instead, and in opposite directions — so whichever ADC the operator
selects, one of the two packets is addressing the other one.

## What it would look like

Whichever packet is wrong leaves the feedback ADC pinned at 31 dB by the
`pa_enabled` line just above. The feedback then arrives ~31 dB down,
`psinfo[4]` reads below 25, and the auto-calibration loop at
`src/ps_menu.c:196` walks `transmitter->attenuation` down 10 dB per step
toward `tx_att_min`, firing `tx_ps_reset()` then `tx_ps_resume()` on each
step. It cannot converge, because it is moving an attenuator that is not in
the path. Signature: PureSignal resetting in a loop with the feedback level
stuck at the bottom of its range.

Probably masked on current firmware. The note at
`src/new_protocol.c:1228` says the TX-specific bytes are what the current
protocol honours and that the high-priority copy is kept only for older
firmware that did not implement them. With `receiver.3.adc=0` — the value
in all three of this station's props files — the TX-specific packet is the
correct one, so a radio obeying it behaves properly and the defect is
invisible.

## Fix direction

Derive the target from `receiver[PS_RX_FEEDBACK]->adc` in both places
rather than hardcoding, so the two packets agree with each other and with
the routing. Applies to `upstream/master` unchanged; the block is
untouched by anything this fork carries.

## Scope

* Protocol 2 only.
* Only reachable with PureSignal enabled. Inert otherwise.
* **This does not explain a damaged PA or SWR bridge, and should not be
  cited as if it did.** `transmitter->attenuation` sits on the receive
  feedback path and cannot raise transmit power. Drive comes from
  `radio_calc_drive_level()` (`src/radio.c:2999`) off the drive or
  tune_drive slider, and nothing in the PureSignal path writes it. The
  only route from a mis-converged PureSignal to real PA stress is gain
  expansion inside WDSP's `calcc`, which is outside this codebase and was
  not evaluated.
* Unrelated to the `fix/p2-unused-adc-bpf-bypass-*` branches, which touch
  only the RX band-pass select (bits 1-6 and 12 of alex0/alex1) and no TX
  bit in either word.
