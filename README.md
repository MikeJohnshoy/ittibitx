# minibitx    -- an experimental test bed
PROJECT STATUS: Still working on receive mode. Transmit mode will come next.

DESCRIPTION:  Use sbitx hardware with external SDR software like quisk or SDRConsole.  Runs on the rpi board in your sbitx as an alternative to the 'sbitx' software that came with the radio.  minibitx allows controlling the sbitx hardware via HAMLIB / rigctl commands, and outputs baseband I&Q data via a subset of HPSDR protocol 1 and/or USB audio.  There are no dependencies on sbitx software, 'minibitx' expects to run stand-alone.  It provides no user interface of its own (other than minimal messages on the console used to start minibitx) - the external SDR software provides all the processing needed for receive and transmit functions. I started by looking at the large files making up the sbitx system, and took functions out to achieve the minimum set of code needed for external SDR apps to run. 

•	main() (src/minibitx.c) brings up the sbitx hardware pieces in order: GPIO/hardware init, the oscillator and software RX VFO, the network thread (hpsdr_poll), the audio codec, and the audio thread (sound_thread_start).

•	Radio hardware control - GPIO setup, LPF band switching, board-revision detection, and the INA260 power monitor - lives in src/radio_hw.c/.h, carried over from the same split done in the sbitx repo. The si5351 oscillator (src/si5351v2.c) and the I2C bus driver (src/i2c.c) are supported with their own single-purpose files.

•	Tuning and remote-control glue - current frequency/mode state, radio_tune_to(), and radio_set_tx() (T/R switch + PTT: drives EXT_PTT and TX_LINE in the correct order with relay-settling delays) - lives in src/radio.c/.h. radio_tune_to() and radio_set_tx() are the only entry points any control surface uses to drive the radio - never a second place that pokes hardware directly. Two callers today: the Hamlib/rigctld server (src/hamlib.c) is the sole frequency control surface (f/F), and the HPSDR command parser (src/hpsdr_p1.c) calls radio_set_tx() only, for MOX/PTT, since some SDR apps key PTT through the I/Q link's C0 byte even while using CAT for everything else.

•	src/hamlib.c/.h runs a minimal rigctld-compatible TCP server on port 4532, alongside the HPSDR/UDP link.  rigctld is the standard side-channel many SDR apps expect.  It implements a tiny plain-text rigctl command set (f/F get/set frequency, t/T get/set PTT, m/M get/set mode - cosmetic only, since minibitx has no onboard demod - dump_state/chk_vfo for capability negotiation, and q/Q to disconnect) and drives radio_tune_to()/radio_set_tx() - the same entry points radio.c exposes to every control surface, described above. It is a small command set because we are not controlling a radio ... the SDR app is the radio!  Point an SDR app's CAT/rig control at 127.0.0.1:4532 (rig model "Hamlib NET rigctl") alongside its HPSDR connection to get live retuning.

•	The external SDR app discovers the radio via the UDP thread or USB audio gadget.

•	The audio thread (src/sound.c) reads IF data from the audio chip at 96k samples per second and sends it straight to sound_process() - no anti-aliasing filter or decimation, since minibitx captures natively at 48k with nothing to filter down from.

•	sound_process() (src/sound.c) performs complex mixing to baseband, then concludes by handing the block's I and Q arrays to each consumer as its own copy: uac_push_iq() (src/usb_gadget.c) and hpsdr_send_iq() (src/hpsdr_p1.c). Neither of those two modules knows the other exists.

•	hpsdr_send_iq() (src/hpsdr_p1.c) packetizes its copy of the IQ into HPSDR Protocol 1 UDP frames for the network stream - it has no dependency on src/usb_gadget.c. USB Audio Class 2.0 (UAC2) support - a gadget in src/usb_gadget.c/.h that presents the radio as a standard USB audio capture device, if the hardware/kernel support it - is fed its own IQ copy directly from sound.c instead. Either stream works without the other; a client on USB audio alone, with no HPSDR app connected, still gets IQ, and vice versa.

•	The external SDR app handles all FFT processing, demodulation of various signal types, and audio routing

•	main()'s idle loop calls status_print() (src/status.c/.h) once a second: a single console line showing current frequency, TX/RX state, and TX drive level (shown as "n/a" until a real drive value exists - there's no TX audio path yet). On a terminal it redraws in place instead of scrolling; redirected to a file or systemd/journald, it falls back to one plain line per second.
