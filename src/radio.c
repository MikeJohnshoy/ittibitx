// radio.c — see radio.h

#include "radio.h"
#include "radio_hw.h"
#include "si5351.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

// hpsdr_p1.c parses EP2 host commands and calls this to change frequency
void remote_execute(char *command) {
  if (strncmp(command, "freq ", 5) == 0) {
    int f = atoi(command + 5);
    if (f > 0) {
      radio_tune_to(f);
    }
  }
}
