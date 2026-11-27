#ifndef LOGGER_H
#define LOGGER_H

/*
 * logger - timestamped event logging and SIGUSR1 status reporting.
 *
 * All functions in this module are called ONLY from the main control flow,
 * never from a signal handler (they use snprintf/localtime_r, which are not
 * async-signal-safe).
 */

/* Opens (creates/appends) the log file. Returns 0 on success, -1 on error. */
int  logger_init(const char *path);

/* Closes the log file descriptor. Safe to call more than once. */
void logger_close(void);

/*
 * Writes one line "YYYY-MM-DD HH:MM:SS [watchdog <pid>] <TAG> <message>"
 * to the log file and echoes it to standard output.
 */
void log_event(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Writes the PID and current state of every supervised child to the log. */
void logger_report_status(void);

#endif /* LOGGER_H */
