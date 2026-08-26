# 05 — process and threading model

Status: stub.

## Scope

- `main()`'s startup sequence in `minibitx.c` (GPIO/hardware init →
  oscillator/VFO → networking and control surfaces → audio codec → audio
  thread) — see
  [`01_hardware_init_and_control.md`](01_hardware_init_and_control.md)
  for the first part of that sequence in detail.
- The thread structure once running: the HPSDR poll/listener thread, the
  Hamlib accept thread plus one thread per connected client, the audio
  thread driving `sound_process()`, and the main thread's idle loop.
- The idle loop itself — `main()`'s `while (1) sleep(1);` calling
  `status_print()` (`status.c`) once a second for the console status
  line (frequency, TX/RX state, TX drive level).
- Failure handling at startup: which subsystems are fatal if they fail
  to come up (GPIO/wiringPi, HPSDR socket bind) versus which are
  best-effort and allowed to be absent (Hamlib/rigctld, the USB gadget).
