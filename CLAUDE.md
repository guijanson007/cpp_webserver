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
server/include/network/EpollReactor.h    epoll fd + fd -> handler table + run()
server/include/network/Connection.h      client fd + read buffer
server/include/server/Server.h           listener, accept loop, client table, clientCallback
server/src/main.cpp                      wiring only
scripts/check_connection_limit.sh        WEB-1 check
scripts/check_reactor.sh                 WEB-2 check (strace double close, SIGSTOP/SIGCONT)
docs/tickets/WEB-<n>.md                  tickets from /next_task
```

## Current architecture (after WEB-4)

- `EpollReactor` is a dumb dispatcher: `add(fd, events, handler)`, `modify`, `remove`,
  `run()` (one `epoll_wait` pass, returns false on fatal error). Handlers are
  `std::function<void(int fd, uint32_t events)>`. `run()` copies the handler before calling it, so a
  handler may `remove()` itself. It knows nothing about sockets or protocols.
- `Server` owns the listening socket and `unordered_map<int, Connection>`. The listener
  is level-triggered; clients are `EPOLLIN | EPOLLET` and are drained until `EAGAIN`.
  `accept()` loops until `EAGAIN`, sheds connections over the cap with `continue`. On
  `EMFILE`/`ENFILE` it uses a spare `/dev/null` fd: release it, accept and close one
  connection, reopen the spare, so the level-triggered listener does not spin.
- `ServerConfig` (`server/ServerConfig.h`) holds `ip`, `port`, `maxConnections` (default
  100) and `backlogSize` (default 10'000). `main` parses `--port`, `--max-connections`,
  `--backlog` into it. The kernel caps the backlog at `net.core.somaxconn`.
- `Connection` owns its `FileDescriptor` and read buffer. Closing = `reactor.remove(fd)`
  then erase from the map (the destructor closes the fd; never call raw `close` on an
  fd a `Connection` owns).
- `main` constructs reactor then server, `server.start()`, `while (reactor.run())`.
- Event arrays are owned by the reactor (`MAX_EVENTS` constexpr member).

## Workflow conventions

- Tickets: `/next_task` writes `WEB-<n>`; put the id in every commit message. Latest:
  WEB-4 (done, PR open). Next: M1 cleanup ticket, then M2 (HTTP parser).
- Verification scripts live in `scripts/`: `check_connection_limit.sh` (WEB-1),
  `check_reactor.sh` (WEB-2, uses strace and `ss`), `check_write_path.sh` (WEB-3),
  `check_scale.sh` (WEB-4: cap, 10k idle connections, fd exhaustion). All pass on `web-4`.
  Scripts count connections with `ss`, not server logs. Pass `--max-connections` explicitly.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Needs CMake >= 4.3. Formatting: `.clang-format` in repo root.

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

Open:

- Argument parsing: `std::stoi` accepts trailing garbage and there are no range checks
  for port, cap or backlog.
- `EMFILE` message prints with `endl` on every discard (rate-limit or drop).
- `MAX_EVENTS` is 10'000, a guess. Settle by profiling.
- Leftover includes (`<asm-generic/socket.h>` in `Server.h`), unused `getFd()` on the
  reactor; `accept4` is still called directly in `Server`.
- Wire protocol (NUL-padded frames) disappears once HTTP lands.
- `flush()` calls `modify()` on every successful flush, and again on every `EAGAIN` even
  when `EPOLLOUT` is already armed (seen with `strace -e epoll_ctl`). Only call it on a
  state change (`isWaitingForWrite()`).
- `processFrames` does `buf.erase(0, pos + 1)` per frame: quadratic on big bursts
  (measured: 20k frames 0.37 s, 40k 0.53 s, 80k 3.39 s). Profiling candidate: parse with
  an offset and compact once.
- `readSome` drains until `EAGAIN` with no cap, so one fast sender can starve the other
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
