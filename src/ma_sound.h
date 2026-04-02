// ma_sound.h
// Public interface for the miniaudio duplex driver.
// Drop-in replacement for sound.h in minibitx.

#ifndef MA_SOUND_H
#define MA_SOUND_H

#include <stdint.h>

// Start the duplex audio thread (capture + playback).
// device_name is informational only (miniaudio uses the default ALSA device).
// Returns 0 on success, -1 on failure.
int  sound_thread_start(const char *device_name);

// Stop the audio thread and release the device.
void sound_thread_stop(void);

// ALSA mixer control for WM8731 codec hardware setup.
// Handles switches, volumes, and enumerated controls.
void sound_mixer(char *card_name, char *element, int make_on);

// User-supplied callback — called from the audio thread each period.
// Defined in minibitx.c (not here).
void sound_process(int32_t *input_rx, int32_t *input_mic,
                   int32_t *output_speaker, int32_t *output_tx,
                   int n_samples);

#endif // MA_SOUND_H