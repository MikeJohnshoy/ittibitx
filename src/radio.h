/*
 * radio.h — minibitx's tuning/control glue.
 *
 * This ties the hardware primitives (radio_hw.c: oscillator via si5351v2.c,
 * LPF switching) together with the software receive VFO (vfo.c, which is
 * DSP, not hardware) under one radio_tune_to() call, and exposes
 * remote_execute() as the single entry point external control surfaces
 * use to drive the radio.
 *
 * Today remote_execute() is called only from the HPSDR command parser
 * (hpsdr_p1.c). When Hamlib/rigctld support is added, it becomes a second
 * caller into the same functions here rather than a second place that
 * pokes hardware directly.
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

/* Parses one command string from a control surface (currently just
 * "freq NNN" from hpsdr_p1.c) and applies it. */
void remote_execute(char *command);

#endif /* RADIO_H */
