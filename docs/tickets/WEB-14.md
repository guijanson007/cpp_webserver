## WEB-14: Add an opt-in ASan + UBSan build and get a clean sanitizer baseline

**Type:** Task
**Milestone:** M4 - Quality
**Priority:** High (prerequisite for the CI sanitizer job; UBSan had never been run on this code)
**Mode:** Pair (CMake option written by the user; verification and docs by Claude)

### Description
WEB-13 ran ASan once with hand-added flags, which nobody could reproduce and CI could not use. This
ticket makes AddressSanitizer + UndefinedBehaviorSanitizer a one-flag build and records a baseline:
every unit test and every end-to-end script run against a sanitized binary, built with GCC and with
Clang. Next: WEB-15 GitHub Actions CI (GCC + Clang, with a sanitizer job), WEB-16 clang-tidy.

### Scope
- In:
  - `option(WEBSERVER_SANITIZE ... OFF)` in the top-level `CMakeLists.txt`. When on,
    `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined` is added
    with `add_compile_options` and `add_link_options` before the `add_subdirectory` calls, so it
    reaches the server, the client, `common_tests` and GoogleTest.
  - `.gitignore`: `build/` widened to `build*/` so `build-asan/` and similar trees stay untracked.
  - Sanitized build, `ctest` and all `scripts/check_*.sh` with GCC and with Clang.
- Out: CI workflow (WEB-15), clang-tidy (WEB-16), TSan (no threads before M6), MSan (needs an
  instrumented libstdc++), the quadratic `buf.erase` behind the slow backpressure case (profiling
  ticket), graceful shutdown (M6).

### Acceptance criteria
- [x] Default build unchanged: `cmake -S . -B build` adds no sanitizer flag (`WEBSERVER_SANITIZE` is `OFF`)
- [x] `-DWEBSERVER_SANITIZE=ON` builds with GCC: 0 warnings
- [ ] `-DWEBSERVER_SANITIZE=ON` builds with Clang with 0 warnings: **not met**, 1 warning inside
      GoogleTest 1.15.2 (third-party, also in a plain Clang build; see Findings). 0 in project code.
- [x] `ctest` under ASan + UBSan: 258/258 with GCC and with Clang, no sanitizer report
- [x] All `scripts/check_*.sh` against the sanitized server: pass, no sanitizer report, except the known
      `check_write_path.sh` backpressure timing case (documented in WEB-13 and below)
- [x] Findings documented (this file)
- [x] README M4 item "ASan and UBSan builds" ticked, build command in README
- [x] No new raw syscalls; no core code changed
- [x] Commits carry `WEB-14`

## Findings

### Sanitizer results
- **No ASan, LSan or UBSan report** from 258 unit tests or from 7 scripts (131 checks), with either
  compiler. The baseline is clean. The only earlier finding (WEB-13: stack-use-after-scope in 5
  `ParserBodyTest` tests) was already fixed.
- `-fno-sanitize-recover=undefined` matters: without it UBSan prints and continues, and a test with
  undefined behaviour still passes. With it, any UB aborts the process, so a test or script fails.
- Server-side reports cannot hide behind the scripts: each script redirects the server's stderr to a
  temp file it deletes on exit. Both runs used `log_path=` in `ASAN_OPTIONS` / `UBSAN_OPTIONS`, so
  any report would have been written to its own file; none was created. Because recovery is off, a
  report would also abort the server and fail the following checks.

### Limitations (still open)
- **LeakSanitizer never checks the server process in the scripts.** LSan runs at normal exit; the
  scripts stop the server with SIGTERM and the server has no signal handling yet, so it dies without a
  leak check. Leak checking covers the unit tests only, which include an in-process `Server` +
  `EpollReactor` (`ServerIdleTimeoutTest`). Fixed for free once graceful shutdown lands (M6).
- **`check_write_path.sh` backpressure fails under ASan** (GCC and Clang): "a second client was starved
  while the first was stalled". Not a sanitizer report. The per-request `buf.erase` in
  `Server::clientCallback` is quadratic on the 100k pipelined requests; with ASan's slowdown the reactor
  stays busy past the second client's 3 s budget. Same failure on `main` before this ticket (WEB-13).
  Passes in the normal Debug build. CI (WEB-15) must either skip that case under ASan or wait for the
  profiling ticket.
- **Clang 22 warns inside GoogleTest 1.15.2:** `gtest-printers.h:524:35: implicit conversion from
  'char8_t' to 'char32_t' may change the meaning of the represented code unit [-Wcharacter-conversion]`.
  It comes from compiling GoogleTest's own sources with our top-level `-Wall -Wextra`, appears in a plain
  Clang build too, and is not in project code. For CI: bump GoogleTest or add
  `-Wno-character-conversion` to the `gtest` target for Clang only.
- Minor: `-fno-omit-frame-pointer` only affects code generation, so it does nothing in
  `add_link_options`. Harmless; a single flag variable would also remove the duplicated list.

## Verification
Environment: Linux 7.3.3-200.fc44.x86_64, glibc 2.43, GCC 16.2.1, Clang 22.1.8.

Normal build (clean rebuild, Debug, `cmake --build build --clean-first`):
- 0 warnings, `ctest`: 258/258 pass, 1 disabled
  (`KeepAliveTest.DISABLED_RepeatedConnectionHeadersAreCombined`).
- Scripts: `check_args.sh` 45, `check_connection_limit.sh` 3, `check_http.sh` 53,
  `check_idle_timeout.sh` 11, `check_reactor.sh` 4, `check_scale.sh` 8, `check_write_path.sh` 7.
  All PASS, 0 FAIL.

Sanitized builds:

```sh
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:log_path=/tmp/san/asan
export UBSAN_OPTIONS=print_stacktrace=1:log_path=/tmp/san/ubsan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DWEBSERVER_SANITIZE=ON
cmake --build build-asan --clean-first
ctest --test-dir build-asan --output-on-failure
for s in scripts/check_*.sh; do "$s" build-asan/server/server; done
# Clang: same with -DCMAKE_CXX_COMPILER=clang++ in a separate build directory
```

| | GCC 16.2.1 | Clang 22.1.8 |
| --- | --- | --- |
| Clean build warnings | 0 | 1 (GoogleTest header, see above) |
| `ctest` | 258/258 pass | 258/258 pass |
| `check_args.sh` | 45 PASS | 45 PASS |
| `check_connection_limit.sh` | 3 PASS | 3 PASS |
| `check_http.sh` | 53 PASS | 53 PASS |
| `check_idle_timeout.sh` | 11 PASS | 11 PASS |
| `check_reactor.sh` | 4 PASS | 4 PASS |
| `check_scale.sh` | 8 PASS | 8 PASS |
| `check_write_path.sh` | 6 PASS, 1 FAIL (backpressure, known) | 6 PASS, 1 FAIL (backpressure, known) |
| Sanitizer reports (`log_path` files) | none | none |
