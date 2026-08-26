# 10 — using minibitx with external digital comm applications (WSJT-X, ...)

Status: stub.

## Scope

- Pointing WSJT-X (or similar digital-mode software) at minibitx's HPSDR
  Protocol 1 UDP stream for I/Q, and at the rigctld server
  (`127.0.0.1:4532`, rig model "Hamlib NET rigctl") for CAT/frequency
  control — see
  [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md)
  for what that server actually implements.
- Whatever audio-routing configuration is needed between the SDR app's
  demodulated audio output and the digital-mode decoder, since minibitx
  itself does no demodulation.
- Known limitations for this use case: no TX audio path yet (see
  [`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md)), so
  transmit-capable digital modes aren't usable end-to-end until that
  lands.
