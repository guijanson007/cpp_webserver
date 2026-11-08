# WEB-3: Add a per-connection output buffer and an `EPOLLOUT` write path

**Status:** Done (acceptance criteria met; see "Outcome")
**Type:** Task
**Milestone:** M1 - Cleanup
**Priority:** High. Every response in M2 needs a write path, and it blocked the rest of M1.
**Estimate:** 3 h
**Mode:** Mentor (user writes the code)

## Description

The server could only read. A partial `write()` or an `EAGAIN` on a non-blocking socket would silently lose data. This ticket adds a per-connection output queue, flushes it, and arms `EPOLLOUT` only while data is pending. It covers the M1 item "add write path with `EPOLLOUT` and per-connection output buffer". To be demonstrable before HTTP exists, the server echoes each decoded NUL-terminated frame back to the client. The echo is throwaway and disappears in M2.

## Scope

- In:
  - Handler signature carries events: `std::function<void(int fd, uint32_t events)>`.
  - Output buffer on `Connection`.
  - `Server::flush(Connection&)`: write until empty or `EAGAIN`, handle partial writes, arm `EPOLLOUT` on `EAGAIN`, restore `EPOLLIN | EPOLLET` when drained.
  - Handle `EPOLLOUT` and `EPOLLERR`. A write error closes only that connection.
  - Half-close: a peer that shuts down its write side still receives the full echo before the server closes.
  - `SO_REUSEPORT` replaced by `SO_REUSEADDR`.
  - Raw `read`/`send` moved behind `network/SocketIo.h`.
- Out (explicitly):
  - `Result<T, Error>` (M2).
  - Configurable connection limit and backlog (WEB-4).
  - HTTP, timeouts, `sendfile`.
  - Bounded reads per wakeup / ready queue (see CLAUDE.md backlog).

## Acceptance criteria (Definition of Done)

- [x] Echo: `hello\0` comes back.
- [x] Backpressure: a client sending about 8 MB without reading does not block other clients, and then receives every byte in order.
- [x] A client that resets the connection (RST) does not kill the server (no SIGPIPE) and its fd is closed once.
- [x] `EPOLLOUT` is not armed when the output buffer is empty (checked with `strace -e epoll_ctl`). Arm/disarm cycle verified on a 20 MB echo.
- [x] Half-close client gets the full echo, then the server closes.
- [x] Builds clean with no new warnings.
- [x] Check script: `scripts/check_write_path.sh` (echo, rst-survival, backpressure, half-close, epollout).
- [x] No raw `read`/`send` outside the wrapper layer (`accept4` is still called directly in `Server`).
- [ ] Commit messages carry `WEB-3` (earlier commits `bb2fd0e`, `d64392b`, `ed96810`, `ffffcc0` do not; use the closing commit).

## Technical notes / hints

- `MSG_NOSIGNAL` on `send` (or ignoring SIGPIPE) avoids the process dying on a closed peer.
- With `EPOLLET`, try writing immediately after queuing and arm `EPOLLOUT` only after `EAGAIN`.
- Closing order: `reactor.remove(fd)` first, then erase from the map. Do not touch the `Connection` reference after `closeClientConnection`, because `flush()` may close the connection it was given.
- Stale-event risk: if a connection closes and `accept()` reuses the same fd number within one `epoll_wait` batch, a stale event can reach the new connection's handler. Not fixed here.

## How to verify

```
cmake --build build
scripts/check_write_path.sh
printf 'hello\0' | nc -q1 localhost 8080
strace -f -e trace=epoll_ctl ./build/server/server
```

## Dependencies

- Blocked by: WEB-2 (done).
- Blocks: WEB-4 (configurable connection limit and backlog), M2.

## Outcome

All checks in `scripts/check_write_path.sh` pass. Follow-ups recorded in CLAUDE.md "Known issues":

- `flush()` calls `modify()` on every successful flush and again on every `EAGAIN` while `EPOLLOUT` is already armed.
- `processFrames` erases from the front of the read buffer per frame: quadratic on big bursts (20k frames 0.37 s, 40k 0.53 s, 80k 3.39 s). First profiling candidate.
- `readSome` has no per-wakeup cap (starvation, unbounded buffer). Memory is bounded by the M2 size limits.
- Idea only: bounded reads plus a ready queue versus level-triggered epoll, decided by profiling after M3.
- Leftovers: `<asm-generic/socket.h>` include, unused reactor `getFd()`, direct `accept4`.

## Interview talking point

Why an edge-triggered `EPOLLOUT` is armed only after `EAGAIN` and disarmed when the buffer drains, and what happens to the loop if it is left armed.
