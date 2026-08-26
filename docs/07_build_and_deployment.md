# 07 — build and deployment

Status: stub.

## Scope

- Build: `Makefile`, the single `minibitx` binary, the sources it's
  linked from, and its library dependencies (`libasound`, `libwiringPi`,
  `pthread`, `libm`, `libdl`).
- The `update` script: what it does (`git stash` / `git pull` from
  `$HOME/minibitx`) and when to use it versus a manual pull.
- Kernel/OS dependencies minibitx assumes are already in place:
  `dtoverlay=audioinjector-wm8731-audio` for the codec, the
  `i2c-rtc-gpio` overlay the si5351 rides on, wiringPi's GPIO access.
- Deployment: running under a plain terminal versus systemd/journald —
  see the status-line fallback behavior noted in
  [`05_process_and_threading_model.md`](05_process_and_threading_model.md).
- `credits` — upstream sbitx code this project is based on.
