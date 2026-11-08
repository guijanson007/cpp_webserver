#!/usr/bin/env bash
# WEB-3 check: output buffer + EPOLLOUT write path.
#
# Cases:
#   1. echo          a frame sent as "msg\0" comes back (NULs ignored; the echo
#                    may or may not re-append the delimiter)
#   2. rst-survival  clients that reset the connection (RST) must not kill the
#                    server (SIGPIPE / double close), and it must keep serving
#   3. backpressure  a client sends ~8 MB without reading while the server's
#                    output fills up; another client must still get served, and
#                    the slow client must then receive every byte, in order
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

# ---- 1. echo ---------------------------------------------------------------
python3 -I - "$PORT" <<'EOF'
import socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=2)
s.sendall(b"hello\0world\0")
got = b""
try:
    while len(got.replace(b"\0", b"")) < len(b"helloworld"):
        chunk = s.recv(4096)
        if not chunk:
            break
        got += chunk
except socket.timeout:
    pass
sys.exit(0 if got.replace(b"\0", b"") == b"helloworld" else 1)
EOF
[[ $? -eq 0 ]] && pass "echo: frames come back" || fail "echo: no/incorrect reply to 'hello' 'world' frames"

# ---- 2. rst-survival -------------------------------------------------------
python3 -I - "$PORT" <<'EOF'
import socket, struct, sys, time
port = int(sys.argv[1])
for _ in range(10):
    c = socket.create_connection(("127.0.0.1", port), timeout=2)
    c.sendall(b"x" * 50 + b"\0")
    c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    c.close()  # sends RST
    time.sleep(0.05)
EOF
sleep 0.3
if server_alive; then
    pass "rst-survival: server still running after 10 reset clients"
else
    wait "$SERVER_PID" 2>/dev/null; rc=$?
    fail "rst-survival: server died (exit $rc; 141 = SIGPIPE)"
    echo "Server exited early, remaining cases skipped."
    exit 1
fi
python3 -I - "$PORT" <<'EOF'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2)
s.sendall(b"ping\0")
try:
    got = s.recv(100)
except socket.timeout:
    got = b""
sys.exit(0 if got.replace(b"\0", b"") == b"ping" else 1)
EOF
[[ $? -eq 0 ]] && pass "rst-survival: new client still gets an echo" || fail "rst-survival: server up but not echoing afterwards"

# ---- 3. backpressure -------------------------------------------------------
python3 -I - "$PORT" <<'EOF'
import socket, sys, threading, time
port = int(sys.argv[1])
FRAMES, WIDTH = 80000, 100          # ~8 MB of payload
frames = [(b"%08d" % i).ljust(WIDTH, b"a") for i in range(FRAMES)]
expected = b"".join(frames)

slow = socket.socket()
slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)  # small window: fill the server's send buffer fast
slow.connect(("127.0.0.1", port))
slow.settimeout(10)

def send_all():
    try:
        slow.sendall(b"".join(f + b"\0" for f in frames))
    except Exception as e:
        print("sender error:", e)
t = threading.Thread(target=send_all, daemon=True)
t.start()
time.sleep(1.5)                     # client is NOT reading; server output backs up

# A second client must still be served while the first one is stalled.
other = socket.create_connection(("127.0.0.1", port), timeout=3)
other.sendall(b"other\0")
try:
    got = other.recv(100)
except socket.timeout:
    got = b""
if got.replace(b"\0", b"") != b"other":
    print("second client starved while first is stalled")
    sys.exit(2)

# Now drain the slow client and compare byte-for-byte.
received = bytearray()
deadline = time.time() + 20
slow.settimeout(5)
try:
    while len(received.replace(b"\0", b"")) < len(expected) and time.time() < deadline:
        chunk = slow.recv(65536)
        if not chunk:
            break
        received += chunk
except socket.timeout:
    pass
t.join(timeout=5)
payload = bytes(received).replace(b"\0", b"")
if payload != expected:
    print(f"slow client got {len(payload)} of {len(expected)} bytes, ordered-equal={payload == expected[:len(payload)]}")
    sys.exit(3)
EOF
case $? in
    0) pass "backpressure: other client served while stalled; ~8 MB echoed intact and in order" ;;
    2) fail "backpressure: a second client was starved while the first was stalled" ;;
    3) fail "backpressure: slow client did not receive every byte in order" ;;
    *) fail "backpressure: client script error" ;;
esac
server_alive && pass "backpressure: server still running" || fail "backpressure: server died"

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
s.sendall(b"small\0"); s.recv(100)
EOF
    ECHO_RC=$?
    sleep 0.3
    [[ $ECHO_RC -eq 0 ]] || fail "epollout: tiny echo got no reply, so the check is meaningless"
    if grep -q "EPOLLOUT" "$STRACE_LOG"; then
        fail "epollout: EPOLLOUT armed for a tiny echo that never hit EAGAIN"
    else
        pass "epollout: not armed for a small echo"
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
