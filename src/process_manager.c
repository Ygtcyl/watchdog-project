#include "process_manager.h"
#include "signal_manager.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <errno.h>

static worker_t workers[PM_MAX_WORKERS];
static size_t worker_count = 0;
static char executable_path[4096];
static int shutting_down = 0;
int pm_init(const worker_spec_t *specs, size_t count,
            const char *worker_path)
{
    if (specs == NULL || worker_path == NULL ||
        count < 2 || count > PM_MAX_WORKERS)
        return -1;

    if (strlen(worker_path) >= sizeof executable_path)
        return -1;

    worker_count = count;
    shutting_down = 0;

    strcpy(executable_path, worker_path);

    for (size_t i = 0; i < count; i++) {
        workers[i].spec = specs[i];
        workers[i].pid = -1;
        workers[i].state = RESTART_PENDING;
        workers[i].restart_count = 0;
    }

    return 0;
}

static int spawn_worker(worker_t *w)
{
    char lifetime[16];
    char exit_code[16];

    snprintf(lifetime, sizeof lifetime, "%d", w->spec.lifetime);
    snprintf(exit_code, sizeof exit_code, "%d", w->spec.exit_code);

    char *args[] = {
        executable_path,
        w->spec.name,
        lifetime,
        exit_code,
        w->spec.ignore_term ? "--ignore-term" : NULL,
        NULL
    };

    pid_t pid = fork();

    if (pid < 0) {
        log_event("ERROR", "fork failed for worker %s",
                  w->spec.name);
        return -1;
    }

    if (pid == 0) {
        if (setpgid(0, 0) < 0)
            _exit(126);

        sm_child_reset();

        execv(executable_path, args);

        _exit(127);
    }

    w->pid = pid;
    w->state = RUNNING;

    log_event("SPAWN",
              "worker %s started: pid=%ld (restarts so far %d/%d)",
              w->spec.name, (long)pid,
              w->restart_count, PM_MAX_RESTARTS);

    return 0;
}

void pm_start_all(void)
{
    if (shutting_down)
        return;

    for (size_t i = 0; i < worker_count; i++) {
        if (spawn_worker(&workers[i]) < 0)
            workers[i].state = RESTART_PENDING;
    }
}

int pm_parse_spec(const char *arg, worker_spec_t *spec)
{
    char buffer[256];
    char *name, *lifetime, *exit_code, *ignore;
    char *end;
    long value;

    if (!arg || !spec || strlen(arg) >= sizeof buffer)
        return -1;

    if (arg[0] == '\0' || arg[0] == ':' ||
        arg[strlen(arg) - 1] == ':' ||
        strstr(arg, "::") != NULL)
        return -1;

    strcpy(buffer, arg);

    name = strtok(buffer, ":");
    lifetime = strtok(NULL, ":");
    exit_code = strtok(NULL, ":");
    ignore = strtok(NULL, ":");

    if (!name || !lifetime || !exit_code)
        return -1;

    if (strlen(name) >= PM_NAME_LEN)
        return -1;

    if (ignore && strcmp(ignore, "ignore") != 0)
        return -1;

    if (strtok(NULL, ":") != NULL)
        return -1;

    memset(spec, 0, sizeof *spec);
    strcpy(spec->name, name);

    value = strtol(lifetime, &end, 10);
    if (*end != '\0' || value < 0 || value > 1000000)
        return -1;

    spec->lifetime = (int)value;

    value = strtol(exit_code, &end, 10);
    if (*end != '\0' || value < 0 || value > 255)
        return -1;

    spec->exit_code = (int)value;
    spec->ignore_term = (ignore != NULL);

    return 0;
}

void pm_reap_children(void)
{
    int status;
    pid_t pid;

    for (;;) {
        pid = waitpid(-1, &status, WNOHANG);

        if (pid < 0 && errno == EINTR)
            continue;

        if (pid <= 0)
            break;

        for (size_t i = 0; i < worker_count; i++) {
            worker_t *w = &workers[i];

            if (w->pid != pid)
                continue;

            w->pid = -1;

            char reason[80];

            if (WIFEXITED(status)) {
                snprintf(reason, sizeof reason,
                         "exited with code %d",
                         WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                snprintf(reason, sizeof reason,
                         "killed by signal %d",
                         WTERMSIG(status));
            } else {
                snprintf(reason, sizeof reason,
                         "unknown exit status");
            }

            if (shutting_down) {
                w->state = STOPPED;

                log_event("STOPPED",
                          "worker %s (pid=%ld) %s",
                          w->spec.name, (long)pid, reason);

            } else if (WIFEXITED(status) &&
                       WEXITSTATUS(status) == 0) {

                w->state = EXITED;

                log_event("EXIT",
                          "worker %s (pid=%ld) finished normally (%s)",
                          w->spec.name, (long)pid, reason);

            } else {
                log_event("CRASH",
                          "worker %s (pid=%ld) terminated unexpectedly: %s",
                          w->spec.name, (long)pid, reason);

                if (w->restart_count < PM_MAX_RESTARTS) {
                    w->state = RESTART_PENDING;

                    log_event("SCHEDULE",
                              "worker %s scheduled for restart",
                              w->spec.name);
                } else {
                    w->state = FAILED;

                    log_event("LIMIT",
                              "worker %s reached the restart limit (%d)",
                              w->spec.name, PM_MAX_RESTARTS);
                }
            }

            break;
        }
    }
}

void pm_periodic_check(void)
{
    pm_reap_children();

    if (shutting_down)
        return;

    for (size_t i = 0; i < worker_count; i++) {
        worker_t *w = &workers[i];

        if (w->state != RESTART_PENDING)
            continue;

        if (w->restart_count >= PM_MAX_RESTARTS) {
            w->state = FAILED;

            log_event("LIMIT",
                      "worker %s reached the restart limit (%d)",
                      w->spec.name, PM_MAX_RESTARTS);

            continue;
        }

        w->restart_count++;

        log_event("RESTART",
                  "restarting worker %s (restart %d/%d)",
                  w->spec.name,
                  w->restart_count,
                  PM_MAX_RESTARTS);

        if (spawn_worker(w) < 0)
            w->state = RESTART_PENDING;
    }
}

int pm_start_timer(unsigned first, unsigned interval)
{
    struct itimerval timer = {0};

    timer.it_value.tv_sec = first;
    timer.it_interval.tv_sec = interval;

    return setitimer(ITIMER_REAL, &timer, NULL);
}

size_t pm_alive_count(void)
{
    size_t count = 0;

    for (size_t i = 0; i < worker_count; i++) {
        if (workers[i].pid > 0)
            count++;
    }

    return count;
}

size_t pm_active_count(void)
{
    size_t count = 0;

    for (size_t i = 0; i < worker_count; i++) {
        if (workers[i].state == RUNNING ||
            workers[i].state == RESTART_PENDING)
            count++;
    }

    return count;
}

void pm_begin_shutdown(void)
{
    shutting_down = 1;

    for (size_t i = 0; i < worker_count; i++) {
        worker_t *w = &workers[i];

        if (w->state == RESTART_PENDING)
            w->state = STOPPED;

        if (w->pid > 0) {
            if (kill(w->pid, SIGTERM) == 0) {
                log_event("STOP",
                          "sent SIGTERM to worker %s (pid=%ld)",
                          w->spec.name, (long)w->pid);
            } else if (errno != ESRCH) {
                log_event("ERROR",
                          "SIGTERM failed for worker %s",
                          w->spec.name);
            }
        }
    }
}

void pm_kill_remaining(void)
{
    for (size_t i = 0; i < worker_count; i++) {
        worker_t *w = &workers[i];

        if (w->pid > 0) {
            if (kill(w->pid, SIGKILL) == 0) {
                log_event("KILL",
                          "worker %s (pid=%ld): sent SIGKILL",
                          w->spec.name, (long)w->pid);
            } else if (errno != ESRCH) {
                log_event("ERROR",
                          "SIGKILL failed for worker %s",
                          w->spec.name);
            }
        }
    }
}

const worker_t *pm_get_workers(size_t *count)
{
    if (count != NULL)
        *count = worker_count;

    return workers;
}
