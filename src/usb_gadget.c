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
static int      uac_buf_pos  = 0;   // next frame slot to fill (in frames)

/* ---------------------------------------------------------------------
 * Module-level state
 * --------------------------------------------------------------------- */
static snd_pcm_t   *uac_pcm_handle = NULL;  // ALSA PCM write handle
static int          uac_gadget_up  = 0;     // 1 after configfs gadget is created
static volatile int uac_active     = 0;     // 1 while host is streaming

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

// Probe for the first available ALSA loopback card by scanning /proc/asound.
// Sets card_idx to the card number and returns 0 on success, -1 if not found.
static int uac_find_loopback_card(int *card_idx) {
    for (int c = 0; c < 32; c++) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/asound/card%d/id", c);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char id[64] = {0};
        if (fgets(id, sizeof(id), f)) {
            // Strip trailing newline
            id[strcspn(id, "\n")] = '\0';
            if (strcmp(id, "Loopback") == 0) {
                *card_idx = c;
                fclose(f);
                return 0;
            }
        }
        fclose(f);
    }
    return -1;   // snd-aloop not loaded or no loopback card present
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
// Idempotent: if the gadget already exists (leftover from a crash), this
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

    // Unbind: write empty string to UDC
    snprintf(path, sizeof(path), "%s/UDC", UAC_GADGET_ROOT);
    uac_write_attr(path, "");

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
 * ALSA loopback PCM setup
 * --------------------------------------------------------------------- */

// Open and configure the ALSA loopback PCM for write (playback side).
// The UAC2 function driver in the kernel reads the capture side of the same
// loopback card and feeds it to the USB host as the audio stream.
// Returns 0 on success, -1 on ALSA error.
static int uac_alsa_open(void) {
    // Locate the loopback card index
    int card_idx = -1;
    if (uac_find_loopback_card(&card_idx) < 0) {
        fprintf(stderr, "uac: snd-aloop not loaded — run: modprobe snd-aloop\n");
        return -1;
    }

    // Playback side of the loopback: device 0, subdevice 1
    char dev_name[64];
    snprintf(dev_name, sizeof(dev_name), "hw:%d,0,1", card_idx);

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

    printf("uac: ALSA loopback PCM opened: %s @ %u Hz, 24-bit, %d ch\n",
           dev_name, rate, UAC_CHANNELS);
    return 0;
}

/* ---------------------------------------------------------------------
 * Public API — see usb_gadget.h
 * --------------------------------------------------------------------- */

int uac_init(void) {
    if (uac_gadget_create() < 0) {
        // uac_gadget_create already printed the reason
        return -1;
    }
    uac_gadget_up = 1;

    if (uac_alsa_open() < 0) {
        uac_gadget_destroy();
        uac_gadget_up = 0;
        return -1;
    }

    uac_active = 1;
    printf("uac: USB IQ audio stream ready — device name: 'sBitx IQ'\n");
    return 0;
}

void uac_push_iq(double i_val, double q_val) {
    if (!uac_pcm_handle || !uac_active) return;

    // Clamp to [-1, 1] before conversion
    if (i_val >  1.0) i_val =  1.0;
    if (i_val < -1.0) i_val = -1.0;
    if (q_val >  1.0) q_val =  1.0;
    if (q_val < -1.0) q_val = -1.0;

    // Scale to 24-bit signed integer range and pack as 3-byte little-endian
    // (SND_PCM_FORMAT_S24_3LE: bytes stored as [LSB, mid, MSB])
    int32_t i_int = (int32_t)(i_val * 8388607.0);   // 2^23 - 1
    int32_t q_int = (int32_t)(q_val * 8388607.0);

    uint8_t *slot = uac_pcm_buf + uac_buf_pos * UAC_FRAME_BYTES;
    // I sample (left channel)
    slot[0] = (uint8_t)( i_int        & 0xFF);
    slot[1] = (uint8_t)((i_int >>  8) & 0xFF);
    slot[2] = (uint8_t)((i_int >> 16) & 0xFF);
    // Q sample (right channel)
    slot[3] = (uint8_t)( q_int        & 0xFF);
    slot[4] = (uint8_t)((q_int >>  8) & 0xFF);
    slot[5] = (uint8_t)((q_int >> 16) & 0xFF);

    uac_buf_pos++;

    if (uac_buf_pos >= UAC_BUF_FRAMES) {
        // Flush a full period to the ALSA loopback
        snd_pcm_sframes_t written = snd_pcm_writei(
            uac_pcm_handle, uac_pcm_buf, (snd_pcm_uframes_t)UAC_BUF_FRAMES);

        if (written == -EPIPE) {
            // Buffer underrun: attempt recovery then retry once
            snd_pcm_prepare(uac_pcm_handle);
            snd_pcm_writei(uac_pcm_handle, uac_pcm_buf,
                           (snd_pcm_uframes_t)UAC_BUF_FRAMES);
        } else if (written < 0) {
            // Other ALSA error — log once, then recover
            fprintf(stderr, "uac: snd_pcm_writei error: %s\n",
                    snd_strerror((int)written));
            snd_pcm_recover(uac_pcm_handle, (int)written, 1 /*silent*/);
        }

        uac_buf_pos = 0;
    }
}

void uac_stop(void) {
    uac_active = 0;

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
