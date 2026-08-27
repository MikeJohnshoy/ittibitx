// minibitx.c
//
// A tiny application that initializes the sbitx radio hardware,
// and allows a remote SDR application to control its operation over the network using
// a subset of openHPSDR Protocol 1 and/or HAMLIB / rigctl.

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

// Standard rigctld TCP port
#define HAMLIB_PORT 4532

// Every hardware/subsystem init step below reports its own result with a
// consistent "init: ..." line (see docs/01_hardware_init_and_control.md),
// ending in the "radio hardware initialization complete" line. After initialization.
// operational state is reported as it's processed
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  printf("Starting miniBitx IQ Streamer...\n");

  // Initialize wiringPi and put all GPIO lines (LPF relays, TX_LINE,
  // TX_POWER, EXT_PTT) into their idle state.
  if (radio_hw_gpio_init() < 0) {
    fprintf(stderr, "init: GPIO/wiringPi setup failed\n");
    return -1;
  }
  printf("init: GPIO configured, T/R relay and PTT held low (RX-safe state)\n");

  // Initialize the si5351 clock generator (this also brings up the I2C
  // bus it needs). si5351bx_init() explicitly powers
  // down all three clocks, so clk1 - the BFO that drives the crystal
  // filter's second mixer stage, fixed at bfo_freq - has to be started
  // here; nothing else in minibitx ever touches clk1.
  si5351bx_init();
  si5351bx_setfreq(1, bfo_freq);
  si5351_reset();
  printf("init: si5351 oscillator ready, BFO (clk1) fixed at %d Hz\n", bfo_freq);

  // Board revision and the INA260 power monitor both live on the same I2C
  // bus si5351bx_init() just brought up, so they can only be probed after
  // it, not before.
  int hw_rev = radio_hw_detect_version();
  printf("init: board revision detected: %s\n",
         hw_rev == SBITX_V2 ? "sBitx v2 (power/SWR bridge present)"
                             : "sBitx DE (original, no power/SWR bridge)");

  if (radio_hw_ina260_configure() == 0) {
    printf("init: INA260 power monitor configured\n");
  } else {
    printf("init: INA260 power monitor not responding, continuing without it\n");
  }

  // Then bring up the software RX VFO's phase table and tune to the
  // startup frequency - radio_tune_to() starts the software oscillator
  // itself, fixed at RX_IF_FREQ_HZ (see radio.h).
  vfo_init_phase_table();
  vfo_start(&lo, RX_IF_FREQ_HZ, 0);
  radio_tune_to(freq_hdr);
  printf("init: RX VFO started, tuned to %d Hz\n", freq_hdr);

  // Initialize Networking (HPSDR Protocol 1)
  if (hpsdr_init() < 0) {
    fprintf(stderr, "init: HPSDR socket bind failed\n");
    return -1;
  }
  hpsdr_poll(); // Starts the listener thread for connection/tuning requests
  printf("init: HPSDR Protocol 1 listening on UDP %d\n", HPSDR_PORT);

  // Bring up the rigctld-compatible control surface (src/hamlib.c) -
  // not a hard failure if the port's unavailable, same as HPSDR/UAC2.
  // hamlib_init() reports its own success ("init: Hamlib/rigctld
  // listening..."); we only need to report the failure case here.
  if (hamlib_init(HAMLIB_PORT) < 0) {
    printf("init: Hamlib/rigctld unavailable on TCP %d, continuing without it\n",
           HAMLIB_PORT);
  }

  // Bring up the USB Audio Class (UAC2) IQ gadget, if the hardware/kernel
  // support it (needs a USB device-mode controller and snd-aloop). Not a
  // hard failure if it's unavailable - minibitx keeps running over
  // HPSDR/UDP either way. uac_init() reports its own success ("init: USB
  // IQ gadget bound to UDC...").
  if (uac_init() < 0) {
    printf("init: USB IQ gadget unavailable, continuing without it\n");
  }

  // Initialize Audio
  setup_audio_codec();
  printf("init: WM8731 audio codec configured\n");
  // this starts the background audio thread which repeatedly calls sound_process()
  if (sound_thread_start("hw:0,0") < 0) {
    fprintf(stderr, "init: audio capture failed to start\n");
    return -1;
  }
  printf("init: audio capture running (hw:0,0 @ 96000 Hz)\n");

  printf("minibitx: radio hardware initialization complete\n");

  // One-time snapshot of frequency/RX-TX/drive state at the moment control
  // passes to whatever external app connects next. status_print() redraws
  // in place on a terminal (see status.c), so it's only ever called this
  // once now - a trailing newline here keeps that from being overwritten
  // by whatever prints next.
  status_print();
  if (isatty(fileno(stdout))) putchar('\n');

  // Idle loop: keep the program alive. Operational state changes (tuning,
  // PTT) are reported as they're processed by hamlib.c/hpsdr_p1.c, not
  // polled here.
  while (1) {
    sleep(1);
  }

  return 0;
}
