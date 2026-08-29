// sound.c
// Minimal ALSA full-duplex driver for minibitx.

#include "sound.h"
#include "radio.h"
#include "hpsdr_p1.h"
#include "usb_gadget.h"
#include "antialias.h"
#include "cw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <alsa/asoundlib.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */
#define SAMPLE_RATE      96000
#define CHANNELS         2        /* stereo: L = RX / R = Mic (capture) */
#define PERIOD_FRAMES    1024     /* frames per period (matches old cfg) */
#define MAX_FRAMES       4096

/* ------------------------------------------------------------------ */
/*  Module state                                                       */
/* ------------------------------------------------------------------ */
static snd_pcm_t *pcm_capture  = NULL;
static snd_pcm_t *pcm_playback = NULL;
static pthread_t  audio_thread;
static volatile int g_running = 0;

/* ------------------------------------------------------------------ */
/*  ALSA mixer helper                                                 */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  Codec hardware setup - barebones WM8731 init                      */
/* ------------------------------------------------------------------ */
void setup_audio_codec(void) {
  sound_mixer("hw:0", "Input Mux", 0);
  sound_mixer("hw:0", "Line", 80);  // 80% of max
  sound_mixer("hw:0", "Mic", 0);
  sound_mixer("hw:0", "Master", 0); // Mute local speaker
  sound_mixer("hw:0", "Output Mixer HiFi", 1);
  sound_mixer("hw:0", "Output Mixer Line Bypass", 0);
  sound_mixer("hw:0", "Output Mixer Mic Sidetone", 0);
}

/* ------------------------------------------------------------------ */
/*  ALSA PCM helpers                                                   */
/* ------------------------------------------------------------------ */
static snd_pcm_t *open_pcm(const char *dev, snd_pcm_stream_t dir)
{
    snd_pcm_t *pcm = NULL;
    int err;

    if ((err = snd_pcm_open(&pcm, dev, dir, 0)) < 0) {
        fprintf(stderr, "sound: cannot open %s (%s): %s\n",
                dev, dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                snd_strerror(err));
        return NULL;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS);

    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);

    snd_pcm_uframes_t period = PERIOD_FRAMES;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);

    /* 4 periods ~ 85 ms buffer - enough headroom for a Pi */
    snd_pcm_uframes_t buffer = period * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);

    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "sound: hw_params failed (%s): %s\n",
                dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    printf("sound: opened %s %s @ %u Hz, period %lu, buffer %lu\n",
           dev, dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
           rate, (unsigned long)period, (unsigned long)buffer);

    return pcm;
}

/* Recover from an ALSA xrun / suspend. Returns 0 on success. */
static int xrun_recover(snd_pcm_t *pcm, int err)
{
    if (err == -EPIPE) {                     /* underrun / overrun */
        fprintf(stderr, "sound: xrun, recovering\n");
        err = snd_pcm_prepare(pcm);
    } else if (err == -ESTRPIPE) {           /* suspended */
        while ((err = snd_pcm_resume(pcm)) == -EAGAIN)
            usleep(10000);
        if (err < 0) err = snd_pcm_prepare(pcm);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/*  IQ mixing                                                         */
/* ------------------------------------------------------------------ */
static void sound_process(int32_t *input_rx, int32_t *input_mic, int32_t *output_speaker,
                           int32_t *output_tx, int n_samples) {
    static double i_samples[4096];
    static double q_samples[4096];
    static int vfo_ready = 0;
    // filter I and Q with independent history per rail, same coefficients (antialias.c) -
    // zero-initialized once, persists across calls (each call is one
    // ~10.7ms block, not a fresh signal).
    static struct antialias_state aa_i;
    static struct antialias_state aa_q;

    (void)input_mic;
    if (n_samples > 4096) n_samples = 4096;

    if (!vfo_ready) {
        vfo_init_phase_table();
        // this init only matters if sound_process() is ever called before
        // main()'s own startup vfo_start()/radio_tune_to()
        vfo_start(&lo, RX_IF_FREQ_HZ, 0);
        vfo_ready = 1;
    }

    for (int n = 0; n < n_samples; n++) {
        int32_t s = input_rx[n];
        int lo_i, lo_q;
        vfo_read_iq(&lo, &lo_i, &lo_q);

        double rf = (double)s / 2147483648.0;

        // mix to IQ
        i_samples[n] = rf * ((double)lo_i / 1073741824.0);
        q_samples[n] = rf * ((double)lo_q / 1073741824.0);
     
        // Anti-alias lowpass, applied to I and Q right after mixing (see
        // docs/dsp_design_notes/antialias_filter_design.md). Helps clean up
        // the self-image near the +-48kHz Nyquist edges, without touching the
        // real signal content well within the crystal filter's passband.
        i_samples[n] = antialias_apply(&aa_i, i_samples[n]);
        q_samples[n] = antialias_apply(&aa_q, q_samples[n]);
    }

    // hand the block's IQ to each consumer as its own copy - hpsdr_p1.c
    // (network) and usb_gadget.c (USB Audio Class gadget) don't know about
    // each other, and either can be active without the other
    hpsdr_send_iq(i_samples, q_samples, n_samples);
    for (int n = 0; n < n_samples; n++)
        uac_push_iq(i_samples[n], q_samples[n]);

    // keep local outputs silent
    memset(output_speaker, 0, n_samples * sizeof(int32_t));
    memset(output_tx, 0, n_samples * sizeof(int32_t));
}

/* ------------------------------------------------------------------ */
/*  Audio thread - capture -> sound_process() -> playback               */
/* ------------------------------------------------------------------ */
static void *audio_loop(void *arg)
{
    (void)arg;

    // Real-time priority: as an ordinary SCHED_OTHER thread this competes
    // with everything else on the system and can be preempted long enough
    // to miss an ALSA period. zbitx's sbitx_sound.c hit underruns from the
    // same cause and fixed it by bumping the audio thread to SCHED_FIFO -
    // do the same here. Not fatal if it fails (no root / no CAP_SYS_NICE /
    // no rtprio limit) - just warn and keep running at normal priority.
    {
        struct sched_param sch;
        sch.sched_priority = sched_get_priority_max(SCHED_FIFO);
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sch);
        if (rc != 0) {
            fprintf(stderr,
                    "sound: WARNING - failed to set audio thread to SCHED_FIFO (%s).\n",
                    strerror(rc));
        }
    }

    int32_t cap_buf[MAX_FRAMES * CHANNELS];
    int32_t rx_buf[MAX_FRAMES];
    int32_t mic_buf[MAX_FRAMES];
    int32_t spk_buf[MAX_FRAMES];
    int32_t tx_buf[MAX_FRAMES];
    int32_t play_buf[MAX_FRAMES * CHANNELS];   // CW sidetone -> WM8731 DAC

    while (g_running) {
        snd_pcm_sframes_t frames = snd_pcm_readi(pcm_capture, cap_buf, PERIOD_FRAMES);
        if (frames < 0) {
            if (xrun_recover(pcm_capture, (int)frames) < 0) {
                fprintf(stderr, "sound: capture recovery failed\n");
                break;
            }
            continue;
        }

        int n = (int)frames;
        if (n > MAX_FRAMES) n = MAX_FRAMES;

        for (int i = 0; i < n; i++) {
            rx_buf[i]  = cap_buf[i * 2];
            mic_buf[i] = cap_buf[i * 2 + 1];
        }

        sound_process(rx_buf, mic_buf, spk_buf, tx_buf, n);

        // Once per audio block - checks the key, manages the CW keying
        // burst's hang timer, and asserts/releases PTT via radio_set_tx()
        // (see cw.c). Runs every iteration, TX or not, since this is what
        // actually notices the key going down in the first place.
        cw_poll_key();

        // Feed pcm_playback every block, TX or not - not just while a CW
        // burst is active. ALSA's underrun detection isn't tied to whether
        // writei() gets called; it's the hardware clock draining the ring
        // buffer against the software pointer. Only writing during a burst
        // meant the device sat with nothing arriving for however long the
        // key was up (seconds, easily), so its ~43ms buffer drained and it
        // underran on its own between every single burst - then the first
        // write of the next burst hit that stale underrun and had to
        // recover, which is exactly the "xrun, recovering" storm logged on
        // every key-down. Writing silence the rest of the time keeps the
        // device continuously running so it never gets the chance to idle
        // out - the same continuously-fed design real sbitx's own full
        // duplex audio path uses.
        if (pcm_playback) {
            if (cw_tx_active()) {
                for (int i = 0; i < n; i++) {
                    double s = cw_get_sample();
                    // Headroom below full-scale int32; the right drive level
                    // for the balanced modulator needs a bench check, this
                    // is a starting point, not a calibrated value.
                    int32_t v = (int32_t)(s * 1000000000.0);
                    play_buf[i * 2]     = v;   // L
                    play_buf[i * 2 + 1] = v;   // R - WM8731 output is stereo,
                                               // sidetone is mono, duplicate it
                }
            } else {
                memset(play_buf, 0, (size_t)n * 2 * sizeof(int32_t));
            }
            snd_pcm_sframes_t wframes = snd_pcm_writei(pcm_playback, play_buf, n);
            if (wframes < 0) xrun_recover(pcm_playback, (int)wframes);
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
int sound_thread_start(const char *device_name)
{
    const char *dev = device_name ? device_name : "hw:0,0";

    pcm_capture = open_pcm(dev, SND_PCM_STREAM_CAPTURE);
    if (!pcm_capture) return -1;

    // Playback: CW sidetone output only (see cw.c) - RX IQ still goes
    // out over the network/UAC2, not through here. Not a hard failure
    // if it doesn't open; audio_loop() checks pcm_playback before
    // writing to it, so minibitx still runs (just without CW TX audio).
    pcm_playback = open_pcm(dev, SND_PCM_STREAM_PLAYBACK);
    if (!pcm_playback) {
        printf("sound: playback unavailable, CW sidetone output disabled\n");
    }

    g_running = 1;
    if (pthread_create(&audio_thread, NULL, audio_loop, NULL) != 0) {
        fprintf(stderr, "sound: pthread_create failed\n");
        snd_pcm_close(pcm_capture);
        pcm_capture = NULL;
        g_running = 0;
        return -1;
    }

    printf("sound: running (%s)\n", dev);
    return 0;
}

void sound_thread_stop(void)
{
    if (!g_running) return;

    g_running = 0;
    pthread_join(audio_thread, NULL);

    if (pcm_capture)  { snd_pcm_drop(pcm_capture);  snd_pcm_close(pcm_capture);  }
    if (pcm_playback) { snd_pcm_drop(pcm_playback); snd_pcm_close(pcm_playback); }
    pcm_capture = pcm_playback = NULL;

    printf("sound: stopped\n");
}
