// radio.c

#include "radio.h"
#include "radio_hw.h"
#include "si5351.h"
#include "sound.h"
#include <stdio.h>
#include <unistd.h>     // usleep() - relay-settling time, not GPIO access
#include <pthread.h>

int freq_hdr = 7074000;
int in_tx = 0;
// actual crystal filter center probably varies across sbitx and zbitx hardware.
// The default bfo_freq matches that crstal filter center freq and works on my
// hardware.  Users can set there own value in hw_settings.ini
int bfo_freq = 40035000;
struct vfo lo;

// "Master" gates the WM8731's whole analog output path - the same
// DAC/output-mixer chain that carries CW (and any future TX) audio out
#define TX_MASTER_VOL 70

void radio_tune_to(uint32_t f) {
    freq_hdr = f;
    si5351bx_setfreq(2, f + bfo_freq - RX_IF_FREQ_HZ);
    vfo_start(&lo, RX_IF_FREQ_HZ, lo.phase);
    set_lpf_40mhz(f);    // enable the correct LPF for this band
}

// ---- TX worker thread ------------------------------------------------
//
// radio_set_tx() used to do its GPIO/relay-settling usleep()s and its
// sound_mixer() call inline, on whatever thread called it. That's fine
// for hamlib's or hpsdr_p1.c's network threads, but cw.c calls it from
// cw_poll_key(), which runs once per ~10.7ms audio block on the AUDIO
// thread (sound.c's audio_loop()). A single call there blocked for
// 20ms+ (PTT settle + relay settle + a fresh ALSA mixer handle open/
// attach/load/close), guaranteeing a missed capture period - exactly
// the "sound: xrun, recovering" logged on every key transition - and
// also making the physical key feel sluggish, since cw_poll_key()
// couldn't return to re-poll it until the blocking sequence finished.
//
// Fix: radio_set_tx() keeps its exact signature and still updates
// in_tx immediately/synchronously (cheap - every other guard in the
// codebase that reads in_tx keeps seeing a prompt, correct value).
// The actual slow hardware sequence is handed off to a dedicated
// worker thread via a mutex/condvar/pending-flag, so the calling
// thread (audio thread included) never blocks.
static pthread_mutex_t tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  tx_cond  = PTHREAD_COND_INITIALIZER;
static int tx_pending    = 0;   // 1 = worker has a state change to apply
static int tx_pending_on = 0;   // the state to apply (1 = TX, 0 = RX)
static pthread_once_t tx_worker_once = PTHREAD_ONCE_INIT;

static void radio_tx_apply(int tx_on)
{
    if (tx_on) {
        radio_hw_set_ptt(1);
        usleep(20000);              // let PTT assert before keying the relay
        radio_hw_set_tx_relay(1);
        sound_mixer("hw:0", "Master", TX_MASTER_VOL); // feed the exciter
    } else {
        sound_mixer("hw:0", "Master", 0); // mute before dropping the relay
        radio_hw_set_ptt(0);
        usleep(5000);               // let the relay settle before dropping PTT
        radio_hw_set_tx_relay(0);
    }
}

static void *radio_tx_worker(void *arg)
{
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&tx_mutex);
        while (!tx_pending)
            pthread_cond_wait(&tx_cond, &tx_mutex);
        int on = tx_pending_on;
        tx_pending = 0;
        pthread_mutex_unlock(&tx_mutex);

        radio_tx_apply(on);
    }

    return NULL;
}

static void radio_tx_worker_start(void)
{
    pthread_t worker;
    pthread_create(&worker, NULL, radio_tx_worker, NULL);
}

// switch between RX and TX
void radio_set_tx(int tx_on) {
    pthread_once(&tx_worker_once, radio_tx_worker_start);

    in_tx = tx_on ? 1 : 0;   // mirrors sbitx: hardware state follows intent,
                             // updated immediately so other threads' guards
                             // (cw_tx_active(), the network MOX logic, ...)
                             // see the new state right away, even though the
                             // physical relay/mixer change is still pending
                             // on the worker thread below

    pthread_mutex_lock(&tx_mutex);
    tx_pending_on = tx_on;
    tx_pending = 1;
    pthread_cond_signal(&tx_cond);
    pthread_mutex_unlock(&tx_mutex);
}
 
