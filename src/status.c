// status.c — see status.h

#include "status.h"
#include "radio.h"
#include <stdio.h>
#include <unistd.h>

void status_print(void) {
    static int is_tty = -1;
    if (is_tty < 0)
        is_tty = isatty(fileno(stdout));

    const char *state = in_tx ? "TX" : "RX";

    if (is_tty) {
        // \r returns to column 0, \033[K clears the rest of the previous
        // line (in case the new content is shorter) — one line, redrawn
        // in place, never a new line.
        printf("\rFreq: %9d Hz   State: %-2s   Drive: n/a   \033[K",
               freq_hdr, state);
        fflush(stdout);
    } else {
        // Not a terminal (redirected to a file, captured by systemd) —
        // no cursor to redraw with, so print one line per call instead.
        printf("status: freq=%d state=%s drive=n/a\n", freq_hdr, state);
    }
}
