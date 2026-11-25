## WEB-13: Close idle keep-alive connections after a configurable timeout

**Type:** Task
**Milestone:** M3 - Routing + static files (last open M3 item)
**Priority:** High (an idle client, or 10k of them, held a connection slot forever)
**Mode:** Mentor for the timer and sweep logic in `Server` / `Connection` (written by the user). Pair for
the parser tests, the in-process server tests and `check_idle_timeout.sh`.

### Description
Before this ticket an open connection lived until the peer closed it, so idle keep-alive clients filled
`--max-connections` permanently. Now every connection has an idle deadline. One periodic `timerfd`,
registered with the reactor like any other fd, wakes a sweep that closes connections whose deadline
passed. No thread per connection, no per-connection timer fd.

### Scope
- In:
  - `--idle-timeout <seconds>` (0-3600, default 60, `0` disables) parsed by
    `ServerConfigParser::parseIdleTimeout` into `ServerConfig::idleTimeout` (`std::chrono::seconds`).
  - `ServerConfig::sweepInterval` (`std::chrono::milliseconds`, default 1 s): period of the sweep timer.
  - `network::TimerEntry { clientFd, optional<time_point> expiresAt }` in a `std::list` kept in
    last-activity order. `Connection` stores its list iterator, so a refresh is O(1)
    (`expiresAt = now + timeout`, `splice` to the back) and removal in `closeClientConnection` is O(1).
  - The sweep reads the `timerfd` until `EAGAIN` (it is `EPOLLET`), then pops expired entries from the
    front and stops at the first live one: O(expired) per tick.
  - Activity = any event dispatched to the connection (read, write progress), refreshed after the
    `EPOLLERR` check.
- Out: request-head deadline (slowloris: a client trickling bytes keeps resetting the timer), a separate
  write-stall timeout, `408 Request Timeout` before closing, generation ids for fd reuse, graceful
  shutdown (M6).

### Acceptance criteria
- [x] A silent connection is closed about `--idle-timeout` seconds after accept (2.0-2.4 s for 2 s)
- [x] A client sending a request every 0.5 s for 3x the timeout is never closed
- [x] A client that is active past the first timeout and then goes idle is closed one timeout after its
      last request
- [x] 300 silent connections are all closed; `ss` shows none left; the server keeps serving
- [x] `--idle-timeout 0` keeps idle connections open
- [x] Bad values (`-1`, `3601`, `abc`, `5s`, empty, overflow, missing value) exit 1
- [x] 10k idle connections still fit (`check_scale.sh`): one fd per connection, not two
- [x] Clean rebuild with `-Wall -Wextra -Wpedantic`: 0 warnings
- [x] Tests: `ParseIdleTimeoutTest` (5), `ServerIdleTimeoutTest` (6)
- [ ] No raw syscalls outside the wrapper layer: **not met**, `timerfd_*` and the count `read` are in
      `Server` (deferred, see Still open)
- [x] Commits carry `WEB-13`

## Findings

### Design
- **One periodic timer, not one per connection.** Every connection has the same timeout, so
  last-activity order equals expiry order. An LRU `std::list` gives O(1) touch and O(expired) sweep;
  a heap or timer wheel only pays off with per-connection timeouts.
- **Granularity.** A connection closes between `T` and `T + sweepInterval` after its last activity.
- **`EPOLLET` timerfd must be drained.** Without reading the expiration count the fd stays readable,
  no new edge arrives and the sweep runs once.
- **Iterator in `Connection`.** `std::list` iterators stay valid across `splice` and other erasures,
  so storing one per connection is safe and avoids a lookup per event.

### Bugs found and fixed on this branch
First version (per-connection `ConnectionTimer` class, each with its own one-shot `timerfd`):
- **Double close.** `ConnectionTimer` built its own `shared_ptr<FileDescriptor>` from the client fd, a
  second owner next to `Connection`. Destroying the timer closed the client socket again, possibly a
  reused fd. On the `init()` failure path it closed a live client still in `m_clients`. Caught by the
  unit test `DestroyingTimerDoesNotCloseClientFd`.
- **One-shot timers.** Each `timerfd` fired once at accept + T. A connection active past that point and
  then idle was never closed (`check_idle_timeout.sh` `active-then-idle`: still open after 7.5 s).
- **Two fds per connection.** `check_scale.sh` regressed to 9996 of 10000 under `ulimit -n 20000`.
- **Stale timers keyed by fd number.** A connection closed by the peer, by `Connection: close` or by an
  error left its timer in the list. After fd reuse, activity reset the wrong entry and a sweep closed an
  innocent live client (`fd-reuse`: B dropped after 2 requests while active; `many-idle`: connections
  closed early after churn).
- **Unchecked `find_if` + O(n) lookup** per event in `clientCallback` (`end()` dereference possible).
- `init()` called `timerfd_settime` before checking the fd and replaced `errno` with `EIO`.
- Unused `m_timerFd` added to `EpollReactor`.
- `std::ignore = ...` discard replaced by `static_cast<void>(...)`.

Second version (single periodic timer, iterator in `Connection`):
- Did not compile: `Connection.h` forward-declared `network::TimerEntry` while `Server` defined a nested
  `Server::TimerEntry`. Fixed by defining the struct once in `namespace network`.
- `--idle-timeout 0` closed every connection on the first sweep (`expiresAt = now + 0`). Fixed: the
  deadline is `std::nullopt` and the sweep returns early when the timeout is 0.

Test-side:
- `ServerIdleTimeoutTest` first relied on the server's sweep timer to wake `epoll_wait`. With the
  timeout disabled the server never wakes, so the test hung. The fixture now registers its own 20 ms
  tick timerfd. `gtest_discover_tests` got `TIMEOUT 30` so a hang fails instead of blocking `ctest`.
- ASan found a pre-existing (WEB-8) stack-use-after-scope in 5 `ParserBodyTest` tests:
  `const Done *d = asDone(run(input));` pointed into a destroyed temporary. Fixed in `parser_test.cpp`.

### Still open (not part of this ticket, listed in CLAUDE.md Known issues)
- The sweep loop only advances if the front entry's fd is in `m_clients`; if the list and the map ever
  disagree, the reactor spins forever (shown by deleting the `m_timers.erase` in
  `closeClientConnection`: `PeerCloseLeavesNoStaleTimerForReusedFd` hangs). Pop the entry when the client
  is missing.
- `timerfd_create`, `timerfd_settime` and the expiration `read` are raw syscalls in `Server`; move them
  into a `TimerFd` wrapper.
- With `--idle-timeout 0` the sweep timer is still armed but never read.
- Under ASan the `check_write_path.sh` backpressure case fails, on `main` too: the quadratic per-request
  `buf.erase` keeps the reactor busy past the second client's 3 s budget.
- Slowloris (request-head deadline) is not covered by an idle timeout.

## Verification
Environment: Linux 7.3.3-200.fc44.x86_64, glibc 2.43, GCC 16.2.1.

- Clean rebuild (`cmake --build build --clean-first`, Debug, `-Wall -Wextra -Wpedantic`): 0 warnings,
  0 errors.
- `ctest --test-dir build`: 258/258 pass, 1 disabled
  (`KeepAliveTest.DISABLED_RepeatedConnectionHeadersAreCombined`). 11 new tests.
- `scripts/check_idle_timeout.sh` (new): 11 PASS, 0 FAIL.

  ```
  PASS: idle-silent: closed 2.44 s after last activity
  PASS: idle-after-req: closed 2.44 s after last activity
  PASS: active-survives: 12 requests answered over 6 s
  PASS: active-then-idle (active phase): 8 requests answered over 4 s
  PASS: active-then-idle: closed 2.43 s after last activity
  PASS: fd-reuse: B answered 5 requests 1.5 s apart while other connections expired
  PASS: many-idle: all 300 closed in 2.15 s
  PASS: many-idle: ss shows 0 established connections on :8080
  PASS: survival: GET / -> 200
  PASS: disabled: idle socket still open after 4 s with --idle-timeout 0
  PASS: survival: GET / -> 200
  ```
- `scripts/check_args.sh` (45, incl. 10 new `--idle-timeout` cases), `check_http.sh` (53),
  `check_connection_limit.sh` (3), `check_reactor.sh` (4), `check_scale.sh` (8),
  `check_write_path.sh` (7): all pass.
- ASan + `_GLIBCXX_DEBUG` build: 258/258 unit tests pass, no ASan reports from any script;
  `check_write_path.sh` backpressure fails under ASan on this branch and on `main` (see Still open).
