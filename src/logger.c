#include "logger.h"
#include "process_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int log_fd = -1;

/* write() until everything is written; retries on EINTR / partial writes. */
static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

int logger_init(const char *path)
{
    log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0) {
        fprintf(stderr, "watchdog: cannot open log file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    return 0;
}

void logger_close(void)
{
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
}

void log_event(const char *tag, const char *fmt, ...)
{
    char msg[512];
    char line[640];
    char stamp[32];
    struct tm tm_now;
    time_t now = time(NULL);
    va_list ap;
    int len;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (localtime_r(&now, &tm_now) == NULL ||
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm_now) == 0)
        snprintf(stamp, sizeof stamp, "%ld", (long)now);

    len = snprintf(line, sizeof line, "%s [watchdog %ld] %-8s %s\n",
                   stamp, (long)getpid(), tag, msg);
    if (len < 0)
        return;
    if ((size_t)len >= sizeof line)
        len = (int)sizeof line - 1;   /* message was truncated */

    if (log_fd >= 0)
        write_all(log_fd, line, (size_t)len);
    write_all(STDOUT_FILENO, line, (size_t)len);
}

void logger_report_status(void)
{
    size_t count = 0;
    const worker_t *w = pm_workers(&count);
    time_t now = time(NULL);
    size_t i;

    log_event("REPORT", "status report requested (SIGUSR1): %zu worker(s), "
              "%zu alive", count, pm_alive_count());
    for (i = 0; i < count; i++) {
        char pid_buf[16] = "-";
        char up_buf[24] = "-";

        if (w[i].pid > 0)
            snprintf(pid_buf, sizeof pid_buf, "%ld", (long)w[i].pid);
        if (w[i].state == WS_RUNNING)
            snprintf(up_buf, sizeof up_buf, "%lds",
                     (long)(now - w[i].started_at));

        log_event("REPORT", "  name=%-10s pid=%-7s state=%-15s "
                  "restarts=%d/%d uptime=%s",
                  w[i].spec.name, pid_buf, pm_state_name(w[i].state),
                  w[i].restarts, PM_MAX_RESTARTS, up_buf);
    }
}
