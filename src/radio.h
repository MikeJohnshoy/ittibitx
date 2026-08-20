/*
 * radio.h — minibitx's tuning/control glue.
 *
 * This ties the hardware primitives (radio_hw.c: oscillator via si5351v2.c,
 * LPF switching, T/R relay and PTT GPIO) together with the software
 * receive VFO (vfo.c, which is DSP, not hardware) under radio_tune_to()
 * and radio_set_tx(), and exposes them as the entry points every control
 * surface drives the radio through - never a second place that pokes
 * hardware directly.
 *
 * Two callers today: hamlib.c's rigctld-compatible server (f/F for
 * frequency, t/T for PTT - the sole frequency control surface) and
 * hpsdr_p1.c's EP2 command parser (MOX/PTT only, from the C0 byte in the
 * Protocol 1 stream - some SDR apps key PTT there even while using CAT
 * for everything else). There used to be a third path, remote_execute(),
 * a string-command shim hpsdr_p1.c called for frequency before hamlib.c
 * existed; it's gone now that hamlib.c calls radio_tune_to() directly.
 *
 * radio_set_tx() is deliberately much smaller than sbitx's tr_switch():
 * sbitx interleaves the same two GPIO writes with ALSA mute calls,
 * mute_count/FFT-state resets, and AGC/volume restoration because it has
 * a full duplex-capable DSP chain to coordinate with. minibitx has no TX
 * audio path yet — it only streams RX IQ out over HPSDR — so there is no
 * such state to coordinate. radio_set_tx() keeps just the hardware
 * sequencing (PTT before the relay on key-down, relay before PTT-off on
 * key-up, with the same relay-settling delays) since that's a property of
 * the relay hardware itself, not of any DSP policy. When a TX audio path
 * is added here, whatever muting/reset logic it needs belongs in
 * radio_set_tx(), the same way it lives in tr_switch() in sbitx.
 */

#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>
#include "vfo.h"

// Fixed low IF (Hz) the analog front end mixes every RF frequency down to.
// si5351bx_setfreq() is what actually selects the operating RF frequency,
// by retuning the hardware LO so the desired signal lands here; the
// software RX oscillator (lo, below) always demodulates this same fixed
// value down to baseband and must never be retuned to the RF frequency
// itself - see radio_tune_to() in radio.c.
#define RX_IF_HZ 24000

extern int freq_hdr;    // current frequency, Hz
extern int in_tx;       // 0 = RX, 1 = TX
extern int bfo_freq;    // center frequency of the crystal filter, Hz
extern struct vfo lo;   // software LO for RX quadrature mixing - fixed at RX_IF_HZ

/* Tunes to f (Hz): sets the si5351 oscillator, restarts the software RX
 * VFO, and selects the matching LPF. */
void radio_tune_to(uint32_t f);

/* Switches between receive (tx_on = 0) and transmit (tx_on = 1): drives
 * EXT_PTT and TX_LINE in the correct order with a relay-settling delay
 * between them, and updates in_tx. Safe to call repeatedly with the same
 * value — it always re-drives the GPIO lines, so callers that see state
 * on every poll (like hpsdr_p1.c) should only call it on a change. */
void radio_set_tx(int tx_on);

#endif /* RADIO_H */
