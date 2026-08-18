# minibitx    -- an experimental test bed
PROJECT STATUS: Currently trying to get it working in receive mode only.

DESCRIPTION:  Minimal set of code to initialize sbitx hardware and allow external SDR software (like quisk or SDRConsole) to connect and use the radio.  It runs on the rpi board in your sbitx as an alternative to the 'sbitx' software that came with the radio.  It provides no user interface at all (other than minimal messages on the console used to start minibitx). I started by looking at the big files making up the sbitx system, and asking what is the minimum set of code I need to preserve to allow external SDR apps to run. Of course I am tempted to keep putting more and more back in, but I'm showing some restraint.

•	main() (src/minibitx.c) brings up the pieces in order: GPIO/hardware init, the oscillator chip and software RX VFO, the network thread (hpsdr_poll), the audio codec, and the audio thread (sound_thread_start). It does no hardware or protocol work itself.

•	Radio hardware control — GPIO setup, LPF band switching, board-revision detection, and the INA260 power monitor — lives in src/radio_hw.c/.h, carried over from the same split done in the sbitx repo. The si5351 oscillator (src/si5351v2.c) and the I2C bus driver (src/i2cbb.c) remain their own single-purpose files.

•	Tuning and remote-control glue — current frequency/mode state, radio_tune_to(), and radio_set_tx() (T/R switch + PTT: drives EXT_PTT and TX_LINE in the correct order with relay-settling delays) — lives in src/radio.c/.h. remote_execute() and radio_set_tx() there are the entry points control surfaces call to drive the radio; today the HPSDR command parser (src/hpsdr_p1.c) calls both (freq changes and the MOX bit), but they're meant to be shared once a second control surface (e.g. Hamlib/rigctld) is added.

•	The external SDR app discovers the radio via the UDP thread and sends a start stream command.

•	The audio thread (src/sound.c) reads IF data from the audio chip at 48k samples per second, does a 24 kHz anti-aliasing filter and sends 48k samples per second to sound_process().

•	sound_process() (src/sound.c) performs complex mixing to baseband, passes the arrays of I and Q data to hpsdr_send_iq() for hpsdr Protocol 1 over the network.

•	hpsdr_send_iq() (src/hpsdr_p1.c) feeds two independent consumers of that same IQ: the HPSDR/UDP stream, and — if the hardware/kernel support it — a USB Audio Class 2.0 (UAC2) gadget in src/usb_gadget.c/.h that presents the radio as a standard USB audio capture device. Either can be used without the other; USB audio doesn't require an HPSDR client to be connected.

•	The external SDR app handles all FFT processing, demodulation of various signal types, and audio routing


