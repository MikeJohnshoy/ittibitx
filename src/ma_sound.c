// ma_sound.c
// Minimal miniaudio full-duplex driver for minibitx.
// Replaces sbitx_sound.c + sound.h
// Retains ALSA mixer control for WM8731 codec hardware setup.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "ma_sound.h"
#include <string.h>
#include <stdio.h>
#include <alsa/asoundlib.h>

// The user-defined callback in minibitx.c
extern void sound_process(int32_t *input_rx, int32_t *input_mic,
                          int32_t *output_speaker, int32_t *output_tx,
                          int n_samples);

static ma_device g_device;
static int       g_running = 0;

// -----------------------------------------------------------------------
// ALSA mixer helper — retained from sbitx for WM8731 codec configuration.
// Handles capture/playback switches, volumes, and enumerated controls.
// -----------------------------------------------------------------------
void sound_mixer(char *card_name, char *element, int make_on)
{
    long min, max;
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;

    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, card_name);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, element);
    snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);

    if (!elem) {
        snd_mixer_close(handle);
        return;
    }

    if (snd_mixer_selem_has_capture_switch(elem))
        snd_mixer_selem_set_capture_switch_all(elem, make_on);
    else if (snd_mixer_selem_has_playback_switch(elem))
        snd_mixer_selem_set_playback_switch_all(elem, make_on);
    else if (snd_mixer_selem_has_playback_volume(elem)) {
        snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
        snd_mixer_selem_set_playback_volume_all(elem, make_on * max / 100);
    }
    else if (snd_mixer_selem_has_capture_volume(elem)) {
        snd_mixer_selem_get_capture_volume_range(elem, &min, &max);
        snd_mixer_selem_set_capture_volume_all(elem, make_on * max / 100);
    }
    else if (snd_mixer_selem_is_enumerated(elem))
        snd_mixer_selem_set_enum_item(elem, 0, make_on);

    snd_mixer_close(handle);
}

// ---- miniaudio duplex callback ----
static void ma_data_callback(ma_device *pDevice,
                              void *pOutput, const void *pInput,
                              ma_uint32 frameCount)
{
    (void)pDevice;

    const int32_t *in  = (const int32_t *)pInput;
    int32_t       *out = (int32_t *)pOutput;
    int n = (int)frameCount;

    static int32_t rx_buf[4096];
    static int32_t mic_buf[4096];
    static int32_t spk_buf[4096];
    static int32_t tx_buf[4096];

    if (n > 4096) n = 4096;

    // Deinterleave stereo capture into two mono arrays.
    // channel roles retained from sbitx application
    //   input_rx  : left channel (RF baseband from codec)
    //   input_mic : right channel (reserved for future TX path)
    //   output_*  : muted here; TX/audio can be added later.
    for (int i = 0; i < n; i++) {
        rx_buf[i]  = in[i * 2];      // left channel
        mic_buf[i] = in[i * 2 + 1];  // right channel
    }

    sound_process(rx_buf, mic_buf, spk_buf, tx_buf, n);

    // Interleave output (currently silent — sound_process memsets to 0)
    for (int i = 0; i < n; i++) {
        out[i * 2]     = spk_buf[i];
        out[i * 2 + 1] = tx_buf[i];
    }
}

int sound_thread_start(const char *device_name)
{
    ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);

    cfg.capture.pDeviceID  = NULL;
    cfg.capture.format     = ma_format_s32;
    cfg.capture.channels   = 2;
    cfg.playback.pDeviceID = NULL;
    cfg.playback.format    = ma_format_s32;
    cfg.playback.channels  = 2;
    cfg.sampleRate         = 48000;
    cfg.periodSizeInFrames = 1024;
    cfg.dataCallback       = ma_data_callback;

    printf("ma_sound: opening duplex device @ 48 kHz S32 stereo\n");

    if (ma_device_init(NULL, &cfg, &g_device) != MA_SUCCESS) {
        fprintf(stderr, "ma_sound: failed to init device\n");
        return -1;
    }

    if (ma_device_start(&g_device) != MA_SUCCESS) {
        fprintf(stderr, "ma_sound: failed to start device\n");
        ma_device_uninit(&g_device);
        return -1;
    }

    g_running = 1;
    printf("ma_sound: running (%s)\n",
           device_name ? device_name : "default");
    return 0;
}

void sound_thread_stop(void)
{
    if (g_running) {
        ma_device_stop(&g_device);
        ma_device_uninit(&g_device);
        g_running = 0;
        printf("ma_sound: stopped\n");
    }
}