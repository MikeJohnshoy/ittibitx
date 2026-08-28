// radio.c

#include "radio.h"
#include "radio_hw.h"
#include "si5351.h"
#include "sound.h"
#include <stdio.h>
#include <unistd.h>     // usleep() - relay-settling time, not GPIO access

int freq_hdr = 7074000;
int in_tx = 0;
// actual crystal filter center probably varies across sbitx and zbitx hardware.
// The default bfo_freq matches that crstal filter center freq and works on my
// hardware.  Users can set there own value in hw_settings.ini
int bfo_freq = 40035000;
struct vfo lo;

// "Master" gates the WM8731's whole analog output path - the same
// DAC/output-mixer chain that carries CW (and any future TX) audio out
#define TX_MASTER_VOL 70

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
        sound_mixer("hw:0", "Master", TX_MASTER_VOL); // feed the exciter
    } else {
        sound_mixer("hw:0", "Master", 0); // mute before dropping the relay
        radio_hw_set_ptt(0);
        usleep(5000);               // let the relay settle before dropping PTT
        radio_hw_set_tx_relay(0);
        in_tx = 0;
    }
}
