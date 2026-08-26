# 08 — troubleshooting and bring-up

Status: stub — fold back into
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md) if
this never grows past a couple of entries.

## Scope

Hardware bring-up gotchas that don't belong in the design docs proper:

- The si5351's I2C bus number is Linux-assigned (currently 22 via the
  `i2c-rtc-gpio` overlay), not a fixed hardware address — re-check with
  `i2cdetect -l` if the si5351 ever stops responding after an OS/kernel
  update, per the note in
  [`01_hardware_init_and_control.md`](01_hardware_init_and_control.md).
- Board-revision detection (`radio_hw_detect_version()`) and what to
  check if it misidentifies DE vs. v2 hardware.
- Anything else discovered during bring-up on real hardware that would
  otherwise get rediscovered the hard way a second time.
