# WEB-4: Make the connection limit configurable and survive fd exhaustion

**Status:** Done (acceptance criteria met; see "Outcome")
**Type:** Task
**Milestone:** M1 - Cleanup
**Priority:** High. It blocks the "10k+ concurrent keep-alive connections" Definition of Done item and is the last big M1 item before the HTTP work.
**Estimate:** 2-3 h
**Mode:** Mentor (user writes the code). Claude wrote `scripts/check_scale.sh`.

## Description

`MAX_CONNECTIONS` is hardcoded at 10, and the `listen()` backlog is computed from it. The backlog is the kernel accept queue, not a client cap, so the two must be separate settings. Reaching 10k connections also depends on `RLIMIT_NOFILE`. When the process runs out of fds, `accept4` fails with `EMFILE`. The level-triggered listener then wakes the loop again and again, so the server busy-spins at 100% CPU. This is the M1 backlog item in `CLAUDE.md`.

## Scope

- In:
  - A `ServerConfig` aggregate (`ip`, `port`, `maxConnections`, `backlog`) with sane defaults. `main` builds it with designated initializers.
  - `Server` takes the config instead of loose arguments.
  - `main` parses three flags into the config: `--port N`, `--max-connections N`, `--backlog N`. A hand-rolled loop over `argv` is enough. This interface is what `scripts/check_scale.sh` relies on.
  - `EMFILE`/`ENFILE` handling in `accept()` so the server does not spin.
- Out (explicitly):
  - HTTP, timeouts and idle reaping (M3).
  - The `flush()` `modify()` churn and the quadratic `processFrames`. Those are separate backlog items.
  - A config file or a CLI library.
  - Raising `RLIMIT_NOFILE` from code. Documenting `ulimit -n` is enough.

## Acceptance criteria (Definition of Done)

- [x] `MAX_CONNECTIONS` is gone. The cap and the backlog come from `ServerConfig`, and the backlog is a separate field with its own default.
- [x] `scripts/check_scale.sh` passes all three phases:
  - cap of 100 with 150 clients keeps exactly 100;
  - 10,000 idle connections are held, a fresh client still gets its echo, and idle CPU is under 10%;
  - fd exhaustion (`ulimit -n 256`, 400 clients) leaves the server alive, with CPU under 50% over 2 s and recovery once fds are freed.
- [x] `scripts/check_connection_limit.sh`, `check_write_path.sh` and `check_reactor.sh` still pass (all three pass on `main` today).
- [x] The behaviour at `EMFILE` is chosen deliberately and explained in a comment.
- [x] Builds clean with no new warnings.
- [x] No new raw syscalls outside `SocketIo`/reactor. `accept4` stays where it is until the wrapper ticket.
- [x] Small, focused commits with `WEB-4` in the message.

## Technical notes / hints

- `Server::start()` and the `m_clients.reserve(...)` call in the constructor both use the constant.
- `ServerConfig` only works with designated initializers if it is an aggregate, so no constructors. Initialize fields in declaration order.
- `net.core.somaxconn` (4096 on this machine) silently caps the backlog passed to `listen()`.
- For `EMFILE` the usual options are:
  - (a) Keep a spare fd open, `accept` and immediately close to drain the queue, then reopen the spare.
  - (b) Disable the listener with `reactor.modify`/`remove` and re-arm it when a connection closes.
  - (c) Log and rate-limit.
  - Pick one and be ready to explain the trade-off.
- In `accept()`, `network::Connection new_client(fd_temp)` is constructed before the `fd_temp < 0` check. It works because the fd is -1, but it is fragile, so construct it only on success. Also make `Connection(int)` `explicit`.
- `std::cout << ... << std::endl` runs on every accept. At 10k connections this is noticeable. Consider dropping or reducing it, since it will matter for benchmarking.

## How to verify

```
cmake --build build
scripts/check_scale.sh                 # needs a hard fd limit >= 20000 (it is 524288 here)
scripts/check_connection_limit.sh      # regression
scripts/check_write_path.sh            # regression
scripts/check_reactor.sh               # regression
```

## Dependencies

Blocked by: none (WEB-3 is merged). Blocks: M2 and the 10k Definition of Done check. Follow-ups: the `flush()` `modify()` churn, and removing the leftover `<asm-generic/socket.h>` include and the unused `getFd()`.

## Outcome

All four scripts pass on `web-4`: `check_scale.sh` (cap of 100 with 150 clients, 10,000 idle connections with 0% idle CPU, fd exhaustion at `ulimit -n 256` with no busy loop and recovery), plus `check_connection_limit.sh`, `check_write_path.sh` and `check_reactor.sh`. The build has no warnings.

What was done:

- `ServerConfig` aggregate (`ip` as `std::string`, `port`, `maxConnections`, `backlogSize`) and `main` flags `--port`, `--max-connections`, `--backlog`. The backlog is separate from the cap.
- `EMFILE`/`ENFILE` handled with a spare fd (`/dev/null`): release it, `accept4` one connection, close it, reopen the spare. The level-triggered listener keeps draining the queue instead of spinning.
- `Connection(int)` is `explicit`. `accept()` builds the `Connection` only after a successful `accept4`. The cap check is `>=` with an explicit `int` cast.
- Per-accept logging removed. Diagnostics go to `cerr`. `sync_with_stdio(false)` in `main`. `MAX_EVENTS` raised from 10 to 10,000 (a guess to settle by profiling).
- `check_reactor.sh` counts accepted connections with `ss` instead of grepping the server log, so it no longer depends on stdout buffering. `check_reactor.sh` and `check_connection_limit.sh` pass `--max-connections` explicitly.

Follow-ups left open (recorded in CLAUDE.md "Known issues"):

- `std::stoi` accepts trailing garbage and has no range checks for port, cap or backlog.
- `<asm-generic/socket.h>` include, unused `getFd()` on `Connection` and the reactor, direct `accept4`.
- `flush()` `modify()` churn and the quadratic `processFrames`.
- The `EMFILE` message prints on every discard.

## Interview talking point

The listen backlog (the kernel accept queue) and the application's connection cap are different things. Level-triggered epoll plus `EMFILE` produces a busy loop unless you handle it.
