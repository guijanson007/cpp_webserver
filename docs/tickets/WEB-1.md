# WEB-1: Stop the server crashing when the client limit is reached

**Status:** Done
**Type:** Bug
**Milestone:** M1 - Cleanup
**Priority:** Blocker. A single extra connection killed the process, which makes every later milestone (10k connections, `wrk`) impossible.
**Estimate:** 1.5 h
**Mode:** Mentor (user writes the code)

## Description
`Server::accept()` threw `runtime_error("Too many clients")` once 10 clients were connected, and `main` did not catch it, so the 11th connection terminated the whole server. The listener also accepted only one connection per epoll wakeup. This ticket makes accepting robust and is the first slice of M1.

## Scope
- In:
  - Never throw on the hot path (the accept loop).
  - Drain the listener with `accept4` until `EAGAIN`.
  - Over the limit, accept and immediately close the connection, with a log line. Do not crash.
  - Handle `accept4` failures: `EAGAIN`/`EWOULDBLOCK` ends the loop, `EINTR` retries, `EMFILE`/`ENFILE` are logged and do not kill the server.
  - Check the return values of `listen` and `epoll_ctl` on that path.
- Out:
  - `EpollReactor` extraction (WEB-2).
  - The `Result<T, Error>` type (a later ticket).
  - Changing the limit value or making it configurable.

## Acceptance criteria
- [x] 15 simultaneous connections leave the server running; the first 10 stay open and the rest are closed immediately.
- [x] After closing some clients, new connections are accepted again.
- [x] The server still prints `Message received: ...` for the existing `client` binary.
- [x] `accept()` contains no `throw` for the over-limit case.
- [x] Builds clean, with no new warnings.
- [x] Focused commit with `WEB-1` in the message.

## How to verify
```
cmake --build build && scripts/check_connection_limit.sh
```

## Dependencies
Blocks WEB-2 (the `EpollReactor` extraction), which will move this accept loop.

## Interview talking point
Why a non-blocking listener must be drained until `EAGAIN` with edge-triggered epoll, and why a server should shed load by closing connections instead of crashing.

## Lessons learned
- The listener socket itself must be `SOCK_NONBLOCK`. With a blocking listener the drain loop never sees `EAGAIN` and sleeps inside `accept4`, so the reactor never returns to `epoll_wait`. Clients that disconnect stay in `CLOSE-WAIT` and their slots are never freed.
- Calling `accept()` twice per loop iteration silently discards a connection that is never registered with epoll.

## Follow-ups (not in this ticket)
- `SO_REUSEADDR` is missing: restarting within ~60 s of a previous run fails to bind (`TIME_WAIT`).
- Dead code: `readUntil` / `ReadMessage` in `Server.h`.
- The shed result is `{-1}` inside an `optional<int>`; an enum would be clearer.
- A persistent `EMFILE` would busy-loop the accept drain.
