# 03 — TX processing pipeline

Status: CW TX implemented and bench-verified (see
[`dsp_design_notes/tx_power_calibration.md`](dsp_design_notes/tx_power_calibration.md)
for the wattmeter session that calibrated it). This document covers
the signal chain the same way
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) covers
receive.

minibitx transmits CW only — a straight key wired into GPIO (see
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md)),
no voice, no digital modes, no keyer logic beyond a single on/off
contact. That's a deliberate scope choice, not a missing feature: any
richer TX mode is an external SDR app's job, same as demodulation is on
receive.

## The chain, end to end

TX reuses the exact same two mixer stages and both si5351 clocks that
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) covers
for RX — same hardware, signal flowing the opposite direction:

```
  cw.c: 700 Hz tone x envelope  (baseband CW waveform, in software)
     |
     v
  WM8731 DAC, right channel     (sound.c - PCM amplitude sets TX power)
     |
     v
  Mixer 2  <---  clk1, si5351 BFO (FIXED - same clock RX uses)
     |           balanced modulator: 700 Hz audio mixed onto bfo_freq
     v
  Crystal filter, fixed at ~bfo_freq  (same filter RX uses)
     |
     v
  Mixer 1  <---  clk2, si5351 RX/TX LO (radio_tune_to(), same as RX)
     |           radio.c: si5351bx_setfreq(2, f + bfo_freq - RX_IF_FREQ_HZ)
     v
  PA  (gain fixed by hardware; drive level set upstream - see below)
     |
     v
  LPF bank  (radio_hw.c: set_lpf_40mhz, same relays RX uses)
     |
     v
  Antenna
```

Everything from the DAC onward is analog hardware; minibitx's own code
only ever touches the two endpoints — generating the baseband waveform
at the top, and the GPIO/relay sequencing that turns the whole path on
and off (`radio_set_tx()`, `radio.c` — covered in
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md),
not repeated here).

## Stage by stage

Following one CW keydown at 7,020,000 Hz (`bfo_freq` at its compiled
default, 40,035,000 Hz) as a worked example.

**Baseband CW waveform.** `cw_get_sample()` (`cw.c`) generates a bare
700 Hz tone (`CW_PITCH_HZ`) via a software NCO (`vfo.c`), multiplied
sample-by-sample by a table-driven attack/decay envelope (480 samples,
5ms, Blackman-Harris-shaped) so key-down/key-up transitions don't
click. `cw_poll_key()` drives the envelope's direction (rising while
the key is down, falling once it's up) and also owns the semi
break-in hang timer that keeps PTT/the relay asserted for a short
window after key-up (`CW_HANG_POLLS`, ~300ms) so the relay doesn't
chatter between individual dits and dahs. The result is a floating
value roughly in [-1, 1] — this *is* the transmitted waveform, still
entirely in software at this point.

**WM8731 DAC, right channel.** `sound.c`'s audio thread converts that
value to a 32-bit PCM sample and writes it out the DAC's right
channel only (the left channel carries an independently-scaled local
sidetone copy for the on-board speaker — see "Adjusting power levels"
below; it has no bearing on transmitted power). This is the one place
in the whole chain where the *amplitude* of what eventually reaches
the antenna is actually set — everything after this point is fixed
analog gain.

**Mixer 2 — the BFO (clk1), same fixed clock RX uses.** The DAC's
analog output drives a balanced modulator that mixes the 700 Hz tone
onto `bfo_freq`, producing sidebands at `bfo_freq` ± 700 Hz (≈
40,034,300 and 40,035,700 Hz). There's no phasing network or
Hilbert-transform stage here — just a real audio tone into a real
balanced modulator — so both sidebands are produced and both survive;
see "Known limitations" below for what that means in practice.

**Crystal filter.** The same fixed bandpass RX uses. Its real measured
passband is roughly ±17.4–18.4 kHz wide (see
[`dsp_design_notes/antialias_filter_design.md`](dsp_design_notes/antialias_filter_design.md)),
so a ±700 Hz sideband spacing sits deep inside it — both tones pass
essentially unattenuated. This was the fix for an earlier bug where
the CW tone was mistakenly generated 24 kHz higher (conflating a
digital RX-only IF constant with a real TX audio frequency), which
pushed the wanted tone into the filter's stopband skirt while
unmodulated LO leak-through sat at the filter's unattenuated center —
see `cw.c`'s `CW_PITCH_HZ` comment for the full story.

**Mixer 1 — the RX/TX LO (clk2), same clock and same `radio_tune_to()`
call RX uses.** This is the only stage that differs by *frequency*
between RX and TX, and it doesn't actually differ in code — `f + bfo_freq -
RX_IF_FREQ_HZ` is computed identically for RX and TX, because
`radio_tune_to()` doesn't distinguish them. For our example: `clk2 =
7,020,000 + 40,035,000 - 24,000 = 47,031,000 Hz`. Mixing the crystal
filter's output back down against this LO is what actually determines
the transmitted RF frequency — see "Known limitations" for the gap
this reuse creates.

**PA.** A fixed-gain analog power amplifier stage. minibitx has no
digital gain control over the PA itself — `radio_set_tx()`
(`radio.c`) only sequences *whether* it's active (PTT/relay
timing, covered in
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md)).
Everything about *how much* power comes out was already decided
upstream, at the DAC stage.

**LPF bank.** The same four relays (`LPF_A`–`LPF_D`) RX uses for
front-end preselection, selected by `set_lpf_40mhz()` from
`radio_tune_to()` — one filter path serves both directions. On TX
this is what keeps harmonics of the fundamental from reaching the
antenna.

## Adjusting power levels

Only one stage in the whole chain sets output power: the PCM
amplitude written to the DAC. Everything downstream (balanced
modulator, crystal filter, PA, LPF) is fixed analog gain that doesn't
change per-band or per-drive-setting — so working backward from a
target wattage always means changing one of these `sound.c` constants,
never anything hardware-facing:

- **`TX_DRIVE`** — mirrors real sbitx's "drive" setting (0–100).
  Fixed at 50 (no live UI/command to adjust it yet); the per-band
  scale table below was itself measured against this value.
- **`hw_settings_tx_scale(freq)`** (`hw_settings.c`) — looks up the
  current band's `scale` entry from `data/hw_settings.ini`'s
  `[tx_band]` sections. This is real sbitx bench data (their own
  `calibrate_band_power()`, compensating for PA gain rolling off
  toward 10m), reused here as a starting point rather than derived
  from scratch.
- **`TX_GAIN_CORRECTION`** — a flat multiplier on top of the above,
  bench-verified against a wattmeter (40m/7.020MHz: real sbitx holds
  5.8W, minibitx needed this factor to reach a comparable 6.1W). Exists
  because sbitx's FFT-domain modulator and minibitx's plain
  oscillator-and-envelope pipeline don't share the same digital-to-analog
  unit conversion, so the *table*'s numbers didn't carry over without
  an anchor correction.
- **`TX_SAMPLE_CLAMP`** — hard-limits the PCM sample so it can't wrap
  around the 32-bit sample format. At today's `TX_GAIN_CORRECTION`,
  every band's pre-clip amplitude already exceeds this clamp, meaning
  every band is currently driven to the same saturated ceiling
  regardless of its `scale` entry — the full story, and the bench
  procedure to properly flatten power per band, is in
  [`dsp_design_notes/tx_power_calibration.md`](dsp_design_notes/tx_power_calibration.md).
- **`TX_MASTER_VOL`** (`radio.c`) — the WM8731 "Master" ALSA control,
  set to 95 during TX. This gates the DAC's whole analog output stage
  (both channels), not a per-channel volume — real sbitx's own code
  comment is explicit that muting it "mutes the PA, killing TX power
  regardless of the DRIVE setting." Lowering it would undo the
  wattmeter-calibrated power above, not just turn down the local
  sidetone.
- **`SIDETONE_SCALE`** (`sound.c`) — the one knob that does *not*
  affect transmitted power. Scales only the DAC's left channel (local
  on-board-speaker monitor); the right channel that actually feeds the
  balanced modulator keeps the full TX-calibrated amplitude regardless
  of this value.

## Known limitations

- **DSB, not SSB.** The balanced modulator stage has no image/sideband
  suppression — both `bfo_freq` ± 700 Hz sidebands are generated and
  both pass the crystal filter, so the actual transmitted signal is
  two tones 1.4 kHz apart, not a single clean carrier. For CW this is
  usually workable (a receiver tuned to either tone just hears "a
  signal"), but it isn't a single-sideband suppressed-carrier
  transmission the way real sbitx's FFT-based modulator produces.
- **TX frequency offset from the dial.** Real sbitx computes a
  *different* LO frequency for TX in CW mode than for RX — it applies
  an additional ∓`rx_pitch` (700 Hz) correction specifically so the
  transmitted carrier lands exactly on the dial frequency, compensating
  for the operator's audio tone being offset by that same pitch.
  `radio_tune_to()` here uses one flat formula for both RX and TX with
  no such correction, so the actual transmitted frequency runs off the
  intended dial frequency by an amount tied to `RX_IF_FREQ_HZ` and the
  CW pitch. This is a real, known gap — flagged during TX debugging,
  not yet fixed, and not addressed by anything in this document.
