# WEB-7: Build an HTTP response serializer and wire the parser into `Server`

**Status:** Open
**Type:** Task
**Milestone:** M2 - HTTP core
**Priority:** High (everything in M2 and M3 depends on the server speaking HTTP, and nothing is demonstrable with `curl` until then)
**Estimate:** 3 h
**Mode:** Mentor for the response serializer and the `Server` wiring (you write them). Pair for the unit tests and for adapting the check scripts (Claude may write them).

## Description
The parser is finished but unused, so the server still echoes NUL-delimited frames. This ticket replaces that protocol with real HTTP/1.1 request-head handling. A fixed response goes out for every parsed request, and parse errors become 4xx/5xx responses. Bodies, routing and static files stay out. This puts `http::parse` into `Server` and gives `curl` something to talk to (CLAUDE.md, M2).

## Scope
- **In:**
  - `server/include/http/Response.h`: a small `Response` (status code, reason phrase, headers, body) with `serialize() -> std::string`. It always emits `Content-Length`, and `Connection: close` when the connection will close. Use `std::format`.
  - Status-to-reason mapping for 200, 400, 404, 405, 413, 414, 431, 500, 501 and 505 (`constexpr` function or table).
  - `Server::clientCallback` calls `http::parse` on the connection's read buffer in a loop.
    - `Done`: queue a fixed `200 OK` (`text/plain`, small body), then consume `consumed_bytes` from the buffer.
    - `NeedMore`: wait for more data.
    - `ParseError`: queue a response with the error's status and `Connection: close`, then close after the flush. Reuse the existing half-close/flush path.
  - Pipelining: if several requests sit in the buffer, answer them in order.
  - Keep-alive: the connection stays open on HTTP/1.1 unless the request has `Connection: close`. HTTP/1.0 closes unless it sends `keep-alive`.
  - HEAD returns the same headers as GET (including `Content-Length`) and no body.
  - Delete `processFrames` and the NUL protocol.
  - Adapt `scripts/check_write_path.sh` and `scripts/check_scale.sh` to HTTP requests, keeping the same scenarios. For backpressure, make the response large enough to force `EPOLLOUT`.
- **Out (explicitly):**
  - Request bodies and `Content-Length` handling (WEB-8). A POST with a body is out of scope here, so test only bodyless requests.
  - Router, static files, MIME types, idle timeouts, chunked encoding.
  - `Date` and `Server` response headers (optional, note as roadmap).
  - The write-buffer cap and the other known issues in the backlog.

## Acceptance criteria (Definition of Done)
- [ ] `curl -v http://localhost:8080/` returns `HTTP/1.1 200 OK` with correct `Content-Length`, and `curl -I` returns the same headers with no body.
- [ ] `curl --http1.0 -v http://localhost:8080/` closes the connection after the response. Two requests on one connection (`curl -v http://localhost:8080/ http://localhost:8080/`) reuse it ("Re-using existing connection").
- [ ] Pipelining check: send two requests in one `write` (`printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n' | nc localhost 8080`). Two responses come back, in order.
- [ ] Malformed input returns the right status and the connection closes:
  - `printf 'garbage\r\n\r\n' | nc ...` gives 400.
  - A missing `Host` on HTTP/1.1 gives 400.
  - `FOO / HTTP/1.1` gives 501.
  - `GET / HTTP/2.0` gives 505.
  - An oversized header gives 431.
  The server stays up afterwards.
- [ ] A request split across several `send` calls (byte by byte in a script) still gets exactly one response.
- [ ] Builds clean with no new warnings.
- [ ] Tests added: `ResponseTest.*` in `tests/response_test.cpp` (added to the `common_tests` source list). They cover the status line, `Content-Length`, the `Connection: close` header, HEAD with no body, and the reason-phrase mapping. A `Server`-level test over `socketpair` is optional.
- [ ] `ctest` passes (existing 74 plus the new tests), and `scripts/check_write_path.sh` and `scripts/check_scale.sh` pass against the HTTP server.
- [ ] No raw `read`, `write` or `epoll_*` outside the wrapper layer. The parser returns `Result`, and `ParseError` is not turned into an exception.
- [ ] Small, focused commits, each with `WEB-7` in the message.

## Technical notes / hints
- **Where the loop lives:** wrap `parse` in `while (true)` over `std::string_view{buf}.substr(offset)`. Keep an `offset` into the read buffer and erase the consumed prefix once after the loop, not per request. This avoids the same quadratic pattern that `processFrames` has.
- **View lifetime:** the `Request` views point into `buf`. Finish using them, including the `Connection`/`HEAD` decision, before you erase or append to `buf`.
- **Error close:** after queueing an error response, set `setReadClosed(true)` so `flush()` closes the connection once the buffer drains, then stop parsing. The remaining buffered bytes after a parse error are untrusted, so drop them.
- **Keep-alive decision:** a small function `shouldKeepAlive(const Request&)` using `getHeaderValue("Connection")` and `iequals`, keyed on `request.version`. It is pure, so it is easy to unit test.
- **Edge-triggered reads:** `readAllAvailable` drains to `EAGAIN`, so a loop that handles every complete request in the buffer loses no events. Flush once after queueing multiple responses, not per response.
- **Cleanup in passing:** `HttpMethod::DELETE` is in the enum and the parser never returns it. Remove it while you are in `Request.h`, or leave it for the cleanup ticket.
- **C++20 features:** `std::format` for the head, `std::string_view` for static reason phrases, `std::span` or `string_view::substr` for the buffer window.

## How to verify
```
cmake --build build && ctest --test-dir build --output-on-failure
build/server/server --port 8080 --max-connections 1000 &
curl -v http://localhost:8080/
curl -I http://localhost:8080/
curl --http1.0 -v http://localhost:8080/
printf 'FOO / HTTP/1.1\r\nHost: x\r\n\r\n' | nc localhost 8080
scripts/check_write_path.sh && scripts/check_scale.sh
```

## Dependencies
- Blocked by: WEB-6 (done).
- Blocks: WEB-8 (`Content-Length` bodies and 413), M3 (router and static files).

## Interview talking point
"How do you handle pipelined requests and partial reads with edge-triggered epoll?" Answer: drain to `EAGAIN`, then parse in a loop until `NeedMore`. Keep a buffer offset and compact once. Close after an error response and drop the untrusted remainder of the buffer.
