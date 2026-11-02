#!/usr/bin/env bash
# WEB-1 check: the server must survive more simultaneous clients than its limit.
#
# Usage: scripts/check_connection_limit.sh [server-binary] [limit] [attempts] [port]
# Defaults: build/server/server, 10, 15, 8080
set -u

SERVER_BIN="${1:-build/server/server}"
LIMIT="${2:-10}"
ATTEMPTS="${3:-15}"
PORT="${4:-8080}"

fail() { echo "FAIL: $*"; exit 1; }
pass() { echo "PASS: $*"; }

[[ -x "$SERVER_BIN" ]] || fail "server binary not found: $SERVER_BIN (build first)"
command -v ss >/dev/null || fail "'ss' (iproute2) is required"

"$SERVER_BIN" >/dev/null 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null' EXIT
sleep 0.5
kill -0 "$SERVER_PID" 2>/dev/null || fail "server did not start (port $PORT in use?)"

# Established connections as seen by the server side of the socket.
server_conns() { ss -Htn state established "( sport = :$PORT )" | wc -l; }

declare -a FDS=()
open_client() {
    local fd
    exec {fd}<>"/dev/tcp/127.0.0.1/$PORT" 2>/dev/null || return 1
    FDS+=("$fd")
}

echo "Opening $ATTEMPTS simultaneous connections (limit $LIMIT)..."
for _ in $(seq "$ATTEMPTS"); do open_client; done
sleep 1

kill -0 "$SERVER_PID" 2>/dev/null || fail "server crashed while handling $ATTEMPTS connections"
pass "server survived $ATTEMPTS connections"

n=$(server_conns)
(( n == LIMIT )) || fail "expected $LIMIT established connections, got $n"
pass "exactly $LIMIT connections kept, the rest were shed"

# Free 5 slots, then confirm the server accepts new clients again.
for _ in 1 2 3 4 5; do
    exec {FDS[0]}>&-
    FDS=("${FDS[@]:1}")
done
sleep 1
for _ in 1 2 3; do open_client; done
sleep 1

kill -0 "$SERVER_PID" 2>/dev/null || fail "server died after clients disconnected"
n=$(server_conns)
(( n == LIMIT - 5 + 3 )) || fail "after recovery expected $((LIMIT - 5 + 3)) connections, got $n"
pass "server accepts new clients after others disconnect"

echo "All checks passed."
