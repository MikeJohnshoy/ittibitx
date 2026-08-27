# 05 — process and threading model

Status: stub.

## Scope

- `main()`'s startup sequence in `minibitx.c` (GPIO/hardware init →
  oscillator/VFO/board-rev/INA260 → networking and control surfaces →
  audio codec → audio thread) — see
  [`01_hardware_init_and_control.md`](01_hardware_init_and_control.md)
  for the first part of that sequence in detail. Each step logs its own
  `init: ...` result; the sequence ends with
  `minibitx: radio hardware initialization complete`.
- The thread structure once running: the HPSDR poll/listener thread, the
  Hamlib accept thread plus one thread per connected client, the audio
  thread driving `sound_process()`, and the main thread's idle loop.
- Console reporting after startup: `main()` calls `status_print()`
  (`status.c`) exactly once, right after the init-complete line, as a
  baseline snapshot (frequency, TX/RX state, TX drive level). It is
  **not** polled every second any more — the idle loop just sleeps.
  Ongoing operational visibility instead comes from the `rigctl:`/
  `hpsdr:` command echoes described in
  [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md),
  since those already report a freq/PTT change at the moment it happens.
- Failure handling at startup: which subsystems are fatal if they fail
  to come up (GPIO/wiringPi, HPSDR socket bind, audio capture) versus
  which are best-effort and allowed to be absent (Hamlib/rigctld, the
  USB gadget, the INA260 power monitor).
