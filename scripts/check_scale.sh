#!/usr/bin/env bash
# WEB-4 check: configurable connection cap, 10k idle connections, fd exhaustion.
#
# Usage: scripts/check_scale.sh [server-binary] [port]
# Defaults: build/server/server, 8080
#
# Expects the server to accept these flags (see docs/tickets/WEB-4.md):
#   --port N  --max-connections N  --backlog N
#
# Phases:
#   1. cap:     --max-connections 100, 150 clients -> exactly 100 kept, server alive
#   2. scale:   ulimit -n 20000, --max-connections 12000, 10000 idle clients,
#               a fresh client still gets its echo, idle CPU stays low
#   3. EMFILE:  server started with ulimit -n 256, 400 clients -> no crash,
#               no busy loop (CPU sampled over 2 s), existing clients still served
set -u

SERVER_BIN="${1:-build/server/server}"
PORT="${2:-8080}"

fail() {
  echo "FAIL: $*"
  exit 1
}
pass() { echo "PASS: $*"; }

[[ -x "$SERVER_BIN" ]] || fail "server binary not found: $SERVER_BIN (build first)"
command -v ss >/dev/null || fail "'ss' (iproute2) is required"
command -v python3 >/dev/null || fail "'python3' is required"

SERVER_PID=""
HOLDER_PID=""
TMPDIR_CHECK="$(mktemp -d)"

stop_all() {
  [[ -n "$HOLDER_PID" ]] && kill "$HOLDER_PID" 2>/dev/null && wait "$HOLDER_PID" 2>/dev/null
  [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null
  HOLDER_PID=""
  SERVER_PID=""
}
trap 'stop_all; rm -rf "$TMPDIR_CHECK"' EXIT

# start_server <nofile-ulimit> <server args...>
start_server() {
  local nofile="$1"
  shift
  (ulimit -n "$nofile" && exec "$SERVER_BIN" --port "$PORT" "$@") >/dev/null 2>&1 &
  SERVER_PID=$!
  sleep 0.5
  kill -0 "$SERVER_PID" 2>/dev/null ||
    fail "server did not start with args '$*' (port $PORT in use, or flags not implemented?)"
}

server_alive() { kill -0 "$SERVER_PID" 2>/dev/null; }

server_conns() { ss -Htn state established "( sport = :$PORT )" | wc -l; }

# open_clients <count>: python holder opens <count> idle connections and keeps
# them open until killed. It writes the number that connected to $TMPDIR_CHECK/opened.
open_clients() {
  local count="$1"
  rm -f "$TMPDIR_CHECK/opened"
  python3 -I - "$count" "$PORT" "$TMPDIR_CHECK/opened" <<'PY' &
import resource, signal, socket, sys, time
count, port, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, count + 64), hard))
socks = []
for _ in range(count):
    s = socket.socket()
    try:
        s.connect(("127.0.0.1", port))
    except OSError:
        break
    socks.append(s)
with open(out, "w") as f:
    f.write(str(len(socks)))
signal.pause()
PY
  HOLDER_PID=$!
  for _ in $(seq 100); do
    [[ -s "$TMPDIR_CHECK/opened" ]] && return 0
    sleep 0.2
  done
  fail "client holder did not finish connecting"
}

close_clients() {
  [[ -n "$HOLDER_PID" ]] && kill "$HOLDER_PID" 2>/dev/null && wait "$HOLDER_PID" 2>/dev/null
  HOLDER_PID=$!
  for _ in $(seq 100); do
    [[ -s "$TMPDIR_CHECK/opened" ]] && return 0
    sleep 0.2
  done
  fail "client holder did not finish connecting"
}

# echo_ok: a fresh client sends one frame and must get it back.
echo_ok() {
  python3 -I - "$PORT" <<'PY'
import socket, sys
data = b""
try:
    s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=3)
    s.sendall(b"ping\0")
    while not data.endswith(b"\0"):
        chunk = s.recv(64)
        if not chunk:
            break
        data += chunk
except OSError:
    pass
    python3 -I - "$PORT" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=3)
s.sendall(b"ping\0")
data = b""
while not data.endswith(b"\0"):
    chunk = s.recv(64)
    if not chunk:
        break
    data += chunk
sys.exit(0 if data == b"ping\0" else 1)
PY
}

# cpu_percent <seconds>: average CPU% of the server over the window.
cpu_percent() {
  local secs="$1" tck t0 t1
  tck=$(getconf CLK_TCK)
  t0=$(awk '{print $14 + $15}' "/proc/$SERVER_PID/stat")
  sleep "$secs"
  t1=$(awk '{print $14 + $15}' "/proc/$SERVER_PID/stat")
  echo $(((t1 - t0) * 100 / (tck * secs)))
}

# ---------------------------------------------------------------- phase 1
echo "[1/3] cap: --max-connections 100, 150 clients"
start_server 4096 --max-connections 100
open_clients 150
sleep 1
server_alive || fail "server crashed with 150 clients and a cap of 100"
n=$(server_conns)
((n == 100)) || fail "expected 100 established connections, got $n"
pass "exactly 100 connections kept, the rest shed, server alive"
echo_ok && fail "a client got through above the cap" || true
close_clients
sleep 1
echo_ok || fail "no echo after the clients left"
pass "slots freed after clients left"
stop_all

# ---------------------------------------------------------------- phase 2
echo "[2/3] scale: 10000 idle keep-alive connections"
hard=$(ulimit -Hn)
if [[ "$hard" != "unlimited" ]] && ((hard < 20000)); then
  echo "SKIP: hard fd limit $hard < 20000"
else
  start_server 20000 --max-connections 12000 --backlog 4096
  open_clients 10000
  opened=$(<"$TMPDIR_CHECK/opened")
  ((opened == 10000)) || fail "client side only opened $opened of 10000 (backlog/somaxconn?)"
  sleep 2
  server_alive || fail "server crashed holding 10000 connections"
  n=$(server_conns)
  ((n == 10000)) || fail "expected 10000 established connections, got $n"
  pass "10000 connections held, server alive"
  echo_ok || fail "no echo for a fresh client while 10000 connections are open"
  pass "fresh client still served"
  cpu=$(cpu_percent 2)
  ((cpu < 10)) || fail "idle server uses ${cpu}% CPU with 10000 idle connections"
  pass "idle CPU ${cpu}%"
  close_clients
  stop_all
fi

# ---------------------------------------------------------------- phase 3
echo "[3/3] EMFILE: server limited to 256 fds, 400 clients"
start_server 256 --max-connections 10000
open_clients 400
sleep 1
server_alive || fail "server crashed when it ran out of fds"
pass "server survived fd exhaustion"
cpu=$(cpu_percent 2)
((cpu < 50)) || fail "server spins at ${cpu}% CPU after EMFILE (level-triggered listener busy loop)"
pass "no busy loop (CPU ${cpu}%)"
close_clients
sleep 1
echo_ok || fail "server does not accept new clients after fds were freed"
pass "server recovers once fds are freed"
stop_all

echo "All checks passed."
