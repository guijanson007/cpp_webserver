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

Current code is flatter (`server/include/server/Server.h`, `server/src/main.cpp`);
reorganise during M1/M2.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Needs CMake >= 4.3. Formatting: `.clang-format` in repo root.

## Known issues (M1 backlog, found in initial review)

- `Server::accept()` throws "Too many clients" when at the cap and nothing catches
  it, so the process dies. `MAX_CONNECTIONS` (10) is far too low for the 10k goal.
- `MAX_CONNECTIONS` / `MAX_EVENTS` are `#define` macros; use `constexpr`.
- `<algorithm>` not included for `std::remove_if` in `Server::close`.
- `readUntil` / `ReadMessage` are dead code and block on non-blocking sockets.
- `inet_pton` returns 0 for a malformed address, but the check is `< 0`.
- `listen`, `epoll_create1`, `epoll_ctl` results are unchecked.
- Listener accepts only one connection per wakeup; loop until `EAGAIN`.
- Server, epoll, and buffers are all mixed together in `main`.
- No write path (no response buffering, no `EPOLLOUT`).
- Wire protocol (NUL-padded frames) disappears once HTTP lands.

## Interview talking points (prepare per milestone)

- M1: why epoll over select/poll; edge- vs level-triggered and the drain-until-EAGAIN
  rule; RAII for fds; move-only types.
- M2: incremental parsing across partial reads; request smuggling and size limits.
- M3: path traversal; keep-alive state machine; timeouts without a thread per conn.
- M4: sanitizer findings; what the tests caught.
- M5/M6: what profiling changed, with numbers.
