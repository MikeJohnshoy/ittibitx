// radio.c - see radio.h

#include "radio.h"
#include "radio_hw.h"
#include "si5351.h"
#include <stdio.h>
#include <wiringPi.h>   // for delay() - relay-settling time, not GPIO access

int freq_hdr = 7074000;
int in_tx = 0;
int bfo_freq = 40035000;
struct vfo lo;

void radio_tune_to(uint32_t f) {
    freq_hdr = f;
    si5351bx_setfreq(2, f + bfo_freq - RX_IF_HZ);
    // The software RX oscillator always demodulates the fixed low IF, not
    // the RF frequency - only the hardware LO above moves when tuning.
    // This used to pass freq_hdr here instead of RX_IF_HZ: on top of being
    // the wrong frequency architecturally (matching sbitx's radio_tune_to(),
    // which never touches the software oscillator at all - see sbitx.c's
    // update_rx_osc(), called only on mode/pitch changes), freq_hdr is tens
    // of MHz, and vfo_start()'s frequency_hz * 65536 overflows a 32-bit int
    // at anything above ~32 kHz - so every retune was both conceptually
    // wrong and numerically garbage. RX_IF_HZ (24000) does neither.
    vfo_start(&lo, RX_IF_HZ, lo.phase);
    set_lpf_40mhz(f);    // enable the correct LPF for this band
    printf("Tuned to: %d Hz\n", f);
}

// Switch between RX and TX. See radio.h for why this is much smaller than
// sbitx's tr_switch() - no TX audio path here yet to coordinate with.
void radio_set_tx(int tx_on) {
    if (tx_on) {
        in_tx = 1;                  // mirrors sbitx: hardware state follows intent
        radio_hw_set_ptt(1);
        delay(20);                  // let PTT assert before keying the relay
        radio_hw_set_tx_relay(1);
        printf("TX on\n");
    } else {
        radio_hw_set_ptt(0);
        delay(5);                   // let the relay settle before dropping PTT
        radio_hw_set_tx_relay(0);
        in_tx = 0;
        printf("TX off\n");
    }
}
