# Epoll HTTP Server in C++20

A Linux-only HTTP/1.1 server built on a hand-written `epoll` reactor. It is both a
resume project and a vehicle for learning modern C++20. The goal is a finished,
tested and benchmarked server where every design decision is backed by profiling data.

> **Status: work in progress.** The networking core (reactor, non-blocking I/O, write
> path, connection limits, error handling) is done, and the server now speaks HTTP/1.1
> requests: it parses heads and `Content-Length` bodies (413 over 1 MiB), dispatches
> them through a small router (exact paths, prefix mounts, 404, 405 with `Allow`), serves
> static files from `--root` under `/static/` with MIME types and kernel-enforced traversal
> protection, and supports keep-alive, pipelining and `HEAD`. A handler that throws gets a
> 500 instead of taking the server down. Idle keep-alive connections are closed after
> `--idle-timeout` seconds (WEB-13), which completes M3. An opt-in ASan + UBSan build runs every
> test and check script clean (WEB-14). CI and clang-tidy are next.
> See [Progress](#progress).

## Platform and design decisions

- **Linux only.** The server uses `epoll` directly. There is no portability layer
  and none is planned.
- **C++20 only** (`CMAKE_CXX_STANDARD 20`, compiler extensions off). `std::expected`
  (C++23) is not used.
- **Zero runtime dependencies.** Only the standard library and Linux syscalls.
  GoogleTest is a dev-only dependency, fetched by CMake.
- **No exceptions on I/O paths.** Failures are values: `Result<T, Error>`, marked
  `[[nodiscard]]` so a dropped error is a compiler warning. One bad connection must never
  take the server down: an exception from a route handler becomes a 500 (WEB-12).
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
                 |  list<TimerEntry> (LRU)       |  parse -> route -> respond per wakeup,
                 |  periodic timerfd             |  idle sweep
                 +-------+---------------+-------+
                         |               |
         +---------------v-----+   +-----v---------------------------+
         |  Connection         |   |  http:: (pure, no sockets)      |
         |  FileDescriptor,    |   |  parse(buffer, limits)          |
         |  read/write buffers |   |    -> Done{Request, consumed}   |
         +---------------+-----+   |     | NeedMore | ParseError    |
                         |         |  Router::route(Request)         |
         +---------------v-----+   |    -> Response (200/404/405)    |
         |  SocketIo           |   |  Request::shouldKeepAlive()     |
         |  readAllAvailable / |   |  Response{...}.serialize()      |
         |  writeSome (Result) |   |  StaticFileHandler (/static/)   |
         +---------------------+   |    -> FileReader: openat2 under |
                                   |       the root fd (BENEATH)     |
                                   +---------------------------------+
```

### Components

| Component | Location | Role |
| --- | --- | --- |
| `FileDescriptor` | `common/include/common/` | RAII, move-only owner of a file descriptor. |
| `Error` | `common/include/common/` | Wraps `std::error_code`. Build it with `Error::from_errno(errno)`. |
| `Result<T, E>` | `common/include/common/` | Success-or-error type (C++20 stand-in for `std::expected`). Also `Result<void>`. |
| `EpollReactor` | `server/include/network/` | Dumb dispatcher. `run()` does one `epoll_wait` pass and calls the registered handler. A handler may remove itself safely. |
| `SocketIo` | `server/include/network/` | Non-blocking read and write wrappers. EOF and `EAGAIN` are success values, not errors. |
| `Connection` | `server/include/network/` | Client fd plus read and write buffers, and an iterator to its `TimerEntry` in the server's idle list. Closing a connection closes its fd through the destructor. |
| `http::parse` | `server/include/http/Parser.h` | Stateless, incremental request parser (head and `Content-Length` body). Returns a `std::variant<Done, NeedMore, ParseError>`. `Done` holds a `Request` plus `consumed_bytes`. |
| `Request` | `server/include/http/Request.h` | `string_view`s into the read buffer (method, target, version, raw headers, body) with a case-insensitive `getHeaderValue()` and `shouldKeepAlive()`. Also `SizeLimits`. |
| `Response` | `server/include/http/Response.h` | Status, body and extra headers. `serialize()` always emits `Content-Length`, plus `Connection: close` when asked. `StatusCode` and `statusCodeToReasonPhrase()` live here. |
| `Router` | `server/include/http/Router.h` | Exact-path router plus prefix mounts. `add(method, path, handler)` and `mount(prefix, handler)` take anything satisfying the `RequestHandler` concept (`Response(const Request&)`). `route()` strips the query string, tries exact routes first, then the longest matching mount (GET/HEAD only), returns 404 for an unknown path, 405 with `Allow` for a wrong method, and lets HEAD use the GET handler. |
| `StaticFileHandler` | `server/include/http/StaticFileHandler.h` | Serves files under a mount prefix from a root directory fd. Percent-decodes, refuses `.` segments (including `..` and dotfiles), backslash and NUL, then reads through `FileReader`. 404 for anything missing, refused or not a regular file. |
| `FileReader` | `common/include/common/FileReader.h` | File wrapper layer. `openDirectory()` (`O_PATH`) and `readRegularFileBeneath(dirfd, rel)` (`openat2` with `RESOLVE_BENEATH`, `fstat` + `S_ISREG`). |
| `MimeTypes` | `server/include/http/MimeTypes.h` | Extension to `Content-Type`, case-insensitive, `application/octet-stream` fallback. |
| `Server` | `server/include/server/` | Listener, accept loop, connection table, cap enforcement, `EMFILE` handling, the per-connection parse and respond loop, and the idle sweep (periodic `timerfd` plus an LRU list of deadlines). |
| `ServerConfig` | `server/include/server/` | `ip`, `port`, `maxConnections`, `backlogSize`, `rootDirectory` (optional, required by `main`), `idleTimeout` (seconds, 0 disables), `sweepInterval`. `ServerConfigParser` validates each flag. |

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
- **Idle timeouts (WEB-13).** Each connection has a deadline, `now + --idle-timeout`, pushed back on
  every event it receives. Deadlines live in a `std::list` kept in last-activity order: a refresh
  `splice`s the entry to the back (O(1), via the iterator stored in `Connection`). One periodic
  `timerfd` (`EPOLLET`, drained on every tick) runs a sweep every `sweepInterval` (1 s) that closes
  expired connections from the front and stops at the first live one. A connection closes between
  `T` and `T + 1 s` after its last activity. `--idle-timeout 0` disables it. A client that trickles
  bytes keeps resetting its deadline (no request-head deadline yet).

### Request handling (WEB-7)

- **Loop.** After each drain of the socket, `Server` calls `http::parse` repeatedly on the
  read buffer. `Done` queues a response and drops the consumed bytes, `NeedMore` waits for
  the next read, and a `ParseError` queues an error response and stops.
- **Responses.** Every parsed request goes through `Router::route` (WEB-10). `HEAD` gets the
  same headers as the GET handler (including `Content-Length`) and no body. The server sets
  `Connection: close` on the response when the connection will close.
- **Pipelining.** Several requests in one read are answered in order, and the output is
  flushed once.
- **Keep-alive.** `Request::shouldKeepAlive()`: HTTP/1.1 stays open unless `Connection`
  contains `close`. HTTP/1.0 closes unless `Connection` contains `keep-alive`. `close` wins
  over `keep-alive`. `Connection` is split on commas and compared case-insensitively. Only
  the first `Connection` header line is looked at (see the roadmap).
- **Errors.** A `ParseError` becomes the response status with `Connection: close`. The
  connection is marked read-closed, so `flush()` closes it after the buffer drains, and any
  bytes after the bad request are dropped.
- **Handler exceptions (WEB-12).** `Router::route` catches anything a handler throws
  (`std::exception` and `...`), logs it to stderr, and answers `500 Internal Server Error`
  with an empty body and `Connection: close`. The server closes when either the client or
  the router asks for it.

### Routing (WEB-10)

- **Matching.** Exact, case-sensitive path match on the request target with the query string
  (`?...`) stripped. No percent-decoding, wildcards or path parameters (roadmap).
- **Statuses.** Unknown path: 404. Known path, method not registered: 405 with `Allow`
  (`GET, HEAD` when a GET handler exists). `HEAD` falls back to the `GET` handler, and the
  server drops the body bytes. 404 and 405 carry `Connection: close` only when the client
  asked to close.
- **Handlers.** Constrained by the `RequestHandler` concept, so lambdas and function pointers
  are accepted and anything else fails at the call site with a readable error. They are stored
  as `std::function`. Handlers must return an owning `Response`, because the `Request` views
  die when the server drops the parsed bytes.
- **Mounts (WEB-11).** `mount(prefix, handler)` serves every path under a prefix. Exact routes
  win over mounts, the longest prefix wins among mounts, and a prefix without a trailing slash
  only matches on a segment boundary (`/assets` matches `/assets/x`, not `/assetsfoo`). Methods
  other than GET and HEAD get 405 with `Allow: GET, HEAD`.
- **Demo routes.** `main` registers `GET /` (`Hello, World!`), `POST /echo` (returns the body)
  and mounts the static file handler on `/static/`.

### Static files (WEB-11)

- **Root.** `--root <dir>` is required. It is opened once at startup as an `O_PATH` directory fd,
  so renaming or replacing the directory afterwards does not change what is served.
- **Traversal protection, two layers.** The handler percent-decodes the path after `/static/`
  and refuses any segment starting with `.` (so `..`, `.` and dotfiles such as `.git`), a
  backslash, a NUL byte or a leading `/`. Then the file is opened with
  `openat2(rootfd, path, RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS)`: the kernel rejects any
  resolution that leaves the root (`EXDEV`), including through symlinks, in one lookup, so there
  is no check-then-open race.
- **Symlinks.** Relative symlinks that stay inside the root are followed. Absolute symlinks are
  refused, even when they point inside the root, and so is a link that leaves the root and comes
  back. Use relative links in a document root.
- **Responses.** 200 with `Content-Type` from the extension (`.html .css .js .json .txt .png .jpg
  .jpeg .gif .svg .ico`, case-insensitive, otherwise `application/octet-stream`). Missing files,
  directories (no `index.html`), FIFOs and refused paths are 404. Malformed percent-encoding is 404.
- **Limits.** The whole file is read into the response on the reactor thread, with no size cap.
  `sendfile()` is a profiling question (see the plan below).

### HTTP parser behaviour

- **Incremental.** `NeedMore` means "append more bytes and call again". Parsing is
  stateless and re-scans from the start, which costs O(N^2) byte-by-byte but is cheap at
  an 8 KiB head limit. Resuming from an offset is a profiling follow-up.
- **Zero-copy.** The `Request` holds views into the caller's buffer. They stay valid only
  until that buffer is modified. `consumed_bytes` tells the caller how much to drop, and
  pipelined bytes after the head are left alone.
- **Limits.** Request line, header block, header count and body are capped by `SizeLimits`
  (defaults 8192 / 8192 / 100 / 1 MiB). Head limits are checked before the terminator
  arrives so a slow peer cannot make the server buffer forever. Over-limit gives 414, 431
  or 413.
- **Strictness (400).** Bare LF or CR, NUL bytes, obs-fold, whitespace before the colon,
  invalid header-name tokens, duplicate `Content-Length` (request smuggling) and, for
  HTTP/1.1, a missing `Host`. Unsupported methods give 501; a well-formed unsupported
  `HTTP/x.y` gives 505.
- **Bodies (WEB-8).** `Content-Length` must be digits only (no sign, no spaces, no overflow,
  no duplicates, else 400). `Request::body` is a view of exactly that many bytes and
  `consumed_bytes` covers head plus body. An incomplete body gives `NeedMore`. A declared
  length over `maxBodySize` gives 413 as soon as the head is parsed, before any body byte
  arrives, and the connection closes. Any `Transfer-Encoding` gives 501 (chunked is
  roadmap-only), which also closes the request-smuggling gap of ignoring it.

## Planned HTTP scope (v1)

- Methods: `GET`, `HEAD`, `POST`.
- Hand-written, incremental parser over `std::string_view`, zero-copy. No llhttp or
  http-parser. **Done for the request line and headers, and wired into the server.**
- Request-line, header and body size limits (414, 431 and 413 done).
- Status codes: 400, 404, 405, 413, 431, 500. **Done (500 since WEB-12).**
- `Content-Length` request bodies. **Done.**
- Keep-alive with idle timeouts.
- Static files with MIME types and path-traversal protection. **Done (WEB-11).**
- Concept-constrained router. **Done (exact paths and prefix mounts).**

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

Every target builds with `-Wall -Wextra -Wpedantic` and a clean GCC build has no warnings
(Clang 22 warns once inside GoogleTest, see Known cleanups).

Sanitizer build (AddressSanitizer + UndefinedBehaviorSanitizer, off by default, WEB-14):

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DWEBSERVER_SANITIZE=ON
cmake --build build-asan
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1
ctest --test-dir build-asan --output-on-failure
for s in scripts/check_*.sh; do "$s" build-asan/server/server; done
```

The flags reach every target, GoogleTest included. UBSan is built with
`-fno-sanitize-recover=undefined`, so undefined behaviour aborts and fails the test instead of
only printing. With GCC and Clang, all unit tests and scripts pass with no sanitizer report,
except the `check_write_path.sh` backpressure case, which is too slow under ASan (see Known
cleanups). LeakSanitizer only checks the unit tests: the scripts stop the server with SIGTERM,
which skips the exit-time leak check until graceful shutdown exists (M6).

Release build:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

## Run

```sh
build/server/server --port 8080 --max-connections 1000 --backlog 10000 --root ./www
```

| Flag | Meaning |
| --- | --- |
| `--port` | TCP port to listen on (1-65535). |
| `--max-connections` | Concurrent connection cap. Pass it explicitly (>= 1). The built-in default is 100. |
| `--backlog` (or `--backlog-size`) | `listen()` backlog (>= 1). Default 10000. |
| `--root` | Document root served under `/static/`. Required; must be an existing directory. |
| `--idle-timeout` | Seconds a connection may stay idle before the server closes it (0-3600, `0` disables). Default 60. |

```sh
curl -v http://localhost:8080/        # 200 OK, "Hello, World!"
curl -I http://localhost:8080/        # same headers, no body
curl -d hello http://localhost:8080/echo     # 200, body "hello"
curl -i http://localhost:8080/nope           # 404
curl -i -X POST http://localhost:8080/       # 405, Allow: GET, HEAD
curl -i http://localhost:8080/static/index.html                  # 200, Content-Type: text/html
curl -i --path-as-is http://localhost:8080/static/../secret.txt  # 404, never the file
printf 'FOO / HTTP/1.1\r\nHost: x\r\n\r\n' | nc localhost 8080   # 501, then the connection closes
```

Arguments are parsed strictly: trailing garbage, signs, empty values, overflow and out-of-range
numbers make the server print an error and exit with status 1. A missing or invalid `--root`
also exits 1, with the reason (`No such file or directory`, `Not a directory`).

Routes: `GET /`, `POST /echo` (demo) and `/static/...` (files from `--root`).

## Test

Unit tests use GoogleTest and live in a single target, `common_tests` (259 tests, 1 of them a
disabled known-gap test). Each test has a 30 s `ctest` timeout so a hang fails:

```sh
ctest --test-dir build --output-on-failure          # everything
build/tests/common_tests --gtest_filter='SocketIo*' # one suite or test
build/tests/common_tests --gtest_filter='Parser*'   # HTTP parser tests
build/tests/common_tests --gtest_filter='Response*:ReasonPhrase*:KeepAlive*'   # WEB-7 tests
build/tests/common_tests --gtest_filter='RouterTest*'   # WEB-10 tests
build/tests/common_tests --gtest_filter='RouterTest.*Throw*:RouterTest.*Exception*'   # WEB-12 tests
build/tests/common_tests --gtest_filter='Mime*:FileReader*:StaticFileHandler*:RouterMount*:ParseRootDirectory*'   # WEB-11 tests
build/tests/common_tests --gtest_filter='ParseIdleTimeout*:ServerIdleTimeout*'   # WEB-13 tests (~13 s)
```

Add new test files to the `common_tests` source list in `tests/CMakeLists.txt`.
`SocketIo` tests use `socketpair`, and the parser, response and keep-alive tests are pure
functions over strings, so none of them need the network. The file and static-handler tests
build a fresh temp directory per test. `ServerIdleTimeoutTest` runs a real `Server` and
`EpollReactor` in-process on a loopback port and drives `run()` itself.

### End-to-end check scripts

These start the server themselves. Each takes `[server-binary] [port]` and defaults to
`build/server/server` and port 8080.

| Script | Verifies |
| --- | --- |
| `scripts/check_connection_limit.sh` | The connection cap is enforced and the server survives going over it. |
| `scripts/check_reactor.sh` | Reactor behaviour (uses `strace` and `ss`). |
| `scripts/check_write_path.sh` | Pipelined responses, RST survival, backpressure (100k pipelined requests, ~8 MB out), half-close flush, `EPOLLOUT` armed only when needed. |
| `scripts/check_scale.sh` | The cap, 10k idle connections, fd exhaustion. |
| `scripts/check_http.sh` | GET, HEAD, keep-alive, HTTP/1.0, `Connection: close`, pipelining, byte-by-byte requests, 400/414/431/501/505 followed by close, POST bodies (framing, pipelining, split, 1 MiB limit, 413 before the body, bad lengths, `Transfer-Encoding`), `curl` GET and POST, routing (404, 405 + `Allow`, HEAD fallback, `/echo`, query string, `Connection: close` on errors), and static files (byte-exact bodies, MIME types, HEAD, 404s, 29 traversal and symlink-escape targets over raw sockets, 405, pipelining). |
| `scripts/check_args.sh` | Command-line validation: bad `--port`, `--max-connections`, `--backlog`, `--root` and `--idle-timeout` values exit 1 with the reason; valid values start the server. |
| `scripts/check_idle_timeout.sh` | Idle timeouts with `--idle-timeout 2`: silent and post-request connections closed in time, active clients kept, active-then-idle closed after the last request, fd reuse after a peer close, 300 idle connections reaped, `--idle-timeout 0` disables. |

The scripts count connections with `ss`, not server logs. All of them start the server with a
temporary `--root`.

## Repository layout

```
common/        shared utilities: FileDescriptor, Error, Result, ParseNumber, FileReader
server/        reactor, sockets, connection handling, HTTP parser, router, static files (http/), main
client/        small client (a load generator is roadmap only)
tests/         GoogleTest unit tests
scripts/       end-to-end check scripts
docs/tickets/  one ticket per unit of work (WEB-<n>)
```

Planned: `docs/PERFORMANCE.md`.

## Progress

### Estimated completion: 83%

```
[#################...]  83%   (39 of 47 required items)
```

How it is computed: the checklist below has 47 required items across milestones M1-M5.
Each counts equally. M6 is optional and excluded. This is a rough measure of item count,
not effort. The HTTP parser and the benchmarking work are likely heavier than the
average done item, so real progress is probably a bit lower.

### Built

**M1: Reactor cleanup (done, 8/8)**

- [x] `EpollReactor` class, with epoll logic out of `main`
- [x] `constexpr` constants instead of `#define`
- [x] Accept crash at the connection cap fixed
- [x] Dead blocking `readUntil` / `ReadMessage` removed
- [x] Write path: per-connection output buffer, `EPOLLOUT` armed on `EAGAIN`, disarmed on drain
- [x] Configurable connection cap and backlog (`ServerConfig` and CLI flags)
- [x] `EMFILE`/`ENFILE` handling, 10k idle connections verified
- [x] Strict command-line validation: `--port` 1-65535, cap and backlog >= 1, exit 1 on bad input (WEB-9)

**M2 (partial): HTTP core, request head (WEB-6)**

- [x] Incremental request parser (`http::parse`, stateless, zero-copy, strict)
- [x] Request-line and header parsing (`Request`, case-insensitive `getHeaderValue`)

**M2 (partial): HTTP responses and wiring (WEB-7)**

- [x] Response builder (`http::Response`, status and reason phrases, `Content-Length`)
- [x] `GET`, `HEAD`, `POST` handling: the parser is wired into `Server`, with keep-alive,
  pipelining and error responses (fixed `200` body; bodies and routing come later)

**M2 (partial): request bodies (WEB-8)**

- [x] `Content-Length` bodies (strict parsing, resumable across partial reads, pipelining-safe)
- [x] Size limits: 414, 431 and 413 (body over `maxBodySize` refused from the head alone)

**M2 (done, 7/7): status codes (WEB-12)**

- [x] Status codes 400, 404, 405, 413, 431, 500 (500 when a route handler throws; 414, 501 and
  505 are returned too)

**M3 (partial): routing (WEB-10)**

- [x] Placeholder echo protocol removed
- [x] Concept-constrained router (`http::Router`, 404/405 with `Allow`, HEAD fallback, query stripped)
- [x] Routing end-to-end checks in `check_http.sh` (8 cases plus curl checks)

**M3 (partial): static files (WEB-11)**

- [x] Static file serving (`StaticFileHandler` mounted on `/static/`, root fd, 404 for non-regular files)
- [x] MIME type table (11 extensions, case-insensitive, `application/octet-stream` fallback)
- [x] Path-traversal protection (`.` segments, backslash and NUL refused; `openat2` with `RESOLVE_BENEATH`)
- [x] Router prefix mounts (`Router::mount`, exact beats prefix, longest prefix wins, segment boundary)
- [x] Required `--root` flag, validated with the OS reason in the error (`ServerConfigParser`)
- [x] Static-file end-to-end checks in `check_http.sh` (10 cases, 29 traversal targets, curl checks) and `--root` cases in `check_args.sh`

**M3 (done, 13/13): keep-alive and idle timeouts (WEB-13)**

- [x] Keep-alive state machine (persistent connections are reused and closed once idle)
- [x] Idle timeouts without a thread per connection (one periodic `timerfd`, LRU deadline list, O(1) refresh)
- [x] `--idle-timeout` flag (0-3600 s, `0` disables, default 60) with `ServerConfigParser::parseIdleTimeout`
- [x] Idle-timeout end-to-end checks: `check_idle_timeout.sh` (11 checks) and `--idle-timeout` cases in `check_args.sh`

**M4 (partial): Quality foundations**

- [x] Router unit tests (`RouterTest`, 13 tests plus concept `static_assert`s) (WEB-10)
- [x] GoogleTest wired in through `FetchContent`
- [x] Unit tests for `Error`, `Result` and `SocketIo` (26 tests)
- [x] Parser unit tests (split at every offset, size limits, malformed input, bodies)
- [x] Response and keep-alive unit tests (`ResponseTest`, `ReasonPhraseTest`, `KeepAliveTest`)
- [x] `parseInteger` unit tests (`ParseNumberTest`, 9 tests) and `check_args.sh` (WEB-9)
- [x] Static file unit tests: `MimeTest` (8), `FileReaderTest` (22), `StaticFileHandlerTest` (33),
  `RouterMountTest` (13), `ParseRootDirectoryTest` (10) (WEB-11)
- [x] Handler-exception unit tests (`RouterTest`, 6 tests: `std::exception`, `throw 42`, mount, HEAD
  fallback, close, still serving) (WEB-12)
- [x] Warning flags (`-Wall -Wextra -Wpedantic`) on every target, zero warnings on a clean build (WEB-12)
- [x] Idle-timeout unit tests: `ParseIdleTimeoutTest` (5) and `ServerIdleTimeoutTest` (6, in-process
  server on loopback) (WEB-13)
- [x] ASan and UBSan builds: `-DWEBSERVER_SANITIZE=ON`, all tests and scripts clean with GCC and
  Clang (WEB-14)

Also done and not counted separately: `Result<T, Error>` error handling with concept
constraints, `SocketIo` wrapper layer, `FileDescriptor` RAII, seven end-to-end check scripts
(the older ones ported to HTTP, plus `check_args.sh` and `check_idle_timeout.sh`), 431/414 size limits in the parser, HTTP/1.0 and
`Connection` token-list handling.

### Planned

**M4: Quality (11/14 done, the eleven done items are listed above)**

- [ ] Buffer unit tests
- [ ] GitHub Actions CI (GCC and Clang)
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

- [x] Strict argument parsing (WEB-9)
- [ ] Rate-limit the `EMFILE` log message
- [ ] Move the direct `accept4` call behind the wrapper layer
- [x] Mark `Result` `[[nodiscard]]` (WEB-12)
- [ ] Bound reads per wakeup so one fast sender cannot starve others
- [ ] Cap the per-connection write buffer
- [x] Remove the unused `HttpMethod::DELETE` (WEB-9)
- [ ] Combine repeated `Connection` headers in `Request::shouldKeepAlive` (RFC 9110 §5.3):
  `Connection: TE` followed by `Connection: close` is read as keep-alive today, because
  `getHeaderValue` returns only the first match. Enable
  `KeepAliveTest.DISABLED_RepeatedConnectionHeadersAreCombined` when fixed.
- [x] Warning flags on every target (WEB-12)
- [x] Catch exceptions from route handlers and answer 500 (WEB-12, in `Router::route`)
- [ ] Build the mount 405 in one place (both `Router::route` and `StaticFileHandler` do it)
- [ ] Idle sweep: pop a timer entry whose connection is missing instead of looping forever (WEB-13)
- [ ] Move the `timerfd` calls in `Server` behind a `TimerFd` wrapper (WEB-13)
- [ ] Request-head deadline against slowloris (a trickling client keeps resetting its idle timer)
- [ ] `check_write_path.sh` backpressure fails under ASan: the per-request `buf.erase` is quadratic
  on 100k pipelined requests (WEB-13, WEB-14; profiling ticket)
- [ ] Clang 22 warns inside GoogleTest 1.15.2 (`-Wcharacter-conversion` in `gtest-printers.h`):
  bump GoogleTest or silence it on the `gtest` target (WEB-14, needed for WEB-15 CI)

### Profiling plan

Every optimisation follows: baseline, profile, hypothesis, change, re-measure. Results go
in `docs/PERFORMANCE.md`. Tools: `perf` and flamegraphs, `perf stat`, `strace -c`,
`heaptrack`, `wrk` with fixed parameters. Profiling build: `-O2 -g -fno-omit-frame-pointer`.

Questions to settle with data: edge- vs level-triggered epoll, read buffer size,
copying vs zero-copy parsing, `read`+`write` vs `sendfile()` (static files are read whole on
the reactor thread today), per-connection buffer reuse,
single thread vs worker pool.

## Contributing workflow

Work is tracked as tickets in `docs/tickets/WEB-<n>.md`. Put the ticket id in every
commit message.
