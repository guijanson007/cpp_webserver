#!/usr/bin/env bash
# WEB-13 check: idle keep-alive connections are closed after --idle-timeout seconds.
#
# Runs the server with --idle-timeout 2. "Closed in time" means EOF/RST arrives between
# 1.8 s and 4.5 s after the last activity (timeout plus sweep granularity plus slack).
#
# Cases:
#   1. idle-silent      connect, send nothing -> closed in time
#   2. idle-after-req   one request, then silence -> response arrives, then closed in time
#   3. active-survives  a request every 0.5 s for 6 s (3x the timeout) -> every one answered
#   4. active-then-idle requests for 4 s (past the first timeout), then silence -> closed in time
#                       after the LAST request (catches a timer that only fires once per connection)
#   5. fd-reuse         open and close A, then B (likely the same fd number) sends a request every
#                       1.5 s for 7 s while silent connections expire around it -> B is never cut
#                       off (catches timers keyed by a reused fd number)
#   6. many-idle        300 silent connections -> all closed in time, ss shows none left
#   7. survival         the server still answers after all of the above
#   8. disabled         --idle-timeout 0 (documented as "no timeout") -> idle socket still open after 4 s
#
# Usage: scripts/check_idle_timeout.sh [server-binary] [port]
# Defaults: build/server/server, 8080
set -u

SERVER_BIN="${1:-build/server/server}"
PORT="${2:-8080}"
FAILS=0

fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
pass() { echo "PASS: $*"; }

[[ -x "$SERVER_BIN" ]] || { echo "FAIL: server binary not found: $SERVER_BIN (build first)"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 is required"; exit 1; }

LOG="$(mktemp)"
ROOT_TMP="$(mktemp -d)"
HELPER="$(mktemp --suffix=.py)"
SERVER_PID=""
trap '[[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null; wait 2>/dev/null; rm -f "$LOG" "$HELPER"; rm -rf "$ROOT_TMP"' EXIT

cat >"$HELPER" <<'PY'
import socket, sys, time

PORT = int(sys.argv[1])
CASE = sys.argv[2]
REQ = b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"
BODY = b"Hello, World!"
LO, HI = 1.8, 4.5


def connect():
    return socket.create_connection(("127.0.0.1", PORT), timeout=10)


def request(s):
    """Send one GET and read the full response. False if the server closed instead."""
    s.sendall(REQ)
    buf = b""
    s.settimeout(3)
    while not buf.endswith(BODY):
        try:
            chunk = s.recv(4096)
        except (ConnectionResetError, socket.timeout):
            return False
        if not chunk:
            return False
        buf += chunk
    return b" 200 " in buf.split(b"\r\n", 1)[0]


def seconds_until_closed(s, limit):
    """Seconds until EOF/RST, or None if still open after `limit`."""
    t0 = time.monotonic()
    s.settimeout(limit)
    try:
        data = s.recv(1)
    except socket.timeout:
        return None
    except ConnectionResetError:
        return time.monotonic() - t0
    if data:
        return -1.0  # unexpected bytes
    return time.monotonic() - t0


def verdict(label, elapsed):
    if elapsed is None:
        print(f"FAIL {label}: still open after {HI + 3:.1f} s")
    elif elapsed < 0:
        print(f"FAIL {label}: server sent unexpected bytes")
    elif elapsed < LO:
        print(f"FAIL {label}: closed after {elapsed:.2f} s, before the timeout")
    elif elapsed > HI:
        print(f"FAIL {label}: closed after {elapsed:.2f} s, too late")
    else:
        print(f"PASS {label}: closed {elapsed:.2f} s after last activity")


def keep_active(s, seconds, label):
    end = time.monotonic() + seconds
    n = 0
    while time.monotonic() < end:
        if not request(s):
            print(f"FAIL {label}: connection dropped while active (after {n} answered requests)")
            return False
        n += 1
        time.sleep(0.5)
    print(f"PASS {label}: {n} requests answered over {seconds} s")
    return True


if CASE == "idle-silent":
    s = connect()
    verdict("idle-silent", seconds_until_closed(s, HI + 3))

elif CASE == "idle-after-req":
    s = connect()
    if not request(s):
        print("FAIL idle-after-req: no response")
    else:
        verdict("idle-after-req", seconds_until_closed(s, HI + 3))

elif CASE == "active-survives":
    keep_active(connect(), 6, "active-survives")

elif CASE == "active-then-idle":
    s = connect()
    if keep_active(s, 4, "active-then-idle (active phase)"):
        verdict("active-then-idle", seconds_until_closed(s, HI + 3))

elif CASE == "fd-reuse":
    import threading
    a = connect()
    request(a)
    a.close()
    time.sleep(0.2)  # let the server see A's FIN and free the fd number
    b = connect()   # usually gets A's old fd number
    # Background traffic: a new (silent) connection every 0.4 s, so timeouts fire and the server
    # sweeps often while B is alive. The server reaps these itself.
    stop = threading.Event()
    extra = []
    def churn():
        while not stop.wait(0.4):
            extra.append(connect())
    t = threading.Thread(target=churn)
    t.start()
    # B is active every 1.5 s, under the 2 s timeout, so it must never be closed.
    end = time.monotonic() + 7
    n = 0
    ok = True
    while time.monotonic() < end:
        if not request(b):
            print(f"FAIL fd-reuse: B dropped while active (after {n} answered requests, 1.5 s apart)")
            ok = False
            break
        n += 1
        time.sleep(1.5)
    stop.set()
    t.join()
    if ok:
        print(f"PASS fd-reuse: B answered {n} requests 1.5 s apart while other connections expired")

elif CASE == "many-idle":
    n = 300
    socks = [connect() for _ in range(n)]
    t0 = time.monotonic()
    deadline = t0 + HI + 2
    early = late = 0
    for s in socks:
        remaining = max(0.01, deadline - time.monotonic())
        e = seconds_until_closed(s, remaining)
        total = time.monotonic() - t0
        if e is None:
            late += 1
        elif total < LO:
            early += 1
    if late or early:
        print(f"FAIL many-idle: {late} of {n} still open, {early} closed too early")
    else:
        print(f"PASS many-idle: all {n} closed in {time.monotonic() - t0:.2f} s")

elif CASE == "disabled":
    s = connect()
    e = seconds_until_closed(s, 4)
    if e is None:
        print("PASS disabled: idle socket still open after 4 s with --idle-timeout 0")
    else:
        print(f"FAIL disabled: closed after {e:.2f} s although --idle-timeout 0")

elif CASE == "survival":
    print("PASS survival: GET / -> 200" if request(connect()) else "FAIL survival: no 200")
PY

start_server() {
    "$SERVER_BIN" --port "$PORT" --max-connections 1000 --root "$ROOT_TMP" "$@" >"$LOG" 2>&1 &
    SERVER_PID=$!
    sleep 0.5
    kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL: server did not start: $(head -c 200 "$LOG")"; exit 1; }
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; SERVER_PID=""
}

run_case() {
    local out
    out="$(python3 -I "$HELPER" "$PORT" "$1" 2>&1)"
    while IFS= read -r line; do
        case "$line" in
            PASS*) pass "${line#PASS }" ;;
            FAIL*) fail "${line#FAIL }" ;;
            *) fail "$1: helper said: $line" ;;
        esac
    done <<<"$out"
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        fail "$1: server died"
        start_server "${CURRENT_ARGS[@]}"
    fi
}

CURRENT_ARGS=(--idle-timeout 2)
start_server "${CURRENT_ARGS[@]}"

# Cases 1-4 are independent; run them in parallel to keep the script short.
pids=()
OUTS=()
for c in idle-silent idle-after-req active-survives active-then-idle; do
    f="$(mktemp)"; OUTS+=("$f")
    (run_case "$c" >"$f"; exit "$FAILS") &
    pids+=($!)
done
for i in "${!pids[@]}"; do
    wait "${pids[$i]}"; FAILS=$((FAILS + $?))
    cat "${OUTS[$i]}"; rm -f "${OUTS[$i]}"
done

# fd-reuse runs alone so no other connection grabs the freed fd number.
run_case fd-reuse
run_case many-idle
left="$(ss -Htn state established "( sport = :$PORT )" 2>/dev/null | wc -l)"
if [[ "$left" -eq 0 ]]; then pass "many-idle: ss shows 0 established connections on :$PORT"
else fail "many-idle: ss still shows $left established connections on :$PORT"; fi

run_case survival
stop_server

CURRENT_ARGS=(--idle-timeout 0)
start_server "${CURRENT_ARGS[@]}"
run_case disabled
run_case survival
stop_server

echo
if [[ $FAILS -eq 0 ]]; then echo "ALL CHECKS PASSED"; else echo "$FAILS CHECK(S) FAILED"; exit 1; fi
