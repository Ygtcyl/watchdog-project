#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <stddef.h>
#include <sys/types.h>

#define PM_MAX_WORKERS 32
#define PM_MAX_RESTARTS 3
#define PM_NAME_LEN 64

typedef enum {
    RUNNING,
    EXITED,
    FAILED,
    RESTART_PENDING,
    STOPPED
} worker_state_t;

typedef struct {
    char name[PM_NAME_LEN];
    int lifetime;
    int exit_code;
    int ignore_term;
} worker_spec_t;

typedef struct {
    worker_spec_t spec;
    pid_t pid;
    worker_state_t state;
    int restart_count;
} worker_t;

int pm_parse_spec(const char *arg, worker_spec_t *spec);
int pm_init(const worker_spec_t *specs, size_t count,
            const char *worker_path);

void pm_start_all(void);
void pm_reap_children(void);
void pm_periodic_check(void);

int pm_start_timer(unsigned first, unsigned interval);

size_t pm_alive_count(void);
size_t pm_active_count(void);

void pm_begin_shutdown(void);
void pm_kill_remaining(void);

const worker_t *pm_get_workers(size_t *count);
#endif

