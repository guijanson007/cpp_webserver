## WEB-16: Add a curated clang-tidy config and a clang-tidy CI job

**Type:** Tech Debt
**Milestone:** M4 - Quality
**Priority:** High (last open M4 tooling item; M5 benchmarks should start from linted code)
**Mode:** Split. Pair for `.clang-tidy`, `tests/.clang-tidy`, `scripts/run_clang_tidy.sh`, the CI job,
the test-file fixes, the client removal and the one-line startup flush in `main.cpp` (written by
Claude). Mentor for every finding in the reactor, parser, router, `Server`, `Result` and `main`
(fixed by the user, reviewed by Claude).

### Description
clang-tidy was the only M4 tool not set up. A broad trial run over `server/src/main.cpp` gave ~100
warnings, mostly noise (unchecked container access, magic numbers, pointer arithmetic), with a few
real findings in between. This ticket picks a strict but quiet check set, fixes or justifies every
finding, and makes CI fail on any new one.

### Scope
- In:
  - `.clang-tidy`: all of `bugprone-*`, `performance-*`, `modernize-*`, `cppcoreguidelines-*`,
    `readability-*`, minus 17 checks, each with a one-line reason in the file. `WarningsAsErrors: '*'`.
    `HeaderFilterRegex` limited to `server/` and `common/` headers (GoogleTest and system headers quiet).
  - `tests/.clang-tidy`: inherits the root config and turns off 4 checks that GoogleTest macros and
    fixtures trip, each with a reason.
  - `scripts/run_clang_tidy.sh [build-dir] [file-regex]`: wraps `run-clang-tidy` over the `server/`,
    `common/` and `tests/` translation units; `RUN_CLANG_TIDY` / `CLANG_TIDY` select the binaries.
  - CI job `clang-tidy` on `ubuntu-24.04`: clang-tidy 18 from apt, configure only (no build).
  - Fix every finding (tests: Claude; core: user).
  - Remove the unused `client/` (dead code, 11 findings, and a real bug: it padded every message
    with NUL bytes to 1028).
- Out: compiler `-Werror` (next ticket), clang-format enforcement, buffer unit tests, the `TimerFd`
  wrapper, a non-optional root in `ServerConfig`.

### Acceptance criteria
- [x] `scripts/run_clang_tidy.sh build` exits 0 locally (clang-tidy 22.1.8)
- [ ] The `clang-tidy` CI job is green on the PR (clang-tidy 18.1.3)
- [ ] A deliberate violation turns the `clang-tidy` job red, then reverted (see Verification)
- [x] Every disabled check has a comment; no bare `NOLINT` (there are no `NOLINT`s at all)
- [x] GoogleTest (`build/_deps`) produces no diagnostics
- [x] 258 tests pass; every `scripts/check_*.sh` passes; build has no warnings
- [x] Commits carry `WEB-16`

## Findings

### Check selection
- The trial run was dominated by `cppcoreguidelines-pro-bounds-avoid-unchecked-container-access`
  (33), `avoid-magic-numbers` (20) and `pro-bounds-pointer-arithmetic` (13). They are off, with
  reasons, together with checks that fight the C APIs this server is built on (`reinterpret_cast` to
  `sockaddr *`, `syscall()` varargs), layout checks that belong to `.clang-format`, and
  `function-cognitive-complexity` (the parser is one linear state machine on purpose).
- Kept and enforced: `bugprone-*` (incl. `unchecked-optional-access`, `exception-escape`),
  `performance-*`, rule of five, `avoid-c-arrays`, `init-variables`, designated initializers
  (a C++20 feature this project is meant to use).
- **clang-tidy 18 (CI) reports a subset of clang-tidy 22 (local).** 22 also flagged the designated
  initializer in `Parser.h`, the signed/unsigned comparison in `Server::accept`, `atoi` and a C-style
  cast. Verified in an Ubuntu 24.04 container before the first push. Consequence: green CI does not
  imply a clean local run; run the script locally before pushing.

### Bugs and real findings
- **UB in `Result<void>::error()`** (`bugprone-unchecked-optional-access`, Known issues): it
  dereferenced an empty `optional` on a success. Now throws `std::bad_optional_access`, which matches
  `Result<T>::error()` (`std::get` throws `bad_variant_access`). The `noexcept` is gone. Decision:
  calling `error()` on a success is a programmer error; a throw surfaces it, and in `main` it reaches
  the top-level catch. (An `assert` would fit "exceptions only at startup" more strictly; revisit if
  it ever fires on the reactor path.)
- **Exception escaping `main`** (`bugprone-exception-escape`, Known issues: "`EpollReactor`
  constructor can throw and `main` does not catch it"): `main` now wraps setup and the loop in
  `try` and prints `Fatal: <what>` with exit 1.
- **`Server` was silently movable.** Fixing `cppcoreguidelines-special-member-functions` by removing
  the deleted copy operations made the move constructor implicitly available, and
  `cppcoreguidelines-avoid-const-or-ref-data-members` then fired on `EpollReactor &m_reactor`.
  `Server` registers `[this]` callbacks with the reactor, so a move would leave dangling handlers.
  Fix: all five special members spelled out, copy and move deleted, on `Server` and `EpollReactor`.
  The reference member stays (non-owning, non-null, never reseated).
- **`value_or({})` as an optional-access "fix" in `Router`** (intermediate commit `4bb0a17`): it
  silenced the check but copied the handler `std::function` on every request and would have turned a
  missing handler into `bad_function_call` -> silent 500. Replaced by binding a reference once and
  testing it. A later `?:` with `std::nullopt` reintroduced the copy (mixed branch types make a
  prvalue `optional`); final version checks the index bound first, then binds `methods[method_idx]`.
- **Element read before the bounds check in `Router::dispatchExact`** (intermediate commit): the
  reference was bound before `method_idx >= MAX_METHODS` was tested. Not reachable (3 methods,
  parser returns 501 for others), fixed by failing fast on the bound.
- **Startup line lost when stdout is redirected** (regression from `std::endl` -> `"\n"`, caught by
  `check_args.sh`): with `sync_with_stdio(false)` and stdout fully buffered, the "listening on port"
  line stayed in the buffer, the reactor loop never returns and SIGTERM does not flush. Fix:
  `<< "\n" << std::flush`. `check_args.sh` passed on `main` and failed on the branch before the fix.
- **`client/` sent NUL padding**: every message was copied into a zeroed 1028-byte array and all 1028
  bytes were sent. Nothing used the client, so it was removed rather than fixed.
- **`main.cpp:93` optional access**: `--root` is required, but the check happened in `main` and the
  `.value()` inside `registerRoutes`. `registerRoutes` now takes the `std::filesystem::path`, and
  `main` checks the optional right before dereferencing it.

### Mechanical fixes
`std::endl` -> `"\n"` (8), `std::array` in `SocketIo::readAllAvailable` and the tests,
`std::span<char *>` for `argv` (`argc` parameter dropped), `std::cmp_greater_equal`,
`ConnectionState : uint8_t`, `HttpMethod method{}`, redundant member initialisers and access
specifier, else-after-return/break, designated initializers in `Parser.h` and `ResponseTest`,
braced returns in `RouterTest`.

### Nits left
- The out-of-range `method_idx` branch in `Router::dispatchExact` answers 405 with a fixed
  `Allow: GET, HEAD, POST` instead of the path's methods. Unreachable today.

## Verification

Local: Fedora, Linux 7.3.3-200.fc44, GCC 16.2.1, clang-tidy 22.1.8. Clean rebuild
(`cmake --fresh`, `--clean-first`) of `d989036`:

| Check | Result |
| --- | --- |
| Build (GCC Debug) | 0 warnings |
| `ctest --test-dir build` | 258/258 passed (1 disabled, unchanged) |
| `scripts/run_clang_tidy.sh build` | exit 0, 14 of 18 TUs linted (the 4 GoogleTest TUs skipped) |
| `check_args.sh` | ALL CHECKS PASSED |
| `check_connection_limit.sh` | All checks passed. |
| `check_http.sh` | All HTTP checks passed. |
| `check_idle_timeout.sh` | ALL CHECKS PASSED |
| `check_reactor.sh` | All executed checks passed. (4 PASS) |
| `check_scale.sh` | All checks passed. |
| `check_write_path.sh` | All write-path checks passed. |

Ubuntu 24.04 container (clang-tidy 18.1.3, `CXX=clang++-18`, CMake 4.3.4), before the core fixes:
the script ran, exit 1, and reported a strict subset of the clang-tidy 22 findings.

CI: see the PR checks (filled in below once runs complete).
