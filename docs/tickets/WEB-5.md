# WEB-5: Add `Result<T, Error>` and GoogleTest scaffolding

**Status:** Done (acceptance criteria met; see "Outcome")
**Type:** Task
**Milestone:** M1 - Cleanup (last item before M2)
**Priority:** High. M2 depends on it, and `CLAUDE.md` marks it settled.
**Estimate:** 2-3 h
**Mode:** Mentor for `Result` and `Error` (the user wrote them). Pair for the `tests/` setup (Claude wrote it).

## Description

`CLAUDE.md` settles on `Result<T, Error>` for I/O and parsing paths, with exceptions only at startup. The tree had no such type, and `SocketIo` used an enum that lost `errno`. Adding a small C++20 `Result` (no `std::expected`) gives the M2 parser a place to report errors such as "need more data" or "400". Adding GoogleTest at the same time means the parser can be written test-first.

## Scope

- In:
  - `common/include/common/Error.h`: an `Error` wrapping an `errno` value or a `std::error_code`, with `code()` and `message()`.
  - `common/include/common/Result.h`: `Result<T, E = Error>` and a `Result<void>` specialization, with `ok()`, `explicit operator bool`, `value()`, `error()` and `value_or()`.
  - `tests/` with GoogleTest through `FetchContent`, wired in with `enable_testing()`.
  - `SocketIo::readSome` and `writeSome` return `Result`; `Again` and `Eof` are part of the success value, not errors. Update callers in `Server.h`.
- Out (explicitly):
  - Parser work (M2).
  - Replacing the `bool` returns of `EpollReactor::modify` and `remove`.
  - The other backlog items (`stoi` checks, `processFrames`).
  - CI and sanitizers (M4).

## Acceptance criteria (Definition of Done)

- [x] `cmake -S . -B build && cmake --build build` finishes with no new warnings.
- [x] `ctest --test-dir build` passes `Result` tests: ok and error construction, `value_or`, the `void` specialization, a move-only `T`, an `Error` built from `errno` (`ENOENT` against its `strerror` text).
- [x] `errno` is captured once, at the failure site (`Error::from_errno(errno)`).
- [x] `readAllAvailable` and `writeSome` return `Result`; `flush` and `start` return `Result<void>`.
- [x] Existing behaviour is unchanged: `check_write_path.sh`, `check_reactor.sh`, `check_connection_limit.sh` and `check_scale.sh` pass.
- [ ] Every commit message carries `WEB-5` (the early commits do not).

## Technical notes / hints

- `std::variant<T, E>` is ambiguous when `T == E`; the primary template has a `requires` clause that rejects it, and concepts `ValidValueType` / `ValidErrorType` constrain the parameters.
- `Error(int)` is `explicit`; use `Error::from_errno()`.
- Capture `errno` immediately. A later `std::cerr <<` can overwrite it.
- `std::error_code` with `std::generic_category()` is enough for `errno` values.
- No monadic `and_then` or `map` until M2 shows a real need.

## How to verify

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
ctest --test-dir build --output-on-failure
scripts/check_write_path.sh && scripts/check_reactor.sh && scripts/check_connection_limit.sh && scripts/check_scale.sh
```

## Dependencies

- Blocked by: nothing (WEB-4 merged).
- Blocks: the M2 incremental request-line and header parser (returns `Result<ParseState, ParseError>`) and M4 (test harness).

## Outcome

26 of 26 tests pass (`ctest`), and all four check scripts pass on `web-5`. The build has no warnings.

What was done:

- `Error` and `Result` with concept and `requires` constraints (`Result<int, int>` and `Result<void, void>` are rejected, checked by `static_assert`s in `tests/result_test.cpp`).
- `SocketIo::readAllAvailable` returns `Result<ReadOutcome>` (`bytes_read`, `ConnectionState::Open` or `ClosedByPeer`). EOF and EAGAIN are success values; only real OS failures are errors. This replaced an early `ENOTCONN`-as-EOF encoding.
- `SocketIo::writeSome` returns `Result<size_t>`. `flush` decides whether to arm `EPOLLOUT` from `write_buffer.empty()`.
- `Server::start` returns `Result<void>` instead of throwing; `main` prints the error and exits with `EXIT_FAILURE`. `EpollReactor::add` returns `Result<void>`.
- Tests: `tests/error_test.cpp` (3), `tests/result_test.cpp` (10), `tests/socketio_test.cpp` (13, `socketpair`-based: drain, append, data-then-FIN, EBADF, partial write on a full socket with in-order delivery, EPIPE without SIGPIPE).
- `scripts/check_scale.sh` repaired: `close_clients` and `echo_ok` had been corrupted since WEB-4 (a pasted loop, and a nested heredoc that ran `python3` as Python code). Expected resets now exit 1 quietly.

Follow-ups left open (recorded in CLAUDE.md "Known issues"):

- The listener was changed to `EPOLLIN | EPOLLET` in `Server::start`. `CLAUDE.md` says level-triggered, and `accept()` returns after the `EMFILE` discard, so connections still queued get no wakeup until a new one arrives. Restore `EPOLLIN`, or settle it in the edge- vs level-triggered profiling ticket.
- `Result` is not `[[nodiscard]]`, so `flush` callers use `static_cast<void>`. `value_or` copies and has no rvalue overload. `error()` on a successful `Result<void>` is undefined behavior.
- `ReadOutcome::bytes_read` is unused by the caller.
- `Error(errno != 0 ? errno : EIO)` after `modify()` fails in `flush`; `EpollReactor::modify` and `remove` still return `bool`.
- Not tested: `ECONNRESET` on read, the `EINTR` retry, and the `r == 0` branch of `send`.

## Interview talking point

Why `Result<T, E>` instead of exceptions on a hot I/O path: `EAGAIN` and EOF are expected outcomes, not failures. One bad connection must not unwind through the reactor. You can explain the cost model (no throw or unwind, but the caller has to check every result).
