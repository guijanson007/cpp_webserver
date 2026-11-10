#!/usr/bin/env bash
# WEB-7 check: HTTP head parsing wired into the server.
#
# Cases:
#   1. get          GET -> 200, Content-Length equals the body length
#   2. head         HEAD -> same headers as GET, no body bytes
#   3. keep-alive   HTTP/1.1 reuses the connection (2 requests, 1 socket)
#   4. http10       HTTP/1.0 closes after the response; "Connection: keep-alive" keeps it open
#   5. conn-close   HTTP/1.1 + "Connection: close" closes after the response
#   6. pipelining   two requests in one send -> two responses, in order
#   7. split        a request sent byte by byte -> exactly one response
#   8. errors       400/431/501/505 statuses, then the server closes the connection
#   9. survival     the server still answers after all of the above
#
# Usage: scripts/check_http.sh [server-binary] [port]
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
trap 'kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; rm -f "$LOG"' EXIT

"$SERVER_BIN" --port "$PORT" --max-connections 1000 >"$LOG" 2>&1 &
SERVER_PID=$!
sleep 0.5
kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL: server did not start (port $PORT in use?)"; exit 1; }

# One python helper runs every case; each case exits 0 on success. Shared helpers come first.
run_case() {
    local name="$1" desc="$2"
    python3 -I - "$PORT" "$name" <<'PY'
import socket, sys, time

port, case = int(sys.argv[1]), sys.argv[2]
HOST_REQ = b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"

def conn():
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    return s

def read_response(s, buf=b"", head_only=False):
    """Read one response; returns (status, headers dict, body, leftover). Raises on EOF mid-response."""
    while b"\r\n\r\n" not in buf:
        d = s.recv(4096)
        if not d:
            raise EOFError("closed before head complete: %r" % buf)
        buf += d
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split()[1])
    headers = {}
    for l in lines[1:]:
        k, v = l.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    n = 0 if head_only else int(headers.get("content-length", "0"))
    while len(rest) < n:
        d = s.recv(4096)
        if not d:
            raise EOFError("closed mid-body")
        rest += d
    return status, headers, rest[:n], rest[n:], lines[0]

def closed_by_server(s, wait=2.0):
    s.settimeout(wait)
    try:
        return s.recv(1) == b""
    except socket.timeout:
        return False
    except ConnectionResetError:
        return True

def still_open(s, wait=0.4):
    s.settimeout(wait)
    try:
        return s.recv(1) != b""   # data or EOF -> not "quietly open"
    except socket.timeout:
        return True

def check(cond, msg):
    if not cond:
        print(msg); sys.exit(1)

if case == "get":
    s = conn(); s.sendall(HOST_REQ)
    st, h, body, rest, line = read_response(s)
    check(line.startswith("HTTP/1.1 200 OK"), "status line: %r" % line)
    check("content-length" in h and int(h["content-length"]) == len(body) > 0, "bad Content-Length: %r / %d body bytes" % (h, len(body)))

elif case == "head":
    s = conn(); s.sendall(HOST_REQ + b"HEAD / HTTP/1.1\r\nHost: x\r\n\r\n")
    st, hg, body, rest, _ = read_response(s)
    st2, hh, body2, rest2, line = read_response(s, rest, head_only=True)
    check(st2 == 200 and line.startswith("HTTP/1.1 200"), "HEAD status: %r" % line)
    check(hh.get("content-length") == hg.get("content-length"), "HEAD Content-Length %r != GET %r" % (hh.get("content-length"), hg.get("content-length")))
    check(rest2 == b"", "HEAD sent %d body bytes" % len(rest2))
    # A follow-up request on the same connection must be answered, proving no stray body bytes.
    s.sendall(HOST_REQ)
    st3, _, _, _, _ = read_response(s)
    check(st3 == 200, "request after HEAD got %d" % st3)

elif case == "keep-alive":
    s = conn()
    for i in range(3):
        s.sendall(HOST_REQ)
        st, h, body, rest, _ = read_response(s)
        check(st == 200 and rest == b"", "request %d on reused connection failed" % i)
    check(still_open(s), "server closed an HTTP/1.1 keep-alive connection")

elif case == "http10":
    s = conn(); s.sendall(b"GET / HTTP/1.0\r\n\r\n")
    st, h, body, rest, _ = read_response(s)
    check(st == 200, "HTTP/1.0 status %d" % st)
    check(closed_by_server(s), "HTTP/1.0 connection was not closed")
    s = conn(); s.sendall(b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
    st, h, body, rest, _ = read_response(s)
    check(st == 200, "HTTP/1.0 keep-alive status %d" % st)
    s.sendall(b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
    st, *_ = read_response(s)
    check(st == 200, "HTTP/1.0 keep-alive connection did not serve a 2nd request")

elif case == "conn-close":
    s = conn(); s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    st, h, body, rest, _ = read_response(s)
    check(st == 200 and h.get("connection", "").lower() == "close", "missing 'Connection: close' in %r" % h)
    check(closed_by_server(s), "connection not closed after Connection: close")
    s = conn(); s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: CLOSE\r\n\r\n")
    read_response(s)
    check(closed_by_server(s), "Connection: CLOSE (any case) not honored")

elif case == "pipelining":
    reqs = b"".join(b"GET /%d HTTP/1.1\r\nHost: x\r\n\r\n" % i for i in range(5))
    s = conn(); s.sendall(reqs)
    buf = b""
    for i in range(5):
        st, h, body, buf, _ = read_response(s, buf)
        check(st == 200, "pipelined response %d status %d" % (i, st))
    check(buf == b"", "extra bytes after 5 responses: %r" % buf)
    s.settimeout(0.4)
    try:
        extra = s.recv(1)
        check(extra == b"", "a 6th response arrived for 5 requests")
    except socket.timeout:
        pass
    # An error in the middle: earlier requests answered, then the error, then close.
    s = conn(); s.sendall(HOST_REQ + b"garbage\r\n\r\n" + HOST_REQ)
    st1, _, _, buf, _ = read_response(s)
    st2, _, _, buf, _ = read_response(s, buf)
    check((st1, st2) == (200, 400), "expected 200 then 400, got %d then %d" % (st1, st2))
    check(closed_by_server(s), "connection not closed after the 400")

elif case == "split":
    s = conn(); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    for b in HOST_REQ:
        s.sendall(bytes([b])); time.sleep(0.005)
    st, h, body, rest, _ = read_response(s)
    check(st == 200, "split request status %d" % st)
    s.settimeout(0.4)
    try:
        extra = s.recv(1)
        check(extra == b"", "unexpected extra bytes after the single response: %r" % extra)
    except socket.timeout:
        pass

elif case == "errors":
    big = b"GET / HTTP/1.1\r\nHost: x\r\n" + b"".join(b"X-Pad-%d: %s\r\n" % (i, b"a" * 100) for i in range(200)) + b"\r\n"
    cases = [
        (b"garbage\r\n\r\n", 400),
        (b"GET / HTTP/1.1\r\nAccept: */*\r\n\r\n", 400),          # missing Host
        (b"GET / HTTP/1.1\nHost: x\n\n", 400),                    # bare LF
        (b"FOO / HTTP/1.1\r\nHost: x\r\n\r\n", 501),
        (b"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505),
        (b"GET /" + b"a" * 10000 + b" HTTP/1.1\r\nHost: x\r\n\r\n", 414),
        (big, 431),
    ]
    for raw, want in cases:
        s = conn(); s.sendall(raw)
        try:
            st, h, body, rest, line = read_response(s)
        except Exception as e:
            check(False, "%r... -> no response (%s)" % (raw[:30], e))
        check(st == want, "%r... -> %d, wanted %d" % (raw[:30], st, want))
        check(h.get("connection", "").lower() == "close", "%d response lacks 'Connection: close'" % st)
        check(closed_by_server(s), "%r... -> connection stayed open after %d" % (raw[:30], st))

elif case == "survival":
    s = conn(); s.sendall(HOST_REQ)
    st, *_ = read_response(s)
    check(st == 200, "server no longer answers: %d" % st)

sys.exit(0)
PY
    local rc=$?
    [[ $rc -eq 0 ]] && pass "$name: $desc" || fail "$name: $desc"
    kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL: server died during '$name'"; exit 1; }
}

run_case get         "200 with correct Content-Length"
run_case head        "same headers as GET, no body"
run_case keep-alive  "HTTP/1.1 connection is reused"
run_case http10      "HTTP/1.0 closes unless keep-alive is requested"
run_case conn-close  "'Connection: close' honored"
run_case pipelining  "pipelined requests answered in order; error mid-stream closes"
run_case split       "byte-by-byte request gets exactly one response"
run_case errors      "400/414/431/501/505 + connection closed"
run_case survival    "server still serving"

# Cross-check with curl when available (the ticket's acceptance commands).
if command -v curl >/dev/null; then
    OUT="$(curl -s -i "http://127.0.0.1:$PORT/")"
    [[ "$OUT" == HTTP/1.1\ 200\ OK* ]] && pass "curl GET" || fail "curl GET: $OUT"
    HEADS="$(curl -s -I "http://127.0.0.1:$PORT/")"
    [[ "$HEADS" == HTTP/1.1\ 200\ OK* ]] && pass "curl -I" || fail "curl -I: $HEADS"
    REUSE="$(curl -sv "http://127.0.0.1:$PORT/" "http://127.0.0.1:$PORT/" 2>&1 >/dev/null | grep -ci 'Re-\?using existing')"
    [[ "$REUSE" -ge 1 ]] && pass "curl reuses the connection" || fail "curl did not reuse the connection"
else
    echo "SKIP: curl not installed"
fi

echo
if [[ $FAILS -eq 0 ]]; then echo "All HTTP checks passed."; exit 0; fi
echo "$FAILS check(s) failed."
exit 1
