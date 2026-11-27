/*
 * worker - a small child program supervised by the watchdog.
 *
 * Usage: worker <name> <lifetime_sec> <exit_code> [--ignore-term]
 *
 *   lifetime_sec  0 = run until a signal arrives; otherwise exit after
 *                 this many seconds with <exit_code>.
 *   exit_code     0 = normal completion, non-zero = simulated error.
 *   --ignore-term ignore SIGTERM (used to test the SIGKILL timeout).
 *
 * On SIGTERM the worker cleans up, then re-raises SIGTERM with the default
 * action so the parent sees "terminated by signal SIGTERM".
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t got_term = 0;

static void on_term(int signo)
{
    (void)signo;
    got_term = 1;
}

static int parse_int(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < 0 || v > 1000000)
        return -1;
    *out = (int)v;
    return 0;
}

int main(int argc, char *argv[])
{
    struct sigaction sa;
    const char *name;
    int lifetime, exit_code, elapsed = 0;
    int ignore_term = 0;

    if (argc < 4 || argc > 5 ||
        parse_int(argv[2], &lifetime) < 0 ||
        parse_int(argv[3], &exit_code) < 0 || exit_code > 255) {
        fprintf(stderr,
                "usage: %s <name> <lifetime_sec> <exit_code> [--ignore-term]\n",
                argv[0]);
        return 2;
    }
    if (argc == 5) {
        if (strcmp(argv[4], "--ignore-term") != 0) {
            fprintf(stderr, "worker: unknown option '%s'\n", argv[4]);
            return 2;
        }
        ignore_term = 1;
    }
    name = argv[1];

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = ignore_term ? SIG_IGN : on_term;
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("worker: sigaction");
        return 2;
    }

    printf("    [worker %s pid=%ld] started (lifetime=%ds, exit_code=%d%s)\n",
           name, (long)getpid(), lifetime, exit_code,
           ignore_term ? ", ignores SIGTERM" : "");
    fflush(stdout);

    while (!got_term && (lifetime == 0 || elapsed < lifetime)) {
        sleep(1);          /* interrupted early by SIGTERM */
        elapsed++;
    }

    if (got_term) {
        printf("    [worker %s pid=%ld] SIGTERM received, cleaning up\n",
               name, (long)getpid());
        fflush(stdout);
        sa.sa_handler = SIG_DFL;
        sigaction(SIGTERM, &sa, NULL);
        raise(SIGTERM);    /* die "by signal", as the parent expects */
        return 1;          /* not reached */
    }

    printf("    [worker %s pid=%ld] work finished, exiting with code %d\n",
           name, (long)getpid(), exit_code);
    fflush(stdout);
    return exit_code;
}
