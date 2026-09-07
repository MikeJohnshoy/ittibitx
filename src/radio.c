// radio.c

#include "radio.h"
#include "radio_hw.h"
#include "si5351.h"
#include "sound.h"
#include "cw.h"         // CW_PITCH_HZ - TX clk2 correction, see radio_tx_apply()
#include "hpsdr_p1.h"   // hpsdr_get_tx_freq()/hpsdr_get_rx_freq() - split CW, see radio_tx_apply()
#include <stdio.h>
#include <unistd.h>     // usleep() - relay-settling time, not GPIO access
#include <pthread.h>

int freq_hdr = 7030000;
int in_tx = 0;
// bfo_freq is NOT meant to sit at the crystal filter's actual center - it's
// deliberately offset from it. That's how a single diode-ring mixer (no
// I/Q hardware on this board - see the schematic) manages CW TX without
// producing two RF tones: placing the BFO off-center puts the wanted
// mixer product near the filter's low-loss center while pushing the
// unwanted one out toward the stopband (full derivation in cw.c's
// TX_IF_OFFSET_HZ comment and docs/dsp_design_notes/
// antialias_filter_design.md). The actual crystal filter center varies
// across sbitx/zbitx boards (measured data: ~40.0124 MHz on one board,
// ~22.6kHz below this default) - this default (40035000) was arrived at
// by ear on one particular board, not from a measurement of it. Users
// can set their own bfo_freq in hw_settings.ini if RX sounds off-center
// on their hardware - but TX_IF_OFFSET_HZ (cw.c) was derived FROM the
// gap between this default and one board's measured filter center, so
// changing bfo_freq without re-deriving TX_IF_OFFSET_HZ to match will
// throw away the CW image suppression that constant depends on.
int bfo_freq = 40035000;
struct vfo lo;

// "Master" gates the WM8731's whole analog output path - the same
// DAC/output-mixer chain that carries CW (and any future TX) audio out.
// Real sbitx's own set_tx_power_levels() (sbitx.c) drives this exact same
// ALSA control to 95 during TX, with the comment "Muting Master also
// mutes the PA, killing TX power regardless of the DRIVE setting" -
// i.e. this isn't a volume knob, it's what gates how much drive actually
// reaches the exciter/PA. Previously set to 70 here, an unverified guess
// (see the cw.c sample-scaling comment for the still-open, separate
// question of whether the digital sample amplitude feeding the DAC is
// also under-calibrated) - matching sbitx's bench-confirmed 95 instead.
#define TX_MASTER_VOL 95

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
        // Split CW: retune to the operator's TX VFO (HPSDR C&C addr
        // 0x01, tracked in hpsdr_p1.c and exposed via
        // hpsdr_get_tx_freq()) before keying. hpsdr_p1.c's own
        // process_ep2_frame() already does the equivalent retune for
        // network-driven MOX (an SDR app asserting its own PTT), but that
        // path is never reached by the LOCAL straight key (cw.c calls
        // radio_set_tx() directly, and process_ep2_frame() defers to the
        // key completely via cw_tx_active()) - this is that same logic
        // for this file's TX-worker path instead, so a physical key
        // gets split behavior too. Falls back to the last-known RX
        // frequency, then does nothing at all (transmits wherever
        // freq_hdr already is, exactly as before this change) if no
        // HPSDR client has ever sent either - so plain non-split
        // operation, and operation with no SDR Console/HPSDR client
        // connected at all (e.g. rigctld-only control), are both
        // unaffected.
        //
        // This runs on the TX worker thread (see the thread-handoff
        // comment above), not the audio thread cw.c calls from - si5351
        // I2C writes and GPIO access have no business on the real-time
        // audio path, same reasoning as everything else in this handoff.
        uint32_t tx_freq = hpsdr_get_tx_freq();
        if (!tx_freq) tx_freq = hpsdr_get_rx_freq();
        if (tx_freq && tx_freq != (uint32_t)freq_hdr)
            radio_tune_to(tx_freq);

        // Correct clk2 for the CW_PITCH_HZ gap between cw.c's TX carrier
        // (CW_PITCH_HZ + TX_IF_OFFSET_HZ) and the RX_IF_FREQ_HZ that
        // radio_tune_to()'s shared formula assumes - left uncorrected,
        // that gap makes the transmitted RF frequency land CW_PITCH_HZ
        // below the tuned dial frequency (bench-confirmed; see
        // docs/03_tx_processing_pipeline.md's "Known limitations" and
        // cw.h's comment on CW_PITCH_HZ for the full derivation). Applied
        // here - the one place all TX (straight key via cw.c, and remote
        // MOX via hpsdr_p1.c) funnels through, using freq_hdr AFTER the
        // possible split retune just above - rather than in
        // radio_tune_to() itself, since that call is also used for plain
        // RX retuning and must not carry this offset there. Done before
        // PTT/the relay so both the retune and the correction are in
        // effect before any RF actually reaches the antenna.
        si5351bx_setfreq(2, freq_hdr + bfo_freq - RX_IF_FREQ_HZ + CW_PITCH_HZ);
        radio_hw_set_ptt(1);
        usleep(20000);              // let PTT assert before keying the relay
        radio_hw_set_tx_relay(1);
        sound_mixer("hw:0", "Master", TX_MASTER_VOL); // feed the exciter
    } else {
        sound_mixer("hw:0", "Master", 0); // mute before dropping the relay
        radio_hw_set_ptt(0);
        usleep(5000);               // let the relay settle before dropping PTT
        radio_hw_set_tx_relay(0);

        // Split CW: snap back to the RX frequency now that TX has fully
        // disengaged, rather than leaving freq_hdr parked on the TX split
        // frequency until the next inbound EP2 packet's "follow the SDR
        // app's tuning while receiving" check (hpsdr_p1.c's
        // handle_command()) happens to notice and correct it. A no-op
        // whenever split never moved us (last_rx_freq == freq_hdr
        // already, or no client has ever sent addr 0x02 either).
        uint32_t rx_freq = hpsdr_get_rx_freq();
        if (rx_freq && rx_freq != (uint32_t)freq_hdr)
            radio_tune_to(rx_freq);

        // Restore clk2 to the plain RX formula (undo the +CW_PITCH_HZ
        // above) now that TX has fully disengaged - matters even when
        // the retune just above was a no-op, since radio_tune_to() only
        // sets the plain (non-CW-corrected) formula itself.
        si5351bx_setfreq(2, freq_hdr + bfo_freq - RX_IF_FREQ_HZ);
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
