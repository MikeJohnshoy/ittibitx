/*
 * radio_hw.h — radio hardware control for minibitx: boot-time GPIO setup,
 * LPF band switching, board-revision detection, and the INA260 power
 * monitor.
 *
 * This is a port of the radio_hw module code carved out of sbitx (LPF relays, 
 * TX_LINE/TX_POWER/EXT_PTT lines, the power/SWR bridge board,
 * the INA260 power monitor) is the same hardware so it makes
 * sense to bring the already-verified control code over wholesale rather
 * than re-derive a smaller subset.
 *
 * minibitx does not yet act on TX_POWER or the INA260 — there is no
 * battery-monitoring logic here today — but the GPIO lines are still
 * initialized to a safe idle state at boot, and the functions are
 * available for the control surfaces (HPSDR now, Hamlib next) to call
 * once that logic exists.
 *
 * radio_hw_set_ptt() / radio_hw_set_tx_relay() are the two GPIO
 * primitives behind PTT: radio_set_tx() in radio.c sequences them (with
 * the relay-settling delay between them) and is what HPSDR's MOX bit and,
 * later, Hamlib's PTT command actually call.
 *
 * As in sbitx, the local oscillator (si5351v2.c/si5351.h) and the I2C bus
 * driver (i2cbb.c/i2cbb.h) are left as their own single-purpose files and
 * are not folded into this one — this file calls into i2cbb.c the same
 * way higher-level control code will eventually call into this file.
 */

#ifndef RADIO_HW_H
#define RADIO_HW_H

/* ---- GPIO pin assignments (wiringPi numbering, sBitx v2 hardware) ----- */

#define TX_LINE   4    // T/R relay control line
#define TX_POWER  27   // set once at boot, LOW; purpose unconfirmed in sbitx
#define EXT_PTT   26   // external PTT input/output line
#define LPF_A     5    // low-pass filter select lines, one active at a time
#define LPF_B     6
#define LPF_C     10
#define LPF_D     11

/* ---- Board hardware revision ------------------------------------------ */

#define SBITX_DE  (0)  // original sBitx, no power/SWR bridge board present
#define SBITX_V2  (1)  // v2-and-later, power/SWR bridge board present

/* Initializes wiringPi and configures TX_LINE, TX_POWER, EXT_PTT, and the
 * four LPF select lines as outputs, driving them to their idle (LOW)
 * state. Call once, before any other GPIO or radio_hw function. Returns 0
 * on success, -1 if the underlying wiringPi setup fails. */
int radio_hw_gpio_init(void);

/* Probes I2C address 0x8 (the power/SWR bridge board) to distinguish
 * original sBitx ("DE") hardware from v2-and-later. Returns SBITX_DE or
 * SBITX_V2. */
int radio_hw_detect_version(void);

/* Selects the low-pass filter appropriate for `frequency` (Hz) by driving
 * exactly one of the four LPF_x lines high and the rest low. No-op if the
 * frequency falls in the same filter's passband as the last call. */
void set_lpf_40mhz(int frequency);

/* Drives the external PTT line (EXT_PTT) high (on) or low (off). No
 * delay, no policy — see radio_set_tx() in radio.c for sequencing. */
void radio_hw_set_ptt(int on);

/* Drives the T/R relay control line (TX_LINE) high (on, transmit) or low
 * (off, receive). Same no-delay/no-policy contract as radio_hw_set_ptt(). */
void radio_hw_set_tx_relay(int on);

/* Reads the INA260 power monitor's voltage (V) and current (A) registers
 * over I2C. On any I2C error, both outputs are set to 0.0. */
void read_voltage_current(float *voltage, float *current);

/* Writes the INA260's configuration register (continuous mode, default
 * averaging). Returns 0 on success, -1 on I2C failure. */
int radio_hw_ina260_configure(void);

#endif /* RADIO_HW_H */
