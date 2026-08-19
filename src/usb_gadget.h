/*
 * usb_gadget.h — USB Audio Class 2.0 (UAC2) IQ streamer for minibitx.
 *
 * Ported from the UAC2 section Mike (KB2ML) added to sbitx's hpsdr_p1.c.
 * That file also carried a much larger HPSDR Protocol 1 rewrite (half-band
 * decimation, MOX debounce, TX IQ upsampling) built against sbitx's GTK app
 * — tx_on()/tx_off()/cmd_exec() and GLib's g_idle_add()/g_timeout_add() for
 * deferring T/R switches onto the GTK main thread. minibitx has none of
 * that (no GTK, no GLib main loop, radio_set_tx()/radio_tune_to() instead),
 * so only this self-contained piece came across. It doesn't touch HPSDR
 * state, GTK, or any sBitx-specific control API — just ALSA and configfs —
 * so it drops in as its own module.
 *
 * Provides a USB Audio Class 2.0 gadget device named "sBitx" that streams
 * 24-bit / 48 kHz stereo IQ (I = left, Q = right) to any SDR application
 * that can consume a USB audio input (e.g. SDR#, HDSDR, GQRX, SDR Console).
 *
 * Architecture overview:
 *   The Linux USB gadget framework is configured via the configfs API under
 *   /sys/kernel/config/usb_gadget/. A UAC2 function is bound with:
 *     - bcdADC = 0x0200 (Audio Class 2.0)
 *     - one AudioStreaming interface: 2-ch, 24-bit PCM, 48000 Hz
 *     - device/product strings: "sBitx"
 *   Once the gadget is bound to a UDC controller (detected automatically),
 *   the host sees a standard USB audio capture device called "sBitx".
 *
 *   Sample delivery:
 *     minibitx's audio thread already produces baseband IQ at 48 kHz (see
 *     sound_process() in sound.c) — no decimation needed here, unlike
 *     sbitx's 96 kHz native rate. uac_push_iq() is called once per sample
 *     directly from sound_process(), the same place that hands the same
 *     block to hpsdr_send_iq() (hpsdr_p1.c) as a separate copy. This runs
 *     independently of whether an HPSDR client is connected — USB audio
 *     and the HPSDR UDP stream are two separate consumers of the same IQ,
 *     and neither module has a dependency on the other.
 *
 *   ALSA loopback bridge:
 *     Linux's snd-aloop module creates a pair of back-to-back PCM devices.
 *     The gadget's UAC2 function reads from one side; we write to the other
 *     via a standard PCM write call. This avoids any kernel-module custom
 *     code and works on any Linux distro with snd-aloop loaded.
 *
 * Configfs gadget path layout (created by uac_gadget_create() in
 * usb_gadget.c):
 *   /sys/kernel/config/usb_gadget/sbitx_iq/
 *     idVendor, idProduct, bcdUSB, bcdDevice
 *     strings/0x409/manufacturer  = "sBitx"
 *     strings/0x409/product       = "sBitx IQ"
 *     strings/0x409/serialnumber  = "0000001"
 *     configs/c.1/
 *       strings/0x409/configuration = "Default"
 *       bmAttributes, MaxPower
 *       function symlink -> functions/uac2.0/
 *     functions/uac2.0/
 *       c_srate  = 48000
 *       c_ssize  = 3        (3 bytes = 24-bit)
 *       c_chmask = 3        (2 channels: L=I, R=Q)
 *       p_srate  = 48000    (playback side, unused but must be set)
 *       p_ssize  = 3
 *       p_chmask = 3
 *
 * Dependencies (must be present on the target system):
 *   Kernel modules : dwc2 (or other device-mode UDC), libcomposite, snd-aloop
 *   Userspace libs : libasound2-dev (ALSA — for PCM write to loopback;
 *                    minibitx already links -lasound for sound.c)
 *   Kernel config  : CONFIG_USB_CONFIGFS_F_UAC2=y
 *
 * Thread safety:
 *   uac_init()/uac_stop() are meant to be called once, from main(), around
 *   hpsdr_init()/hpsdr_poll(). uac_push_iq() runs on minibitx's audio
 *   thread (same thread that calls hpsdr_send_iq()). No locking is needed
 *   — the ALSA PCM handle is only ever written by that one thread.
 */

#ifndef USB_GADGET_H
#define USB_GADGET_H

/* Configure the UAC2 gadget via configfs and open the ALSA loopback PCM.
 * Call once, after hpsdr_init()/hpsdr_poll(). Returns 0 if both the gadget
 * and the PCM are ready, -1 if either step fails (e.g. no UDC, snd-aloop
 * not loaded) — not a hard failure for the rest of minibitx, which keeps
 * running over HPSDR/UDP either way. */
int uac_init(void);

/* Deliver one 48 kHz IQ sample pair, normalized to [-1.0, +1.0]. Buffers
 * internally and flushes to the ALSA loopback one period at a time. No-op
 * if uac_init() hasn't succeeded (or after uac_stop()). */
void uac_push_iq(double i_val, double q_val);

/* Tear down the gadget and release the ALSA PCM handle. Safe to call even
 * if uac_init() was never called or failed. */
void uac_stop(void);

/* Returns 1 while the UAC2 stream is initialized and ready to accept
 * samples, 0 otherwise. */
int uac_is_active(void);

#endif /* USB_GADGET_H */
