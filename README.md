# Epoll HTTP Server in C++20

A Linux-only HTTP/1.1 server built on a hand-written `epoll` reactor. It is both a
resume project and a vehicle for learning modern C++20. The goal is a finished,
tested and benchmarked server where every design decision is backed by profiling data.

> **Status: work in progress.** The networking core (reactor, non-blocking I/O, write
> path, connection limits, error handling) is done. The HTTP layer is not built yet, so
> the server currently speaks a placeholder NUL-delimited echo protocol, not HTTP.
> See [Progress](#progress).

## Platform and design decisions

- **Linux only.** The server uses `epoll` directly. There is no portability layer
  and none is planned.
- **C++20 only** (`CMAKE_CXX_STANDARD 20`, compiler extensions off). `std::expected`
  (C++23) is not used.
- **Zero runtime dependencies.** Only the standard library and Linux syscalls.
  GoogleTest is a dev-only dependency, fetched by CMake.
- **No exceptions on I/O paths.** Failures are values: `Result<T, Error>`. One bad
  connection must never take the server down.
- **No raw `read`/`write`/`epoll_*` outside a thin wrapper layer.**
- **Single-threaded reactor first.** A worker pool is an optional, late milestone.
- **Out of scope:** TLS, HTTP/2, WebSockets. To get TLS, put the server behind Nginx
  or Caddy.

## Architecture

```
                 +-------------------------------+
  clients  --->  |  listening socket (LT)        |
                 +---------------+---------------+
                                 | EPOLLIN
                 +---------------v---------------+
                 |  EpollReactor                 |  epoll_wait, fd -> handler table
                 |  add / modify / remove / run  |  knows nothing about sockets or HTTP
                 +---------------+---------------+
                                 | handler(fd, events)
                 +---------------v---------------+
                 |  Server                       |  accept loop, connection table,
                 |  unordered_map<fd, Connection>|  connection cap, EMFILE handling
                 +---------------+---------------+
                                 |
                 +---------------v---------------+
                 |  Connection                   |  owns FileDescriptor,
                 |  read buffer / write buffer   |  per-connection buffers
                 +---------------+---------------+
                                 |
                 +---------------v---------------+
                 |  SocketIo                     |  readAllAvailable / writeSome
                 |  (the only raw read/send)     |  return Result<...>
                 +-------------------------------+
```

### Components

| Component | Location | Role |
|---|---|---|
| `FileDescriptor` | `common/include/common/` | RAII, move-only owner of a file descriptor. |
| `Error` | `common/include/common/` | Wraps `std::error_code`. Build it with `Error::from_errno(errno)`. |
| `Result<T, E>` | `common/include/common/` | Success-or-error type (C++20 stand-in for `std::expected`). Also `Result<void>`. |
| `EpollReactor` | `server/include/network/` | Dumb dispatcher. `run()` does one `epoll_wait` pass and calls the registered handler. A handler may remove itself safely. |
| `SocketIo` | `server/include/network/` | Non-blocking read and write wrappers. EOF and `EAGAIN` are success values, not errors. |
| `Connection` | `server/include/network/` | Client fd plus read and write buffers. Closing a connection closes its fd through the destructor. |
| `Server` | `server/include/server/` | Listener, accept loop, connection table, cap enforcement, `EMFILE` handling. |
| `ServerConfig` | `server/include/server/` | `ip`, `port`, `maxConnections`, `backlogSize`. |

### Behaviour worth knowing

- **Event model.** The listener is level-triggered. Client sockets are `EPOLLIN | EPOLLET`
  and are drained until `EAGAIN`.
- **Write path.** Output goes into a per-connection buffer. `EPOLLOUT` is armed only when
  a write hits `EAGAIN` and disarmed once the buffer drains. A half-closed connection
  flushes before it closes. `MSG_NOSIGNAL` prevents `SIGPIPE`.
- **Connection cap.** Connections over `--max-connections` are accepted and shed
  immediately.
- **fd exhaustion.** On `EMFILE`/`ENFILE` the server keeps a spare `/dev/null` fd. It
  releases the spare, accepts and closes one connection, then reopens the spare, so the
  level-triggered listener does not spin.
- **Backlog.** Set separately with `--backlog`. The kernel caps it at `net.core.somaxconn`.

## Planned HTTP scope (v1)

- Methods: `GET`, `HEAD`, `POST`.
- Hand-written, incremental, resumable parser over `std::string_view` / `std::span`,
  zero-copy where possible. No llhttp or http-parser.
- Request-line and header size limits.
- Status codes: 400, 404, 405, 413, 431, 500.
- `Content-Length` request bodies.
- Keep-alive with idle timeouts.
- Static files with MIME types and path-traversal protection.
- Concept-constrained router.

Roadmap only (documented, not built): chunked transfer encoding, `Range` requests, gzip,
libFuzzer fuzzing of the parser, a load-generator client.

### C++20 features this project is meant to exercise

`std::span`, `std::string_view`, concepts (router/handler constraints), `std::format`,
designated initializers for config structs, ranges, `<=>` for small types, and
`std::jthread` + `std::stop_token` if the worker pool is built. Coroutines are an optional
stretch.

## Build

Requirements: Linux, a C++20 compiler (GCC or Clang), CMake >= 4.3. Internet access on the
first configure so CMake can fetch GoogleTest (dev-only).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Release build:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

## Run

```sh
build/server/server --port 8080 --max-connections 1000 --backlog 10000
```

| Flag | Meaning |
|---|---|
| `--port` | TCP port to listen on. |
| `--max-connections` | Concurrent connection cap. Pass it explicitly. The built-in default is 100. |
| `--backlog` (or `--backlog-size`) | `listen()` backlog. Default 10000. |

Until the HTTP layer lands, the server echoes NUL-terminated frames:

```sh
printf 'hello\0' | nc localhost 8080
```

## Test

Unit tests use GoogleTest and live in a single target, `common_tests`:

```sh
ctest --test-dir build --output-on-failure          # everything
build/tests/common_tests --gtest_filter='SocketIo*' # one suite or test
```

Add new test files to the `common_tests` source list in `tests/CMakeLists.txt`.
`SocketIo` tests use `socketpair`, so they need no network.

### End-to-end check scripts

These start the server themselves. Each takes `[server-binary] [port]` and defaults to
`build/server/server` and port 8080.

| Script | Verifies |
|---|---|
| `scripts/check_connection_limit.sh` | The connection cap is enforced and the server survives going over it. |
| `scripts/check_reactor.sh` | Reactor behaviour (uses `strace` and `ss`). |
| `scripts/check_write_path.sh` | Echo, RST survival, backpressure, `EPOLLOUT` armed only when needed. |
| `scripts/check_scale.sh` | The cap, 10k idle connections, fd exhaustion. |

The scripts count connections with `ss`, not server logs.

## Repository layout

```
common/        shared utilities: FileDescriptor, Error, Result
server/        reactor, sockets, connection handling, main
client/        small client (a load generator is roadmap only)
tests/         GoogleTest unit tests
scripts/       end-to-end check scripts
docs/tickets/  one ticket per unit of work (WEB-<n>)
```

Planned: `server/http/` (parser, request/response, router) and `docs/PERFORMANCE.md`.

## Progress

### Estimated completion: 26%

```
[#####...............]  26%   (9 of 34 required items)
```

How it is computed: the checklist below has 34 required items across milestones M1-M5.
Each counts equally. M6 is optional and excluded. This is a rough measure of item count,
not effort. The HTTP parser and the benchmarking work are likely heavier than the
average done item, so real progress is probably a bit lower.

### Built

**M1: Reactor cleanup (done)**
- [x] `EpollReactor` class, with epoll logic out of `main`
- [x] `constexpr` constants instead of `#define`
- [x] Accept crash at the connection cap fixed
- [x] Dead blocking `readUntil` / `ReadMessage` removed
- [x] Write path: per-connection output buffer, `EPOLLOUT` armed on `EAGAIN`, disarmed on drain
- [x] Configurable connection cap and backlog (`ServerConfig` and CLI flags)
- [x] `EMFILE`/`ENFILE` handling, 10k idle connections verified

**M4 (partial): Quality foundations**
- [x] GoogleTest wired in through `FetchContent`
- [x] Unit tests for `Error`, `Result` and `SocketIo` (26 tests)

Also done and not counted separately: `Result<T, Error>` error handling with concept
constraints, `SocketIo` wrapper layer, `FileDescriptor` RAII, four end-to-end check scripts.

### Planned

**M2: HTTP core (0/7)**
- [ ] Incremental, resumable request parser
- [ ] Request-line and header parsing
- [ ] Size limits (413, 431)
- [ ] `Content-Length` bodies
- [ ] Response builder
- [ ] `GET`, `HEAD`, `POST` handling
- [ ] Status codes 400, 404, 405, 413, 431, 500

**M3: Routing and static files (0/7)**
- [ ] Concept-constrained router
- [ ] Static file serving
- [ ] MIME type table
- [ ] Path-traversal protection
- [ ] Keep-alive state machine
- [ ] Idle timeouts without a thread per connection
- [ ] Remove the placeholder echo protocol

**M4: Quality (2/8 done, the two done items are listed above)**
- [ ] Parser unit tests
- [ ] Router unit tests
- [ ] Buffer unit tests
- [ ] GitHub Actions CI (GCC and Clang)
- [ ] ASan and UBSan builds
- [ ] clang-tidy configuration

**M5: Polish (0/5)**
- [ ] Architecture diagram polish and final README
- [ ] `wrk` benchmark against Nginx serving the same static file
- [ ] `docs/PERFORMANCE.md` with before/after tables, committed flamegraphs, CPU model and kernel
- [ ] `curl` and `wrk` both work against the server
- [ ] 10k+ concurrent keep-alive connections without crashing

**M6: Optional, only after M5 ships**
- [ ] `std::jthread` worker pool
- [ ] Graceful shutdown on SIGINT/SIGTERM
- [ ] Profile against the single-threaded server

**Known cleanups (small tickets)**
- [ ] Strict argument parsing (`std::stoi` accepts trailing garbage, no range checks)
- [ ] Rate-limit the `EMFILE` log message
- [ ] Move the direct `accept4` call behind the wrapper layer
- [ ] Mark `Result` `[[nodiscard]]`
- [ ] Bound reads per wakeup so one fast sender cannot starve others

### Profiling plan

Every optimisation follows: baseline, profile, hypothesis, change, re-measure. Results go
in `docs/PERFORMANCE.md`. Tools: `perf` and flamegraphs, `perf stat`, `strace -c`,
`heaptrack`, `wrk` with fixed parameters. Profiling build: `-O2 -g -fno-omit-frame-pointer`.

Questions to settle with data: edge- vs level-triggered epoll, read buffer size,
copying vs zero-copy parsing, `read`+`write` vs `sendfile()`, per-connection buffer reuse,
single thread vs worker pool.

## Contributing workflow

Work is tracked as tickets in `docs/tickets/WEB-<n>.md`. Put the ticket id in every
commit message.
