# ittibitx
Minimal set of code to initialize sbitx hardware and connect to external SDR software like SDRConsole.

•	main() initializes the hardware and spawns the network thread (hpsdr_poll) and audio thread (sound_thread_start).

•	The external SDR app discovers the radio via the UDP thread and sends a start stream command.

•	The ALSA thread constantly reads chunks of 96kHz analog data from the radio hardware and hands it to sound_process().

•	sound_process() performs complex mixing to baseband, dumps the arrays of I and Q data into hpsdr_send_iq(), which decimates it to 48kHz, and sends EP6 packets (hpsdr Protocol 1) out over the network.

•	The external SDR app handles all FFT processing, demodulation, AGC, and audio routing
