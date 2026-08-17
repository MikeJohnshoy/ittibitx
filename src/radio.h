/*
 * radio.h — minibitx's tuning/control glue.
 *
 * This ties the hardware primitives (radio_hw.c: oscillator via si5351v2.c,
 * LPF switching, T/R relay and PTT GPIO) together with the software
 * receive VFO (vfo.c, which is DSP, not hardware) under radio_tune_to()
 * and radio_set_tx(), and exposes remote_execute() as the single entry
 * point external control surfaces use to drive the radio.
 *
 * Today remote_execute() is called only from the HPSDR command parser
 * (hpsdr_p1.c), which also calls radio_set_tx() directly on MOX changes.
 * When Hamlib/rigctld support is added, it becomes a second caller into
 * the same functions here rather than a second place that pokes hardware
 * directly.
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

extern int freq_hdr;    // current frequency, Hz
extern int in_tx;       // 0 = RX, 1 = TX
extern int bfo_freq;    // center frequency of the crystal filter, Hz
extern struct vfo lo;   // software LO for RX quadrature mixing

/* Tunes to f (Hz): sets the si5351 oscillator, restarts the software RX
 * VFO, and selects the matching LPF. */
void radio_tune_to(uint32_t f);

/* Switches between receive (tx_on = 0) and transmit (tx_on = 1): drives
 * EXT_PTT and TX_LINE in the correct order with a relay-settling delay
 * between them, and updates in_tx. Safe to call repeatedly with the same
 * value — it always re-drives the GPIO lines, so callers that see state
 * on every poll (like hpsdr_p1.c) should only call it on a change. */
void radio_set_tx(int tx_on);

/* Parses one command string from a control surface (currently just
 * "freq NNN" from hpsdr_p1.c) and applies it. */
void remote_execute(char *command);

#endif /* RADIO_H */
