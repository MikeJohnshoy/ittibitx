# DSP design notes

Home for DSP design notes — measured data, derivations,
and concrete specs to build against, kept separate from the
"how the shipped code works" docs elsewhere in `docs/`.

Each doc in this folder should open with a `Status:` line stating whether
it's proposed, in progress, or implemented, following the convention
already used in `antialias_filter_design.md`.

## Contents

- crystal filter response and the FIR anti-alias filter spec derived
  from it for the RX chain. Status: implemented (`antialias.c`).
- per-band TX scale table doesn't transfer to minibitx's pipeline
  unchanged, and a step-by-step wattmeter procedure (one QRP CW
  frequency per band) to re-derive per-band values for a flat 5W.
  Status: proposed procedure, updated for the current TX pipeline;
  only 40m has been bench-run so far.
-  
