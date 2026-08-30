# DSP design notes

Home for DSP design notes — measured data, derivations,
and concrete specs to build against, kept separate from the
"how the shipped code works" docs elsewhere in `docs/`.

Each doc in this folder should open with a `Status:` line stating whether
it's proposed, in progress, or implemented, following the convention
already used in `antialias_filter_design.md`.

## Contents

- [`antialias_filter_design.md`](antialias_filter_design.md) — measured
  crystal filter response and a proposed FIR anti-alias filter spec for
  the RX chain. Status: proposed, not yet implemented in `sound.c`.
- [`tx_power_calibration.md`](tx_power_calibration.md) — why sbitx's
  per-band TX scale table doesn't transfer to minibitx's saturating CW
  pipeline unchanged, and a step-by-step wattmeter procedure to
  re-derive per-band values for flat power. Status: proposed procedure,
  not yet executed.
-  
