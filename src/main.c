/*
 * watchdog - POSIX signal-based process monitoring and management system.
 *
 * Usage: watchdog [-l logfile] [-w worker_path] [-t shutdown_timeout]
 *                 [name:lifetime:exit_code[:ignore] ...]
 */
#include "logger.h"
#include "process_manager.h"
#include "signal_manager.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_LOG_FILE         "watchdog.log"
#define DEFAULT_SHUTDOWN_TIMEOUT 5   /* seconds before SIGKILL */
#define CHECK_INTERVAL           1   /* periodic check period, seconds */

/* Used when no worker is given on the command line. */
static const char *default_specs[] = {
    "alpha:0:0",   /* runs until stopped                    */
    "beta:0:0",    /* runs until stopped                    */
    "gamma:10:0",  /* finishes normally after 10 seconds    */
};

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-l logfile] [-w worker_path] [-t shutdown_timeout] "
        "[spec ...]\n"
        "  spec = name:lifetime:exit_code[:ignore]\n"
        "    lifetime   seconds before the worker exits (0 = run forever)\n"
        "    exit_code  exit code when the lifetime ends (0 = normal)\n"
        "    ignore     worker ignores SIGTERM (tests the SIGKILL timeout)\n"
        "  defaults: -l %s -t %d, workers alpha:0:0 beta:0:0 gamma:10:0\n",
        prog, DEFAULT_LOG_FILE, DEFAULT_SHUTDOWN_TIMEOUT);
}

/* "./watchdog" -> "./worker", "/x/y/watchdog" -> "/x/y/worker". */
static void default_worker_path(const char *argv0, char *out, size_t len)
{
    const char *slash = strrchr(argv0, '/');

    if (slash == NULL)
        snprintf(out, len, "./worker");
    else
        snprintf(out, len, "%.*s/worker", (int)(slash - argv0), argv0);
}

/* Monotonic clock in milliseconds (not affected by wall-clock changes). */
static long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

/* Graceful shutdown: no new launches, SIGTERM, wait, SIGKILL on timeout. */
static void graceful_shutdown(int signo, int timeout)
{
    long long deadline;
    int killed = 0;

    log_event("SHUTDOWN", "received %s: stopping new launches, sending SIGTERM "
              "to %zu running worker(s), timeout %ds",
              signo == SIGINT ? "SIGINT" : "SIGTERM", pm_alive_count(), timeout);
    pm_begin_shutdown();
    deadline = monotonic_ms() + (long long)timeout * 1000LL;
    g_shutdown_sig = 0;
    /* Re-arm the timer so the first tick lands exactly on the deadline,
     * then keep ticking every second as a reaping safety net.          */
    g_sigalrm = 0;
    if (pm_start_timer((unsigned)timeout, CHECK_INTERVAL) < 0)
        log_event("ERROR", "setitimer() failed during shutdown");

    while (pm_alive_count() > 0) {
        if (g_sigchld) {
            g_sigchld = 0;
            pm_reap_children();
            continue;
        }
        if (g_sigusr1) {
            g_sigusr1 = 0;
            logger_report_status();
        }
        if (g_shutdown_sig) {
            g_shutdown_sig = 0;
            log_event("SHUTDOWN", "already shutting down; signal ignored");
        }
        if (g_sigalrm) {
            g_sigalrm = 0;
            pm_reap_children();
            if (!killed && pm_alive_count() > 0 && monotonic_ms() >= deadline) {
                pm_kill_remaining();
                killed = 1;
            }
            continue;
        }
        sm_wait();
    }
    log_event("SHUTDOWN", "all workers terminated and reaped");
}

int main(int argc, char *argv[])
{
    const char *log_path = DEFAULT_LOG_FILE;
    char worker_path[PATH_MAX];
    worker_spec_t specs[PM_MAX_WORKERS];
    size_t n_specs = 0;
    int timeout = DEFAULT_SHUTDOWN_TIMEOUT;
    int opt, i;

    default_worker_path(argv[0], worker_path, sizeof worker_path);

    while ((opt = getopt(argc, argv, "l:w:t:h")) != -1) {
        switch (opt) {
        case 'l':
            log_path = optarg;
            break;
        case 'w':
            snprintf(worker_path, sizeof worker_path, "%s", optarg);
            break;
        case 't': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || v < 1 || v > 3600) {
                fprintf(stderr, "watchdog: invalid timeout '%s'\n", optarg);
                return EXIT_FAILURE;
            }
            timeout = (int)v;
            break;
        }
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* Worker specifications (command line or defaults). */
    if (optind < argc) {
        for (i = optind; i < argc; i++) {
            if (n_specs >= PM_MAX_WORKERS) {
                fprintf(stderr, "watchdog: at most %d workers\n", PM_MAX_WORKERS);
                return EXIT_FAILURE;
            }
            if (pm_parse_spec(argv[i], &specs[n_specs]) < 0) {
                fprintf(stderr, "watchdog: invalid worker spec '%s'\n", argv[i]);
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            n_specs++;
        }
    } else {
        for (i = 0; i < (int)(sizeof default_specs / sizeof *default_specs); i++)
            pm_parse_spec(default_specs[i], &specs[n_specs++]);
    }
    if (n_specs < 2) {
        fprintf(stderr, "watchdog: at least two workers are required\n");
        return EXIT_FAILURE;
    }

    if (logger_init(log_path) < 0)
        return EXIT_FAILURE;

    /* Signals are blocked from here on; they are delivered only in sm_wait(). */
    if (sm_init() < 0) {
        perror("watchdog: signal setup");
        logger_close();
        return EXIT_FAILURE;
    }
    if (pm_init(specs, n_specs, worker_path) < 0) {
        fprintf(stderr, "watchdog: invalid worker configuration\n");
        logger_close();
        return EXIT_FAILURE;
    }

    log_event("START", "watchdog started: pid=%ld, %zu worker(s), worker=%s, "
              "log=%s, restart limit=%d, shutdown timeout=%ds",
              (long)getpid(), n_specs, worker_path, log_path,
              PM_MAX_RESTARTS, timeout);
    if (access(worker_path, X_OK) < 0)
        log_event("WARN", "worker executable '%s' is not executable; "
                  "starts will fail and be limited by the restart limit",
                  worker_path);

    pm_start_all();
    if (pm_start_timer(CHECK_INTERVAL, CHECK_INTERVAL) < 0)
        log_event("ERROR", "setitimer() failed; periodic check disabled");

    /* Main control loop: handle flags set by the signal handlers. */
    for (;;) {
        if (g_shutdown_sig) {
            graceful_shutdown((int)g_shutdown_sig, timeout);
            break;
        }
        if (g_sigchld) {
            g_sigchld = 0;
            pm_reap_children();
        }
        if (g_sigalrm) {
            g_sigalrm = 0;
            pm_periodic_check();
        }
        if (g_sigusr1) {
            g_sigusr1 = 0;
            logger_report_status();
        }
        if (pm_active_count() == 0) {
            log_event("SHUTDOWN", "no running or restartable workers left");
            break;
        }
        sm_wait();
    }

    pm_start_timer(0, 0);   /* disarm the timer */
    log_event("EXIT", "watchdog exiting");
    logger_close();
    return EXIT_SUCCESS;
}
