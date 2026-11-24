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
# WEB-8 (Content-Length bodies):
#  10. body              POST with a body -> 200, connection stays open and usable
#  11. body-zero         POST/GET with Content-Length: 0 and GET with a body keep framing intact
#  12. body-pipelined    POST + GET in one send -> two 200s, no stray 400
#  13. body-split        head, then the body byte by byte -> exactly one response
#  14. body-limit        a body of exactly 1 MiB is accepted
#  15. body-too-large    over the limit -> 413 + close, answered before any body byte is sent
#  16. body-bad-length   -1, +5, "5 5", abc, empty, overflow -> 400 + close
#  17. body-te           Transfer-Encoding -> 501 + close
#  18. body-survival     the server still answers after all of the above
# WEB-10 (router; routes: GET / -> "Hello, World!", POST /echo -> echoes the body):
#  19. route-get       GET / -> 200 with the handler body
#  20. route-query     query string ignored when matching (also on pipelined requests)
#  21. route-404       unknown path / near-miss paths -> 404, connection stays open, not "Connection: close"
#  22. route-405       wrong method -> 405 + Allow (GET, HEAD on /; POST on /echo), connection stays open
#  23. route-head      HEAD / -> 200 same Content-Length as GET, no body; HEAD /echo -> 405
#  24. route-echo      POST /echo returns the body byte for byte (binary-safe)
#  25. route-close     404/405 with "Connection: close" carry the header and close
#  26. route-survival  the server still answers after the routing cases
# WEB-11 (static files; --root is a temp dir served under /static/, a secret file sits next to it):
#  27. static-get        GET /static/index.html -> 200, byte-exact body, text/html, keep-alive
#  28. static-head       HEAD -> same headers as GET, no body, next request still framed
#  29. static-mime       css, txt, upper-case .PNG, unknown extension -> octet-stream
#  30. static-binary     a 300 KB binary file (all byte values) arrives byte for byte
#  31. static-404        missing file, directories, "/static" without slash -> 404, keep-alive preserved
#  32. static-traversal  "..", ".", encoded "..", absolute path, backslash, NUL, dotfiles, escaping symlinks:
#                        never 200, never the secret bytes (raw sockets, so no client normalisation)
#  33. static-405        POST under the mount -> 405 + "Allow: GET, HEAD"
#  34. static-pipelined  static, dynamic, static in one send -> three responses in order
#  35. static-close      "Connection: close" on a static file -> header + close
#  36. static-survival   the server still answers after the static cases
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
# WEB-11 fixture: $STATIC_DIR/www is the document root, $STATIC_DIR/secret.txt must never be served.
STATIC_DIR="$(mktemp -d)"
trap 'kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; rm -f "$LOG"; rm -rf "$STATIC_DIR"' EXIT
WWW="$STATIC_DIR/www"
mkdir -p "$WWW/sub" "$STATIC_DIR/www-evil"
printf 'TOP-SECRET-CONTENT\n' >"$STATIC_DIR/secret.txt"
printf 'TOP-SECRET-CONTENT\n' >"$STATIC_DIR/www-evil/secret.txt"
printf '<!doctype html><h1>static index</h1>\n' >"$WWW/index.html"
printf 'body { color: red; }\n' >"$WWW/style.css"
printf 'nested page\n' >"$WWW/sub/page.txt"
printf 'png-ish\n' >"$WWW/LOGO.PNG"
printf 'raw\n' >"$WWW/data.unknownext"
python3 -I -c 'import sys; open(sys.argv[1], "wb").write(bytes(range(256)) * 1200)' "$WWW/blob.bin"
ln -s "$STATIC_DIR/secret.txt" "$WWW/leak.txt"
ln -s "$STATIC_DIR" "$WWW/up"
ln -s "$STATIC_DIR/www-evil/secret.txt" "$WWW/evil.txt"
ln -s /etc/passwd "$WWW/passwd"
ln -s ../secret.txt "$WWW/leak-rel.txt"
ln -s .. "$WWW/up-rel"
ln -s index.html "$WWW/alias.html"
mkdir -p "$WWW/.git"
printf 'TOP-SECRET-CONTENT\n' >"$WWW/.git/config"
printf 'TOP-SECRET-CONTENT\n' >"$WWW/.env"

"$SERVER_BIN" --port "$PORT" --max-connections 1000 --root "$WWW" >"$LOG" 2>&1 &
SERVER_PID=$!
sleep 0.5
kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL: server did not start (port $PORT in use?)"; exit 1; }

# One python helper runs every case; each case exits 0 on success. Shared helpers come first.
run_case() {
    local name="$1" desc="$2"
    python3 -I - "$PORT" "$name" "$STATIC_DIR" <<'PY'
import os, socket, sys, time

port, case, static_dir = int(sys.argv[1]), sys.argv[2], sys.argv[3]
WWW = os.path.join(static_dir, "www")
SECRET = b"TOP-SECRET"

def file_bytes(rel):
    with open(os.path.join(WWW, rel), "rb") as f:
        return f.read()

def req(method, target, extra=b""):
    if isinstance(target, str):
        target = target.encode("latin-1")
    body = b"Content-Length: 0\r\n" if method == b"POST" else b""
    return method + b" " + target + b" HTTP/1.1\r\nHost: x\r\n" + body + extra + b"\r\n"
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
    reqs = b"".join(b"GET /?i=%d HTTP/1.1\r\nHost: x\r\n\r\n" % i for i in range(5))
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

# ---- WEB-8: Content-Length bodies ----
elif case == "body":
    s = conn(); s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello")
    st, h, body, rest, line = read_response(s)
    check(st == 200 and rest == b"", "POST with body: %r" % line)
    check(h.get("connection", "").lower() != "close", "POST response closes an HTTP/1.1 keep-alive connection")
    s.sendall(HOST_REQ)
    st2, *_ = read_response(s)
    check(st2 == 200, "request after a POST got %d (body leaked into the next request?)" % st2)
    check(still_open(s), "connection closed after a POST")

elif case == "body-zero":
    s = conn()
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")
    st, *_ = read_response(s)
    check(st == 200, "POST with Content-Length: 0 got %d" % st)
    s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")
    st, *_ = read_response(s)
    check(st == 200, "GET with Content-Length: 0 got %d" % st)
    # GET with a body: accepted, the body is consumed so the next request is framed correctly.
    s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nabcd" + HOST_REQ)
    buf = b""
    for i in range(2):
        st, _, _, buf, _ = read_response(s, buf)
        check(st == 200, "GET-with-body sequence, response %d got %d" % (i, st))

elif case == "body-pipelined":
    post = b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
    s = conn(); s.sendall(post + HOST_REQ)
    buf = b""
    for i in range(2):
        st, _, _, buf, _ = read_response(s, buf)
        check(st == 200, "pipelined POST+GET, response %d got %d" % (i, st))
    check(buf == b"", "extra bytes after 2 responses: %r" % buf)
    s.settimeout(0.4)
    try:
        check(s.recv(1) == b"", "a 3rd response arrived for 2 requests")
    except socket.timeout:
        pass
    # Body that looks like a request: must be treated as data, not as a second request.
    inner = HOST_REQ
    s = conn()
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(inner) + inner)
    st, _, _, buf, _ = read_response(s)
    check(st == 200, "POST with a request-shaped body got %d" % st)
    s.settimeout(0.4)
    try:
        extra = s.recv(1)
        check(extra == b"", "request-shaped body was executed as a 2nd request (smuggling)")
    except socket.timeout:
        pass

elif case == "body-split":
    s = conn(); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n")
    time.sleep(0.05)
    s.settimeout(0.3)
    try:
        d = s.recv(1)
        check(False, "server answered before the body arrived: %r" % d)
    except socket.timeout:
        pass
    for b in b"0123456789":
        s.sendall(bytes([b])); time.sleep(0.005)
    st, h, body, rest, _ = read_response(s)
    check(st == 200, "split body status %d" % st)
    s.settimeout(0.4)
    try:
        extra = s.recv(1)
        check(extra == b"", "unexpected extra bytes after the single response: %r" % extra)
    except socket.timeout:
        pass
    # Head itself split in the middle of the Content-Length line, then the body.
    s = conn(); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    for chunk in (b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Le", b"ngth: 3\r\n\r\nab", b"c"):
        s.sendall(chunk); time.sleep(0.05)
    st, *_ = read_response(s)
    check(st == 200, "head split mid-header got %d" % st)

elif case == "body-limit":
    n = 1 << 20
    s = conn()
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % n + b"a" * n)
    st, *_ = read_response(s)
    check(st == 200, "body of exactly 1 MiB got %d" % st)
    s.sendall(HOST_REQ)
    st, *_ = read_response(s)
    check(st == 200, "request after a 1 MiB body got %d" % st)

elif case == "body-too-large":
    n = (1 << 20) + 1
    s = conn()
    # Head only: the 413 must arrive without a single body byte being sent.
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % n)
    st, h, *_ = read_response(s)
    check(st == 413, "1 MiB + 1 declared -> %d, wanted 413" % st)
    check(h.get("connection", "").lower() == "close", "413 lacks 'Connection: close'")
    check(closed_by_server(s), "connection stayed open after the 413")
    s = conn()
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999\r\n\r\n")
    st, *_ = read_response(s)
    check(st == 413, "999999999 declared -> %d, wanted 413" % st)

elif case == "body-bad-length":
    for v in (b"-1", b"+5", b"5 5", b"abc", b"", b"99999999999999999999", b"0x10", b"1.5"):
        s = conn()
        s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: " + v + b"\r\n\r\nhello")
        try:
            st, h, *_ = read_response(s)
        except Exception as e:
            check(False, "Content-Length %r -> no response (%s)" % (v, e))
        check(st == 400, "Content-Length %r -> %d, wanted 400" % (v, st))
        check(closed_by_server(s), "Content-Length %r -> connection stayed open" % v)
    s = conn()   # duplicate Content-Length
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello")
    st, *_ = read_response(s)
    check(st == 400, "duplicate Content-Length -> %d, wanted 400" % st)

elif case == "body-te":
    s = conn()
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n")
    st, h, *_ = read_response(s)
    check(st == 501, "Transfer-Encoding: chunked -> %d, wanted 501" % st)
    check(closed_by_server(s), "connection stayed open after the 501")
    s = conn()   # TE + CL together is the classic smuggling shape
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
    st, *_ = read_response(s)
    check(st == 501, "Content-Length + Transfer-Encoding -> %d, wanted 501" % st)

elif case == "body-survival":
    s = conn(); s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nok")
    st, *_ = read_response(s)
    check(st == 200, "server no longer answers POST: %d" % st)

# ---- WEB-10: router ----
elif case == "route-get":
    s = conn(); s.sendall(HOST_REQ)
    st, h, body, rest, _ = read_response(s)
    check(st == 200 and body == b"Hello, World!", "GET / -> %d %r" % (st, body))

elif case == "route-query":
    s = conn(); s.sendall(b"GET /?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n")
    st, _, body, rest, _ = read_response(s)
    check(st == 200 and body == b"Hello, World!", "GET /?a=1&b=2 -> %d %r" % (st, body))
    s.sendall(b"GET /? HTTP/1.1\r\nHost: x\r\n\r\n")
    st, *_ = read_response(s)
    check(st == 200, "GET /? (empty query) -> %d" % st)

elif case == "route-404":
    s = conn()
    for path in (b"/nope", b"/echo/", b"/ECHO", b"//", b"/index.html", b"/nope?x=1"):
        s.sendall(b"GET " + path + b" HTTP/1.1\r\nHost: x\r\n\r\n")
        st, h, body, rest, _ = read_response(s)
        check(st == 404, "GET %r -> %d, wanted 404" % (path, st))
        check(h.get("connection", "").lower() != "close", "404 for %r says 'Connection: close' on a keep-alive request" % path)
    check(still_open(s), "connection closed after 404s on a keep-alive request")
    s.sendall(HOST_REQ)
    st, *_ = read_response(s)
    check(st == 200, "request after the 404s got %d" % st)

elif case == "route-405":
    s = conn()
    s.sendall(b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi")
    st, h, body, rest, _ = read_response(s)
    check(st == 405, "POST / -> %d, wanted 405" % st)
    allow = [m.strip() for m in h.get("allow", "").split(",") if m.strip()]
    check(sorted(allow) == ["GET", "HEAD"], "Allow on / is %r, wanted GET, HEAD" % h.get("allow"))
    check(h.get("connection", "").lower() != "close", "405 closes a keep-alive connection")
    s.sendall(b"GET /echo HTTP/1.1\r\nHost: x\r\n\r\n")
    st, h, *_ = read_response(s)
    check(st == 405, "GET /echo -> %d, wanted 405" % st)
    check(h.get("allow", "").strip() == "POST", "Allow on /echo is %r, wanted POST" % h.get("allow"))
    s.sendall(HOST_REQ)
    st, *_ = read_response(s)
    check(st == 200, "request after the 405s got %d (the POST body leaked?)" % st)

elif case == "route-head":
    s = conn()
    s.sendall(HOST_REQ)
    _, hg, *_ = read_response(s)
    s.sendall(b"HEAD / HTTP/1.1\r\nHost: x\r\n\r\n")
    st, hh, body, rest, line = read_response(s, head_only=True)
    check(st == 200, "HEAD / -> %r" % line)
    check(hh.get("content-length") == hg.get("content-length"), "HEAD Content-Length %r != GET %r" % (hh.get("content-length"), hg.get("content-length")))
    check(rest == b"", "HEAD sent body bytes: %r" % rest)
    s.sendall(b"HEAD /echo HTTP/1.1\r\nHost: x\r\n\r\n")
    st, hh, body, rest, line = read_response(s, head_only=True)
    check(st == 405, "HEAD /echo -> %r, wanted 405" % line)
    s.sendall(b"HEAD /nope HTTP/1.1\r\nHost: x\r\n\r\n")
    st, hh, body, rest, line = read_response(s, head_only=True)
    check(st == 404, "HEAD /nope -> %r, wanted 404" % line)
    s.sendall(HOST_REQ)
    st, *_ = read_response(s)
    check(st == 200, "request after HEAD responses got %d (stray body bytes?)" % st)

elif case == "route-echo":
    payload = bytes(range(256)) * 4 + b"\r\n\r\nGET / HTTP/1.1\r\n"
    s = conn()
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(payload) + payload)
    st, h, body, rest, _ = read_response(s)
    check(st == 200 and body == payload, "echo mismatch: status %d, %d bytes back, %d sent" % (st, len(body), len(payload)))
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")
    st, h, body, rest, _ = read_response(s)
    check(st == 200 and body == b"" and h.get("content-length") == "0", "empty echo: %d %r %r" % (st, body, h))
    s.sendall(b"POST /echo?x=1 HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc")
    st, h, body, rest, _ = read_response(s)
    check(st == 200 and body == b"abc", "echo with query: %d %r" % (st, body))

elif case == "route-close":
    for raw, want in ((b"GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 404),
                      (b"POST / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", 405),
                      (b"GET /nope HTTP/1.0\r\n\r\n", 404)):
        s = conn(); s.sendall(raw)
        st, h, *_ = read_response(s)
        check(st == want, "%r -> %d, wanted %d" % (raw[:20], st, want))
        check(h.get("connection", "").lower() == "close", "%d for %r lacks 'Connection: close'" % (st, raw[:20]))
        check(closed_by_server(s), "connection stayed open after %d for %r" % (st, raw[:20]))

elif case == "static-get":
    s = conn(); s.sendall(req(b"GET", "/static/index.html"))
    st, h, body, rest, line = read_response(s)
    check(st == 200, "GET /static/index.html -> %r" % line)
    check(body == file_bytes("index.html"), "body differs from the file: %r" % body[:80])
    check(h.get("content-type", "").startswith("text/html"), "Content-Type: %r" % h.get("content-type"))
    check(h.get("connection", "").lower() != "close", "static 200 closes a keep-alive connection")
    s.sendall(req(b"GET", "/static/sub/page.txt"))
    st, h, body, rest, line = read_response(s)
    check(st == 200 and body == file_bytes("sub/page.txt"), "second request on the connection: %r %r" % (line, body))
    s.sendall(req(b"GET", "/static/index.html?v=2"))
    st, h, body, *_ = read_response(s)
    check(st == 200 and body == file_bytes("index.html"), "query string not ignored: %d" % st)
    s.sendall(req(b"GET", "/static/alias.html"))
    st, h, body, *_ = read_response(s)
    check(st == 200 and body == file_bytes("index.html"), "relative symlink inside the root: %d" % st)

elif case == "static-head":
    s = conn(); s.sendall(req(b"GET", "/static/style.css") + req(b"HEAD", "/static/style.css"))
    st, hg, body, rest, _ = read_response(s)
    st2, hh, body2, rest2, line = read_response(s, rest, head_only=True)
    check(st2 == 200, "HEAD -> %r" % line)
    check(hh.get("content-length") == hg.get("content-length") == str(len(file_bytes("style.css"))),
          "HEAD Content-Length %r, GET %r" % (hh.get("content-length"), hg.get("content-length")))
    check(hh.get("content-type") == hg.get("content-type"), "HEAD Content-Type differs from GET")
    check(rest2 == b"", "HEAD sent body bytes: %r" % rest2)
    s.sendall(HOST_REQ)
    st3, *_ = read_response(s)
    check(st3 == 200, "request after HEAD got %d (stray body bytes?)" % st3)

elif case == "static-mime":
    s = conn()
    for target, want in (("/static/style.css", "text/css"), ("/static/sub/page.txt", "text/plain"),
                         ("/static/LOGO.PNG", "image/png"), ("/static/data.unknownext", "application/octet-stream"),
                         ("/static/blob.bin", "application/octet-stream")):
        s.sendall(req(b"GET", target))
        st, h, body, *_ = read_response(s)
        check(st == 200, "%s -> %d" % (target, st))
        check(h.get("content-type", "").split(";")[0].strip() == want, "%s Content-Type %r, wanted %s" % (target, h.get("content-type"), want))

elif case == "static-binary":
    want = file_bytes("blob.bin")
    s = conn(); s.sendall(req(b"GET", "/static/blob.bin"))
    st, h, body, *_ = read_response(s)
    check(st == 200 and int(h.get("content-length", -1)) == len(want), "status %d, Content-Length %r, file %d" % (st, h.get("content-length"), len(want)))
    check(body == want, "binary body differs (%d bytes back)" % len(body))

elif case == "static-404":
    s = conn()
    for target in ("/static/missing.txt", "/static/sub/missing.txt", "/static/", "/static/sub", "/static/sub/",
                   "/static", "/static/index.html/x"):
        s.sendall(req(b"GET", target))
        st, h, body, rest, line = read_response(s)
        check(st == 404, "%s -> %r, wanted 404" % (target, line))
        check(h.get("connection", "").lower() != "close", "404 for %s closed a keep-alive connection" % target)
    check(still_open(s), "connection closed after static 404s")

elif case == "static-traversal":
    abs_secret = os.path.join(static_dir, "secret.txt")
    targets = [b"/static/../secret.txt", b"/static/sub/../../secret.txt", b"/static/..",
               b"/static/../www-evil/secret.txt", b"/static/%2e%2e/secret.txt", b"/static/%2E%2E/secret.txt",
               b"/static/..%2fsecret.txt", b"/static/%2e%2e%2fsecret.txt", b"/static/.%2e/secret.txt",
               b"/static/" + abs_secret.encode(), b"/static/%2F" + abs_secret.lstrip("/").encode(),
               b"/static/..\\secret.txt", b"/static/..%5csecret.txt", b"/static/index.html%00.txt",
               b"/static/leak.txt", b"/static/up/secret.txt", b"/static/evil.txt", b"/static/passwd",
               b"/static/leak-rel.txt", b"/static/up-rel/secret.txt", b"/static/up-rel/www/index.html",
               b"/static/.git/config", b"/static/.env", b"/static/%2egit/config", b"/static/./index.html"]
    for t in targets:
        s = conn(); s.sendall(req(b"GET", t))
        try:
            st, h, body, rest, line = read_response(s)
        except (EOFError, ConnectionResetError):
            continue  # closed without a response: nothing leaked
        check(st != 200, "%r was served (200)" % t)
        check(400 <= st < 500, "%r -> %r, wanted a 4xx" % (t, line))
        check(SECRET not in body and b"root:" not in body, "%r leaked file content" % t)
    s = conn(); s.sendall(HOST_REQ)
    st, *_ = read_response(s)
    check(st == 200, "server unhealthy after traversal attempts: %d" % st)

elif case == "static-405":
    s = conn(); s.sendall(req(b"POST", "/static/index.html"))
    st, h, body, rest, line = read_response(s)
    check(st == 405, "POST /static/index.html -> %r" % line)
    check(h.get("allow", "").replace(" ", "") == "GET,HEAD", "Allow: %r" % h.get("allow"))
    check(still_open(s), "405 under the mount closed a keep-alive connection")

elif case == "static-pipelined":
    s = conn(); s.sendall(req(b"GET", "/static/index.html") + HOST_REQ + req(b"GET", "/static/sub/page.txt"))
    st1, _, b1, rest, _ = read_response(s)
    st2, _, b2, rest, _ = read_response(s, rest)
    st3, _, b3, rest, _ = read_response(s, rest)
    check((st1, st2, st3) == (200, 200, 200), "statuses %r" % ((st1, st2, st3),))
    check(b1 == file_bytes("index.html") and b2 == b"Hello, World!" and b3 == file_bytes("sub/page.txt"),
          "out of order or wrong bodies: %r %r %r" % (b1[:20], b2[:20], b3[:20]))

elif case == "static-close":
    s = conn(); s.sendall(req(b"GET", "/static/index.html", b"Connection: close\r\n"))
    st, h, body, *_ = read_response(s)
    check(st == 200 and body == file_bytes("index.html"), "status %d" % st)
    check(h.get("connection", "").lower() == "close", "no 'Connection: close' on the static 200")
    check(closed_by_server(s), "connection stayed open after Connection: close")

elif case == "static-survival" or case == "route-survival":
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

# WEB-8: Content-Length bodies.
run_case body              "POST with a body: 200, keep-alive, next request framed correctly"
run_case body-zero         "Content-Length: 0 and GET-with-body keep framing intact"
run_case body-pipelined    "POST + GET in one send -> two 200s; request-shaped body is data"
run_case body-split        "body byte by byte (and a split head) -> exactly one response"
run_case body-limit        "a body of exactly 1 MiB is accepted"
run_case body-too-large    "over the limit -> 413 + close before any body byte is sent"
run_case body-bad-length   "-1, +5, '5 5', abc, empty, overflow, duplicate -> 400 + close"
run_case body-te           "Transfer-Encoding (alone or with Content-Length) -> 501 + close"
run_case body-survival     "server still serving after the body cases"

# WEB-10: router.
run_case route-get       "GET / -> the handler body"
run_case route-query     "query string ignored when matching"
run_case route-404       "unknown paths -> 404, keep-alive preserved"
run_case route-405       "wrong method -> 405 + Allow, keep-alive preserved"
run_case route-head      "HEAD falls back to GET; HEAD on POST-only path -> 405"
run_case route-echo      "POST /echo returns the body byte for byte"
run_case route-close     "404/405 honor Connection: close and HTTP/1.0"
run_case route-survival  "server still serving after the routing cases"

# WEB-11: static files.
run_case static-get        "GET /static/index.html -> byte-exact file, text/html, keep-alive"
run_case static-head       "HEAD on a static file: same headers, no body"
run_case static-mime       "Content-Type per extension, case-insensitive, octet-stream fallback"
run_case static-binary     "300 KB binary file arrives byte for byte"
run_case static-404        "missing files and directories -> 404, keep-alive preserved"
run_case static-traversal  "traversal and escaping symlinks never serve outside the root"
run_case static-405        "POST under the mount -> 405 + Allow: GET, HEAD"
run_case static-pipelined  "static and dynamic responses pipelined in order"
run_case static-close      "Connection: close honored on a static file"
run_case static-survival   "server still serving after the static cases"

# Cross-check with curl when available (the ticket's acceptance commands).
if command -v curl >/dev/null; then
    OUT="$(curl -s -i "http://127.0.0.1:$PORT/")"
    [[ "$OUT" == HTTP/1.1\ 200\ OK* ]] && pass "curl GET" || fail "curl GET: $OUT"
    HEADS="$(curl -s -I "http://127.0.0.1:$PORT/")"
    [[ "$HEADS" == HTTP/1.1\ 200\ OK* ]] && pass "curl -I" || fail "curl -I: $HEADS"
    REUSE="$(curl -sv "http://127.0.0.1:$PORT/" "http://127.0.0.1:$PORT/" 2>&1 >/dev/null | grep -ci 'Re-\?using existing')"
    [[ "$REUSE" -ge 1 ]] && pass "curl reuses the connection" || fail "curl did not reuse the connection"
    POST_OUT="$(curl -s -i -d 'hello' "http://127.0.0.1:$PORT/echo")"
    [[ "$POST_OUT" == HTTP/1.1\ 200\ OK* ]] && pass "curl -d POST" || fail "curl -d POST: $POST_OUT"
    POST_REUSE="$(curl -sv -d 'hello' "http://127.0.0.1:$PORT/echo" --next -d 'world' "http://127.0.0.1:$PORT/echo" 2>&1 >/dev/null | grep -ci 'Re-\?using existing')"
    [[ "$POST_REUSE" -ge 1 ]] && pass "curl POST reuses the connection" || fail "curl POST did not reuse the connection"
    [[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/nope")" == 404 ]] && pass "curl /nope -> 404" || fail "curl /nope"
    [[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/")" == 405 ]] && pass "curl -X POST / -> 405" || fail "curl -X POST /"
    curl -s -i -X POST "http://127.0.0.1:$PORT/" | grep -qi '^Allow: GET, HEAD' && pass "curl 405 carries Allow: GET, HEAD" || fail "curl 405 lacks Allow: GET, HEAD"
    [[ "$(curl -s -d hi "http://127.0.0.1:$PORT/echo")" == hi ]] && pass "curl -d hi /echo -> hi" || fail "curl /echo"
    [[ "$(curl -s "http://127.0.0.1:$PORT/?a=1")" == "Hello, World!" ]] && pass "curl /?a=1 matches /" || fail "curl /?a=1"
    BIG_STATUS="$(head -c 2000000 /dev/zero | curl -s -o /dev/null -w '%{http_code}' --data-binary @- "http://127.0.0.1:$PORT/echo")"
    [[ "$BIG_STATUS" == 413 ]] && pass "curl 2 MB POST -> 413" || fail "curl 2 MB POST -> $BIG_STATUS (wanted 413)"
    # WEB-11 acceptance commands.
    STATIC_OUT="$(mktemp)"
    curl -s -o "$STATIC_OUT" "http://127.0.0.1:$PORT/static/index.html"
    cmp -s "$STATIC_OUT" "$WWW/index.html" && pass "curl /static/index.html matches the file" || fail "curl /static/index.html differs from the file"
    rm -f "$STATIC_OUT"
    curl -s -I "http://127.0.0.1:$PORT/static/style.css" | grep -qi '^Content-Type: text/css' && pass "curl -I style.css -> text/css" || fail "curl -I style.css lacks text/css"
    [[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/static/missing.txt")" == 404 ]] && pass "curl missing static -> 404" || fail "curl missing static"
    TRAV="$(curl -s --path-as-is -w '\n%{http_code}' "http://127.0.0.1:$PORT/static/../secret.txt")"
    [[ "$TRAV" != *TOP-SECRET* && "${TRAV##*$'\n'}" != 200 ]] && pass "curl --path-as-is traversal blocked" || fail "curl --path-as-is traversal: $TRAV"
    LEAK="$(curl -s -w '\n%{http_code}' "http://127.0.0.1:$PORT/static/leak.txt")"
    [[ "$LEAK" != *TOP-SECRET* && "${LEAK##*$'\n'}" == 404 ]] && pass "curl symlink escape -> 404" || fail "curl symlink escape: $LEAK"
    [[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/static/index.html")" == 405 ]] && pass "curl POST static -> 405" || fail "curl POST static"
else
    echo "SKIP: curl not installed"
fi

echo
if [[ $FAILS -eq 0 ]]; then echo "All HTTP checks passed."; exit 0; fi
echo "$FAILS check(s) failed."
exit 1
