# minibitx    -- an experimental test bed
STATUS  Compiles, establishes connection with SDRConsole, but no IQ data being passed yet ...

Minimal set of code to initialize sbitx hardware and connect to external SDR software, like quisk or SDRConsole.

•	main() (src/minibitx.c) brings up the pieces in order: GPIO/hardware init, the oscillator and software RX VFO, the network thread (hpsdr_poll), the audio codec, and the audio thread (sound_thread_start). It does no hardware or protocol work itself.

•	Radio hardware control — GPIO setup, LPF band switching, board-revision detection, and the INA260 power monitor — lives in src/radio_hw.c/.h, carried over from the same split done in the sbitx repo. The si5351 oscillator (src/si5351v2.c) and the I2C bus driver (src/i2cbb.c) remain their own single-purpose files.

•	Tuning and remote-control glue — current frequency/mode state and radio_tune_to() — lives in src/radio.c/.h. remote_execute() there is the single entry point control surfaces call to drive the radio; today only the HPSDR command parser (src/hpsdr_p1.c) calls it, but it's meant to be shared once a second control surface (e.g. Hamlib/rigctld) is added.

•	The external SDR app discovers the radio via the UDP thread and sends a start stream command.

•	The audio thread (src/sound.c) reads IF data from the audio chip at 48k samples per second, does a 24 kHz anti-aliasing filter and sends 48k samples per second to sound_process().

•	sound_process() (src/sound.c) performs complex mixing to baseband, passes the arrays of I and Q data to hpsdr_send_iq() for hpsdr Protocol 1 over the network.

•	The external SDR app handles all FFT processing, demodulation of various signal types, and audio routing
