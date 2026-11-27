# watchdog — POSIX Signal-Based Process Monitor

A supervisor written in C. It starts worker processes with `fork()` + `execv()`,
detects their termination with `SIGCHLD` + `waitpid()`, restarts the ones that
die unexpectedly (at most 3 times), writes a status report on `SIGUSR1`, and
shuts down gracefully on `SIGINT` / `SIGTERM`. Every event is written to a
timestamped log file.

## Build

```sh
make          # builds ./watchdog and ./worker
make test     # builds and runs all test scenarios (tests/test_scenarios.sh)
make clean
```

You need Linux, gcc, make and bash. The code compiles with
`-std=c11 -Wall -Wextra -Wpedantic -Werror`.

## Usage

```sh
./watchdog [-l logfile] [-w worker_path] [-t shutdown_timeout] [spec ...]
```

| Option | Default | Meaning |
|--------|---------|---------|
| `-l` | `watchdog.log` | log file (opened in append mode) |
| `-w` | `worker` next to `watchdog` | worker executable |
| `-t` | `5` | seconds to wait after SIGTERM before sending SIGKILL |

A `spec` has the form `name:lifetime:exit_code[:ignore]`:

- `lifetime`: the worker exits after this many seconds. `0` means it runs until it receives a signal.
- `exit_code`: the code the worker exits with. `0` is a normal exit; any other value simulates an error.
- `ignore`: the worker ignores SIGTERM. Use it to test the SIGKILL timeout.

Without specs, the watchdog starts three workers:
- `alpha:0:0` and `beta:0:0` run until they are stopped.
- `gamma:10:0` finishes normally after 10 seconds.

## Signals

From another terminal:

```sh
kill -SIGUSR1 <WATCHDOG_PID>   # write PID + state of every child to the log
kill -SIGTERM <WATCHDOG_PID>   # graceful shutdown (Ctrl+C does the same)
kill -SIGKILL <WORKER_PID>     # simulate a crash -> worker is restarted
```

## Behaviour

| Child terminates with | Classified as | Action |
|-----------------------|---------------|--------|
| exit code 0 | normal | not restarted (`EXITED`) |
| exit code != 0 | unexpected | restarted after 1 s, max 3 times, then `FAILED` |
| a signal | unexpected | restarted after 1 s, max 3 times, then `FAILED` |
| anything during shutdown | — | reaped only (`STOPPED`) |

The shutdown sequence works like this:
1. The watchdog stops all new launches.
2. It sends SIGTERM to every child.
3. It waits for the children to exit and reaps each one.
4. If a child is still alive after the timeout, the watchdog sends it SIGKILL and reaps it.

When no worker is running and none is waiting for a restart, the watchdog exits on its own.

## Design

```
include/ src/
  signal_manager   sigaction() handlers that only set volatile sig_atomic_t
                   flags; signals stay blocked and are delivered only inside
                   sigsuspend(), so there is no check-then-sleep race
  process_manager  fork/exec, waitpid(WNOHANG) loop, exit classification,
                   bounded restarts, setitimer()/SIGALRM periodic check,
                   SIGTERM/SIGKILL shutdown
  logger           timestamped write() logging, SIGUSR1 status report
  main             argument parsing, main control loop, graceful shutdown
workers/worker.c   test child program
tests/             automated scenarios (logs are written to tests/logs/)
docs/              sample log output
```

- No work is done inside signal handlers. Logging, `fork`, `waitpid` and `malloc` all run in the main loop.
- Before `exec`, each child resets its signal dispositions and its signal mask. It then moves into its own process group, so a terminal Ctrl+C reaches only the watchdog. The watchdog then stops the children itself.

## Tests

`make test` covers these scenarios:
- normal operation
- normal termination
- unexpected termination and restart
- restart limit
- SIGUSR1 report
- shutdown with SIGTERM
- shutdown with Ctrl+C (SIGINT sent to the process group)
- SIGKILL after the shutdown timeout
- bounded restarts when exec fails
- log verification (timestamps and all event types)

You can see example output in `docs/sample_watchdog.log`.
