#ifndef HPSDR_P1_H
#define HPSDR_P1_H

#include <stdint.h>

#define HPSDR_PORT 1024
#define HPSDR_PKT_SIZE 1032
#define SAMPLES_PER_PACKET 126

int  hpsdr_init(void);
void hpsdr_stop(void);
void hpsdr_send_iq(double *i_samples, double *q_samples, int n);
void hpsdr_poll(void);
int  hpsdr_is_connected(void);

// Last non-zero TX VFO / RX1 frequencies seen from the connected HPSDR
// client's EP2 C&C stream (addr 0x01 / 0x02 - see hpsdr_p1.c). Zero until
// a client has sent at least one such frame. Exposed so radio.c's
// radio_tx_apply() can retune for split CW on a LOCAL key, not just on
// network-driven MOX (process_ep2_frame() already handles that case
// internally) - see the split-CW comment in radio_tx_apply().
uint32_t hpsdr_get_tx_freq(void);
uint32_t hpsdr_get_rx_freq(void);

#endif // HPSDR_P1_H
