# Epoll HTTP Server in C++20

A Linux-only HTTP/1.1 server built on a hand-written `epoll` reactor. It is both a
resume project and a vehicle for learning modern C++20. The goal is a finished,
tested and benchmarked server where every design decision is backed by profiling data.

> **Status: work in progress.** The networking core (reactor, non-blocking I/O, write
> path, connection limits, error handling) is done, and the server now speaks HTTP/1.1
> request heads: it parses them, answers with a fixed `200 OK` (or a 4xx/5xx error),
> and supports keep-alive, pipelining and `HEAD`. There is no router, no request body
> handling and no static files yet, so every successful request gets the same
> `Hello, World!` body. See [Progress](#progress).

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
                 |  unordered_map<fd, Connection>|  connection cap, EMFILE handling,
                 |                               |  parse -> respond loop per wakeup
                 +-------+---------------+-------+
                         |               |
         +---------------v-----+   +-----v---------------------------+
         |  Connection         |   |  http:: (pure, no sockets)      |
         |  FileDescriptor,    |   |  parse(buffer, limits)          |
         |  read/write buffers |   |    -> Done{Request, consumed}   |
         +---------------+-----+   |     | NeedMore | ParseError    |
                         |         |  Request::shouldKeepAlive()     |
         +---------------v-----+   |  Response{...}.serialize()      |
         |  SocketIo           |   +---------------------------------+
         |  readAllAvailable / |
         |  writeSome (Result) |
         +---------------------+
```

### Components

| Component | Location | Role |
| --- | --- | --- |
| `FileDescriptor` | `common/include/common/` | RAII, move-only owner of a file descriptor. |
| `Error` | `common/include/common/` | Wraps `std::error_code`. Build it with `Error::from_errno(errno)`. |
| `Result<T, E>` | `common/include/common/` | Success-or-error type (C++20 stand-in for `std::expected`). Also `Result<void>`. |
| `EpollReactor` | `server/include/network/` | Dumb dispatcher. `run()` does one `epoll_wait` pass and calls the registered handler. A handler may remove itself safely. |
| `SocketIo` | `server/include/network/` | Non-blocking read and write wrappers. EOF and `EAGAIN` are success values, not errors. |
| `Connection` | `server/include/network/` | Client fd plus read and write buffers. Closing a connection closes its fd through the destructor. |
| `http::parse` | `server/include/http/Parser.h` | Stateless, incremental request-head parser. Returns a `std::variant<Done, NeedMore, ParseError>`. `Done` holds a `Request` plus `consumed_bytes`. |
| `Request` | `server/include/http/Request.h` | `string_view`s into the read buffer (method, target, version, raw headers) with a case-insensitive `getHeaderValue()` and `shouldKeepAlive()`. Also `SizeLimits`. |
| `Response` | `server/include/http/Response.h` | Status, body and extra headers. `serialize()` always emits `Content-Length`, plus `Connection: close` when asked. `StatusCode` and `statusCodeToReasonPhrase()` live here. |
| `Server` | `server/include/server/` | Listener, accept loop, connection table, cap enforcement, `EMFILE` handling, and the per-connection parse and respond loop. |
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

### Request handling (WEB-7)

- **Loop.** After each drain of the socket, `Server` calls `http::parse` repeatedly on the
  read buffer. `Done` queues a response and drops the consumed bytes, `NeedMore` waits for
  the next read, and a `ParseError` queues an error response and stops.
- **Responses.** Every parsed request gets `200 OK` with a `text/plain` `Hello, World!`
  body. `HEAD` gets the same headers (including `Content-Length`) and no body.
- **Pipelining.** Several requests in one read are answered in order, and the output is
  flushed once.
- **Keep-alive.** `Request::shouldKeepAlive()`: HTTP/1.1 stays open unless `Connection`
  contains `close`. HTTP/1.0 closes unless `Connection` contains `keep-alive`. `close` wins
  over `keep-alive`. `Connection` is split on commas and compared case-insensitively. Only
  the first `Connection` header line is looked at (see the roadmap).
- **Errors.** A `ParseError` becomes the response status with `Connection: close`. The
  connection is marked read-closed, so `flush()` closes it after the buffer drains, and any
  bytes after the bad request are dropped.

### HTTP parser behaviour

- **Incremental.** `NeedMore` means "append more bytes and call again". Parsing is
  stateless and re-scans from the start, which costs O(N^2) byte-by-byte but is cheap at
  an 8 KiB head limit. Resuming from an offset is a profiling follow-up.
- **Zero-copy.** The `Request` holds views into the caller's buffer. They stay valid only
  until that buffer is modified. `consumed_bytes` tells the caller how much to drop, and
  pipelined bytes after the head are left alone.
- **Limits.** Request line, header block and header count are capped by `SizeLimits`
  (defaults 8192 / 8192 / 100), checked before the terminator arrives so a slow peer
  cannot make the server buffer forever. Over-limit gives 414 or 431.
- **Strictness (400).** Bare LF or CR, NUL bytes, obs-fold, whitespace before the colon,
  invalid header-name tokens, duplicate `Content-Length` (request smuggling) and, for
  HTTP/1.1, a missing `Host`. Unsupported methods give 501; a well-formed unsupported
  `HTTP/x.y` gives 505.
- **Not parsed yet.** Request bodies (`Content-Length`), so `Request::body` is empty. A POST
  with a body is not supported until the next ticket.

## Planned HTTP scope (v1)

- Methods: `GET`, `HEAD`, `POST`.
- Hand-written, incremental parser over `std::string_view`, zero-copy. No llhttp or
  http-parser. **Done for the request line and headers, and wired into the server.**
- Request-line and header size limits (431 and 414 done; 413 comes with bodies).
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
| --- | --- |
| `--port` | TCP port to listen on. |
| `--max-connections` | Concurrent connection cap. Pass it explicitly. The built-in default is 100. |
| `--backlog` (or `--backlog-size`) | `listen()` backlog. Default 10000. |

```sh
curl -v http://localhost:8080/        # 200 OK, "Hello, World!"
curl -I http://localhost:8080/        # same headers, no body
printf 'FOO / HTTP/1.1\r\nHost: x\r\n\r\n' | nc localhost 8080   # 501, then the connection closes
```

Every request currently gets the same fixed response. Routing and static files come in M3.

## Test

Unit tests use GoogleTest and live in a single target, `common_tests` (123 tests, 1 disabled
known-gap test):

```sh
ctest --test-dir build --output-on-failure          # everything
build/tests/common_tests --gtest_filter='SocketIo*' # one suite or test
build/tests/common_tests --gtest_filter='Parser*'   # HTTP parser tests
build/tests/common_tests --gtest_filter='Response*:ReasonPhrase*:KeepAlive*'   # WEB-7 tests
```

Add new test files to the `common_tests` source list in `tests/CMakeLists.txt`.
`SocketIo` tests use `socketpair`, and the parser, response and keep-alive tests are pure
functions over strings, so none of them need the network.

### End-to-end check scripts

These start the server themselves. Each takes `[server-binary] [port]` and defaults to
`build/server/server` and port 8080.

| Script | Verifies |
| --- | --- |
| `scripts/check_connection_limit.sh` | The connection cap is enforced and the server survives going over it. |
| `scripts/check_reactor.sh` | Reactor behaviour (uses `strace` and `ss`). |
| `scripts/check_write_path.sh` | Pipelined responses, RST survival, backpressure (100k pipelined requests, ~8 MB out), half-close flush, `EPOLLOUT` armed only when needed. |
| `scripts/check_scale.sh` | The cap, 10k idle connections, fd exhaustion. |
| `scripts/check_http.sh` | GET, HEAD, keep-alive, HTTP/1.0, `Connection: close`, pipelining, byte-by-byte requests, 400/414/431/501/505 followed by close, `curl`. |

The scripts count connections with `ss`, not server logs.

## Repository layout

```
common/        shared utilities: FileDescriptor, Error, Result
server/        reactor, sockets, connection handling, HTTP parser (http/), main
client/        small client (a load generator is roadmap only)
tests/         GoogleTest unit tests
scripts/       end-to-end check scripts
docs/tickets/  one ticket per unit of work (WEB-<n>)
```

Planned: the router in `server/include/http/` and `docs/PERFORMANCE.md`.

## Progress

### Estimated completion: 44%

```
[#########...........]  44%   (15 of 34 required items)
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

**M2 (partial): HTTP core, request head (WEB-6)**

- [x] Incremental request parser (`http::parse`, stateless, zero-copy, strict)
- [x] Request-line and header parsing (`Request`, case-insensitive `getHeaderValue`)

**M2 (partial): HTTP responses and wiring (WEB-7)**

- [x] Response builder (`http::Response`, status and reason phrases, `Content-Length`)
- [x] `GET`, `HEAD`, `POST` handling: the parser is wired into `Server`, with keep-alive,
  pipelining and error responses (fixed `200` body; bodies and routing come later)

**M3 (partial)**

- [x] Placeholder echo protocol removed

**M4 (partial): Quality foundations**

- [x] GoogleTest wired in through `FetchContent`
- [x] Unit tests for `Error`, `Result` and `SocketIo` (26 tests)
- [x] Parser unit tests (48 tests: split at every offset, size limits, malformed input)
- [x] Response and keep-alive unit tests (`ResponseTest`, `ReasonPhraseTest`, `KeepAliveTest`)

Also done and not counted separately: `Result<T, Error>` error handling with concept
constraints, `SocketIo` wrapper layer, `FileDescriptor` RAII, five end-to-end check scripts
(the older ones ported to HTTP), 431/414 size limits in the parser, HTTP/1.0 and
`Connection` token-list handling.

### Planned

**M2: HTTP core (4/7 done, listed above)**

- [ ] Size limits (431 and 414 done in the parser; 413 needs bodies)
- [ ] `Content-Length` bodies (next: WEB-8)
- [ ] Status codes 400, 404, 405, 413, 431, 500 (400, 431, 414, 501, 505 are returned today;
  404 and 405 need the router, 413 needs bodies)

**M3: Routing and static files (1/7)**

- [ ] Concept-constrained router
- [ ] Static file serving
- [ ] MIME type table
- [ ] Path-traversal protection
- [ ] Keep-alive state machine (keep-alive decision and reuse work; idle timeouts do not)
- [ ] Idle timeouts without a thread per connection

**M4: Quality (3/8 done, the three done items are listed above)**

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
- [ ] Cap the per-connection write buffer
- [ ] Remove the unused `HttpMethod::DELETE`
- [ ] Combine repeated `Connection` headers in `Request::shouldKeepAlive` (RFC 9110 §5.3):
  `Connection: TE` followed by `Connection: close` is read as keep-alive today, because
  `getHeaderValue` returns only the first match. Enable
  `KeepAliveTest.DISABLED_RepeatedConnectionHeadersAreCombined` when fixed.
- [ ] Warning flags on the `server` and `common` targets (only tests have them today)

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
