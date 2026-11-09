# WEB-6: Write an incremental HTTP/1.1 request-line and header parser (standalone)

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
- [ ] The same request fed in one chunk and byte-by-byte yields identical results.
- [ ] A request split at every possible offset parses correctly (loop test).
- [ ] `GET / HTTP/1.1\r\nHost: x\r\n\r\n` returns `Done` with the correct `consumed`. Pipelined trailing bytes are not consumed.
- [ ] Oversized request line or header block returns the 431 or 414-class error without scanning unbounded input.
- [ ] Malformed inputs (no version, lowercase method, `\n`-only line endings, space before the colon, NUL byte) return an error, not a crash.
- [ ] Builds clean with no new warnings.
- [ ] Tests are added to `common_tests` in `tests/CMakeLists.txt`: `ParserTest.*` in `tests/parser_test.cpp`.
- [ ] No raw syscalls; errors via `Result<T, Error>`, no exceptions.
- [ ] Small, focused commits, each with `WEB-6` in the message.

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
