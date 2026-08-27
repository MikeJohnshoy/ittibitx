# 01 — sbitx hardware: initialization and control

This covers everything that has to be brought up before any signal ever
flows: GPIO lines, the si5351 clock generator and the I2C bus it rides
on, and the WM8731 audio codec. It stops at the point where the receive
signal chain can actually start moving samples — see
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) for what
happens to those samples once they arrive.

## Startup order, in `main()` (`minibitx.c`)

1. `radio_hw_gpio_init()` — GPIO lines and RX-safe idle state (below)
2. `si5351bx_init()` / `si5351bx_setfreq(1, bfo_freq)` / `si5351_reset()`
   — oscillator bring-up (below)
3. `radio_hw_detect_version()` / `radio_hw_ina260_configure()` — board
   revision and power monitor, probed right after the si5351 bring-up
   above since they share its I2C bus (below)
4. `vfo_init_phase_table()` / `vfo_start()` / `radio_tune_to()` —
   software RX VFO and initial tuning (covered in
   [`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md))
5. Networking and control surfaces — HPSDR, Hamlib/rigctld, USB gadget
   (covered in
   [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md))

Each step above prints one console line reporting its own result, in a
consistent `init: ...` format, ending with `minibitx: radio hardware
initialization complete` once every step has run. See
[`05_process_and_threading_model.md`](05_process_and_threading_model.md)
for what the console reports after that point.
5. `setup_audio_codec()` / `sound_thread_start()` — WM8731 codec and
   capture stream (below)

The ordering matters for one reason in particular: GPIO init happens
*first*, before anything that could conceivably key the transmitter
exists yet.

## GPIO setup and the RX-safe idle state

`radio_hw_gpio_init()` (`radio_hw.c`) calls `wiringPiSetupGpio()`, sets
`TX_LINE`, `TX_POWER`, `EXT_PTT`, and the four LPF select lines
(`LPF_A`–`LPF_D`) to `OUTPUT`, and then drives all of them low:

```c
digitalWrite(LPF_A, LOW);
digitalWrite(LPF_B, LOW);
digitalWrite(LPF_C, LOW);
digitalWrite(LPF_D, LOW);
digitalWrite(EXT_PTT, LOW);
digitalWrite(TX_LINE, LOW);
digitalWrite(TX_POWER, LOW);
```

`EXT_PTT` and `TX_LINE` low is the T/R relay's RX-idle state. Because
this runs before the si5351, the VFO, the network threads, or either
control surface (Hamlib, HPSDR's MOX handling) exist, there is no code
path in the process's lifetime where the radio could power on
transmitting — the relay and PTT lines are guaranteed low before
anything capable of calling `radio_set_tx()` is even initialized.

`TX_POWER` is also set low at boot; its exact purpose is inherited from
sbitx and unconfirmed here (see `radio_hw.h`).

## Board revision detection

`radio_hw_detect_version()` probes I2C address `0x8` with a 4-byte block
read. If that read fails, it reports `SBITX_DE` (original sbitx, no
power/SWR bridge board); if it succeeds, `SBITX_V2` (v2-and-later
hardware, power/SWR bridge present). It's called once, from `main()`,
right after the si5351/I2C bus comes up — its result is logged
(`init: board revision detected: ...`) but nothing else in the codebase
branches on it yet; it exists as a hook for hardware-revision-dependent
behavior to attach to later.

## LPF bank switching

`set_lpf_40mhz(frequency)` selects one of four low-pass filter relays
based on the tuned frequency:

| Frequency | Relay |
|---|---|
| < 5.5 MHz | `LPF_D` |
| < 10.5 MHz | `LPF_C` |
| < 18.5 MHz | `LPF_B` |
| < 30 MHz | `LPF_A` |

It's a no-op if the new selection matches the last one (tracked in a
static `prev_lpf`), so retuning within a band doesn't chatter the relays.
This is called from `radio_tune_to()` — see
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) — not
independently.

## INA260 power monitor

`radio_hw_ina260_configure()` writes `0x6127` (continuous mode, default
averaging) to the INA260's config register at I2C address `0x40`. It's
called once, from `main()`, alongside board revision detection above —
`init: INA260 power monitor configured`, or a non-fatal
`init: INA260 power monitor not responding, continuing without it` if
the write fails. `read_voltage_current()` then reads the voltage and
current registers (1.25 mV/LSB and 1.25 mA/LSB respectively) on demand,
treating an all-ones current reading as out-of-range/invalid rather than
a real value. Used for status reporting (`status.c`), not for any
control decision.

## si5351 oscillator and I2C bus

The si5351 generates both mixer LOs used in the RX chain (see
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) for what
each clock actually does). `si5351bx_init()` (`si5351v2.c`) powers down
all three clocks and brings up the I2C connection it needs; `main()`
then explicitly starts `clk1` at `bfo_freq` — the one clock nothing else
in minibitx ever touches again — before calling `si5351_reset()`.

The si5351 sits on I2C bus 22 (`SI5351_I2C_BUS` in `si5351v2.c`), sharing
the physical bus with the board's RTC via the `i2c-rtc-gpio` device tree
overlay. That bus number came from `i2cdetect -y 22` showing a device at
`SI5351_ADDR` (`0x60`) — not GPIO23/22 as originally assumed. It's a
Linux-assigned bus number, not a fixed hardware address, so it can
change across kernel/config updates; see
[`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md)
if the si5351 ever stops responding after an OS update.

`i2c.c` wraps this as a thin layer over `/dev/i2c-N` and the standard
SMBus ioctls (byte read/write, block read/write) — nothing sbitx- or
si5351-specific lives there.

## WM8731 audio codec bring-up

`setup_audio_codec()` (`sound.c`) configures the codec entirely through
ALSA mixer controls — no direct I2C register access from minibitx itself
(the kernel's `wm8731` driver, brought up via the
`dtoverlay=audioinjector-wm8731-audio` device tree overlay, does those
writes on minibitx's behalf):

```c
sound_mixer("hw:0", "Input Mux", 0);
sound_mixer("hw:0", "Line", 80);  // 80% of max
sound_mixer("hw:0", "Mic", 0);
sound_mixer("hw:0", "Master", 0); // Mute local speaker
```

Once the mixer is configured, `sound_thread_start("hw:0,0")` opens the
ALSA capture (and playback) PCM devices at the fixed 96 kHz sample rate
and starts the audio thread that repeatedly calls `sound_process()`.
That call is the boundary this document stops at: by the time it
returns, raw IF samples are already flowing in from an open, configured
capture stream. What happens to those samples from there —
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md).
