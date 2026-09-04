/*
 * usb_gadget.c — see usb_gadget.h for scope and architecture.
 *
 * Ported near-verbatim from the UAC2 section of sbitx's hpsdr_p1.c
 * (Mike/KB2ML). No sBitx/GTK dependency in this file — only ALSA and
 * Linux configfs/sysfs — so the port was mechanical.
 */

#include "usb_gadget.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

/* ---------------------------------------------------------------------
 * Compile-time configuration
 * --------------------------------------------------------------------- */

// Root of the Linux USB gadget configfs hierarchy
#define UAC_GADGET_ROOT     "/sys/kernel/config/usb_gadget/sbitx_iq"

// PCM parameters to match the UAC2 descriptor
#define UAC_RATE            48000
#define UAC_CHANNELS        2
#define UAC_SAMPLE_BYTES    3           // packed on-wire bytes (24-bit PCM)
#define UAC_PERIOD_FRAMES   512         // ALSA period size in frames
#define UAC_PERIODS         4           // number of periods in the ring buffer

// One frame = UAC_CHANNELS * UAC_SAMPLE_BYTES bytes
#define UAC_FRAME_BYTES     (UAC_CHANNELS * UAC_SAMPLE_BYTES)

// Internal ring: hold up to UAC_PERIOD_FRAMES samples before each ALSA write
#define UAC_BUF_FRAMES      UAC_PERIOD_FRAMES
static uint8_t  uac_pcm_buf[UAC_BUF_FRAMES * UAC_FRAME_BYTES];

/* ---------------------------------------------------------------------
 * IQ handoff queue - see the "producer/consumer split" note above
 * uac_push_iq() below. Same lock-free SPSC ring design as hpsdr_p1.c's
 * IQ queue (that file's comments have the full derivation of why a
 * plain mutex isn't safe to share with a real-time producer thread) -
 * producer (uac_push_iq(), called from sound.c's SCHED_FIFO audio
 * thread) only ever writes uac_q_head; consumer (uac_writer_thread())
 * only ever writes uac_q_tail. Neither ever blocks on the other.
 * --------------------------------------------------------------------- */
#define UAC_QUEUE_CAP   8192               // power of two; ~170ms at 48kHz -
                                            // generous slack against USB-side stalls
#define UAC_QUEUE_MASK  (UAC_QUEUE_CAP - 1)

static double       uac_q_i[UAC_QUEUE_CAP];
static double       uac_q_q[UAC_QUEUE_CAP];
static atomic_uint  uac_q_head = 0;
static atomic_uint  uac_q_tail = 0;

/* ---------------------------------------------------------------------
 * Module-level state
 * --------------------------------------------------------------------- */
static snd_pcm_t   *uac_pcm_handle = NULL;  // ALSA PCM write handle - owned
                                             // by uac_writer_thread() only
static int          uac_gadget_up  = 0;     // 1 after configfs gadget is created
static volatile int uac_active     = 0;     // 1 while host is streaming
static pthread_t    uac_writer_tid;
static volatile int uac_writer_running = 0; // 1 while uac_writer_thread() should keep looping

/* ---------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

// Write a NUL-terminated string to a sysfs/configfs attribute file.
// Returns 0 on success, -1 on error (errno is preserved).
static int uac_write_attr(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t n = write(fd, value, strlen(value));
    close(fd);
    return (n == (ssize_t)strlen(value)) ? 0 : -1;
}

// Create a directory if it does not already exist.
// Mirrors `mkdir -p` for a single level.
static int uac_mkdir(const char *path) {
    if (mkdir(path, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

// Create a symbolic link, tolerating EEXIST.
static int uac_symlink(const char *target, const char *link) {
    if (symlink(target, link) < 0 && errno != EEXIST) return -1;
    return 0;
}

// Probe for an ALSA card whose /proc/asound/cardN/id matches target_id
// exactly. Sets card_idx to the card number and returns 0 on success,
// -1 if not found.
static int uac_find_card_by_id(const char *target_id, int *card_idx) {
    for (int c = 0; c < 32; c++) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/asound/card%d/id", c);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char id[64] = {0};
        if (fgets(id, sizeof(id), f)) {
            // Strip trailing newline
            id[strcspn(id, "\n")] = '\0';
            if (strcmp(id, target_id) == 0) {
                *card_idx = c;
                fclose(f);
                return 0;
            }
        }
        fclose(f);
    }
    return -1;
}

// Detect the first UDC (USB Device Controller) available on this system by
// listing /sys/class/udc/. Copies the UDC name into buf (max len).
// Returns 0 on success, -1 if no UDC is found.
static int uac_find_udc(char *buf, size_t len) {
    DIR *d = opendir("/sys/class/udc");
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(buf, len, "%s", de->d_name);
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}

/* ---------------------------------------------------------------------
 * Gadget configuration via configfs
 * --------------------------------------------------------------------- */

// Create and configure the UAC2 gadget under configfs.
// Idempotent: if the gadget already exists (leftover from a previous run
// that didn't shut down cleanly - Ctrl+C, systemd restarting us, a crash,
// or SIGKILL - none of which reach uac_stop()/uac_gadget_destroy()), this
// function detects the existing tree and skips redundant mkdir/write calls.
// Returns 0 on success, -1 on any configfs error.
static int uac_gadget_create(void) {
    char path[256];

    // --- Gadget root ---
    if (uac_mkdir(UAC_GADGET_ROOT) < 0) {
        fprintf(stderr, "uac: cannot create gadget root %s: %s\n",
                UAC_GADGET_ROOT, strerror(errno));
        return -1;
    }

    // Any gadget directory that exists from here on needs cleaning up on
    // exit, whether or not the rest of this function (in particular the
    // bind at the very end) actually succeeds - see uac_stop(), which
    // only calls uac_gadget_destroy() when this is set. Previously this
    // was only set by uac_init() after a full success, so a run whose
    // bind attempt failed (exactly the case this whole function exists
    // to recover from) would skip cleanup on its own exit too, leaving
    // the mess for the *next* start's self-heal check below instead of
    // fixing it now.
    uac_gadget_up = 1;

    // If this gadget is left over from a previous run, it may still be
    // BOUND to a UDC from back then - "leftover but idle" isn't actually
    // possible here, since binding to a UDC is the very last step below,
    // so any leftover tree that exists at all is either fully unbound or
    // still bound exactly as the previous process left it. The kernel
    // refuses to write a UDC name into an already-bound gadget's UDC file
    // (EBUSY), even to rebind the same name - so without this check, the
    // bind attempt near the end of this function fails every time except
    // the very first boot (bench-confirmed: "uac: cannot bind to UDC
    // '<name>': Device or resource busy" on every minibitx restart after
    // the first, since nothing in this process's normal shutdown paths -
    // there mostly aren't any; see minibitx.c's main() - ever unbinds it).
    // Unbind first if so; safe/idempotent even when nothing was actually
    // bound (writing "\n" to an already-empty UDC file is a no-op).
    snprintf(path, sizeof(path), "%s/UDC", UAC_GADGET_ROOT);
    {
        FILE *udc_check = fopen(path, "r");
        if (udc_check) {
            char cur[256] = {0};
            if (fgets(cur, sizeof(cur), udc_check))
                cur[strcspn(cur, "\n")] = '\0';
            fclose(udc_check);
            if (cur[0] != '\0') {
                printf("uac: gadget still bound to '%s' from a previous run - unbinding first\n", cur);
                // NOT "" - see uac_gadget_destroy()'s comment on why an
                // actually-empty (0-byte) write here doesn't work, even
                // though it looks equivalent.
                if (uac_write_attr(path, "\n") < 0)
                    fprintf(stderr, "uac: unbind write failed: %s\n", strerror(errno));
            }
        }
        // ENOENT here just means no leftover tree at all (first boot,
        // or a prior clean uac_gadget_destroy()) - nothing to unbind.
    }

    // USB IDs: use the HermesLite vendor/product pair to stay compatible with
    // SDR apps that enumerate by USB ID, while the product string distinguishes us.
    uac_write_attr(UAC_GADGET_ROOT "/idVendor",  "0x04B4");   // Cypress / generic
    uac_write_attr(UAC_GADGET_ROOT "/idProduct", "0x0008");   // generic audio
    uac_write_attr(UAC_GADGET_ROOT "/bcdUSB",    "0x0200");   // USB 2.0
    uac_write_attr(UAC_GADGET_ROOT "/bcdDevice", "0x0100");

    // --- String descriptors (English) ---
    snprintf(path, sizeof(path), "%s/strings/0x409", UAC_GADGET_ROOT);
    uac_mkdir(path);
    snprintf(path, sizeof(path), "%s/strings/0x409/manufacturer", UAC_GADGET_ROOT);
    uac_write_attr(path, "sBitx");
    snprintf(path, sizeof(path), "%s/strings/0x409/product", UAC_GADGET_ROOT);
    uac_write_attr(path, "sBitx IQ");
    snprintf(path, sizeof(path), "%s/strings/0x409/serialnumber", UAC_GADGET_ROOT);
    uac_write_attr(path, "0000001");

    // --- UAC2 function ---
    snprintf(path, sizeof(path), "%s/functions/uac2.0", UAC_GADGET_ROOT);
    if (uac_mkdir(path) < 0 && errno != EEXIST) {
        fprintf(stderr, "uac: cannot create uac2 function: %s\n", strerror(errno));
        return -1;
    }

    // Capture (host reads IQ from us): 2 ch, 24-bit, 48 kHz
    snprintf(path, sizeof(path), "%s/functions/uac2.0/c_srate",  UAC_GADGET_ROOT);
    uac_write_attr(path, "48000");
    snprintf(path, sizeof(path), "%s/functions/uac2.0/c_ssize",  UAC_GADGET_ROOT);
    uac_write_attr(path, "3");           // 3 bytes = 24-bit PCM
    snprintf(path, sizeof(path), "%s/functions/uac2.0/c_chmask", UAC_GADGET_ROOT);
    uac_write_attr(path, "3");           // bitmask: ch0 | ch1  = L+R

    // Playback (host -> device, unused but the UAC2 function requires it)
    snprintf(path, sizeof(path), "%s/functions/uac2.0/p_srate",  UAC_GADGET_ROOT);
    uac_write_attr(path, "48000");
    snprintf(path, sizeof(path), "%s/functions/uac2.0/p_ssize",  UAC_GADGET_ROOT);
    uac_write_attr(path, "3");
    snprintf(path, sizeof(path), "%s/functions/uac2.0/p_chmask", UAC_GADGET_ROOT);
    uac_write_attr(path, "3");

    // --- Config c.1 ---
    snprintf(path, sizeof(path), "%s/configs/c.1", UAC_GADGET_ROOT);
    uac_mkdir(path);
    snprintf(path, sizeof(path), "%s/configs/c.1/strings/0x409", UAC_GADGET_ROOT);
    uac_mkdir(path);
    snprintf(path, sizeof(path), "%s/configs/c.1/strings/0x409/configuration",
             UAC_GADGET_ROOT);
    uac_write_attr(path, "Default");
    snprintf(path, sizeof(path), "%s/configs/c.1/bmAttributes", UAC_GADGET_ROOT);
    uac_write_attr(path, "0xC0");        // self-powered + bus-powered
    snprintf(path, sizeof(path), "%s/configs/c.1/MaxPower", UAC_GADGET_ROOT);
    uac_write_attr(path, "250");         // 250 x 2 mA = 500 mA

    // --- Link function into config ---
    char func_abs[256], link_path[256];
    snprintf(func_abs,  sizeof(func_abs),  "%s/functions/uac2.0", UAC_GADGET_ROOT);
    snprintf(link_path, sizeof(link_path), "%s/configs/c.1/uac2.0", UAC_GADGET_ROOT);
    uac_symlink(func_abs, link_path);

    // --- Bind to the UDC ---
    char udc_name[256] = {0};   // sized to match dirent.d_name's worst case
    if (uac_find_udc(udc_name, sizeof(udc_name)) < 0) {
        fprintf(stderr, "uac: no UDC found — USB gadget not available\n");
        // Not a hard failure: minibitx continues over HPSDR/UDP without UAC
        return -1;
    }
    snprintf(path, sizeof(path), "%s/UDC", UAC_GADGET_ROOT);
    if (uac_write_attr(path, udc_name) < 0) {
        fprintf(stderr, "uac: cannot bind to UDC '%s': %s\n", udc_name, strerror(errno));
        return -1;
    }

    printf("init: USB IQ gadget bound to UDC '%s'\n", udc_name);
    return 0;
}

// Tear down the gadget: unbind from UDC, unlink function, remove configfs nodes.
// A best-effort cleanup — errors are logged but not fatal.
static void uac_gadget_destroy(void) {
    char path[256];

    // Unbind: write a single newline to UDC - NOT a true empty (0-byte)
    // write. uac_write_attr(path, "") passes strlen("") == 0 through to
    // write(2), and a 0-length write here does not reliably reach the
    // kernel gadget driver's UDC store callback (bench-confirmed: the
    // write "succeeds" - uac_write_attr() returns 0, since 0 bytes
    // requested really did become 0 bytes written - but the gadget stays
    // bound, and the next bind attempt still fails with EBUSY). The
    // universally-used shell idiom for this is `echo "" > UDC`, which
    // writes one byte (a newline), not zero - matching that exactly
    // rather than what looks like the more literal translation of "empty
    // string" is what actually triggers the kernel's unbind path.
    snprintf(path, sizeof(path), "%s/UDC", UAC_GADGET_ROOT);
    uac_write_attr(path, "\n");

    // Remove the function symlink from the config
    snprintf(path, sizeof(path), "%s/configs/c.1/uac2.0", UAC_GADGET_ROOT);
    unlink(path);

    // Remove config strings, config, function strings directories in order
    // (configfs requires directories to be emptied before rmdir)
    char dirs[5][256];
    snprintf(dirs[0], 256, "%s/configs/c.1/strings/0x409",  UAC_GADGET_ROOT);
    snprintf(dirs[1], 256, "%s/configs/c.1",                UAC_GADGET_ROOT);
    snprintf(dirs[2], 256, "%s/functions/uac2.0",           UAC_GADGET_ROOT);
    snprintf(dirs[3], 256, "%s/strings/0x409",              UAC_GADGET_ROOT);
    snprintf(dirs[4], 256, "%s",                            UAC_GADGET_ROOT);
    for (int i = 0; i < 5; i++)
        rmdir(dirs[i]);   // silently tolerate ENOTEMPTY / ENOENT

    printf("uac: gadget removed\n");
}

/* ---------------------------------------------------------------------
 * UAC2 gadget ALSA PCM setup
 * --------------------------------------------------------------------- */

// Open and configure the UAC2 gadget's own ALSA PCM for write (playback
// side). Once uac_gadget_create() binds the gadget to a UDC, the kernel's
// UAC2 function driver (u_audio/f_uac2) registers its OWN independent ALSA
// card for it - always reported with id "UAC2Gadget" in
// /proc/asound/cards (bench-confirmed; this comes from the driver itself,
// not from anything sbitx_iq-specific we configure via configfs, so it's
// stable regardless of the gadget's configfs instance name). That card's
// local "playback" PCM device (device 0) is what's actually wired to the
// real USB isochronous endpoint the connected host reads as its capture/
// recording stream - writing here is what reaches the host; nothing else
// does.
//
// An earlier version of this function instead wrote to a separate
// `snd-aloop` "Loopback" card, on the mistaken assumption that the UAC2
// function reads its capture-side data from that loopback pair
// automatically. It doesn't: snd-aloop creates a fully self-contained
// pair of PCM devices with no relationship to any other card, so every
// sample written there stayed local to the Pi and never reached the USB
// link at all. That bug was invisible from minibitx's own console (the
// ALSA write to the loopback "succeeded" either way) and invisible from
// the host's side too (USB enumeration and configuration only depend on
// the configfs descriptors, not on what's actually feeding the PCM) - the
// only symptom was silence in the recording app, which is what exposed it
// (bench-confirmed 2026-09: 0 amplitude on a strong FT8 signal in
// Audacity on Windows, with the host-side descriptors otherwise fully
// correct and Current Config Value accepted). See
// docs/usb_gadget_os_setup.md §11 for the full bench story.
// Returns 0 on success, -1 on ALSA error.
static int uac_alsa_open(void) {
    int card_idx = -1;
    if (uac_find_card_by_id("UAC2Gadget", &card_idx) < 0) {
        fprintf(stderr, "uac: UAC2Gadget ALSA card not found - is the gadget bound to a UDC?\n");
        return -1;
    }

    // Device 0's playback substream - the local write side that feeds the
    // host's capture stream. Device 0's *capture* substream is the reverse
    // direction (host-to-device audio, unused here - see p_srate/p_ssize/
    // p_chmask in uac_gadget_create()), not this device string.
    char dev_name[64];
    snprintf(dev_name, sizeof(dev_name), "hw:%d,0", card_idx);

    int err;
    if ((err = snd_pcm_open(&uac_pcm_handle, dev_name,
                            SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "uac: snd_pcm_open(%s) failed: %s\n",
                dev_name, snd_strerror(err));
        uac_pcm_handle = NULL;
        return -1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(uac_pcm_handle, hw);

    // Interleaved, 24-bit packed LE, 48 kHz, 2 channels
    snd_pcm_hw_params_set_access(uac_pcm_handle, hw,
                                 SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(uac_pcm_handle, hw,
                                 SND_PCM_FORMAT_S24_3LE);
    unsigned int rate = UAC_RATE;
    snd_pcm_hw_params_set_rate_near(uac_pcm_handle, hw, &rate, NULL);
    snd_pcm_hw_params_set_channels(uac_pcm_handle, hw, UAC_CHANNELS);

    snd_pcm_uframes_t period = UAC_PERIOD_FRAMES;
    snd_pcm_hw_params_set_period_size_near(uac_pcm_handle, hw, &period, NULL);
    unsigned int periods = UAC_PERIODS;
    snd_pcm_hw_params_set_periods_near(uac_pcm_handle, hw, &periods, NULL);

    if ((err = snd_pcm_hw_params(uac_pcm_handle, hw)) < 0) {
        fprintf(stderr, "uac: snd_pcm_hw_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(uac_pcm_handle);
        uac_pcm_handle = NULL;
        return -1;
    }

    if ((err = snd_pcm_prepare(uac_pcm_handle)) < 0) {
        fprintf(stderr, "uac: snd_pcm_prepare failed: %s\n", snd_strerror(err));
        snd_pcm_close(uac_pcm_handle);
        uac_pcm_handle = NULL;
        return -1;
    }

    printf("uac: UAC2Gadget ALSA PCM opened: %s @ %u Hz, 24-bit, %d ch\n",
           dev_name, rate, UAC_CHANNELS);
    return 0;
}

/* ---------------------------------------------------------------------
 * Writer thread — owns uac_pcm_handle exclusively; the only thing that
 * ever calls snd_pcm_writei() on it.
 *
 * BUG FIX (reported: enabling the USB gadget alone, with no host
 * actually draining "sBitx IQ" yet, reintroduced sound.c's real
 * hardware xrun flood on hw:0,0 - the exact failure mode already fixed
 * once this project for hpsdr_p1.c, just in a different file). The
 * previous version of this file did the ALSA write inline,
 * synchronously, from uac_push_iq() - which is called from
 * sound_process(), on sound.c's SCHED_FIFO real-time audio thread. If
 * nothing is reading the other end of this PCM (no host plugged in, or
 * plugged in but no app has opened "sBitx IQ" for capture yet), the
 * gadget's own ring buffer fills within a few blocks and every
 * subsequent snd_pcm_writei() call here returns -EPIPE,
 * triggering an snd_pcm_prepare()+retry - real kernel/ioctl work,
 * paid for on the audio thread, every ~10.7ms, forever, silently (this
 * function printed nothing on -EPIPE). That's enough added latency per
 * iteration for the audio thread to miss hw:0,0's own real capture/
 * playback deadlines - i.e. a completely unrelated, absent-or-idle USB
 * host can starve the actual radio hardware. Same lesson as
 * hpsdr_p1.c's pacer-thread rewrite: a real-time producer thread must
 * never be able to block (or spend unbounded time) on a downstream
 * consumer's pace. Fix: uac_push_iq() (still called from the audio
 * thread) now only ever touches the lock-free queue above - it can't
 * block, and drops samples silently on overflow instead. This thread
 * is the sole consumer: it waits for a full period's worth of samples,
 * packs and writes them to the gadget's PCM exactly as before, and if
 * that blocks or errors, only USB audio quality suffers - sound.c's
 * real hardware path never sees it.
 * --------------------------------------------------------------------- */
static void *uac_writer_thread(void *arg) {
    (void)arg;

    // Consecutive-failure counter for the throttled logging below - see
    // the comment at the write-error handling at the bottom of the loop.
    unsigned err_streak = 0;

    while (uac_writer_running) {
        unsigned head = atomic_load_explicit(&uac_q_head, memory_order_acquire);
        unsigned tail = atomic_load_explicit(&uac_q_tail, memory_order_relaxed);
        unsigned available = (head - tail) & UAC_QUEUE_MASK;

        if (available < UAC_BUF_FRAMES) {
            // Not enough queued yet - this thread isn't real-time
            // critical (see above), so a plain short sleep is fine.
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000L };  // 2ms
            nanosleep(&ts, NULL);
            continue;
        }

        for (int s = 0; s < UAC_BUF_FRAMES; s++) {
            double i_val = uac_q_i[tail];
            double q_val = uac_q_q[tail];
            tail = (tail + 1) & UAC_QUEUE_MASK;

            // Clamp to [-1, 1] before conversion
            if (i_val >  1.0) i_val =  1.0;
            if (i_val < -1.0) i_val = -1.0;
            if (q_val >  1.0) q_val =  1.0;
            if (q_val < -1.0) q_val = -1.0;

            // Scale to 24-bit signed integer range and pack as 3-byte
            // little-endian (SND_PCM_FORMAT_S24_3LE: [LSB, mid, MSB])
            int32_t i_int = (int32_t)(i_val * 8388607.0);   // 2^23 - 1
            int32_t q_int = (int32_t)(q_val * 8388607.0);

            uint8_t *slot = uac_pcm_buf + s * UAC_FRAME_BYTES;
            // I sample (left channel)
            slot[0] = (uint8_t)( i_int        & 0xFF);
            slot[1] = (uint8_t)((i_int >>  8) & 0xFF);
            slot[2] = (uint8_t)((i_int >> 16) & 0xFF);
            // Q sample (right channel)
            slot[3] = (uint8_t)( q_int        & 0xFF);
            slot[4] = (uint8_t)((q_int >>  8) & 0xFF);
            slot[5] = (uint8_t)((q_int >> 16) & 0xFF);
        }
        atomic_store_explicit(&uac_q_tail, tail, memory_order_release);

        // Flush a full period to the gadget's PCM. This can block (or
        // fail) if nothing is draining the other side - that's now
        // confined to this thread only.
        snd_pcm_sframes_t written = snd_pcm_writei(
            uac_pcm_handle, uac_pcm_buf, (snd_pcm_uframes_t)UAC_BUF_FRAMES);

        if (written == -EPIPE) {
            // Buffer underrun (most commonly: no host draining the
            // gadget yet) - attempt recovery then retry once.
            snd_pcm_prepare(uac_pcm_handle);
            written = snd_pcm_writei(uac_pcm_handle, uac_pcm_buf,
                                     (snd_pcm_uframes_t)UAC_BUF_FRAMES);
        }

        if (written < 0) {
            // Most commonly this means no USB host has actually
            // activated this gadget's capture streaming interface yet -
            // cable unplugged, or plugged in but no app has opened the
            // capture stream for reading. In that state the kernel's
            // UAC2 function driver can't queue endpoint requests at
            // all, and every write here fails - usually with -EIO
            // rather than -EPIPE (bench-observed 2026-09; unlike the
            // old snd-aloop path this replaced, which silently buffered
            // instead of failing outright - see
            // docs/usb_gadget_os_setup.md §11). This recovers on its
            // own once a host attaches and starts draining - it's not
            // an error worth stopping over - but printing on every
            // single ~10.7ms period for as long as minibitx runs with
            // no host attached would flood the console/journal, so this
            // is throttled to the first occurrence and then once every
            // 500 (~5s at this period size) instead of every one.
            if (err_streak == 0 || err_streak % 500 == 0) {
                fprintf(stderr,
                        "uac: snd_pcm_writei error: %s (host not draining? "
                        "%u consecutive)\n",
                        snd_strerror((int)written), err_streak + 1);
            }
            err_streak++;
            snd_pcm_recover(uac_pcm_handle, (int)written, 1 /*silent*/);
        } else {
            err_streak = 0;
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------------
 * Public API — see usb_gadget.h
 * --------------------------------------------------------------------- */

int uac_init(void) {
    if (uac_gadget_create() < 0) {
        // uac_gadget_create already printed the reason. Note: it may
        // still have left a gadget directory that needs cleanup - it
        // sets uac_gadget_up itself now (as soon as it creates one),
        // precisely so a failed bind here doesn't skip that cleanup.
        return -1;
    }

    if (uac_alsa_open() < 0) {
        uac_gadget_destroy();
        uac_gadget_up = 0;
        return -1;
    }

    uac_writer_running = 1;
    if (pthread_create(&uac_writer_tid, NULL, uac_writer_thread, NULL) != 0) {
        fprintf(stderr, "uac: failed to start writer thread\n");
        uac_writer_running = 0;
        snd_pcm_close(uac_pcm_handle);
        uac_pcm_handle = NULL;
        uac_gadget_destroy();
        uac_gadget_up = 0;
        return -1;
    }

    uac_active = 1;
    printf("uac: USB IQ audio stream ready — device name: 'sBitx IQ'\n");
    return 0;
}

void uac_push_iq(double i_val, double q_val) {
    if (!uac_active) return;

    // Lock-free producer (see the writer-thread comment above): never
    // blocks, never touches uac_q_tail. On overflow (writer thread
    // stalled behind a slow/absent USB host) it silently drops the new
    // sample rather than waiting - exactly hpsdr_p1.c's
    // hpsdr_send_iq()/IQ_QUEUE pattern.
    unsigned head = atomic_load_explicit(&uac_q_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&uac_q_tail, memory_order_acquire);
    unsigned next_head = (head + 1) & UAC_QUEUE_MASK;
    if (next_head == tail) return;   // queue full - drop this sample

    uac_q_i[head] = i_val;
    uac_q_q[head] = q_val;
    atomic_store_explicit(&uac_q_head, next_head, memory_order_release);
}

void uac_stop(void) {
    uac_active = 0;

    if (uac_writer_running) {
        uac_writer_running = 0;
        pthread_join(uac_writer_tid, NULL);
    }

    if (uac_pcm_handle) {
        snd_pcm_drain(uac_pcm_handle);
        snd_pcm_close(uac_pcm_handle);
        uac_pcm_handle = NULL;
        printf("uac: ALSA PCM closed\n");
    }

    if (uac_gadget_up) {
        uac_gadget_destroy();
        uac_gadget_up = 0;
    }
}

int uac_is_active(void) {
    return uac_active;
}
