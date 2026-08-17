// minibitx.c
// A tiny application that initializes the sbitx radio hardware,
// and allows a remote SDR application to control its operation over the network using
// a subset of openHPSDR Protocol 1.
//
// Hardware control (GPIO/LPF/oscillator/codec) lives in radio_hw.c and
// si5351v2.c; tuning/control glue lives in radio.c; audio capture and
// IQ mixing live in sound.c. This file just brings them up in order.

#include "hpsdr_p1.h"
#include "si5351.h"
#include "sound.h"
#include "vfo.h"
#include "radio.h"
#include "radio_hw.h"
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  printf("Starting miniBitx IQ Streamer...\n");

  // Initialize wiringPi and put all GPIO lines (LPF relays, TX_LINE,
  // TX_POWER, EXT_PTT) into their idle state.
  if (radio_hw_gpio_init() < 0) {
    fprintf(stderr, "Failed to init wiringPi/GPIO\n");
    return -1;
  }

  // Initialize the si5351 clock generator (this also brings up the I2C
  // bus it needs — see si5351v2.c) and the software RX VFO, then tune to
  // the startup frequency.
  si5351bx_init();
  vfo_init_phase_table();
  vfo_start(&lo, freq_hdr, 0);
  radio_tune_to(freq_hdr);

  // Initialize Networking (HPSDR Protocol 1)
  if (hpsdr_init() < 0) {
    fprintf(stderr, "Failed to bind HPSDR socket\n");
    return -1;
  }
  hpsdr_poll(); // Starts the listener thread for connection/tuning requests

  // Initialize Audio
  setup_audio_codec();
  // this starts the background audio thread which repeatedly calls sound_process()
  sound_thread_start("hw:0,0");

  // main loop does nothing but keep the program alive
  while (1) {
    sleep(1);
  }

  return 0;
}
