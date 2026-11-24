## WEB-11: Serve static files from a document root with MIME types and path-traversal protection

**Type:** Task
**Milestone:** M3 - Routing + static files
**Priority:** High (the M5 benchmark against Nginx needs a static file)
**Mode:** Mentor for the handler, file wrapper, router mount and config (written by the user). Pair for
the tests and the check scripts.

### Description
The definition of done compares requests/sec against Nginx serving the same static file. This ticket
maps the `/static/` URL prefix to a document root given by `--root`, sets `Content-Type` from the file
extension, and refuses any path that resolves outside the root. The kernel enforces the boundary:
every file is opened relative to a root directory fd with `openat2(RESOLVE_BENEATH)`.

### Scope
- In:
  - `--root <dir>` (required). `ServerConfig::rootDirectory` is a `std::optional<std::filesystem::path>`;
    `ServerConfigParser::parseRootDirectory` validates it and the error carries the OS reason.
  - `Router::mount(prefix, handler)`: prefix mounts for GET/HEAD, longest prefix wins, exact routes win
    over mounts, a mount without a trailing slash matches on a segment boundary only, other methods get
    405 + `Allow: GET, HEAD`.
  - `http::StaticFileHandler`: query string stripped, percent-decoded (malformed `%` is a 404), every
    segment starting with `.` refused (`..`, `.`, dotfiles such as `.git`), backslash and NUL refused.
    Missing files, directories and non-regular files are 404.
  - `common/FileReader.h`: `openDirectory()` (`O_PATH | O_DIRECTORY`, held in a `shared_ptr` so the
    handler stays copyable for `std::function`) and `readRegularFileBeneath(dirfd, rel)`
    (`syscall(SYS_openat2)` with `RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS`, `O_NONBLOCK`, `fstat` +
    `S_ISREG`).
  - `http::mimeTypeForExtension` / `mimeTypeForPath`: 11 extensions, case-insensitive,
    `application/octet-stream` fallback.
- Out: `sendfile()` / zero-copy (profiling ticket), `index.html` for directories, `Range`, ETag /
  `If-Modified-Since`, caching, directory listing, idle timeouts (next ticket).
- Changed from the original ticket: percent-decoding was out of scope and is now built (inside the
  static handler only; the router still matches the raw target). Dotfiles are refused, which the
  original ticket did not ask for.

### Acceptance criteria
- [x] `curl -i localhost:8080/static/index.html` -> 200, `Content-Type: text/html`, body equals the file (`cmp`)
- [x] `curl -I localhost:8080/static/style.css` -> 200, `Content-Type: text/css`, correct `Content-Length`, no body
- [x] `curl -i localhost:8080/static/missing.txt` -> 404
- [x] `curl --path-as-is -i localhost:8080/static/../secret.txt` -> 4xx, never the file
- [x] A symlink inside the root pointing to a file outside it (absolute or relative) -> 404
- [x] `curl -i localhost:8080/static/` (a directory) -> 404
- [x] `curl -i -X POST localhost:8080/static/index.html` -> 405 with `Allow: GET, HEAD`
- [x] `--root /does/not/exist` -> error with the reason, exit 1; no `--root` -> exit 1
- [x] Builds with no new warnings
- [x] Tests: `MimeTest` (8), `FileReaderTest` (22), `StaticFileHandlerTest` (33), `RouterMountTest` (13),
      `ParseRootDirectoryTest` (10)
- [x] `check_http.sh` `static-*` cases; `check_args.sh` `--root` cases
- [x] File I/O only in the wrapper (`common/FileReader.h`), errors via `Result<T, Error>`
- [x] Commits carry `WEB-11`

## Findings

### Design
- **Why `openat2(RESOLVE_BENEATH)` and not `canonical()` + prefix check.** The first version
  canonicalised the path, compared components with the root, then opened the path again. That resolves
  the path twice: a symlink swapped in between escapes the root (TOCTOU). Opening relative to a root fd
  with `RESOLVE_BENEATH` is one kernel lookup that fails with `EXDEV` on any escape, including `..`,
  absolute paths and symlinks. Holding the root as an fd also pins the directory inode: renaming or
  replacing the root path after startup does not change what is served (tested).
- **Consequence: absolute symlinks are refused**, even when they point inside the root, and so is a
  relative link that leaves the root and comes back in. Deploy roots with relative links only.
- **Two layers.** The lexical check in the handler (no `.` segments, no `\`, no NUL, no leading `/`)
  gives a clear 404 policy and blocks dotfiles; the kernel check is the security boundary.
- `fstat` on the opened fd, not `stat` on the path, so the type check is about the file actually read.
  `O_NONBLOCK` keeps `open()` on a FIFO from blocking the reactor before `S_ISREG` rejects it.

### Bugs found and fixed on this branch
- `parseRootDirectory`: `std::filesystem::status(p, ec)` sets `ec = ENOENT` for a missing path, so the
  `!exists()` branch after `if (ec)` never ran. The `not_found` check now comes first.
- `--root` validated `argv[i + 1]` but stored `argv[++i]` (the validated value was thrown away).
- The `--root` error message dropped the reason (`Not a directory`, `No such file or directory`).
- The default root was `"."`: running the server from the repo root would have served `.git/`. Now
  `--root` is required (`std::optional`, exit 1 when missing).
- `Router::add` stripped a leading `".."` from route paths (programmer input, protects nothing). Removed.
- Dotfiles were served (`/static/.git/config`). Now refused.
- `::openat2` only has a glibc wrapper from glibc 2.43; replaced by `syscall(SYS_openat2, ...)` so
  older CI images build.
- `syscall(OPENAT2_REGULAR, ...)`: a flag (`1 << 32`) was passed as the syscall number. It truncates
  to 0, which is `read` on x86-64, so every file open was `read(dirfd, ...)` on an `O_PATH` fd and
  failed with `EBADF`. 29 tests failed (every 200 case, every `EXDEV` assertion). Fixed with
  `SYS_openat2`. The traversal tests that only asserted "not 200" passed by accident while this bug
  was in, which is why the tests also assert the exact errno.
- `FileDescriptor::operator bool` was implicit (`fd + 1` compiled). Now `explicit`.
- `FileReader.h` missed `<memory>` and `<string_view>`.

### Still open (not part of this ticket)
- `Server::clientCallback` does not catch exceptions from `Router::route`. A handler that throws
  (`std::bad_alloc`, `std::format`) terminates the server. Should become a 500.
- The 405 for non-GET/HEAD under a mount is built in both `Router::route` and `StaticFileHandler`.
- Files are read whole into memory on the reactor thread with no size cap. `sendfile()` and a size
  limit belong to the `read`+`write` vs `sendfile()` profiling ticket.
- Percent-decoding happens in the static handler only; exact routes still match the raw target.

## Verification
Environment: Linux 7.3.3-200.fc44.x86_64, glibc 2.43, GCC 16.2.1.

- Fresh build (`rm -rf build`, Debug): no warnings or errors.
- `ctest --test-dir build`: 241/241 pass, 1 disabled
  (`KeepAliveTest.DISABLED_RepeatedConnectionHeadersAreCombined`). 86 new tests.
- `scripts/check_http.sh`: 53 PASS, 0 FAIL. New cases `static-get`, `static-head`, `static-mime`,
  `static-binary` (300 KB), `static-404`, `static-traversal` (29 targets over raw sockets: `..`, `.`,
  encoded `..`/`/`/`\`, absolute path, NUL, dotfiles, absolute and relative escaping symlinks,
  out-and-back through a symlinked directory), `static-405`, `static-pipelined`, `static-close`,
  `static-survival`, plus 6 curl checks (`cmp` against the file, `--path-as-is`, symlink escape).
- `scripts/check_args.sh`: 35 PASS (new: missing path, regular file, empty, dangling symlink, missing
  value, no `--root`, valid root).
- `check_connection_limit.sh` (3), `check_reactor.sh` (4), `check_scale.sh` (8),
  `check_write_path.sh` (7): all pass. All scripts now start the server with `--root`.
