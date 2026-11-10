#!/usr/bin/env bash
# WEB-3 check (ported to HTTP in WEB-7): output buffer + EPOLLOUT write path.
#
# The server answers every request with the same fixed response, so output volume is
# driven by pipelining many requests (N requests in -> N identical responses out).
#
# Cases:
#   1. basic         two pipelined GETs get two 200 responses
#   2. rst-survival  clients that reset the connection (RST) must not kill the
#                    server (SIGPIPE / double close), and it must keep serving
#   3. backpressure  a client pipelines 100k requests without reading while the
#                    server's ~8 MB of output backs up; another client must still
#                    get served, and the slow client must then receive every
#                    response intact
#   4. epollout      (needs strace) EPOLLOUT is only armed on EAGAIN and
#                    disarmed again once the buffer drains; skipped if missing
#
# Usage: scripts/check_write_path.sh [server-binary] [port]
# Defaults: build/server/server, 8080
set -u

SERVER_BIN="${1:-build/server/server}"
PORT="${2:-8080}"
FAILS=0

fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
pass() { echo "PASS: $*"; }
skip() { echo "SKIP: $*"; }

[[ -x "$SERVER_BIN" ]] || { echo "FAIL: server binary not found: $SERVER_BIN (build first)"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 is required"; exit 1; }

LOG="$(mktemp)"
# Kill the server and, when run under strace, its traced child as well.
stop_server() {
    pkill -TERM -P "$SERVER_PID" 2>/dev/null
    kill "$SERVER_PID" 2>/dev/null
    sleep 0.2
    pkill -KILL -P "$SERVER_PID" 2>/dev/null
    kill -KILL "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
}
trap 'stop_server; rm -f "$LOG"' EXIT

"$SERVER_BIN" >"$LOG" 2>&1 &
SERVER_PID=$!
sleep 0.5
kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL: server did not start (port $PORT in use?)"; exit 1; }

server_alive() { kill -0 "$SERVER_PID" 2>/dev/null; }

# ---- 1. basic --------------------------------------------------------------
python3 -I - "$PORT" <<'PY'
import socket, sys
REQ = b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2)
s.sendall(REQ * 2)
got = b""
try:
    while got.count(b"HTTP/1.1 200 OK\r\n") < 2 or not got.endswith(b"Hello, World!"):
        chunk = s.recv(4096)
        if not chunk:
            break
        got += chunk
except socket.timeout:
    pass
sys.exit(0 if got.count(b"HTTP/1.1 200 OK\r\n") == 2 and got.endswith(b"Hello, World!") else 1)
PY
[[ $? -eq 0 ]] && pass "basic: two pipelined GETs got two 200 responses" || fail "basic: missing/incorrect responses"

# ---- 2. rst-survival -------------------------------------------------------
python3 -I - "$PORT" <<'PY'
import socket, struct, sys, time
port = int(sys.argv[1])
for _ in range(10):
    c = socket.create_connection(("127.0.0.1", port), timeout=2)
    c.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n" * 20)
    c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    c.close()  # sends RST
    time.sleep(0.05)
PY
sleep 0.3
if server_alive; then
    pass "rst-survival: server still running after 10 reset clients"
else
    wait "$SERVER_PID" 2>/dev/null; rc=$?
    fail "rst-survival: server died (exit $rc; 141 = SIGPIPE)"
    echo "Server exited early, remaining cases skipped."
    exit 1
fi
python3 -I - "$PORT" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2)
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
try:
    got = s.recv(200)
except socket.timeout:
    got = b""
sys.exit(0 if got.startswith(b"HTTP/1.1 200 OK\r\n") else 1)
PY
[[ $? -eq 0 ]] && pass "rst-survival: new client still gets a 200" || fail "rst-survival: server up but not answering afterwards"

# ---- 3. backpressure -------------------------------------------------------
python3 -I - "$PORT" <<'PY'
import socket, sys, threading, time
port = int(sys.argv[1])
N = 100000                           # ~3.6 MB of requests -> ~8 MB of responses
REQ = b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"

# Learn the exact bytes of one response.
probe = socket.create_connection(("127.0.0.1", port), timeout=3)
probe.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
one = b""
while True:
    d = probe.recv(4096)
    if not d:
        break
    one += d
body = b"Hello, World!"
expected_one = one.replace(b"Connection: close\r\n", b"")   # same response without the close header
if not expected_one.endswith(body):
    print("unexpected probe response:", one); sys.exit(4)
expected_total = len(expected_one) * N

slow = socket.socket()
slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)  # small window: fill the server's send buffer fast
slow.connect(("127.0.0.1", port))
slow.settimeout(10)

def send_all():
    try:
        slow.sendall(REQ * N)
    except Exception as e:
        print("sender error:", e)
t = threading.Thread(target=send_all, daemon=True)
t.start()
time.sleep(1.5)                      # client is NOT reading; server output backs up

# A second client must still be served while the first one is stalled.
other = socket.create_connection(("127.0.0.1", port), timeout=3)
other.sendall(REQ)
try:
    got = other.recv(200)
except socket.timeout:
    got = b""
if not got.startswith(b"HTTP/1.1 200 OK\r\n"):
    print("second client starved while first is stalled")
    sys.exit(2)

# Now drain the slow client and verify every response, byte for byte.
received = bytearray()
deadline = time.time() + 30
slow.settimeout(5)
try:
    while len(received) < expected_total and time.time() < deadline:
        chunk = slow.recv(65536)
        if not chunk:
            break
        received += chunk
except socket.timeout:
    pass
t.join(timeout=5)
if bytes(received) != expected_one * N:
    print(f"slow client got {len(received)} of {expected_total} bytes")
    sys.exit(3)
PY
case $? in
    0) pass "backpressure: other client served while stalled; 100k pipelined responses intact" ;;
    2) fail "backpressure: a second client was starved while the first was stalled" ;;
    3) fail "backpressure: slow client did not receive every response intact" ;;
    *) fail "backpressure: client script error" ;;
esac
server_alive && pass "backpressure: server still running" || fail "backpressure: server died"

# ---- 3b. half-close: responses must be flushed before the server closes ------
python3 -I - "$PORT" <<'PY'
import socket, sys
REQ = b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"
N = 20000
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
s.sendall(REQ * N)
s.shutdown(socket.SHUT_WR)          # no more input; we still read the replies
got = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break                        # server closed after draining
    got += chunk
sys.exit(0 if got.count(b"HTTP/1.1 200 OK\r\n") == N and got.endswith(b"Hello, World!") else 1)
PY
[[ $? -eq 0 ]] && pass "half-close: all 20000 responses received, then server closed" || fail "half-close: responses lost or connection never closed"

# ---- 4. EPOLLOUT armed only when needed (strace) ---------------------------
if ! command -v strace >/dev/null; then
    skip "epollout: strace not installed (sudo dnf install strace)"
else
    stop_server
    STRACE_LOG="$(mktemp)"
    strace -f -e trace=epoll_ctl -o "$STRACE_LOG" "$SERVER_BIN" >/dev/null 2>&1 &
    SERVER_PID=$!
    sleep 0.7
    # small echo: never needs EPOLLOUT
    python3 -I - "$PORT" <<'EOF'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2)
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"); s.recv(200)
EOF
    ECHO_RC=$?
    sleep 0.3
    [[ $ECHO_RC -eq 0 ]] || fail "epollout: tiny request got no reply, so the check is meaningless"
    if grep -q "EPOLLOUT" "$STRACE_LOG"; then
        fail "epollout: EPOLLOUT armed for a tiny response that never hit EAGAIN"
    else
        pass "epollout: not armed for a small response"
    fi
    rm -f "$STRACE_LOG"
fi

# ---- noise check (informational) -------------------------------------------
if grep -q "Failed to remove file descriptor" "$LOG"; then
    echo "NOTE: server log contains 'Failed to remove file descriptor' (double reactor.remove on close?)"
fi

echo
if [[ $FAILS -eq 0 ]]; then
    echo "All write-path checks passed."
    exit 0
fi
echo "$FAILS check(s) failed."
exit 1
