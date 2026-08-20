// minibitx.c
// A tiny application that initializes the sbitx radio hardware,
// and allows a remote SDR application to control its operation over the network using
// a subset of openHPSDR Protocol 1.
//
// Hardware control (GPIO/LPF/oscillator/codec) lives in radio_hw.c and
// si5351v2.c; tuning/control glue lives in radio.c; audio capture and
// IQ mixing live in sound.c; the optional USB Audio Class IQ gadget lives
// in usb_gadget.c; the console status line lives in status.c. This file
// just brings them up in order.

#include "hpsdr_p1.h"
#include "si5351.h"
#include "sound.h"
#include "vfo.h"
#include "radio.h"
#include "radio_hw.h"
#include "usb_gadget.h"
#include "status.h"
#include "hamlib.h"
#include <stdio.h>
#include <unistd.h>

// Standard rigctld TCP port. See hamlib.h for why this exists: SDR
// Console (and likely other clients) don't push live frequency changes
// over the HPSDR link once streaming - this is the CAT-style control
// surface they expect instead, running alongside it.
#define HAMLIB_PORT 4532

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
  // bus it needs - see si5351v2.c). si5351bx_init() explicitly powers
  // down all three clocks, so clk1 - the BFO that drives the crystal
  // filter's second mixer stage, fixed at bfo_freq - has to be started
  // here; nothing else in minibitx ever touches clk1. Without it the RF
  // front end has no LO for that stage, so the ADC sees noise instead of
  // the actual downconverted signal - no CW/FT8/WWV, just a noise floor,
  // even though clk2 (RX tuning) and the software IF mixing are both
  // correct. Matches sbitx.c's setup_oscillators().
  si5351bx_init();
  si5351bx_setfreq(1, bfo_freq);
  si5351_reset();

  // Then bring up the software RX VFO's phase table and tune to the
  // startup frequency - radio_tune_to() starts the software oscillator
  // itself, fixed at RX_IF_FREQ_HZ (see radio.h).
  vfo_init_phase_table();
  vfo_start(&lo, RX_IF_FREQ_HZ, 0);
  radio_tune_to(freq_hdr);

  // Initialize Networking (HPSDR Protocol 1)
  if (hpsdr_init() < 0) {
    fprintf(stderr, "Failed to bind HPSDR socket\n");
    return -1;
  }
  hpsdr_poll(); // Starts the listener thread for connection/tuning requests

  // Bring up the rigctld-compatible control surface (src/hamlib.c) -
  // not a hard failure if the port's unavailable, same as HPSDR/UAC2.
  if (hamlib_init(HAMLIB_PORT) < 0) {
    fprintf(stderr, "Hamlib/rigctld server unavailable, continuing without it\n");
  }

  // Bring up the USB Audio Class (UAC2) IQ gadget, if the hardware/kernel
  // support it (needs a USB device-mode controller and snd-aloop). Not a
  // hard failure if it's unavailable - minibitx keeps running over
  // HPSDR/UDP either way.
  if (uac_init() < 0) {
    fprintf(stderr, "USB IQ gadget unavailable, continuing without it\n");
  }

  // Initialize Audio
  setup_audio_codec();
  // this starts the background audio thread which repeatedly calls sound_process()
  sound_thread_start("hw:0,0");

  // Idle loop: keep the program alive and, once a second, refresh the
  // console status line (freq / TX-RX / drive) - see status.c.
  while (1) {
    sleep(1);
    status_print();
  }

  return 0;
}
