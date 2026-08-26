# 04 — remote control and I/Q output

This covers everything minibitx exposes to the outside world: how an
external control surface tunes and keys the radio, and how the baseband
I/Q produced in
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) gets to
the SDR application actually using it. The two are documented together
because one file — `hpsdr_p1.c` — does both jobs.

## The single-entry-point pattern

`radio_tune_to()` and `radio_set_tx()` (`radio.c`) are the *only*
functions that touch hardware to change frequency or key the
transmitter. Every control surface calls into these two functions rather
than poking GPIO or the si5351 directly — so adding a new control
surface never means a second place that can put the hardware in an
inconsistent state. Today there are two callers:

- **Hamlib/rigctld** (`hamlib.c`) — the sole live frequency control
  surface (`F`).
- **HPSDR's inbound command parser** (`hpsdr_p1.c`) — calls
  `radio_set_tx()` only, for MOX/PTT (below).

## Hamlib / rigctld server

`hamlib.c` runs a minimal rigctld-compatible TCP server (`hamlib_init()`,
default port 4532, one thread accepting connections and one per client).
It implements a small plain-text rigctl command set:

| Command | Behavior |
|---|---|
| `f` / `F <hz>` | get / set frequency — `F` calls `radio_tune_to()` |
| `t` / `T <0\|1>` | get / set PTT — `T` calls `radio_set_tx()`; any nonzero value means TX (no separate mic/data state) |
| `m` / `M <mode> <passband>` | get / set mode — **cosmetic only**, stored but never acted on, since minibitx has no onboard demod |
| `chk_vfo` | always reports "not in VFO mode" (single-VFO radio) |
| `dump_state` | minimal capability dump for client negotiation — deliberately advertises no RIT/XIT/IF-shift/preamp/attenuator/onboard-filter support, and an empty TX range (no TX audio path yet) |
| `q` / `Q` / `quit` | disconnect |

It's a small command set on purpose: minibitx isn't the thing making
demod/filtering decisions, the SDR app is. Point an SDR app's CAT/rig
control at `127.0.0.1:4532` (rig model "Hamlib NET rigctl") alongside its
HPSDR connection for live retuning.

## HPSDR Protocol 1 — inbound (control) and outbound (I/Q)

`hpsdr_p1.c` implements a minimal openHPSDR Protocol 1 link over UDP,
and handles both directions of that link:

**Inbound (EP2):** frequency and mode control live entirely in the
rigctld server above — the *only* thing `hpsdr_p1.c` reads from the
inbound stream is the MOX (PTT) bit, since some SDR apps key PTT through
the I/Q link's C0 byte even while using CAT for everything else. When it
sees that bit change, it calls `radio_set_tx()` — the same entry point
Hamlib uses, never a separate path.

**Outbound (I/Q):** `hpsdr_send_iq()` packetizes the baseband I/Q handed
to it by `sound_process()` into HPSDR Protocol 1 UDP frames and sends
them inline, without flow control or mutexes. I and Q values are scaled
up before sending to make SDR apps happier. This file has no dependency
on `usb_gadget.c` — each holds its own independent copy of the I/Q.

## USB Audio Class (UAC2) output

`usb_gadget.c` presents the radio as a standard USB Audio Class 2.0
capture device, if the hardware/kernel support it (needs a USB
device-mode controller and `snd-aloop`). It's fed its own I/Q copy
directly from `sound.c`, with no ALSA/gadget dependency on
`hpsdr_p1.c`. Ported near-verbatim from the UAC2 section of sbitx's
`hpsdr_p1.c`, since that code had no sBitx/GTK dependency of its own —
only ALSA and Linux configfs/sysfs — making the port mechanical.

Either stream works without the other: a client connected over USB audio
alone, with no HPSDR app connected, still gets I/Q, and vice versa.
Neither `uac_init()` nor `hamlib_init()` failing is treated as fatal at
startup — minibitx keeps running on whatever subset of control/streaming
surfaces came up successfully; see
[`05_process_and_threading_model.md`](05_process_and_threading_model.md).
