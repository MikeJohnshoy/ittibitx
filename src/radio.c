// radio.c - see radio.h

#include "radio.h"
#include "radio_hw.h"
#include "si5351.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wiringPi.h>   // for delay() - relay-settling time, not GPIO access

int freq_hdr = 7074000;
int in_tx = 0;
int bfo_freq = 40035000;
struct vfo lo;

void radio_tune_to(uint32_t f) {
    freq_hdr = f;
    si5351bx_setfreq(2, f + bfo_freq - 24000);
    vfo_start(&lo, freq_hdr, lo.phase);
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

// hpsdr_p1.c parses EP2 host commands and calls this to change frequency
void remote_execute(char *command) {
  if (strncmp(command, "freq ", 5) == 0) {
    int f = atoi(command + 5);
    if (f > 0) {
      radio_tune_to(f);
    }
  }
}
