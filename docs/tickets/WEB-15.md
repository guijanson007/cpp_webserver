## WEB-15: Add GitHub Actions CI (GCC, Clang, sanitizer job)

**Type:** Task
**Milestone:** M4 - Quality
**Priority:** High ("CI green" is the first Definition of Done item, and the other M4 items build on it)
**Mode:** Pair (workflow, CMake and docs written by Claude; the one-line `FileReader.h` include fix
was found by CI)

### Description
There was no automated check: every result so far came from one Fedora machine with GCC 16 and
Clang 22. This ticket adds a GitHub Actions workflow that builds and tests every push and pull
request to `main` with GCC, with Clang, and under ASan + UBSan. That covers the "CI green"
Definition of Done item and runs the code on an older toolchain than the dev machine.
Next: WEB-16 clang-tidy (as a CI job), then `-Werror`.

### Scope
- In:
  - `.github/workflows/ci.yml` on `push` / `pull_request` to `main` and `workflow_dispatch`.
  - Matrix `build (gcc)` / `build (clang)`: Debug build, `ctest --output-on-failure --parallel 4`;
    the GCC job also runs `scripts/check_http.sh build/server/server 18080`.
  - `sanitize (asan + ubsan, gcc)`: `-DWEBSERVER_SANITIZE=ON`, ctest with
    `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1` and `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`.
  - CMake 4.3.4 pinned via `pipx` (`cmake_minimum_required(VERSION 4.3)`); every job prints CMake,
    compiler and kernel versions.
  - Fix the WEB-14 open item: Clang 21+ `-Wcharacter-conversion` inside GoogleTest 1.15.2.
  - README badge, CI section, M4 checkbox; CLAUDE.md updates.
- Out: clang-tidy (WEB-16), `-Werror` (follow-up), `check_scale.sh` (needs `ulimit -n 20000` and
  10k sockets), `check_reactor.sh` (needs strace / ptrace), `check_write_path.sh` under ASan (known
  slow backpressure case, profiling ticket), Release builds, caching.

### Acceptance criteria
- [x] All jobs green on PR #14 (run 45864902329)
- [x] Clean Clang 22 build has no warnings from GoogleTest (was 1); 0 warnings in the whole CI log
- [x] The CI log shows CMake, compiler and kernel versions for each job
- [x] A deliberately broken test turns CI red (see Verification), then reverted
- [x] The sanitizer job cannot pass with a report: `abort_on_error=1`, `halt_on_error=1`, and the
      build already uses `-fno-sanitize-recover=undefined`
- [x] README badge, CI section, M4 item ticked; CLAUDE.md "No CI" line and WEB-14 open item updated
- [x] Commits carry `WEB-15`

## Findings

- **The first CI run failed in all three jobs on a real portability bug.**
  `common/include/common/FileReader.h` used `struct open_how`, `RESOLVE_BENEATH` and
  `RESOLVE_NO_MAGICLINKS` with only `<fcntl.h>` included. Fedora's glibc 2.43 declares them there
  (`bits/openat2.h`); Ubuntu 24.04's glibc 2.39 does not, so `openat2` traversal protection
  (WEB-11) only compiled on recent glibc. Fix: `#include <linux/openat2.h>`. Checked that it
  coexists with glibc's own definitions (glibc guards with `__glibc_has_open_how` / `#ifndef`)
  with GCC and Clang, in both include orders.
- **GoogleTest warning:** it comes only from `gtest-all.cc` (the `gtest` target), not from our test
  sources. Fix: `-Wno-character-conversion` on `gtest`, `gtest_main`, `gmock`, `gmock_main`, only
  when `check_cxx_compiler_flag(-Wcharacter-conversion)` succeeds. The positive form is probed
  because GCC silently accepts any `-Wno-<unknown>` and older Clang warns about an unknown one.
  Bumping GoogleTest was not chosen: no guarantee a newer release fixes it, and the flag is scoped
  to third-party code.
- **Runner toolchain:** Ubuntu 24.04, GCC 13.3, Clang 18.1.3, glibc 2.39. All 258 tests pass with
  both, so nothing in the code needed GCC 16 (`std::format` is fine from GCC 13).
- **ASan on runners:** the sanitize job runs `sysctl vm.mmap_rnd_bits=28`. Recent runner kernels use
  32 bits of mmap ASLR entropy, which can break ASan's shadow memory layout with random
  `AddressSanitizer:DEADLYSIGNAL` crashes.
- **`actions/checkout@v4`** produced a Node.js 20 deprecation annotation on every job; bumped to
  `@v5` (Node 24), no annotations since.
- `ctest --parallel 4` is safe: `ServerIdleTimeoutTest` asks the kernel for a free port, nothing
  binds a fixed one.

## Verification

Local (Fedora, Linux 7.3.3, glibc 2.43, GCC 16.2.1, Clang 22.1.8):

| Build | Warnings | `ctest` |
| --- | --- | --- |
| GCC Debug | 0 | 258/258 |
| Clang Debug, clean configure | 0 (was 1) | 258/258 |
| GCC `WEBSERVER_SANITIZE=ON`, CI env vars | 0 | 258/258 |

`scripts/check_http.sh build/server/server 18080`: all passed.

CI (`ubuntu-24.04`, CMake 4.3.4):

| Run | Commit | Result |
| --- | --- | --- |
| [45864208718](https://github.com/guijanson007/cpp_webserver/actions/runs/45864208718) | `81903fb` workflow added | red: all 3 jobs, `'open_how' was not declared` (real bug, above) |
| [45864565960](https://github.com/guijanson007/cpp_webserver/actions/runs/45864565960) | `478747f` `<linux/openat2.h>` | green, Node 20 annotations |
| [45864902329](https://github.com/guijanson007/cpp_webserver/actions/runs/45864902329) | checkout@v5 | green, no annotations, 0 warnings in the log |
| [45865229400](https://github.com/guijanson007/cpp_webserver/actions/runs/45865229400) | `afc2a57` deliberate `FAIL()` test | red as intended: all 3 jobs failed at "Unit tests" (`CiRedCheck.DeliberateFailure`, 258/259 passed); reverted in the next commit |

Per job on the green run: `build (gcc)` GCC 13.3.0, 258/258, "All HTTP checks passed."; `build
(clang)` Clang 18.1.3, 258/258; `sanitize` GCC 13.3.0, 258/258, no sanitizer report.
