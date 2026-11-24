#!/usr/bin/env bash
# WEB-9 check: strict command-line argument validation.
#
# Cases:
#   1. rejected   bad --port / --max-connections / --backlog values exit 1 within 2 s,
#                 print something on stderr and name the offending option
#   2. range      out-of-range messages show the allowed range (--port: 1-65535)
#   3. incomplete unknown flag and a flag with no value exit 1
#   4. accepted   valid values start the server, which answers curl; the printed port is the bound port
#   5. root       (WEB-11) --root rejects a missing path, a regular file, an empty value, a dangling
#                 symlink and a missing value, each with the reason on stderr; a valid directory starts
#
# Usage: scripts/check_args.sh [server-binary] [port]
# Defaults: build/server/server, 8080
set -u

SERVER_BIN="${1:-build/server/server}"
PORT="${2:-8080}"
FAILS=0

fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
pass() { echo "PASS: $*"; }

[[ -x "$SERVER_BIN" ]] || { echo "FAIL: server binary not found: $SERVER_BIN (build first)"; exit 1; }

ERR="$(mktemp)"
LOG="$(mktemp)"
SERVER_PID=""
ROOT_TMP="$(mktemp -d)"
trap '[[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null; rm -f "$ERR" "$LOG"; rm -rf "$ROOT_TMP"' EXIT

# expect_reject <option-word> <args...>: server must exit 1 on its own and complain about <option-word>.
expect_reject() {
    local word="$1"; shift
    timeout 2 "$SERVER_BIN" "$@" >"$LOG" 2>"$ERR"
    local code=$?
    local label="[$*]"
    if [[ $code -eq 124 ]]; then
        fail "$label server kept running instead of rejecting the argument"
    elif [[ $code -ne 1 ]]; then
        fail "$label exit code $code, expected 1"
    elif [[ ! -s "$ERR" ]]; then
        fail "$label exit 1 but nothing on stderr"
    elif ! grep -qi -- "$word" "$ERR"; then
        fail "$label error does not name the option ('$word'): $(head -c 160 "$ERR" | tr '\n' ' ')"
    else
        pass "$label rejected: $(head -1 "$ERR")"
    fi
}

# 1. rejected values, each message must name the option
for v in 70000 65536 0 -1 8080abc "" abc " 8080" 99999999999999999999; do
    expect_reject port --port "$v" --max-connections 10
done
for v in 0 -1 10x "" 99999999999999999999; do
    expect_reject connections --port "$PORT" --max-connections "$v"
done
for v in 0 -1 abc ""; do
    expect_reject backlog --port "$PORT" --max-connections 10 --backlog "$v"
done

# expect_reject_reason <reason-regex> <args...>: like expect_reject on "root", and stderr must also
# carry the OS reason (strerror text), not just "invalid".
expect_reject_reason() {
    local reason="$1"; shift
    expect_reject root "$@"
    if grep -qiE -- "$reason" "$ERR"; then pass "[$*] reason present ($reason)"
    else fail "[$*] reason missing, expected /$reason/: $(head -c 160 "$ERR" | tr '\n' ' ')"; fi
}

# 2. the --port message shows the allowed range
timeout 2 "$SERVER_BIN" --port 70000 --max-connections 10 >"$LOG" 2>"$ERR"
if grep -q "65535" "$ERR"; then pass "range: --port 70000 message shows the upper bound"
else fail "range: --port message does not show 65535: $(head -c 160 "$ERR")"; fi

# 3. unknown flag, missing value
expect_reject bogus --bogus
expect_reject port --port

# 4. valid values start the server
"$SERVER_BIN" --port "$PORT" --max-connections 1000 --backlog 128 >"$LOG" 2>"$ERR" &
SERVER_PID=$!
sleep 0.5
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    fail "accepted: server did not start with valid arguments: $(head -c 160 "$ERR")"
else
    if grep -q "port $PORT" "$LOG"; then pass "accepted: startup line prints port $PORT"
    else fail "accepted: startup line does not mention port $PORT: $(head -c 160 "$LOG")"; fi
    if command -v ss >/dev/null && ss -ltn "sport = :$PORT" | grep -q LISTEN; then
        pass "accepted: kernel is listening on $PORT"
    else
        fail "accepted: nothing listening on $PORT"
    fi
    if command -v curl >/dev/null; then
        code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$PORT/")"
        [[ "$code" == "200" ]] && pass "accepted: curl -> 200" || fail "accepted: curl returned '$code'"
    fi
fi

# 5. --root (WEB-11)
kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; SERVER_PID=""
mkdir -p "$ROOT_TMP/www"
echo hi >"$ROOT_TMP/file.txt"
ln -s "$ROOT_TMP/gone" "$ROOT_TMP/dangling"
expect_reject_reason "no such file" --port "$PORT" --max-connections 10 --root "$ROOT_TMP/missing"
expect_reject_reason "not a directory" --port "$PORT" --max-connections 10 --root "$ROOT_TMP/file.txt"
expect_reject_reason "invalid" --port "$PORT" --max-connections 10 --root ""
expect_reject_reason "no such file" --port "$PORT" --max-connections 10 --root "$ROOT_TMP/dangling"
expect_reject root --port "$PORT" --max-connections 10 --root

"$SERVER_BIN" --port "$PORT" --max-connections 10 --root "$ROOT_TMP/www" >"$LOG" 2>"$ERR" &
SERVER_PID=$!
sleep 0.5
if kill -0 "$SERVER_PID" 2>/dev/null; then pass "root: valid directory starts the server"
else fail "root: server did not start with --root $ROOT_TMP/www: $(head -c 160 "$ERR")"; fi

echo
if [[ $FAILS -eq 0 ]]; then echo "ALL CHECKS PASSED"; else echo "$FAILS CHECK(S) FAILED"; exit 1; fi
