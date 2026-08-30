# 03 — TX processing pipeline

Status: CW TX implemented and bench-verified, including single-sideband
image suppression (~40dB, confirmed on-air on two independent SDR
displays — see "Known limitations" below). This document covers
the signal chain the same way
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) covers
receive.

Note: [`dsp_design_notes/tx_power_calibration.md`](dsp_design_notes/tx_power_calibration.md)
still describes an *earlier* calibration session (`TX_GAIN_CORRECTION =
4.0`) and its "every band clips to the same ceiling" finding. That
finding no longer holds — `TX_GAIN_CORRECTION` is now 0.045 (see
"Adjusting power levels" below), a ~89x reduction, and bands are no
longer saturating against `TX_SAMPLE_CLAMP`. That doc hasn't been
re-run against the current pipeline yet; treat its specific numbers as
historical until it is.

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
  cw.c: two NCOs x envelope      (sidetone at 700Hz, TX carrier IF-shifted
     |                            to 700Hz + TX_IF_OFFSET_HZ ~= 23.3kHz)
     v
  WM8731 DAC, right=TX carrier,  (sound.c - PCM amplitude sets TX power;
          left=sidetone           left channel never reaches the PA)
     |
     v
  Mixer 2  <---  clk1, si5351 BFO (FIXED - same clock RX uses)
     |           balanced modulator: ~23.3kHz carrier mixed onto bfo_freq -
     |           BFO sits at the filter's edge, not its center, so only
     |           the difference product lands in the passband (see below)
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

**Baseband CW waveform — two oscillators, one envelope.** `cw.c` runs
two software NCOs (`vfo.c`) sharing a single table-driven attack/decay
envelope (480 samples, 5ms, Blackman-Harris-shaped) so key-down/key-up
transitions don't click. `cw_poll_key()` drives the envelope's direction
(rising while the key is down, falling once it's up) and also owns the
semi break-in hang timer that keeps PTT/the relay asserted for a short
window after key-up (`CW_HANG_POLLS`, ~300ms) so the relay doesn't
chatter between individual dits and dahs.

`cw_get_sample()` reads the first NCO, tuned to the bare `CW_PITCH_HZ`
(700 Hz) — this is the sidetone the operator actually hears, unchanged
from earlier. `cw_get_tx_sample()` reads the second NCO, tuned to
`CW_PITCH_HZ + TX_IF_OFFSET_HZ` (~23.3 kHz) — this is what actually gets
transmitted, and it's deliberately *not* the pitch you hear; see
`TX_IF_OFFSET_HZ`'s comment in `cw.c` and "Mixer 2" below for why. Both
functions read the same envelope position for a given sample (only
`cw_get_sample()` advances it — callers must call it first), so the two
stay in lockstep. Each returns a floating value roughly in [-1, 1].

**WM8731 DAC, both channels now doing real work.** `sound.c`'s audio
thread converts each oscillator's value to a 32-bit PCM sample: the
right channel gets `cw_get_tx_sample()` at the full wattmeter-calibrated
amplitude (this is what reaches the balanced modulator and, eventually,
the PA); the left channel gets `cw_get_sample()` at a small, fixed,
independent amplitude for the local on-board-speaker monitor only (see
"Adjusting power levels" below — the left channel never reaches the PA,
regardless of its amplitude). The right channel's amplitude is the one
place in the whole chain that sets how much power eventually reaches
the antenna — everything after this point is fixed analog gain.

**Mixer 2 — the BFO (clk1), same fixed clock RX uses, deliberately
placed at the filter's edge.** The DAC's analog output drives a
balanced modulator that mixes the ~23.3 kHz TX carrier onto `bfo_freq`,
producing two products: `bfo_freq` − 23.3kHz (≈ 40,011,700 Hz) and
`bfo_freq` + 23.3kHz (≈ 40,058,300 Hz). There's no phasing network or
Hilbert-transform stage here — this hardware has a single real balanced
modulator (confirmed against the schematic), and a real signal times a
real LO always produces both sum and difference, no way around it. What
makes this come out single-sideband anyway is *where* `bfo_freq` sits:
40,035,000 Hz is not the crystal filter's center — it's deliberately
~22.6 kHz above it, the same "BFO at the filter's edge" placement real
sbitx's own design article describes (VU2ESE, "The sBitx": clock 1 sits
~25 kHz above the filter's passband center for this exact reason). With
the BFO off-center like this, the *difference* product lands right at
the filter's real center (deep in the passband) while the *sum* product
lands far into the stopband — see "Crystal filter" below. Before this,
`cw.c` fed a bare 700 Hz tone straight into this same mixer, so both
products (`bfo_freq` ± 700 Hz) landed within ~5-6 kHz of each other, far
too close together for this filter to tell apart — see "Known
limitations" for how that showed up on the air.

**Crystal filter.** The same fixed bandpass RX uses, measured (see
[`dsp_design_notes/antialias_filter_design.md`](dsp_design_notes/antialias_filter_design.md))
at ~40.0124 MHz center, ~35 kHz wide (-4dB cutoffs at ±17.4/17.5 kHz),
with a skirt steep enough to reach -61dB by +28.5 kHz above center. The
wanted difference product (~40,011,700 Hz) sits almost exactly at that
measured center — close to peak passband, minimal attenuation. The
unwanted sum product (~40,058,300 Hz) sits about 28.5 kHz above the
passband's upper edge, right where the measured data shows -61dB of
rejection. On the air this measured out to ~40dB of actual suppression
between the two — real, usable, but short of the idealized curve, most
likely because the curve came from a different (if representative)
physical filter than the one on this board. This is also, not
coincidentally, why the *old* 700Hz-straight-to-the-mixer scheme didn't
work: both of its products landed only ~5-6 kHz from `bfo_freq`, nowhere
near this filter's edge, so neither one got meaningfully rejected.

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
- **`TX_GAIN_CORRECTION`** — a flat multiplier on top of the above, now
  **0.045**, bench-verified against a wattmeter (40m/7.020MHz: 5.1W,
  matching real sbitx's own 4.8W measured on the same board at the same
  drive setting). This replaced an earlier value of 4.0 that was
  calibrated against the old (pre-`TX_IF_OFFSET_HZ`) scheme, where both
  transmitted products sat on the crystal filter's skirt and lost real
  power to its own attenuation before ever reaching the antenna. Now
  that the wanted product sits at the filter's point of *least*
  attenuation (see "Mixer 2" above), the same digital drive level
  produces much more RF output — the swing from 4.0 to 0.045 (~89x) is
  the direct consequence of removing that incidental loss, discovered
  the hard way (a first re-test at the old 4.0 measured >23W on a board
  whose PA had only ever been bench-verified safe up to ~6-8W). Re-derive
  this value from scratch on any board where the filter's measured
  center or `bfo_freq` differ meaningfully from this one — the relation
  between "how far the wanted product sits from the filter's peak" and
  "how much gain that costs you" isn't obvious in advance, and the
  power vs. gain curve near the low end here didn't turn out to be a
  clean square law either (bench data, not derived) — treat this as
  something to re-bisect against a wattmeter on new hardware, not a
  constant to trust blind.
- **`TX_SAMPLE_CLAMP`** — hard-limits the PCM sample so it can't wrap
  around the 32-bit sample format. At today's much lower
  `TX_GAIN_CORRECTION` (0.045), no band's pre-clip amplitude reaches
  this clamp any more — the "every band saturates to the same ceiling"
  problem documented in
  [`dsp_design_notes/tx_power_calibration.md`](dsp_design_notes/tx_power_calibration.md)
  no longer applies as written; that doc's specific numbers are stale
  and its band-by-band procedure needs re-running against the current
  pipeline (not yet done).
- **`TX_MASTER_VOL`** (`radio.c`) — the WM8731 "Master" ALSA control,
  set to 95 during TX. This gates the DAC's whole analog output stage
  (both channels), not a per-channel volume — real sbitx's own code
  comment is explicit that muting it "mutes the PA, killing TX power
  regardless of the DRIVE setting." Lowering it would undo the
  wattmeter-calibrated power above, not just turn down the local
  sidetone.
- **`SIDETONE_PEAK_AMPLITUDE`** (`sound.c`) — the one knob that does
  *not* affect transmitted power: a fixed PCM peak amplitude applied
  only to the DAC's left channel (local on-board-speaker monitor, at
  the `CW_PITCH_HZ` sidetone — see "Baseband CW waveform" above); the
  right channel that actually feeds the balanced modulator uses the
  full TX-calibrated amplitude regardless of this value. It used to be
  `amp * SIDETONE_SCALE` — coupled to the same `amp` as the TX channel —
  which meant the sidetone silently went near-inaudible the moment
  `TX_GAIN_CORRECTION` dropped ~89x above. It's a fixed comfort-level
  constant now, independent of whatever `TX_GAIN_CORRECTION` is
  currently bench-calibrated to.

## Known limitations

- **Image suppressed, not eliminated (~40dB).** The `TX_IF_OFFSET_HZ`
  placement (see "Mixer 2" above) fixed what used to be a genuine DSB
  problem (two equal-strength tones 1.4 kHz apart) — confirmed on-air,
  on two independent SDR displays, at roughly 40dB of suppression
  between the wanted and unwanted product. That's real, usable
  single(-ish)-sideband CW, comparable to what many communications-grade
  phasing/filter-method exciters achieve — but it's not infinite. The
  crystal filter's own measured skirt (see
  [`dsp_design_notes/antialias_filter_design.md`](dsp_design_notes/antialias_filter_design.md))
  theoretically supports closer to -61dB at the unwanted product's
  offset; the ~20dB gap between that and the measured ~40dB most likely
  comes from this being a different physical filter unit than the one
  characterized in that doc, plus whatever the diode mixer's own
  balance and any minor path nonlinearity contribute. Nothing here
  suggests it's fixable further without new measurement data specific
  to this board's actual filter.
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
