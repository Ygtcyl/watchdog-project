#ifndef SIGNAL_MANAGER_H
#define SIGNAL_MANAGER_H

#include <signal.h>

/*
 * signal_manager - installs the watchdog's signal handlers.
 *
 * Handlers are async-signal-safe: they only store into the volatile
 * sig_atomic_t flags below. All real work (reaping, logging, restarting,
 * shutting down) happens in the main control flow.
 */

extern volatile sig_atomic_t g_sigchld;      /* a child changed state      */
extern volatile sig_atomic_t g_sigusr1;      /* status report requested    */
extern volatile sig_atomic_t g_sigalrm;      /* periodic timer tick        */
extern volatile sig_atomic_t g_shutdown_sig; /* SIGINT/SIGTERM number or 0 */

/*
 * Blocks SIGCHLD, SIGUSR1, SIGALRM, SIGINT and SIGTERM, then installs the
 * handlers with sigaction(). While blocked, these signals can only be
 * delivered inside sm_wait(), which removes the race between checking a
 * flag and going to sleep. Returns 0 on success, -1 on error.
 */
int  sm_init(void);

/* Atomically unblocks the handled signals and sleeps until one arrives. */
void sm_wait(void);

/*
 * Called in a freshly forked child before exec(): restores default
 * dispositions and the original signal mask, so the worker does not
 * inherit the watchdog's blocked signals.
 */
void sm_child_reset(void);

#endif /* SIGNAL_MANAGER_H */
