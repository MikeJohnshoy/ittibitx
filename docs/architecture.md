# minibitx RF signal chain

This documents the receive signal path from antenna to baseband I/Q — the
part that's easy to get wrong, because most of it lives in analog hardware
and a fixed relationship between two si5351 clocks.

minibitx has no onboard demodulation, no waterfall, no mode logic — the
connected SDR app does all of that. This document only covers what happens
before the signal leaves the Pi as baseband I/Q.

## The chain, end to end

```
  Antenna
     |
     v
  LPF bank (radio_hw.c: set_lpf_40mhz, one of 4 relays by band)
     |
     v
  Mixer 1  <---  clk2, si5351 RX LO (SWEEPS with tuning)
     |            radio.c: si5351bx_setfreq(2, f + bfo_freq - RX_IF_HZ)
     v
  Crystal filter, fixed at bfo_freq (~40.035 MHz, based on hardware spec review)
     |
     v
  Mixer 2  <---  clk1, si5351 BFO (FIXED, never swept, calculated to shift output
     |           of mixer 1 to baseband) set on startup minibitx.c: si5351bx_setfreq(1, bfo_freq)
     v
  Low IF, fixed at RX_IF_HZ (24000 Hz)
     |
     v
  ADC / wm8731 audio codec (sound.c, 48 kHz sample rate)
     |
     v
  Software VFO (vfo.c, "lo" in radio.c) <--- FIXED at RX_IF_HZ, never swept
     |            sound.c: sound_process() calls vfo_read_iq() per sample
     v
  Baseband I/Q (centered at 0 Hz)
     |
     +---------------------------+
     v                           v
  hpsdr_p1.c (HPSDR/UDP)   usb_gadget.c (USB Audio Class)
```

Two mixer stages, two si5351 clocks, two different jobs. 

## Stage by stage

**LPF bank.** `radio_hw.c`'s `set_lpf_40mhz(frequency)` selects one of four
low-pass filter relays (`LPF_A`–`LPF_D`) based on the tuned frequency —
under 5.5 MHz, under 10.5 MHz, under 18.5 MHz, or under 30 MHz — and is a
no-op if the frequency falls in the same band as the last call. Pure analog
front-end filtering; nothing here talks to either si5351 clock.

**Mixer 1 — the RX LO (clk2), which sweeps with tuning.** This is the only
clock that moves when you retune. `radio_tune_to(f)` in `radio.c` sets it
with `si5351bx_setfreq(2, f + bfo_freq - RX_IF_HZ)` — mixing the desired RF
frequency `f` up to a fixed intermediate frequency at `bfo_freq`
(40,035,000 Hz). Whatever `f` you tune to, the output of this stage always
lands at the same fixed IF; that's the whole point of a superheterodyne
front end, and it's also *why* nothing downstream of this stage needs to
know the current operating frequency.

**Crystal filter.** A fixed bandpass filter centered at `bfo_freq`. This is
the receiver's actual selectivity — everything outside its passband is
rejected before the signal ever reaches Mixer 2. minibitx does no
mode-dependent filtering of its own (no CW/SSB bandwidth switching); the
connected SDR app is expected to do any further filtering digitally on the
IQ it receives.

**Mixer 2 — the BFO (clk1), which is fixed and started once.** This mixer
brings the crystal-filter output (still up at ~40 MHz) down to a low IF of
`RX_IF_HZ` (24000 Hz) that the audio codec can actually sample. Its LO is
si5351 `clk1`, set once in `minibitx.c` at startup —
`si5351bx_setfreq(1, bfo_freq)` — and never touched again for the life of
the process. It does not sweep. It cannot sweep: it's fixed at `bfo_freq`
regardless of what frequency you're tuned to, because Mixer 1 already did
the job of bringing the *desired* signal to that same fixed `bfo_freq`
point — Mixer 2 only has to undo the fixed offset, not track the tuning.

**ADC / audio codec.** `sound.c` opens the ALSA capture device at 96 kHz
and hands each block of raw samples to `sound_process()`. brought up via
the dtoverlay=audioinjector-wm8731-audio.  That kernel driver does the actual
I2C register writes to the WM8731 (input mux, gain, mute, etc.) itself;
minibitx only ever reaches it indirectly,What arrives here
is the low IF signal — real-valued, centered around `RX_IF_HZ`, not yet
I/Q.

**Software VFO — fixed at RX_IF_HZ, never swept.** `vfo.c` implements a
digital NCO (`struct vfo`, the global `lo` in `radio.c`) that generates
quadrature (cos/sin) mixing signals. `sound_process()` calls
`vfo_read_iq()` once per sample and multiplies the incoming real IF sample
by both the cosine and sine outputs, producing the I and Q channels — a
standard digital quadrature downconversion, taking the fixed 24 kHz IF down
to baseband (0 Hz). Like the BFO, this oscillator's frequency is fixed at
`RX_IF_HZ` and does not change when you retune; only its *phase* is
preserved across calls; see `RX_IF_HZ` in `radio.h` for the single place
this constant is defined.

**Baseband I/Q → the two streaming consumers.** `sound_process()` concludes
by handing its I/Q arrays to `hpsdr_send_iq()` (`hpsdr_p1.c`, packetized as
HPSDR Protocol 1 over UDP) and `uac_push_iq()` (`usb_gadget.c`, streamed as
a USB Audio Class 2.0 capture device) — two independent copies, neither
module aware the other exists. See `README.md` for how those two files (and
the rest of `src/`) are organized.

## The control path

`radio_tune_to(f)` in `radio.c` is the only function that changes what RF
frequency the receiver is listening to, and it only ever touches two
things: `clk2` (Mixer 1's LO, via `si5351bx_setfreq(2, ...)`) and the LPF
bank (`set_lpf_40mhz(f)`). It does **not** touch `clk1` (the BFO) and does
**not** change the software VFO's frequency — both stay fixed at their
respective constants (`bfo_freq`, `RX_IF_HZ`) for the life of the process.
`radio_tune_to()` is called from exactly two places: `minibitx.c` at
startup, and `hamlib.c`'s rigctld-compatible `F` (set frequency) command —
the sole live frequency control surface, per `radio.h`'s file comment.

## TX

Not implemented yet. `radio_set_tx()` in `radio.c` sequences the PTT and
T/R relay GPIO lines, but there is no TX audio path — no upsampling, no
TX IQ ring buffer, nothing feeding a modulator. See the comment in
`radio.h` for what sbitx's much larger `tr_switch()` does that minibitx's
`radio_set_tx()` deliberately doesn't (yet).
