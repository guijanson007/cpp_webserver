## WEB-9: Validate command-line arguments and remove leftover cruft

**Type:** Bug
**Milestone:** M2/M3 bridge (cleanup ahead of M3)
**Priority:** Medium (`--port 70000` binds 4464 while printing 70000, and `--max-connections -1` starts a server that sheds every client)
**Estimate:** 1.5-2 h
**Mode:** Mentor for `parseArguments` (the user writes it). Pair for the tests and the check script (Claude may write them).

### Description
`parseArguments` in `server/src/main.cpp` uses `std::stoi`, which accepts trailing garbage (`8080abc`) and has no range checks. Out-of-range values silently wrap or misbehave. This ticket makes bad input fail at startup with a clear message and exit code 1. It also removes two small leftovers listed in the CLAUDE.md known-issues backlog.

### Scope
- In:
  - Strict parsing of `--port` (1-65535), `--max-connections` (>= 1) and `--backlog` (>= 1).
  - Reject trailing garbage, empty strings, signs where they make no sense, and overflow.
  - Remove the `<asm-generic/socket.h>` include.
  - Remove `HttpMethod::DELETE`, and any use of it in tests.
- Out (explicitly):
  - `[[nodiscard]]` on `Result` and project-wide warning flags. These go in a follow-up ticket because they will touch many call sites.
  - Moving `accept4` into the wrapper layer.
  - New flags and a `--help` option (README roadmap).

### Acceptance criteria (Definition of Done)
- [x] `build/server/server --port 70000 --max-connections 10` exits with 1 and prints an error that names the argument and the allowed range.
- [x] The same happens for `--port 8080abc`, `--port ""`, `--port -1`, `--max-connections 0`, `--max-connections -1` and `--backlog abc`.
- [x] `--port 8080 --max-connections 1000 --backlog 128` still starts normally.
- [x] Builds clean with no new warnings.
- [x] Tests added, named below.
- [x] No exceptions needed for control flow on the new path. Parse failures return `Result<T, Error>` or similar, and `main` prints the error. (Exceptions at startup are allowed, but the `Result` convention is preferred.)
- [x] No leftover `DELETE` or `asm-generic` references (`grep` returns nothing).
- [x] Small, focused commits, each with `WEB-9` in the message.

### Technical notes / hints
- `std::from_chars` (`<charconv>`) is the same tool the parser already uses for `Content-Length`. It reports both failure and "stopped early" through `ptr` and `ec`. Check that `ptr == end` to reject trailing garbage.
- Parse into a wide enough type before range-checking, or parse straight into a narrow type and let `ec == result_out_of_range` catch overflow. Decide which and be ready to explain it.
- A small generic helper such as `template <std::integral T> Result<T> parseInt(std::string_view, T min, T max)` makes testing easy. Put it somewhere testable, not in `main.cpp`. For example, add `common/include/common/ParseNumber.h` and register its test in the `common_tests` source list.
- Check `ServerConfig` field types. If `port` is an `int`, decide whether to narrow it to `uint16_t` once validated.
- The current `else` branch throws for an incomplete argument such as a trailing `--port` with no value. Keep that behaviour, but with a better message.
- Bug to flag: `network::EpollReactor epollReactor;` is constructed before the `try` in `main`, so its throwing constructor is outside the handler. This is already in the backlog, but it is worth fixing while you are in this function.

### How to verify
```
cmake --build build && ctest --test-dir build --output-on-failure
for a in "--port 70000" "--port 8080abc" "--port -1" "--max-connections 0" "--backlog abc"; do
  build/server/server $a; echo "exit=$?"; done        # all: error message, exit=1
build/server/server --port 8080 --max-connections 1000 &  curl -i localhost:8080/
scripts/check_http.sh
grep -rn "asm-generic\|DELETE" server tests            # no output
```
Tests to add: `ParseNumberTest.{Valid,TrailingGarbage,Empty,Negative,Overflow,BelowMin,AboveMax,Boundaries}`.

### Dependencies
Blocked by: nothing.
Blocks: nothing hard. A warning-flags and `[[nodiscard]]` ticket (WEB-10) and M3 (router + static files) follow.

### Interview talking point
"Parse, don't validate: untrusted input is converted into a typed, range-checked value at the boundary (`from_chars` plus an end-pointer check), so the rest of the program never sees an invalid port."

## Findings
- `parseArguments` returns `Result<ServerConfig, std::string>`; the number parsing lives in
  `common/ParseNumber.h` (`parseInteger<T>(str, min, max)`), so it is unit-testable.
- `scripts/check_args.sh` on a build of `main` (stoi): 19 failures (e.g. `--port 70000` and
  `--port 8080abc` kept the server running). On a draft where the `--port` branch returned the bare
  parser message it caught 9 failures (the message did not name the option); fixed before merge.
- Still open (not part of this ticket): `EpollReactor` is constructed before argument parsing and
  can throw outside any handler.

## Verification (fresh build, `web-9`)
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build   # no warnings
ctest --test-dir build --output-on-failure
  100% tests passed, 0 tests failed out of 142   (1 disabled: KeepAliveTest.RepeatedConnectionHeadersAreCombined)
scripts/check_args.sh              ALL CHECKS PASSED
scripts/check_http.sh              All HTTP checks passed.
scripts/check_connection_limit.sh  All checks passed.
scripts/check_reactor.sh           All executed checks passed.
scripts/check_write_path.sh        All write-path checks passed.
scripts/check_scale.sh             All checks passed.
```
