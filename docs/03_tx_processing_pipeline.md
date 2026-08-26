# 03 — TX processing pipeline

Status: stub — TX is the next major area of work; expand this as it's
built out.

## Scope

What TX support exists today, and what's still missing:

- **What exists:** `radio_set_tx()` (`radio.c`) sequences the PTT and T/R
  relay GPIO lines — PTT asserts before the relay keys on key-down, the
  relay drops before PTT releases on key-up, each with a settling delay.
  It's reachable from Hamlib's `T` command and from HPSDR's MOX bit (see
  [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md)).
- **What's missing:** there is no TX audio path at all — no upsampling,
  no TX I/Q ring buffer, nothing feeding a modulator. `radio_set_tx()` is
  deliberately much smaller than sbitx's `tr_switch()`, which also
  coordinates ALSA muting, mute-count/FFT-state resets, and AGC/volume
  restoration for a full duplex-capable DSP chain that minibitx doesn't
  have yet.

When a TX audio path is added, this document should cover its signal
chain the same way [`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md)
covers receive, and whatever muting/reset logic it needs belongs
alongside it rather than in `radio_set_tx()`.
