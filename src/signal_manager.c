#include "signal_manager.h"

#include <stddef.h>
#include <string.h>

volatile sig_atomic_t g_sigchld      = 0;
volatile sig_atomic_t g_sigusr1      = 0;
volatile sig_atomic_t g_sigalrm      = 0;
volatile sig_atomic_t g_shutdown_sig = 0;

static const int handled_signals[] = {
    SIGCHLD, SIGUSR1, SIGALRM, SIGINT, SIGTERM
};
#define N_HANDLED (sizeof handled_signals / sizeof handled_signals[0])

static sigset_t original_mask; /* mask before sm_init()           */
static sigset_t wait_mask;     /* mask used while in sigsuspend() */

/*
 * The only handler. It touches nothing but sig_atomic_t flags, so it is
 * async-signal-safe. errno is not modified.
 */
static void on_signal(int signo)
{
    switch (signo) {
    case SIGCHLD: g_sigchld = 1;          break;
    case SIGUSR1: g_sigusr1 = 1;          break;
    case SIGALRM: g_sigalrm = 1;          break;
    case SIGINT:
    case SIGTERM: g_shutdown_sig = signo; break;
    default:                              break;
    }
}

int sm_init(void)
{
    sigset_t block;
    struct sigaction sa;
    size_t i;

    /* 1) Block first, so no signal can arrive half-way through setup. */
    sigemptyset(&block);
    for (i = 0; i < N_HANDLED; i++)
        sigaddset(&block, handled_signals[i]);
    if (sigprocmask(SIG_BLOCK, &block, &original_mask) < 0)
        return -1;

    /* 2) Mask used while waiting: the original mask minus our signals. */
    wait_mask = original_mask;
    for (i = 0; i < N_HANDLED; i++)
        sigdelset(&wait_mask, handled_signals[i]);

    /* 3) Install the handler. Every handled signal is blocked while the
     *    handler runs; SA_NOCLDSTOP: no SIGCHLD for stopped children.     */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sa.sa_mask = block;
    for (i = 0; i < N_HANDLED; i++) {
        int flags = SA_RESTART;
        if (handled_signals[i] == SIGCHLD)
            flags |= SA_NOCLDSTOP;
        sa.sa_flags = flags;
        if (sigaction(handled_signals[i], &sa, NULL) < 0)
            return -1;
    }
    return 0;
}

void sm_wait(void)
{
    /* Returns -1/EINTR after a handler ran; that is the expected result. */
    (void)sigsuspend(&wait_mask);
}

void sm_child_reset(void)
{
    struct sigaction sa;
    size_t i;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < N_HANDLED; i++)
        (void)sigaction(handled_signals[i], &sa, NULL);
    (void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
}
