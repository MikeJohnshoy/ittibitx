// radio.c - see radio.h

#include "radio.h"
#include "radio_hw.h"
#include "si5351.h"
#include <stdio.h>
#include <unistd.h>     // usleep() - relay-settling time, not GPIO access

int freq_hdr = 7074000;
int in_tx = 0;
// hardware data from Evan (AC9TU) indicates the  actual crystal filter 
// center is at 40.0124 MHz. The prior value here (40.035 MHz) was 22.6kHz
// off - so the desired signal was landing well off-center in the passband, not
// symmetric on the 24kHz digital IF as radio_tune_to()'s math assumes.
int bfo_freq = 40012400;    // old value was 40035000
struct vfo lo;

void radio_tune_to(uint32_t f) {
    freq_hdr = f;
    si5351bx_setfreq(2, f + bfo_freq - RX_IF_FREQ_HZ);
    vfo_start(&lo, RX_IF_FREQ_HZ, lo.phase);
    set_lpf_40mhz(f);    // enable the correct LPF for this band
}

// switch between RX and TX
void radio_set_tx(int tx_on) {
    if (tx_on) {
        in_tx = 1;                  // mirrors sbitx: hardware state follows intent
        radio_hw_set_ptt(1);
        usleep(20000);              // let PTT assert before keying the relay
        radio_hw_set_tx_relay(1);
    } else {
        radio_hw_set_ptt(0);
        usleep(5000);               // let the relay settle before dropping PTT
        radio_hw_set_tx_relay(0);
        in_tx = 0;
    }
}
