# WEB-2: Extract an `EpollReactor` class from `main`

**Status:** Open
**Type:** Task
**Milestone:** M1 - Cleanup
**Priority:** High. Every later M1/M2 item (write path, parser, timeouts) plugs into the reactor, so it has to exist first.
**Estimate:** 2.5 h
**Mode:** Mentor (user writes the code)

## Description

`main.cpp` currently mixes epoll setup, the event loop, accept handling, per-connection read buffers and message parsing. The reactor is core code, so you write it. The goal is to move the epoll mechanics into a class under `server/net/` and leave `main` as wiring. This is the M1 item "`EpollReactor` class (move epoll logic out of `main`)". It also fixes the double-close and `EINTR` bugs found in review:

- `main.cpp` calls raw `close(client_fd)` when `epoll_ctl(ADD)` fails. The `FileDescriptor` in `Server::m_clients` still owns that fd, so it is closed twice. The slot also stays counted against the limit.
- `epoll_wait` returning `EINTR` makes `main` return `EXIT_FAILURE`.

## Scope

- In:
  - `EpollReactor` owns the epoll `FileDescriptor` and the wait loop.
  - It has `add(fd, events)`, `modify(fd, events)` and `remove(fd)`. Each checks the syscall result.
  - It dispatches events to a handler callback, for example `std::function<void(int fd, uint32_t events)>`.
  - `epoll_create1` is checked. Failing at startup may throw.
  - `epoll_wait` retries on `EINTR`.
  - `MAX_EVENTS` becomes a `constexpr` member of the reactor.
  - Fix the double close: on failed registration, the connection is released through `Server::closeClientConnection`, not a raw `close`.
  - Keep the current behaviour: the `client` binary still produces `Message received: ...`.
- Out (explicitly):
  - The write path and `EPOLLOUT` (WEB-3).
  - `Result<T, Error>`, which stays plain error handling for now.
  - Making the connection limit configurable.
  - Replacing the NUL-framed protocol.
  - Moving files into the `server/net/` layout. Only the new class goes there. The rest of the reorganisation comes with M2.

## Acceptance criteria (Definition of Done)

- [ ] `main.cpp` contains no `epoll_*` calls and no `epoll_event`.
- [ ] `EpollReactor` is move-only or non-copyable, and the epoll fd is released through RAII.
- [ ] `./build/server/server` plus `./build/client/client` prints `Message received: ...` as before.
- [ ] `scripts/check_connection_limit.sh` still passes (the first 10 connections stay open, the rest are closed).
- [ ] Forcing `epoll_ctl(ADD)` to fail, for example by temporarily calling `add` with a bad fd, does not trigger a double close. Check this with `strace -f -e close`.
- [ ] Sending `SIGSTOP` and then `SIGCONT` to the server, or attaching and detaching `strace -p`, does not kill it.
- [ ] Builds clean with no new warnings.
- [ ] No raw `epoll_*` calls outside the reactor class.
- [ ] Small, focused commit(s), with `WEB-2` in the message.

## Technical notes / hints

- Look at `main.cpp:39-52` for the registration pattern and `main.cpp:63-130` for the loop.
- Decide who owns what. `Server` owns the accepted sockets (`m_clients`), and the reactor should not own client fds. Be ready to explain why.
- `std::function` is fine now. A concept-constrained template parameter is the C++20 alternative, and you can note it as a later profiling candidate.
- Keep edge-triggered `EPOLLET` for now, along with the drain-until-`EAGAIN` rule. The EPOLLET versus level-triggered decision is settled later with data.
- `epoll_event.data.fd` is fine for now. A pointer or index in `data.ptr` becomes interesting later.
- `Server::accept()` still logs "Connection accepted" before the over-limit check, which is misleading. Fix this if you touch the function.
- The linear `remove_if` in `closeClientConnection` is out of scope. Add a TODO and a note for the profiling ticket.

## How to verify

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
./build/server/server &
./build/client/client
./scripts/check_connection_limit.sh
strace -f -e trace=close,epoll_ctl,epoll_wait -p $(pgrep server)
```

## Dependencies

- Blocked by: WEB-1 (done).
- Blocks: WEB-3 (write path and `EPOLLOUT`), later connection-limit configuration, and keep-alive timeouts.

## Interview talking point

Why `epoll` over `select`/`poll` (O(ready) versus O(n)), and why an edge-triggered fd must be drained until `EAGAIN`.
