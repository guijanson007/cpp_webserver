# WEB-6: Write an incremental HTTP/1.1 request-line and header parser (standalone)

**Status:** Done (acceptance criteria met; see "Outcome")
**Type:** Task
**Milestone:** M2 - HTTP core
**Priority:** High (first unstarted milestone; everything in M2 and M3 depends on the parser)
**Estimate:** 3 h
**Mode:** Mentor (you write the parser; Claude reviews and may write the test scaffolding)

## Description
The server still speaks the NUL-framed echo protocol, which goes away with HTTP. This ticket builds only the parsing core as a pure, socket-free unit: feed it bytes, get back "need more", "complete request head", or an error. Keeping it separate from `Server` makes it unit-testable and fuzz-ready later. It covers CLAUDE.md's "hand-written, incremental, resumable parser over `std::string_view`".

## Scope
- In:
  - `server/include/http/Request.h`: method enum (GET, HEAD, POST), target, version, and headers as `string_view`s into the connection's read buffer.
  - `server/include/http/Parser.h`: `parse(std::string_view buffer) -> Result<ParseOutcome>`. The outcome is `NeedMore` or `Done{consumed bytes}`.
  - Size limits as a config struct with designated initializers: request line max, total header block max, header count max.
  - Error types that map to statuses: 400 malformed, 431 headers too large, 501 or 405 unknown method, 505 bad version.
  - Header names are case-insensitive. Reject bare `\n`, spaces before the colon, and obs-fold. Reject duplicate `Content-Length`.
- Out (explicitly):
  - Body reading and `Content-Length` handling, which is the next ticket.
  - Response builder, routing, wiring into `Server`/`Connection`.
  - Chunked encoding and the other roadmap items.

## Acceptance criteria (Definition of Done)
- [x] The same request fed in one chunk and byte-by-byte yields identical results.
- [x] A request split at every possible offset parses correctly (loop test).
- [x] `GET / HTTP/1.1\r\nHost: x\r\n\r\n` returns `Done` with the correct `consumed`. Pipelined trailing bytes are not consumed.
- [x] Oversized request line or header block returns the 431 or 414-class error without scanning unbounded input.
- [x] Malformed inputs (no version, lowercase method, `\n`-only line endings, space before the colon, NUL byte) return an error, not a crash.
- [x] Builds clean with no new warnings.
- [x] Tests are added to `common_tests` in `tests/CMakeLists.txt`: `ParserTest.*` in `tests/parser_test.cpp`.
- [x] No raw syscalls; errors via `Result<T, Error>`, no exceptions.
- [x] Small, focused commits, each with `WEB-6` in the message.

## Technical notes / hints
- Decide up front whether the parser keeps state (a resumable state machine) or re-scans from the start on each call. Re-scanning is simple and fine when the header limit is small (for example 8 KiB), so state its cost in a comment. Resuming from the last offset is the interview-grade answer.
- The `string_view`s are only valid while the buffer is not reallocated or erased. Decide who owns the buffer and when it may be compacted. This is the reason `Done` returns `consumed`.
- The `Result` rules are `T != E` and a nothrow-move `T`, so keep `Request` cheap to move.
- Use `std::string_view::find`, `remove_prefix` and `std::ranges` for header processing. For case-insensitive names, a `constexpr` ASCII-lowercase compare is enough.
- Request smuggling basics: reject ambiguous framing early (duplicate or conflicting `Content-Length`).

## How to verify
```
cmake --build build && ctest --test-dir build --output-on-failure
build/tests/common_tests --gtest_filter='Parser*'
```
Optionally run with ASan: `cmake -S . -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`.

## Dependencies
Blocked by: none. Blocks: the body/`Content-Length` ticket, the response builder, and wiring the parser into `Server`, which replaces `processFrames`.

## Interview talking point
"The parser is incremental because TCP gives no message boundaries, and its size limits close off the memory-exhaustion and request-smuggling classes of attack."

## Outcome

74 of 74 tests pass (`ctest`), 48 of them new (`ParserTest.*`, `ParserMalformedTest.*`, `RequestTest.*` in `tests/parser_test.cpp`). A separate `-Wall -Wextra -fsanitize=address` build shows no warnings or findings in the parser or its tests.

What was done:

- `server/include/http/Parser.h`: stateless `http::parse(buffer, limits)` returning `Result<std::variant<Done, NeedMore>, ParseError>`. `Done` carries the `Request` and `consumed_bytes`. Protocol errors are `ParseError` values (400, 414, 431, 501, 505), not `Error`s. The lifetime contract and the O(N^2) re-scan cost are documented in a comment.
- `server/include/http/Request.h`: `Request` is a socket-free aggregate of `string_view`s into the read buffer, with `getHeaderValue(name)` (case-insensitive, value trimmed). `SizeLimits` has defaults (8192 / 8192 / 100). `iequals` is `constexpr`.
- Rejected with 400: bare `\n` or `\r`, NUL bytes, space or tab before the colon, obs-fold and leading whitespace, empty or non-token header names, missing colon, duplicate `Content-Length`, trailing junk after the version, and HTTP/1.1 without `Host` (an addition beyond the ticket, per RFC 9112). 505 is returned only for a well-formed unsupported `HTTP/x.y`; 501 for an unsupported method.
- The review of the first draft found an `size_t` underflow for requests with no headers, accepted duplicate `Content-Length` and space-before-colon, hung on bare LF, wrong 400 vs 505 codes, and a size limit that depended on whether the terminator had arrived. All are fixed and covered by tests. `Request` also lost its `Connection&` member so the parser is socket-free.
- Fixed `Server` constructor reading `config.maxConnections` after the move (now `m_config`).

Left for later tickets: wiring the parser into `Server` (replaces `processFrames`), `Content-Length` bodies (`Request::body` is always empty), the response builder, removing the unreachable `HttpMethod::DELETE`, and the optional trailing CRLF in `Request::header` (it includes the CRLF after the last header line, empty when there are no headers).

## Interview talking point (as defended)

The parser is stateless and re-scans on `NeedMore`, which is simple and costs O(N^2) byte-by-byte over an 8 KiB head. Resuming from an offset is the profiling follow-up. Every length limit applies before the terminator arrives, so a peer cannot make the server buffer forever.
