// cw.h

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
// as the key goes down/up. Range is approximately [-1, 1]. Owns the
// envelope advance - call this before cw_get_tx_sample() for the same
// sample, not after.
double cw_get_sample(void);

// Call once per audio sample, immediately after cw_get_sample(), while
// cw_tx_active() is true. Returns the actual TX-modulating waveform -
// same envelope position cw_get_sample() just advanced to, but at the
// IF-shifted carrier (CW_PITCH_HZ + TX_IF_OFFSET_HZ) that lands inside
// the crystal filter's passband instead of producing two RF tones. See
// cw.c's TX_IF_OFFSET_HZ comment. Range is approximately [-1, 1].
double cw_get_tx_sample(void);

#endif /* CW_H */
