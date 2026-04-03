#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

/* Start the full-duplex ALSA audio thread (capture + playback).
   device_name is the ALSA PCM name, e.g. "hw:0,0".
   Returns 0 on success, -1 on failure. */
int  sound_thread_start(const char *device_name);

/* Stop the audio thread and release ALSA devices. */
void sound_thread_stop(void);

/* ALSA mixer control for WM8731 codec hardware setup.
   Handles switches, volumes, and enumerated controls. */
void sound_mixer(char *card_name, char *element, int make_on);

/* User-supplied callback — called from the audio thread each period.
   Defined in minibitx.c (not here). */
void sound_process(int32_t *input_rx, int32_t *input_mic,
                   int32_t *output_speaker, int32_t *output_tx,
                   int n_samples);

#endif /* SOUND_H */