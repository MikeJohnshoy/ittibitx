# DSP design notes

Home for DSP design notes — measured data, derivations,
and concrete specs to build against, kept separate from the
"how the shipped code works" docs elsewhere in `docs/`.

Each doc in this folder should open with a `Status:` line stating whether
it's proposed, in progress, or implemented, following the convention
already used in `antialias_filter_design.md`.

## Contents

- [`antialias_filter_design.md`](antialias_filter_design.md) — measured
  crystal filter response and the FIR anti-alias filter spec derived
  from it for the RX chain. Status: implemented (`antialias.c`).
- [`tx_power_calibration.md`](tx_power_calibration.md) — why sbitx's
  per-band TX scale table doesn't transfer to minibitx's pipeline
  unchanged, and a step-by-step wattmeter procedure (one QRP CW
  frequency per band) to re-derive per-band values for a flat 5W.
  Status: done - all 9 bands bench-confirmed in the 4.7-5.5W target
  window.
- [`usb_uac_decimation_design.md`](usb_uac_decimation_design.md) — the
  96kHz->48kHz decimating lowpass that makes `usb_gadget.c`'s UAC2
  gadget actually deliver the 48kHz it advertises, cascaded after
  `antialias_filter_design.md`'s own filter. Status: implemented
  (`decim48k.c`), bench-verified numerically; not yet verified against
  a real UAC2 host.
