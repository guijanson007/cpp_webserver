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
server/include/http/Router.h             RequestHandler concept, Router (exact path, prefix mounts, 404/405, HEAD->GET)
server/include/http/StaticFileHandler.h  /static/ handler: decode, refuse `.` segments, read beneath root fd
server/include/http/MimeTypes.h          extension -> Content-Type
common/include/common/FileReader.h       openDirectory (O_PATH), readRegularFileBeneath (openat2 RESOLVE_BENEATH)
server/include/server/ServerConfigParser.h  per-flag validation (port, cap, backlog, --root)
server/include/server/Server.h           listener, accept loop, client table, clientCallback
server/src/main.cpp                      wiring only
tests/                                   GoogleTest (FetchContent): error, result, socketio, parser, response, request (keep-alive), router, mime, file_reader, static_file_handler, server_config_parser, server_idle_timeout tests
scripts/check_*.sh                       WEB-1..WEB-4 and WEB-7 checks (see Workflow conventions)
docs/tickets/WEB-<n>.md                  tickets from /next_task
```

## Current architecture (after WEB-13)

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
  100), `backlogSize` (default 10'000), `rootDirectory`, `idleTimeout` (`std::chrono::seconds`,
  default 60, 0 disables) and `sweepInterval` (`milliseconds`, default 1 s, no flag). `main` parses
  `--port`, `--max-connections`, `--backlog`, `--root`, `--idle-timeout` into it. The kernel caps
  the backlog at `net.core.somaxconn`.
- Idle timeouts (WEB-13): `network::TimerEntry{clientFd, optional<time_point> expiresAt}` (in
  `Connection.h`) in `Server::m_timers`, a `std::list` in last-activity order. `Connection` stores its
  list iterator: `clientCallback` refreshes `expiresAt` and `splice`s to the back on every event (after
  the `EPOLLERR` check); `closeClientConnection` erases it (so every close path removes the timer).
  `Server::start()` arms one periodic `timerfd` (`EPOLLET`) every `sweepInterval`; `timerCallback`
  drains it until `EAGAIN`, then closes expired entries from the front and stops at the first live
  one. Timeout 0: `expiresAt` is `nullopt` and the sweep returns early. The `timerfd_*` calls are raw
  in `Server` (known issue).
- HTTP parser (`http/Parser.h`, wired into `Server` since WEB-7, bodies since WEB-8): `http::parse(std::string_view
  buffer, const SizeLimits &) -> ParseOutcome = std::variant<Done, NeedMore, ParseError>` (a plain
  variant, not a `Result`: protocol errors are expected outcomes). Stateless: on
  `NeedMore` the caller appends more bytes and calls again (re-scan, O(N^2) byte-by-byte, fine at
  8 KiB; while a body is incomplete only the head is re-scanned). `Done` holds a `Request` (views into
  `buffer`, valid until the buffer is modified) and `consumed_bytes` (head plus body). Protocol errors
  are `ParseError` values (400/413/414/431/501/505; the enum also lists 404/405/500, which the parser
  never returns), not `Error`.
  Supports GET/HEAD/POST over HTTP/1.0 and 1.1; HTTP/1.1 requires `Host`. Rejects bare LF/CR, NUL,
  obs-fold, space before colon, bad header-name tokens, duplicate `Content-Length`.
  `Request::getHeaderValue` is case-insensitive (first match only).
  Bodies (WEB-8): `Content-Length` is digits only (`std::from_chars`; sign, spaces, empty, overflow
  are 400). `Request::body` is a view of exactly that many bytes. Incomplete body is `NeedMore`.
  Declared length over `SizeLimits::maxBodySize` (1 MiB) is 413 straight after the head, before any
  body byte arrives. Any `Transfer-Encoding` is 501 (chunked is roadmap). The server still ignores
  the body content (no router).
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
- `http::Router` (WEB-10): `add(HttpMethod, path, RequestHandler)` (concept: `Response(const Request&)`),
  `route(const Request&) -> Response`. Exact path with the query string stripped, no percent-decoding.
  Unknown path 404, wrong method 405 + `Allow`, HEAD falls back to the GET handler. Routes live in
  `unordered_map<string, array<optional<function>, MAX_METHODS>>` with transparent lookup. `Server`
  owns a `Router` passed in by `main` (demo routes `GET /`, `POST /echo`), and `clientCallback` sets
  `resp.close_connection |= !shouldKeepAlive()` after routing. A throwing handler is a 500 + close
  (WEB-12, caught in `Router::route`). Handlers return owning `Response`s.
- Static files (WEB-11): `--root` is required (`ServerConfig::rootDirectory` is `std::optional`).
  `main` mounts `StaticFileHandler` on `/static/` with `Router::mount` (exact routes win, longest
  prefix wins, GET/HEAD only, else 405). The handler holds the root as a `shared_ptr<const
  FileDescriptor>` (`O_PATH`), percent-decodes, refuses any segment starting with `.`, `\`, NUL,
  leading `/`, then `readRegularFileBeneath` calls `syscall(SYS_openat2)` with `RESOLVE_BENEATH |
  RESOLVE_NO_MAGICLINKS` (escape = `EXDEV`; absolute symlinks are refused) and `fstat` + `S_ISREG`.
  Every failure is 404. Files are read whole on the reactor thread, no size cap.
- `Connection` owns its `FileDescriptor`, read/write buffers and its timer-list iterator. Closing = `reactor.remove(fd)`
  then erase from the map (the destructor closes the fd; never call raw `close` on an
  fd a `Connection` owns).
- `main` constructs reactor then server, `server.start()`, `while (reactor.run())`.
- Event arrays are owned by the reactor (`MAX_EVENTS` constexpr member).

## Workflow conventions

- Tickets: `/next_task` writes `WEB-<n>`; put the id in every commit message. Latest:
  WEB-13 (done: idle timeouts with one periodic `timerfd` + LRU list, `--idle-timeout`,
  `check_idle_timeout.sh`, in-process `ServerIdleTimeoutTest`, ctest `TIMEOUT 30`; M3 complete).
  Before that WEB-12 (done: handler exceptions -> 500 + close in `Router::route`, close flags combined in `Server`,
  `class [[nodiscard]] Result`, `-Wall -Wextra -Wpedantic` on every target). Before that WEB-11 (done: static files, MIME, `openat2` traversal protection, `Router::mount`, required `--root`,
  `static-*` cases in `check_http.sh`). Before that WEB-10 (done: router, 404/405, routing section in `check_http.sh`). Before that WEB-9 (done: strict argument parsing, `check_args.sh`). Before that WEB-8 (done: `Content-Length` bodies, 413, `Transfer-Encoding` 501, read-result check before
  parsing, body section in `check_http.sh`). Next: M4 quality (CI, sanitizers, clang-tidy) or the
  WEB-13 follow-ups in Known issues.
- Verification scripts live in `scripts/`: `check_connection_limit.sh` (WEB-1),
  `check_reactor.sh` (WEB-2, uses strace and `ss`), `check_write_path.sh` (WEB-3),
  `check_scale.sh` (WEB-4: cap, 10k idle connections, fd exhaustion), `check_args.sh` (WEB-9: CLI validation), `check_idle_timeout.sh` (WEB-13: idle close, activity
  refresh, fd reuse, 300 idle, `--idle-timeout 0`), `check_http.sh` (WEB-7/WEB-8: GET/HEAD,
  keep-alive, HTTP/1.0, pipelining, error statuses, POST bodies, 413, routing, static files, curl). `check_write_path.sh` and `check_scale.sh`
  speak HTTP now (backpressure = 100k pipelined requests). All pass on `web-13`. Every script starts the server with a temp `--root` (it is required).
  Scripts count connections with `ss`, not server logs. Pass `--max-connections` explicitly.
- Unit tests: `ctest --test-dir build --output-on-failure` (259 tests on `web-13`, 1 of
  them `DISABLED_`: repeated `Connection` headers; 30 s timeout per test). Add a test with every new parser/router/buffer
  unit; `SocketIo` tests use `socketpair`; parser, response and keep-alive tests are socket-free.

## Webserver ticket workflow (WEB-N)

A ticket is only closeable when all of the following are done:

1. docs/tickets/WEB-N.md exists (scope, findings, bugs, verification).
2. Tests have been added or updated and all pass.
3. scripts/check_*.sh passes.
4. README and roadmap are updated.
5. The branch is pushed and a PR is opened against main.
Do not declare a ticket done until you have run the actual end-to-end check against a fresh build.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Needs CMake >= 4.3. Formatting: `.clang-format` in repo root.

```
build/server/server --port 8080 --max-connections 1000 --root ./www   # run (always pass the cap; --root is required)
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

Fixed (WEB-8): `Content-Length` bodies framed (a POST body used to be parsed as the next request),
`SizeLimits::maxBodySize` and 413 before the body arrives, `Transfer-Encoding` rejected with 501,
`clientCallback` checks the `readAllAvailable` result before parsing, +11 tests, 9 body checks and
curl POST checks in `check_http.sh`.

Fixed (WEB-9): strict argument parsing (`common/ParseNumber.h`, ranges, exit 1), `check_args.sh`,
leftover `asm-generic` include and `HttpMethod::DELETE` removed, +9 tests.

Fixed (WEB-11): static files, MIME, traversal protection via `openat2(RESOLVE_BENEATH)` on a root fd
(replaced a `canonical()` + prefix check that had a TOCTOU race), `Router::mount`, required `--root`,
dotfiles refused, `FileDescriptor::operator bool` explicit, `syscall(OPENAT2_REGULAR, ...)` passing a
flag as the syscall number (was `read`, EBADF), +86 tests.

Fixed (WEB-12): `Router::route` catches `std::exception` and `...` from handlers -> 500, empty body,
`close_connection = true`; `Server` closes when `!req.shouldKeepAlive() || resp.close_connection` (an
intermediate version trusted only the handler flag and broke `Connection: close`, caught by
`check_http.sh`); `class [[nodiscard]] Result`; `add_compile_options(-Wall -Wextra -Wpedantic)` at top
level (also reaches GoogleTest); client warnings fixed; +6 `RouterTest` tests.

Fixed (WEB-13): idle timeouts (see Current architecture); `std::ignore` discard in
`closeClientConnection` replaced by `static_cast<void>`; ASan stack-use-after-scope in 5
`ParserBodyTest` tests (pointer into a temporary `Outcome`); +11 tests, `check_idle_timeout.sh`.

Open:

- Mount 405 is built in both `Router::route` and `StaticFileHandler`.
- Static files are read whole on the reactor thread with no size cap (`sendfile` profiling ticket).
- (WEB-13) `Server::timerCallback` sweep loop `while (front().expiresAt <= now)
  closeClientConnection(front().clientFd)` only advances if that fd is in `m_clients`. If `m_timers`
  and `m_clients` ever disagree, the reactor spins forever (seen by removing the `m_timers.erase` in
  `closeClientConnection`: `PeerCloseLeavesNoStaleTimerForReusedFd` hangs). Pop the front entry
  itself when the client is missing.
- (WEB-13) `timerfd_create`, `timerfd_settime` and the expiration-count `read` are raw syscalls in
  `Server` (outside the wrapper layer). Move them into a `TimerFd` wrapper in `network/` returning
  `Result`.
- (WEB-13) With `--idle-timeout 0` (disabled) the sweep timerfd is still armed every
  `sweepInterval` but never read (`timerCallback` returns first). Harmless, but don't arm it.
- Under ASan (+ `_GLIBCXX_DEBUG`) the `check_write_path.sh` backpressure case fails ("a second client
  was starved"), on `main` (`ab171df`) as well: the per-request `buf.erase` below keeps the reactor
  busy past the other client's 3 s budget. Same root cause as the quadratic-erase item.

- `EMFILE` message prints on every discard (rate-limit or drop).
- `MAX_EVENTS` is 10'000, a guess. Settle by profiling.
- `accept4` is still called directly in `Server` (outside the wrapper layer).
- `error()` on a successful `Result<void>` is undefined behavior; `Error(errno != 0 ? errno : EIO)` fallbacks after `modify()`.
- `ReadOutcome::bytes_read` is unused by the caller. Untested `SocketIo` paths:
  `ECONNRESET` on read, the `EINTR` retry, `send` returning 0.
- Repeated `Connection` header lines are not combined (`getHeaderValue` returns the first):
  `Connection: TE` then `Connection: close` is read as keep-alive. Disabled test:
  `KeepAliveTest.DISABLED_RepeatedConnectionHeadersAreCombined`. Roadmap in README.
- HTTP/1.0 keep-alive responses carry no `Connection: keep-alive` header; a strict 1.0 client may
  close anyway.
- `ParseError` duplicates `StatusCode` (and lists codes the parser never returns); `Server` casts
  between them.
- `EpollReactor` dispatches by fd number from a fetched batch: a stale event can reach a new
  connection that reused the fd in the same batch (use a generation id in `epoll_event.data`).
- `flush()` reads `errno` after `closeClientConnection()`; return the `Result` error from
  `modify()` instead. The write buffer has no cap (a non-reading client grows it).
- `EpollReactor` constructor can throw and `main` does not catch it.
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
