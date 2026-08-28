// cw.h
//
// Straight-key CW support: polls a physical key wired into GPIO
// (radio_hw.h's CW_KEY line), holds PTT/the T/R relay for the duration
// of a keying burst, and generates a single sidetone gated by a
// table-driven envelope so key-down/key-up transitions don't click.
//
// Deliberately narrow - no iambic/paddle keyer logic lives here; that's
// an external keyer's job (its output is just another on/off contact
// into the same key line). minibitx stays a support layer for external
// SDR apps, not a full transceiver - this gets clean dots and dashes
// out, nothing more.

#ifndef CW_H
#define CW_H

// Call once at startup, after radio_hw_gpio_init() (CW_KEY must already
// be configured) and after vfo_init_phase_table().
void cw_init(void);

// Call once per audio block (~10.7ms - sound.c's PERIOD_FRAMES at
// 96kHz) from the audio thread. Polls the key, manages the keying-burst
// hang timer, and is the only place this module calls radio_set_tx().
void cw_poll_key(void);

// True while a keying burst has TX asserted (radio_set_tx(1) called and
// not yet released). sound.c checks this before pulling samples from
// cw_get_sample().
int cw_tx_active(void);

// Call once per audio sample while cw_tx_active() is true. Returns the
// next output sample: the sidetone, scaled by the attack/decay envelope
// as the key goes down/up. Range is approximately [-1, 1].
double cw_get_sample(void);

#endif /* CW_H */
