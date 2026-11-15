# WEB-8: Parse `Content-Length` request bodies and return 413 for oversized ones

**Status:** Done
**Type:** Task
**Milestone:** M2 - HTTP core
**Priority:** High (last core M2 feature; every POST was mis-framed before this)
**Estimate:** 3 h
**Mode:** Mentor for the parser and `Server` changes (user wrote them). Pair for the tests and the check script (Claude wrote them).

## Description
The parser stopped at the blank line after the headers and never framed the body, so a `POST` body was read as the next request. It now reads the `Content-Length` value, waits for the full body (resumable across partial reads), exposes it as `Request::body`, and returns 413 when the declared length is over `SizeLimits::maxBodySize`. This completes the M2 "Content-Length bodies + size limits" item.

## Scope
- **In:**
  - `SizeLimits::maxBodySize` (default 1 MiB).
  - Strict `Content-Length` parsing: digits only, no sign, no inner whitespace, no empty value, overflow is 400 (`std::from_chars`).
  - `Done::request.body` is a view of exactly `Content-Length` bytes after the head. `consumed_bytes` covers head plus body.
  - Incomplete body returns `NeedMore`. A declared length over the limit returns `ContentTooLarge` as soon as the head is parsed, before any body byte arrives.
  - `Transfer-Encoding` present returns 501 (chunked is roadmap-only), including together with `Content-Length`.
  - `Server::clientCallback` checks the `readAllAvailable` result before parsing.
  - `scripts/check_http.sh` body section and curl POST checks.
- **Out (explicitly):** chunked decoding, `Expect: 100-continue`, streaming bodies to disk, router/echo endpoints, body-read timeouts.

## Findings and bugs
- **Mis-framing bug on `main` (fixed):** `Content-Length` was only checked for duplicates. A `POST` with a body left the body bytes in the buffer, which were parsed as a second request (a 400, or worse, a request-shaped body executed). Confirmed by running the new check script against a build of `main`: 9 checks failed there.
- **Result check order (fixed):** `clientCallback` ran the parse loop before checking the `readAllAvailable` result, so an I/O error still parsed and queued responses before closing. The check now comes first.
- **413 and unread body bytes:** the server closes after the 413 with the body unread. A client still sending the body may get an RST and might not see the 413. Draining is out of scope. `curl` with a 2 MB `--data-binary` reported 413 in the check script, but this is timing-dependent.
- **Not changed (backlog):** `buf.erase(0, consumed_bytes)` per pipelined request is still quadratic. With bodies it moves more bytes per request, so it stays on the profiling list. `ParseError` still lists 404/405/500, which the parser never returns.

## Acceptance criteria
- [x] `curl -d 'hello'` returns one 200 and the connection stays open.
- [x] POST then GET pipelined in one `write` returns two 200s, in order.
- [x] Body sent byte by byte (and a head split mid-header) gets exactly one response.
- [x] Declared length over the limit returns 413 + `Connection: close` with only the head sent.
- [x] Bad lengths (`-1`, `+5`, `5 5`, `abc`, empty, overflow, `0x10`, `1.5`, duplicate) return 400 and close.
- [x] `Transfer-Encoding` (alone or with `Content-Length`) returns 501 and closes.
- [x] Server survives all of the above.
- [x] Builds clean, no warnings.
- [x] Parser/request tests added (133 run, 1 disabled, up from 122).
- [x] No raw syscalls outside the wrapper layer. Protocol errors stay `ParseError` values.
- [x] Small commits with `WEB-8` in the message.

## Verification (fresh build, `web-8`)
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build   # no warnings
ctest --test-dir build --output-on-failure
  100% tests passed, 0 tests failed out of 133   (1 disabled: KeepAliveTest.RepeatedConnectionHeadersAreCombined)
scripts/check_http.sh            All HTTP checks passed. (23 checks: 9 head, 9 body, 5 curl incl. POST reuse and 2 MB POST -> 413)
scripts/check_connection_limit.sh  All checks passed.
scripts/check_reactor.sh           All executed checks passed.
scripts/check_write_path.sh        All write-path checks passed.
scripts/check_scale.sh             All checks passed.
```
`scripts/check_http.sh` run against a build of `main` (no body framing): 9 failures, so the new checks do detect the missing feature.

## Dependencies
- Blocked by: WEB-7 (done).
- Blocks: M3 (router and static files need a complete `Request`), later body-size and read-cap profiling.

## Interview talking point
Request smuggling comes from two parsers disagreeing about where a message ends. The body is framed strictly (one `Content-Length`, digits only, `Transfer-Encoding` rejected because it is not implemented), and oversized bodies are refused from the head alone, before they are read.
