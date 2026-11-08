#!/usr/bin/env bash
# WEB-2 checks for the epoll reactor refactor.
#
#   1. strace: no fd is ever closed twice (no close() returning EBADF), including
#      the over-limit path.
#   2. SIGSTOP/SIGCONT: the server survives being paused and still accepts clients.
#   3. ptrace attach/detach (strace -p): EINTR in epoll_wait must not kill the server.
#
# Usage: scripts/check_reactor.sh [server-binary] [port]
# Defaults: build/server/server, 8080. Needs: strace, ss (iproute2), bash /dev/tcp.
# Checks 1 and 3 are skipped (with a warning) when strace is not installed.
set -u

SERVER_BIN="${1:-build/server/server}"
PORT="${2:-8080}"
WORKDIR="$(mktemp -d)"
LOG="$WORKDIR/server.log"
STRACE_LOG="$WORKDIR/strace.log"

fail() { echo "FAIL: $*"; exit 1; }
pass() { echo "PASS: $*"; }
skip() { echo "SKIP: $*"; }

[[ -x "$SERVER_BIN" ]] || fail "server binary not found: $SERVER_BIN (build first)"

HAVE_STRACE=0
command -v strace >/dev/null && HAVE_STRACE=1

SERVER_PID=""
WRAPPER_PID=""
declare -a FDS=()

cleanup() {
    for fd in "${FDS[@]:-}"; do [[ -n "$fd" ]] && exec {fd}>&- 2>/dev/null; done
    [[ -n "$SERVER_PID" ]] && kill -CONT "$SERVER_PID" 2>/dev/null
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null
    [[ -n "$WRAPPER_PID" ]] && wait "$WRAPPER_PID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

open_client() {
    local fd
    exec {fd}<>"/dev/tcp/127.0.0.1/$PORT" 2>/dev/null || return 1
    FDS+=("$fd")
}

close_clients() {
    for fd in "${FDS[@]:-}"; do [[ -n "$fd" ]] && exec {fd}>&- 2>/dev/null; done
    FDS=()
}

command -v ss >/dev/null || fail "ss (iproute2) is required"
# Connections the kernel shows as established on the server side. Clients stay open
# until close_clients, so this equals the number accepted so far. Independent of
# server logging and stdout buffering.
accepted_count() { ss -Htn state established "( sport = :$PORT )" | wc -l; }
alive() { kill -0 "$SERVER_PID" 2>/dev/null; }

# ---------------------------------------------------------------- 1. double close
if (( HAVE_STRACE )); then
    echo "[1/3] strace: looking for double close()"
    strace -f -o "$STRACE_LOG" -e trace=close,accept4,epoll_ctl "$SERVER_BIN" --max-connections 10 >"$LOG" 2>&1 &
    WRAPPER_PID=$!
    sleep 1
    # The traced server is the child of strace.
    SERVER_PID="$(pgrep -P "$WRAPPER_PID" -x "$(basename "$SERVER_BIN")" | head -n1)"
    [[ -n "$SERVER_PID" ]] || fail "could not find the traced server process (port $PORT in use?)"

    # 15 clients: 10 accepted, 5 shed on the over-limit path. Then everyone leaves.
    for _ in $(seq 15); do open_client; done
    sleep 1
    close_clients
    sleep 1

    alive || fail "server died while clients connected and disconnected"
    n=$(grep -c 'close(.*) *= -1 EBADF' "$STRACE_LOG")
    (( n == 0 )) || { grep 'EBADF' "$STRACE_LOG" | head; fail "$n close() call(s) returned EBADF: an fd was closed twice"; }
    ok=$(grep -c 'close(.*) *= 0' "$STRACE_LOG")
    (( ok >= 15 )) || fail "expected at least 15 successful close() calls (10 accepted + 5 shed), saw $ok"
    pass "no double close (0 EBADF, $ok successful close() calls)"

    kill "$SERVER_PID" 2>/dev/null
    wait "$WRAPPER_PID" 2>/dev/null
    WRAPPER_PID=""
    SERVER_PID=""
else
    skip "[1/3] strace not installed (sudo dnf install strace)"
fi

# ---------------------------------------------------------------- 2. SIGSTOP / SIGCONT
echo "[2/3] SIGSTOP/SIGCONT"
"$SERVER_BIN" --max-connections 10 >"$LOG" 2>&1 &
SERVER_PID=$!
sleep 0.5
alive || fail "server did not start (port $PORT in use?)"

kill -STOP "$SERVER_PID"
sleep 0.5
# The kernel completes the handshake while the process is stopped; the server must
# pick the connection up after SIGCONT.
open_client || fail "could not connect while server was stopped"
open_client || fail "could not connect while server was stopped"
kill -CONT "$SERVER_PID"
sleep 1

alive || fail "server died after SIGSTOP/SIGCONT"
n=$(accepted_count)
(( n == 2 )) || fail "expected 2 connections accepted after SIGCONT, got $n"
pass "server survived SIGSTOP/SIGCONT and accepted the queued clients"

open_client || fail "could not connect after SIGCONT"
sleep 0.5
n=$(accepted_count)
(( n == 3 )) || fail "expected a fresh connection to be accepted, got $n"
pass "server still accepts new clients after SIGCONT"

# ---------------------------------------------------------------- 3. ptrace attach/detach
if (( HAVE_STRACE )); then
    echo "[3/3] strace -p attach/detach (EINTR in epoll_wait)"
    # SIGINT makes strace detach cleanly; the tracee keeps running.
    timeout -s INT 2 strace -f -p "$SERVER_PID" -o /dev/null 2>/dev/null
    sleep 0.5
    alive || fail "server died after strace attach/detach"
    open_client || fail "could not connect after strace detach"
    sleep 0.5
    n=$(accepted_count)
    (( n == 4 )) || fail "expected 4 accepted connections after detach, got $n"
    pass "server survived strace attach/detach and still accepts clients"
else
    skip "[3/3] strace not installed"
fi

echo "All executed checks passed."
