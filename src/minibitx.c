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
  // bus it needs - see si5351v2.c) and the software RX VFO, then tune to
  // the startup frequency.
  si5351bx_init();
  vfo_init_phase_table();
  // radio_tune_to() below immediately re-starts the software RX VFO at
  // the correct fixed IF (RX_IF_FREQ_HZ - see radio.h), so this call
  // just needs the phase table ready and lo in a valid state first.
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
