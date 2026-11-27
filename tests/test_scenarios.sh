#!/usr/bin/env bash
#
# Automated test scenarios for the watchdog (Project 1, Section 7).
# Run from the project root with:  make test
#
# Every scenario starts its own watchdog with its own log file under
# tests/logs/ and checks the log, the process table and the exit behaviour.

set -u
cd "$(dirname "$0")/.." || exit 1

LOGDIR=tests/logs
rm -rf "$LOGDIR"
mkdir -p "$LOGDIR"

PASS=0
FAIL=0
BG=""      # PID of the background job (watchdog or setsid wrapper)
WD=""      # PID of the watchdog itself (read from its START line)
LOG=""

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }
check() { local d="$1"; shift; if "$@"; then pass "$d"; else fail "$d"; fi; }
title() { echo; echo "== $1"; }

# wait_for <regex> [timeout_sec]  -- poll the current log
wait_for() {
    local re="$1" t="${2:-5}" i=0
    while (( i < t * 10 )); do
        grep -Eq -- "$re" "$LOG" 2>/dev/null && return 0
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

count() { grep -Ec -- "$1" "$LOG" 2>/dev/null; }

# Latest PID of a worker according to the log.
worker_pid() {
    grep -E "SPAWN +worker $1 started" "$LOG" | tail -n 1 | sed -E 's/.*pid=([0-9]+).*/\1/'
}

alive()       { local s; s=$(ps -o stat= -p "$1" 2>/dev/null); [[ -n "$s" && "$s" != Z* ]]; }
gone()        { ! alive "$1"; }
parent_of()   { ps -o ppid= -p "$1" 2>/dev/null | tr -d ' '; }
pgid_of()     { ps -o pgid= -p "$1" 2>/dev/null | tr -d ' '; }
no_zombies()  { ! ps -o stat= --ppid "$WD" 2>/dev/null | grep -q '^Z'; }

# start <name> [setsid] -- <watchdog args...>
start() {
    local name="$1"; shift
    local prefix=()
    if [[ "${1:-}" == setsid ]]; then prefix=(setsid); shift; fi
    [[ "${1:-}" == -- ]] && shift
    LOG="$LOGDIR/$name.log"
    "${prefix[@]}" ./watchdog -l "$LOG" "$@" > "$LOGDIR/$name.out" 2>&1 &
    BG=$!
    if wait_for 'START +watchdog started: pid=' 5; then
        WD=$(grep -E 'START +watchdog started' "$LOG" | sed -E 's/.*pid=([0-9]+).*/\1/')
    else
        WD=$BG
    fi
}

# wait_exit [timeout_sec] -- wait until the watchdog process has exited
wait_exit() {
    local t="${1:-10}" i=0
    while (( i < t * 10 )); do
        if gone "$WD"; then wait "$BG" 2>/dev/null; return 0; fi
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

# Make sure nothing is left behind if a check failed mid-way.
cleanup() {
    if [[ -n "$WD" ]] && alive "$WD"; then
        kill -KILL "$WD" 2>/dev/null
    fi
    # Kill only worker PIDs started by these tests, never unrelated processes.
    local p
    for p in $(grep -hEo 'SPAWN +worker .* pid=[0-9]+' "$LOGDIR"/s*.log 2>/dev/null \
               | sed -E 's/.*pid=//'); do
        if alive "$p" && [[ "$(ps -o comm= -p "$p")" == worker ]]; then
            kill -KILL "$p" 2>/dev/null
        fi
    done
    wait 2>/dev/null
}
trap cleanup EXIT

[[ -x ./watchdog && -x ./worker ]] || { echo "build first: make"; exit 1; }

# ---------------------------------------------------------------------
title "1. Normal operation: at least two children are started and tracked"
start s1 -- alpha:0:0 beta:0:0
check "two SPAWN events logged"          wait_for 'SPAWN +worker beta started' 3
A=$(worker_pid alpha); B=$(worker_pid beta)
check "alpha (pid $A) is alive"          alive "$A"
check "beta (pid $B) is alive"           alive "$B"
check "watchdog is the parent of alpha"  test "$(parent_of "$A")" = "$WD"
check "watchdog is the parent of beta"   test "$(parent_of "$B")" = "$WD"
check "alpha runs in its own process group" test "$(pgid_of "$A")" = "$A"

# ---------------------------------------------------------------------
title "5. Status reporting: SIGUSR1 writes PID and state of each child"
kill -USR1 "$WD"
check "report header written"            wait_for 'REPORT +status report requested' 3
check "report contains alpha pid + state" wait_for "REPORT +name=alpha +pid=$A +state=RUNNING" 2
check "report contains beta pid + state"  wait_for "REPORT +name=beta +pid=$B +state=RUNNING" 2

# ---------------------------------------------------------------------
title "3. Unexpected termination: SIGKILL to a worker triggers a restart"
check "verified target $A is a worker child of the watchdog" \
      test "$(parent_of "$A")" = "$WD" -a "$(ps -o comm= -p "$A")" = worker
kill -KILL "$A"
check "termination by signal detected"   wait_for "CRASH +worker alpha \(pid=$A\) terminated unexpectedly: killed by signal 9" 3
check "restart performed"                wait_for 'RESTART +restarting worker alpha \(restart 1/3\)' 4
check "new alpha process spawned"        wait_for 'SPAWN +worker alpha started: pid=[0-9]+ \(restarts so far 1/3\)' 2
A2=$(worker_pid alpha)
check "new alpha pid ($A2) differs and is alive" test "$A2" != "$A" -a -n "$A2"
check "new alpha is alive"               alive "$A2"
check "old alpha was reaped (no zombies)" no_zombies

# ---------------------------------------------------------------------
title "6a. Graceful shutdown with SIGTERM"
kill -TERM "$WD"
check "watchdog exited"                  wait_exit 8
check "shutdown reason logged"           wait_for 'SHUTDOWN +received SIGTERM' 1
check "SIGTERM sent to children"         test "$(count 'STOP +sent SIGTERM to worker')" -eq 2
check "alpha reaped during shutdown"     wait_for 'STOPPED +worker alpha' 1
check "beta reaped during shutdown"      wait_for 'STOPPED +worker beta' 1
check "all workers reaped message"       wait_for 'SHUTDOWN +all workers terminated and reaped' 1
check "no restart after shutdown began"  test "$(count 'RESTART')" -eq 1
check "alpha process is gone"            gone "$A2"
check "beta process is gone"             gone "$B"

# ---------------------------------------------------------------------
title "2. Normal termination: exit code 0 is detected and not restarted"
start s2 -- alpha:0:0 short:1:0
check "normal exit detected"             wait_for 'EXIT +worker short \(pid=[0-9]+\) finished normally \(exited with code 0\)' 4
sleep 2.5
check "short was not restarted"          test "$(count 'RESTART +restarting worker short')" -eq 0
check "short was spawned only once"      test "$(count 'SPAWN +worker short')" -eq 1
kill -USR1 "$WD"
check "report shows short as EXITED"     wait_for 'REPORT +name=short +pid=- +state=EXITED' 3
check "no zombies"                       no_zombies
kill -TERM "$WD"; check "watchdog exited" wait_exit 8

# ---------------------------------------------------------------------
title "4. Restart limit: a crashing worker is restarted at most 3 times"
start s4 -- alpha:0:0 crash:1:3
check "error exit classified as unexpected" wait_for 'CRASH +worker crash .* exited with code 3' 3
check "restart limit reached"            wait_for 'LIMIT +worker crash reached the restart limit \(3\)' 15
sleep 3
check "exactly 3 restarts"               test "$(count 'RESTART +restarting worker crash')" -eq 3
check "exactly 4 starts (1 + 3)"         test "$(count 'SPAWN +worker crash')" -eq 4
check "no further attempts after limit"  test "$(count 'SCHEDULE +worker crash')" -eq 3
check "other worker still running"       alive "$(worker_pid alpha)"
check "no zombies"                       no_zombies
kill -USR1 "$WD"
check "report shows crash as FAILED"     wait_for 'REPORT +name=crash +pid=- +state=FAILED +restarts=3/3' 3
kill -TERM "$WD"; check "watchdog exited" wait_exit 8

# ---------------------------------------------------------------------
title "6b. Ctrl+C (SIGINT to the terminal's foreground process group)"
start s6b setsid -- alpha:0:0 beta:0:0
check "both workers started"             wait_for 'SPAWN +worker beta started' 3
A=$(worker_pid alpha); B=$(worker_pid beta)
check "watchdog leads its own group (like a shell job)" test "$(pgid_of "$WD")" = "$WD"
kill -INT -- "-$WD"                      # exactly what the terminal does on Ctrl+C
check "watchdog exited"                  wait_exit 8
check "shutdown reason logged"           wait_for 'SHUTDOWN +received SIGINT' 1
check "workers were stopped by the watchdog, not by Ctrl+C directly" \
      test "$(count 'STOP +sent SIGTERM to worker')" -eq 2
check "no worker counted as crashed"     test "$(count 'CRASH')" -eq 0
check "all workers reaped"               wait_for 'SHUTDOWN +all workers terminated and reaped' 1
check "alpha process is gone"            gone "$A"
check "beta process is gone"             gone "$B"

# ---------------------------------------------------------------------
title "6c. Shutdown timeout: a worker that ignores SIGTERM gets SIGKILL"
start s6c -- -t 2 alpha:0:0 stubborn:0:0:ignore
check "both workers started"             wait_for 'SPAWN +worker stubborn started' 3
S=$(worker_pid stubborn)
T0=$(date +%s%3N)
kill -TERM "$WD"
check "watchdog exited"                  wait_exit 8
T1=$(date +%s%3N)
check "SIGKILL sent after the timeout"   wait_for 'KILL +worker stubborn .* sent SIGKILL' 1
check "stubborn reaped (signal 9)"       wait_for 'STOPPED +worker stubborn .* killed by signal 9' 1
check "alpha stopped normally by SIGTERM" wait_for 'STOPPED +worker alpha .* signal 15' 1
check "SIGKILL not sent before the 2s timeout ($((T1 - T0)) ms)" test $((T1 - T0)) -ge 2000
check "stubborn process is gone"         gone "$S"

# ---------------------------------------------------------------------
title "8. Bounded restarts when exec() fails (no infinite loop)"
start s8 -- -w ./does-not-exist a:0:0 b:0:0
check "watchdog stops by itself"         wait_exit 20
check "a reached the limit"              wait_for 'LIMIT +worker a reached' 1
check "b reached the limit"              wait_for 'LIMIT +worker b reached' 1
check "exec failure seen as exit 127"    wait_for 'CRASH +worker a .* exited with code 127' 1
check "a started exactly 4 times"        test "$(count 'SPAWN +worker a ')" -eq 4
check "nothing left to supervise"        wait_for 'SHUTDOWN +no running or restartable workers left' 1

# ---------------------------------------------------------------------
title "7. Log verification: timestamps and all significant events"
ALL="$LOGDIR/all.log"
cat "$LOGDIR"/s*.log > "$ALL"
LOG="$ALL"
TS='^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} \[watchdog [0-9]+\] '
check "every log line has a timestamp"   test "$(grep -Evc "$TS" "$ALL")" -eq 0
for tag in START SPAWN EXIT CRASH SCHEDULE RESTART LIMIT REPORT SHUTDOWN STOP STOPPED KILL; do
    check "event '$tag' is logged"       test "$(grep -Ec "$TS$tag " "$ALL")" -gt 0
done

# ---------------------------------------------------------------------
echo
echo "Result: $PASS passed, $FAIL failed (logs in $LOGDIR/)"
[[ $FAIL -eq 0 ]]
