# Epoll HTTP Server in C++20

Resume project and C++20 learning vehicle. A Linux-only HTTP/1.1 server built on a
hand-written epoll reactor. Target: finished, tested, benchmarked, and every design
decision backed by profiling data.

## Working agreement

- **Mentor mode (core code):** reactor, HTTP parser, router, thread pool. The user
  writes these. Claude explains, reviews, and points out bugs. Do not write them.
- **Pair mode (plumbing):** Claude may write tests, CI, CMake, clang-tidy config,
  docs scaffolding.
- Flag any bug seen in user code, even if unrelated to the current task.
- Push back on scope creep. If an idea is outside the milestones below, say so and
  suggest adding it to the README roadmap instead.
- Pace: user has about 2-3 h/day, no hard deadline. Quality over speed.

## Decisions (settled)

- Linux only (epoll). No portability layer. State this in the README.
- C++20 only (`CMAKE_CXX_STANDARD 20`, extensions off). No `std::expected` (C++23).
- Zero runtime dependencies: standard library and Linux syscalls only.
- Dev-only dependencies are fine: GoogleTest via CMake `FetchContent`.
- Errors: `Result<T, Error>` on I/O and parsing paths (own type, wraps `errno` /
  `std::error_code`). Exceptions only at startup (e.g. failed bind). One bad
  connection must never take the server down.
- No raw `read`/`write`/`epoll_*` outside a thin wrapper layer.
- Hand-written, incremental, resumable HTTP/1.1 parser over `std::string_view` /
  `std::span`, zero-copy where possible. No llhttp / http-parser.
- Concurrency: single-threaded epoll reactor first. Worker pool is optional M6.
- TLS, HTTP/2, WebSockets are out. If TLS is wanted, document fronting with Nginx/Caddy.

## C++20 features to use (and be able to explain in interviews)

`std::span`, `std::string_view`, concepts (constrain router/handler types),
`std::jthread` + `std::stop_token` (M6), `std::format`, designated initializers for
config structs, ranges for header/route processing, `<=>` for small types.
Coroutines: optional stretch only.

## Scope

**In (v1):** GET, HEAD, POST. Request-line and header parsing with size limits.
Status codes 400/404/405/413/431/500. `Content-Length` bodies. Keep-alive with idle
timeouts. Static files with MIME types and path-traversal protection. Simple router.

**Roadmap only (README, not built):** chunked encoding, `Range`, gzip, libFuzzer
fuzzing of the parser, load-generator `client/`.

## Milestones (each shippable on its own)

1. **M1 Cleanup:** `EpollReactor` class (move epoll logic out of `main`), `constexpr`
   instead of `#define`, fix accept crash, remove dead blocking `readUntil` /
   `ReadMessage`, add write path with `EPOLLOUT` and per-connection output buffer.
2. **M2 HTTP core:** incremental parser, response builder, methods, status codes,
   size limits, `Content-Length` bodies.
3. **M3 Routing + static files:** concept-constrained router, static files, MIME,
   traversal protection, keep-alive + timeouts.
4. **M4 Quality:** GoogleTest unit tests (parser, router, buffers), GitHub Actions CI
   (GCC + Clang), ASan/UBSan builds, clang-tidy.
5. **M5 Polish:** README with architecture diagram, `wrk` benchmark vs Nginx,
   `docs/PERFORMANCE.md`.
6. **M6 (optional, only after M5 ships):** `std::jthread` worker pool, graceful
   shutdown on SIGINT/SIGTERM (`signalfd` or atomic flag), profile vs single thread.

## Definition of done (M1-M5)

- CI green.
- `curl` and `wrk` both work.
- Handles 10k+ concurrent keep-alive connections without crashing. Connection limit
  is configurable (not the current 10).
- README table: requests/sec vs Nginx serving the same static file. Report honestly.

## Profiling (a headline resume point)

Every optimisation follows: baseline -> profile -> hypothesis -> change -> re-measure.
Record results in `docs/PERFORMANCE.md` (before/after table, flamegraph committed),
plus CPU model and kernel so runs are reproducible.

- Tools: `perf` + flamegraphs, `perf stat`, `strace -c`, `heaptrack`, `wrk` with
  fixed parameters.
- Profiling build: `-O2 -g -fno-omit-frame-pointer`.
- Decisions to settle with data: edge- vs level-triggered epoll; read buffer size
  (currently magic 1028 bytes); copying vs zero-copy parsing; `read`+`write` vs
  `sendfile()`; per-request allocations / reusable per-connection buffers; single
  thread vs pool (M6).

## Layout

```
common/            shared utilities (FileDescriptor, Result, ...)
server/net/        reactor, sockets
server/http/       parser, request/response, router
client/            small client (load generator is roadmap)
tests/             GoogleTest
docs/              PERFORMANCE.md, architecture notes
```

Current code is flatter; reorganise during M1/M2. Actual files today:

```
common/include/common/FileDescriptor.h   RAII, move-only fd
common/include/common/Error.h            wraps std::error_code; Error::from_errno()
common/include/common/Result.h           Result<T, E = Error> and Result<void, E> (std::variant / optional)
server/include/network/EpollReactor.h    epoll fd + fd -> handler table + run()
server/include/network/SocketIo.h        readAllAvailable / writeSome over Result
server/include/network/Connection.h      client fd + read/write buffers
server/include/http/Request.h            Request (string_views into the read buffer), SizeLimits, iequals
server/include/http/Parser.h             http::parse(): stateless request-head parser, ParseError
server/include/http/Response.h           Response::serialize(), StatusCode, reason phrases
server/include/server/Server.h           listener, accept loop, client table, clientCallback
server/src/main.cpp                      wiring only
tests/                                   GoogleTest (FetchContent): error, result, socketio, parser, response, request (keep-alive) tests
scripts/check_*.sh                       WEB-1..WEB-4 and WEB-7 checks (see Workflow conventions)
docs/tickets/WEB-<n>.md                  tickets from /next_task
```

## Current architecture (after WEB-7)

- `EpollReactor` is a dumb dispatcher: `add(fd, events, handler)`, `modify`, `remove`,
  `run()` (one `epoll_wait` pass). All four return `Result<void>`. Handlers are
  `std::function<void(int fd, uint32_t events)>`. `run()` copies the handler before calling it, so a
  handler may `remove()` itself. It knows nothing about sockets or protocols.
- Errors: `Result<T, Error>` (`common/Result.h`) on I/O paths; `Error` wraps a
  `std::error_code`, build it with `Error::from_errno(errno)` at the failure site
  (`Error(int)` is explicit). `Result<T, E>` rejects `T == E` and `void` misuse with
  `requires`. `Server::start()` returns `Result<void>`; `main` prints the error and exits
  (no exceptions). Expected outcomes are success values, not errors:
  `SocketIo::readAllAvailable` returns `Result<ReadOutcome>` (`bytes_read`,
  `ConnectionState::Open|ClosedByPeer`; drains until `EAGAIN` or EOF) and
  `SocketIo::writeSome` returns `Result<size_t>` (bytes sent; a non-empty buffer afterwards
  means the socket is full, so `flush()` arms `EPOLLOUT`).
- `Server` owns the listening socket and `unordered_map<int, Connection>`. The listener
  is level-triggered; clients are `EPOLLIN | EPOLLET` and are drained until `EAGAIN`.
  `accept()` loops until `EAGAIN`, sheds connections over the cap with `continue`. On
  `EMFILE`/`ENFILE` it uses a spare `/dev/null` fd: release it, accept and close one
  connection, reopen the spare, so the level-triggered listener does not spin.
- `ServerConfig` (`server/ServerConfig.h`) holds `ip`, `port`, `maxConnections` (default
  100) and `backlogSize` (default 10'000). `main` parses `--port`, `--max-connections`,
  `--backlog` into it. The kernel caps the backlog at `net.core.somaxconn`.
- HTTP head parser (`http/Parser.h`, wired into `Server` since WEB-7): `http::parse(std::string_view
  buffer, const SizeLimits &) -> ParseOutcome = std::variant<Done, NeedMore, ParseError>` (a plain
  variant, not a `Result`: protocol errors are expected outcomes). Stateless: on
  `NeedMore` the caller appends more bytes and calls again (re-scan, O(N^2) byte-by-byte, fine at
  8 KiB). `Done` holds a `Request` (views into `buffer`, valid until the buffer is modified) and
  `consumed_bytes`. Protocol errors are `ParseError` values (400/414/431/501/505; the enum also lists
  404/405/413/500, which the parser never returns), not `Error`.
  Supports GET/HEAD/POST over HTTP/1.0 and 1.1; HTTP/1.1 requires `Host`. Rejects bare LF/CR, NUL,
  obs-fold, space before colon, bad header-name tokens, duplicate `Content-Length`.
  `Request::getHeaderValue` is case-insensitive (first match only). Bodies are not parsed yet
  (`body` is empty).
- `Request::shouldKeepAlive()`: HTTP/1.1 persistent unless the `Connection` value list contains
  `close`; HTTP/1.0 closes unless it contains `keep-alive`; `close` wins. The value is split on commas,
  case-insensitive, OWS trimmed. Only the first `Connection` header line is read (README roadmap).
- `http::Response` (`http/Response.h`): `Response(StatusCode, body, extraHeaders, close)`,
  `serialize()` always emits `Content-Length`, plus `Connection: close` when asked. `StatusCode` and
  `statusCodeToReasonPhrase()` live in the same header (`ParseError` values cast to `StatusCode`).
- `Server::clientCallback` after `readAllAvailable`: loop `http::parse(buf)`. `Done` queues a fixed
  `200 OK` (`text/plain`, `Hello, World!`; `HEAD` drops the body bytes), `buf.erase(0, consumed_bytes)`
  per request, and `setReadClosed(true)` + stop if the connection must close. `ParseError` queues the
  error status with `Connection: close`, `setReadClosed(true)`, stops (rest of the buffer is dropped).
  One `flush()` per wakeup closes a read-closed connection once the output drains. Pipelining works
  because the loop runs until `NeedMore`. There is no router yet (M3); every request gets the same body.
- `Connection` owns its `FileDescriptor` and read buffer. Closing = `reactor.remove(fd)`
  then erase from the map (the destructor closes the fd; never call raw `close` on an
  fd a `Connection` owns).
- `main` constructs reactor then server, `server.start()`, `while (reactor.run())`.
- Event arrays are owned by the reactor (`MAX_EVENTS` constexpr member).

## Workflow conventions

- Tickets: `/next_task` writes `WEB-<n>`; put the id in every commit message. Latest:
  WEB-7 (done: `http::Response`, parser wired into `Server`, keep-alive/pipelining/HEAD, NUL echo
  removed, tests for response and keep-alive, old check scripts ported to HTTP). Next: WEB-8 `Content-Length`
  bodies + 413, and/or a small cleanup ticket (argument validation, `[[nodiscard]] Result`, warning
  flags).
- Verification scripts live in `scripts/`: `check_connection_limit.sh` (WEB-1),
  `check_reactor.sh` (WEB-2, uses strace and `ss`), `check_write_path.sh` (WEB-3),
  `check_scale.sh` (WEB-4: cap, 10k idle connections, fd exhaustion), `check_http.sh` (WEB-7: GET/HEAD,
  keep-alive, HTTP/1.0, pipelining, error statuses, curl). `check_write_path.sh` and `check_scale.sh`
  speak HTTP now (backpressure = 100k pipelined requests). All pass on `web-7`.
  Scripts count connections with `ss`, not server logs. Pass `--max-connections` explicitly.
- Unit tests: `ctest --test-dir build --output-on-failure` (123 tests on `web-7`, 1 of
  them `DISABLED_`: repeated `Connection` headers). Add a test with every new parser/router/buffer
  unit; `SocketIo` tests use `socketpair`; parser, response and keep-alive tests are socket-free.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Needs CMake >= 4.3. Formatting: `.clang-format` in repo root.

```
build/server/server --port 8080 --max-connections 1000    # run (always pass the cap)
ctest --test-dir build --output-on-failure                # all unit tests
build/tests/common_tests --gtest_filter='SocketIo*'       # one suite/test (binary: common_tests)
scripts/check_write_path.sh [server-binary] [port]        # check scripts default to build/server/server, port 8080
```

All tests live in the single `common_tests` target (`tests/CMakeLists.txt`); add new
test files to its source list. No CI, README or Cursor/Copilot rules exist yet (M4/M5).

## Known issues (M1 backlog)

Fixed (WEB-1, WEB-2): accept crash at the cap, `#define` macros, `<algorithm>`, dead
`readUntil`/`ReadMessage`, `inet_pton` check, `listen`/`epoll_create1`/`epoll_ctl`
checks, one-accept-per-wakeup, epoll mixed into `main`, double close on failed epoll
registration, `EINTR` in `epoll_wait`.

Fixed (WEB-3): write path (`Connection` output buffer, `EPOLLOUT` armed on
`EAGAIN` and disarmed on drain, half-close flushes before closing), `MSG_NOSIGNAL`,
handler carries `uint32_t events`, `SO_REUSEADDR` with a named `int opt`, `bool` returns
on `add`/`modify`/`remove`, fd I/O behind `network/SocketIo.h`, over-limit shed via
`Connection::close()`. `scripts/check_write_path.sh` covers echo, RST survival,
backpressure, `EPOLLOUT` not armed for a small echo.

Fixed (WEB-4): configurable connection cap and separate backlog via `ServerConfig` and
flags, `EMFILE`/`ENFILE` busy loop (spare fd), `explicit Connection(int)`, `Connection`
built only after a successful accept, per-accept logging removed, `check_reactor.sh`
no longer depends on server logs. 10k idle connections verified by `check_scale.sh`.

Fixed (WEB-5): `Result<T, Error>` and `Error` with concept constraints, `Server::start`
and `EpollReactor` return `Result<void>` (no startup exceptions), EOF/EAGAIN as success
values via `ReadOutcome`, `writeSome` erases once, `flush()` only calls `modify()` on a
state change, listener back to level-triggered, GoogleTest scaffolding (26 tests),
`check_scale.sh` helper functions repaired.

Fixed (WEB-6): `http::parse` and `Request` (see Current architecture), `Server` constructor no
longer reads the moved-from `config` (uses `m_config`), 48 parser tests.

Fixed (WEB-7): `Response` serializer, parser wired into `Server` (keep-alive, pipelining, `HEAD`,
error responses that close after the flush), NUL-framed echo and `processFrames` removed,
`Request::shouldKeepAlive` with `Connection` token lists, `ResponseTest`/`ReasonPhraseTest`/
`KeepAliveTest` (+49 tests), `scripts/check_http.sh`, other check scripts ported to HTTP.

Open:

- Argument parsing: `std::stoi` accepts trailing garbage and there are no range checks
  for port, cap or backlog.
- `EMFILE` message prints on every discard (rate-limit or drop).
- `MAX_EVENTS` is 10'000, a guess. Settle by profiling.
- Leftover include (`<asm-generic/socket.h>` in `Server.h`); `accept4` is still called
  directly in `Server` (outside the wrapper layer).
- `Result` is not `[[nodiscard]]` (callers use `static_cast<void>(flush(...))`);
  `error()` on a successful `Result<void>`
  is undefined behavior; `Error(errno != 0 ? errno : EIO)` fallbacks after `modify()`.
- `ReadOutcome::bytes_read` is unused by the caller. Untested `SocketIo` paths:
  `ECONNRESET` on read, the `EINTR` retry, `send` returning 0.
- Repeated `Connection` header lines are not combined (`getHeaderValue` returns the first):
  `Connection: TE` then `Connection: close` is read as keep-alive. Disabled test:
  `KeepAliveTest.DISABLED_RepeatedConnectionHeadersAreCombined`. Roadmap in README.
- HTTP/1.0 keep-alive responses carry no `Connection: keep-alive` header; a strict 1.0 client may
  close anyway.
- `ParseError` duplicates `StatusCode` (and lists codes the parser never returns); `Server` casts
  between them.
- `HttpMethod::DELETE` is in the enum but the parser never returns it (501); remove it.
- Argument parsing review (verified): `--port 70000` binds 4464 while printing 70000,
  `--max-connections -1` starts and sheds every client.
- `EpollReactor` dispatches by fd number from a fetched batch: a stale event can reach a new
  connection that reused the fd in the same batch (use a generation id in `epoll_event.data`).
- `flush()` reads `errno` after `closeClientConnection()`; return the `Result` error from
  `modify()` instead. The write buffer has no cap (a non-reading client grows it).
- `EpollReactor` constructor throws outside `main`'s `try`; warning flags exist only on the
  test target.
- `Server::clientCallback` does `buf.erase(0, consumed_bytes)` per pipelined request: quadratic on big
  bursts (the 100k-request backpressure case in `check_write_path.sh` costs ~4 s of CPU). Profiling
  candidate: parse from an offset and compact once after the loop. (The old per-frame
  `processFrames` erase is gone with the NUL protocol.)
- `readAllAvailable` drains until `EAGAIN` with no cap, so one fast sender can starve the other
  clients and grow the read buffer without bound. Memory is fixed by the M2 size limits
  (413/431), not by a read cap.
- **Idea (roadmap, not built):** bounded reads per wakeup plus a ready queue so an
  `EPOLLET` connection that was not fully drained is resumed without losing its edge.
  Needs `epoll_wait` with a zero timeout while the queue is non-empty and cleanup when a
  connection closes, which changes the "dumb dispatcher" reactor design. Decide together
  with edge- vs level-triggered (level-triggered needs no queue) as a profiling ticket
  after M3: baseline = `wrk` with one bulk sender next to many small clients, measure the
  small clients' latency.

## Interview talking points (prepare per milestone)

- M1: why epoll over select/poll; edge- vs level-triggered and the drain-until-EAGAIN
  rule; RAII for fds; move-only types.
- M2: incremental parsing across partial reads; request smuggling and size limits.
- M3: path traversal; keep-alive state machine; timeouts without a thread per conn.
- M4: sanitizer findings; what the tests caught.
- M5/M6: what profiling changed, with numbers.
