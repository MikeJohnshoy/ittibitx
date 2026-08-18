/*
 * status.h — low-overhead console status line.
 *
 * I don't really expect this to be in the final version of minibitx,
 * but this does help development. No GUI — just a single line on stdout showing
 * frequency, TX/RX state, and (once it exists) TX drive level. Meant to be
 * called from an already-idle loop (main()'s `while(1) sleep(1);`) at a
 * low, fixed rate — around 1 Hz is plenty; this is not meant to be called
 * from the audio or network hot paths.
 *
 * On a real terminal, the line redraws in place (carriage return + clear
 * to end of line) instead of scrolling. When stdout isn't a TTY (e.g.
 * redirected to a file or captured by systemd/journald), it falls back to
 * one plain line per call, since there's no terminal to redraw on and a
 * fixed-rate log line is more useful there than an unreadable run of \r.
 */

#ifndef STATUS_H
#define STATUS_H

/* Print/update the status line: current frequency, TX/RX state, and TX
 * drive level (shown as "n/a" until a real TX drive value exists). Cheap
 * enough to call once a second from main()'s idle loop. */
void status_print(void);

#endif /* STATUS_H */
