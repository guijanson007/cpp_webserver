## WEB-12: Turn handler exceptions into 500 and make `Result` `[[nodiscard]]`

**Type:** Bug
**Milestone:** M3 - Routing + static files (hardening before idle timeouts)
**Priority:** High (a throwing handler took the whole server down, breaking "one bad connection must
never take the server down")
**Mode:** Mentor for the router guard and the server close logic (written by the user). Pair for the
router tests.

### Description
Request handlers are user code. `StaticFileHandler` reads whole files into a `std::string`
(`std::bad_alloc`, `std::length_error`), and `std::format` or an empty `std::function` can throw too.
Before this ticket any of those escaped `Server::clientCallback` and `EpollReactor::run()` and reached
`std::terminate`. Now the router catches at the dispatch boundary and answers 500 for that one request.
Two safety nets came with it: `Result` is `[[nodiscard]]`, so ignoring an I/O error is a compiler
warning, and every target builds with `-Wall -Wextra -Wpedantic`.

### Scope
- In:
  - `Router::route` wraps exact and mount dispatch in `try`. `catch (const std::exception &)` logs
    `what()` to `std::cerr`, `catch (...)` logs a generic line. Both return
    `500 Internal Server Error`, empty body, `close_connection = true`.
  - `Server::clientCallback` combines the close flags:
    `resp.close_connection = !req.shouldKeepAlive() || resp.close_connection`, and closes the read side
    from that combined flag.
  - `class [[nodiscard]] Result` (both the primary template and `Result<void, E>`).
    `closeClientConnection` discards `m_reactor.remove(fd)` explicitly with `std::ignore`.
  - `add_compile_options(-Wall -Wextra -Wpedantic)` in the top-level `CMakeLists.txt`. The two client
    warnings it exposed (unused `signum`, unused `fd_socket`) are fixed.
- Out: idle timeouts (next ticket), `flush()` reading `errno` after `closeClientConnection()`, the
  duplicated mount 405, the `EpollReactor` constructor throwing, `-Werror` (with CI in M4).

### Acceptance criteria
- [x] A handler throwing `std::runtime_error` or `std::bad_alloc` -> 500, empty body
- [x] A handler throwing a non-`std::exception` (`throw 42`) -> 500
- [x] The 500 requests close even for an HTTP/1.1 keep-alive request
- [x] The router keeps serving after a throw
- [x] `Result` is `[[nodiscard]]`; the one intentional discard is explicit
- [x] Clean rebuild with all warning flags on every target: 0 warnings
- [x] Tests: `RouterTest.ThrowingHandlerReturns500`, `NonStdExceptionReturns500`,
      `ThrowingMountHandlerReturns500`, `ThrowingHeadFallbackReturns500`,
      `ThrowingHandlerRequestsClose`, `RouterStillServesAfterAThrow`
- [x] All `scripts/check_*.sh` pass
- [x] Commits carry `WEB-12`

## Findings

### Design
- **Guard in `Router::route`, not in `Server`.** The router is the dispatch boundary and is pure over a
  `Request`, so the 500 path is unit-testable without a socket. The cost: an exception thrown in
  `Server` itself (`serialize()`, `queueWrite`) is still not caught. Those are allocation failures on
  the reactor thread, where giving up is acceptable.
- **A 500 always closes.** After a throw the handler's state is unknown, so the connection is not
  reused. The exception text goes to `std::cerr`, never into the body (no internals leaked).
- **`[[nodiscard]]` on the type, not on each function.** Every function returning `Result` now warns
  when its result is dropped, including future ones (the `timerfd` calls in the idle-timeouts ticket).

### Bugs found and fixed on this branch
- First version of the guard only caught `std::exception`; `throw 42` still terminated. Added
  `catch (...)`.
- First version returned `!request.shouldKeepAlive()` for the 500, so a keep-alive request kept the
  connection after a failed handler. Now `true` (caught by `ThrowingHandlerRequestsClose`).
- **Regression caught by `check_http.sh`:** the first server change replaced the
  `close_conn = !req.shouldKeepAlive()` overwrite with `resp.close_connection` alone. The demo handlers
  return `close = false`, so `Connection: close` and HTTP/1.0 requests stayed open with no
  `Connection: close` header (`http10` and `conn-close` failed; a pipelined second request after
  `Connection: close` was answered). Fixed by combining the two flags.
- The first "warning free" claim came from an incremental build; a clean rebuild showed two warnings in
  `client/src/main.cpp`. Fixed.

### Still open (not part of this ticket)
- `std::ignore = expr` is only specified for `std::tie` until C++26 (works on GCC and Clang). The rest
  of the code uses `static_cast<void>(...)`; pick one style.
- `add_compile_options` also applies the flags to the `FetchContent` GoogleTest build. Clean today; an
  `INTERFACE` warnings target would keep third-party code out.
- The 405 for non-GET/HEAD under a mount is built in both `Router::route` and `StaticFileHandler`.
- `flush()` reads `errno` after `closeClientConnection()`.

## Verification
Environment: Linux 7.3.3-200.fc44.x86_64, glibc 2.43, GCC 16.2.1.

- Fresh build (`rm -rf build`, Debug, `-Wall -Wextra -Wpedantic` on every target): 0 warnings, 0 errors.
- `ctest --test-dir build`: 247/247 pass, 1 disabled
  (`KeepAliveTest.DISABLED_RepeatedConnectionHeadersAreCombined`). 6 new tests.
- `scripts/check_http.sh`: 53 PASS, 0 FAIL (`http10` and `conn-close` failed mid-branch, see above).
- `scripts/check_args.sh` (35), `check_connection_limit.sh` (3), `check_reactor.sh` (4),
  `check_scale.sh` (8), `check_write_path.sh` (7): all pass.
- No route in `main` throws, so the 500 path is covered by the unit tests only.
