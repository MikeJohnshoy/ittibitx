# minibitx    -- an experimental test bed
PROJECT STATUS: Currently trying to get it working in receive mode only.

DESCRIPTION:  Minimal set of code to initialize sbitx hardware and allow external SDR software (like quisk or SDRConsole) to connect and use the radio.  It runs on the rpi board in your sbitx as an alternative to the 'sbitx' software that came with the radio.  It provides no user interface at all (other than minimal messages on the console used to start minibitx). I started by looking at the big files making up the sbitx system, and asking what is the minimum set of code I need to preserve to allow external SDR apps to run. Of course I am tempted to keep putting more and more back in, but I'm showing some restraint.

•	main() (src/minibitx.c) brings up the pieces in order: GPIO/hardware init, the oscillator and software RX VFO, the network thread (hpsdr_poll), the audio codec, and the audio thread (sound_thread_start). It does no hardware or protocol work itself.

•	Radio hardware control - GPIO setup, LPF band switching, board-revision detection, and the INA260 power monitor - lives in src/radio_hw.c/.h, carried over from the same split done in the sbitx repo. The si5351 oscillator (src/si5351v2.c) and the I2C bus driver (src/i2cbb.c) remain their own single-purpose files.

•	Tuning and remote-control glue - current frequency/mode state, radio_tune_to(), and radio_set_tx() (T/R switch + PTT: drives EXT_PTT and TX_LINE in the correct order with relay-settling delays) - lives in src/radio.c/.h. radio_tune_to() and radio_set_tx() are the only entry points any control surface uses to drive the radio - never a second place that pokes hardware directly. Two callers today: the Hamlib/rigctld server (src/hamlib.c) is the sole frequency control surface (f/F), and the HPSDR command parser (src/hpsdr_p1.c) calls radio_set_tx() only, for MOX/PTT, since some SDR apps key PTT through the I/Q link's C0 byte even while using CAT for everything else.

•	src/hamlib.c/.h runs a minimal rigctld-compatible TCP server on port 4532, alongside the HPSDR/UDP link.  rigctld is the standard side-channel many SDR apps expect.  It implements a tiny plain-text rigctl command set (f/F get/set frequency, t/T get/set PTT, m/M get/set mode - cosmetic only, since minibitx has no onboard demod - dump_state/chk_vfo for capability negotiation, and q/Q to disconnect) and drives radio_tune_to()/radio_set_tx() - the same entry points radio.c exposes to every control surface, described above. It is a small command set because we are not controlling a radio ... the SDR app is the radio!  Point an SDR app's CAT/rig control at 127.0.0.1:4532 (rig model "Hamlib NET rigctl") alongside its HPSDR connection to get live retuning.

•	The external SDR app discovers the radio via the UDP thread and sends a start stream command.

•	The audio thread (src/sound.c) reads IF data from the audio chip at 48k samples per second and sends it straight to sound_process() - no anti-aliasing filter or decimation, since minibitx captures natively at 48k with nothing to filter down from.

•	sound_process() (src/sound.c) performs complex mixing to baseband, then concludes by handing the block's I and Q arrays to each consumer as its own copy: uac_push_iq() (src/usb_gadget.c) and hpsdr_send_iq() (src/hpsdr_p1.c). Neither of those two modules knows the other exists.

•	hpsdr_send_iq() (src/hpsdr_p1.c) packetizes its copy of the IQ into HPSDR Protocol 1 UDP frames for the network stream - it has no dependency on src/usb_gadget.c. USB Audio Class 2.0 (UAC2) support - a gadget in src/usb_gadget.c/.h that presents the radio as a standard USB audio capture device, if the hardware/kernel support it - is fed its own IQ copy directly from sound.c instead. Either stream works without the other; a client on USB audio alone, with no HPSDR app connected, still gets IQ, and vice versa.

•	The external SDR app handles all FFT processing, demodulation of various signal types, and audio routing

•	main()'s idle loop calls status_print() (src/status.c/.h) once a second: a single console line showing current frequency, TX/RX state, and TX drive level (shown as "n/a" until a real drive value exists - there's no TX audio path yet). On a terminal it redraws in place instead of scrolling; redirected to a file or systemd/journald, it falls back to one plain line per second.
