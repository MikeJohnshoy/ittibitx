/*
 * hamlib.h - minimal rigctld-compatible TCP control server.
 *
 * Implements a tiny subset of Hamlib's plain-text rigctld wire protocol 
 * for SDR apps that expect CAT-style live frequency control of an external 
 * radio over a separate connection from the HPSDR IQ and USB audio links.
 *
 * Implements: f/F (get/set frequency), t/T (get/set PTT), m/M (get/set
 * mode (cosmetic only, minibitx has no onboard demod), dump_state and chk_vfo
 * and q/Q to close a connection cleanly. Unknown commands
 * get a clean RPRT error reply rather than being silently ignored, so
 * a client never hangs waiting on us.
 */

#ifndef HAMLIB_H
#define HAMLIB_H

// Starts the rigctld-compatible TCP server on the given port (4532 is
// the standard rigctld port) in a background thread. Returns 0 on
// success, -1 on failure (bind/listen error) - non-fatal, same pattern
// as hpsdr_init()/uac_init(): minibitx keeps running either way.
int hamlib_init(int port);

// Stops the server and closes the listening socket.
void hamlib_stop(void);

#endif /* HAMLIB_H */
