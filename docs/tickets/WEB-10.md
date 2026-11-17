## WEB-10: Add a concept-constrained router and wire it into `Server`

**Type:** Task
**Milestone:** M3 - Routing + static files
**Priority:** High (every later M3 item needs a dispatch point)
**Mode:** Mentor for the router (written by the user). Pair for the tests and the check script.

### Description
`Server::clientCallback` used to build a hard-coded `200 Hello, World!` for every parsed request.
M3 needs a router so handlers can be registered per method and path. This ticket adds
`http::Router`, a `RequestHandler` concept, and the 404/405 statuses the parser never produces.

### Scope
- In: `RequestHandler` concept (`Response(const Request&)`), `Router::add(method, path, handler)`
  and `Router::route(request) -> Response`. Exact-path matching, query string stripped before
  matching. 404 for unknown paths, 405 with `Allow` for a known path and wrong method, HEAD falls
  back to the GET handler. `Server` owns a `Router`. `main` registers `GET /` and `POST /echo`.
- Out: static files, MIME, traversal protection, path parameters, wildcards, middleware,
  idle timeouts, percent-decoding.

### Acceptance criteria
- [x] `curl -i localhost:8080/` -> 200 with the handler body
- [x] `curl -i localhost:8080/nope` -> 404
- [x] `curl -i -X POST localhost:8080/` -> 405 with `Allow: GET, HEAD`
- [x] `curl -I localhost:8080/` -> 200, headers only
- [x] `curl -d hi localhost:8080/echo` -> `hi`
- [x] `GET /?a=1` matches `/`
- [x] Builds with no new warnings
- [x] Tests: `RouterTest.*` in `tests/router_test.cpp` (13 tests, plus `static_assert`s on the concept)
- [x] No raw syscalls added (the router is socket-free)
- [x] Commits carry `WEB-10`

## Findings
- Design: `unordered_map<string, array<optional<function>, MAX_METHODS>>` with a transparent
  hash and `std::equal_to<>`, so lookups take a `string_view` without allocating.
  `HttpMethod` is now `uint8_t`-backed with explicit values, used as the array index.
- Bugs found by the new tests and the end-to-end script, all fixed on this branch:
  - `Response(StatusCode, bool)` overload: a string literal body (`Response(Ok, "hi")`) converts
    `const char*` -> `bool` (a standard conversion) and beats the conversion to `std::string`,
    so the body came out empty. It also broke two existing `ResponseTest` cases. Overload removed.
  - `Router::route` passed `shouldKeepAlive()` as the `close` argument, so the flag was inverted
    on 404/405.
  - 405 had no `Allow` header, and HEAD on a GET-only path returned 405.
  - `Server` computed `close_conn` but did not apply it to the handler's response, so a 200 to a
    `Connection: close` request closed the socket without sending `Connection: close`
    (found by `check_http.sh` case `conn-close`).
  - `Response::operator()(const Request&)` stub (empty, unused-parameter warning) removed.
- Still open (not part of this ticket): `Allow` is hand-built from three `if` branches, which
  does not scale past three methods. `MAX_METHODS` is not tied to the enum by a `static_assert`.
  `ParseError` still duplicates `StatusCode`.

## Verification
- Fresh build (`rm -rf build`, Debug): no warnings or errors.
- `ctest --test-dir build`: 155/155 pass, 1 disabled (`KeepAliveTest.DISABLED_RepeatedConnectionHeadersAreCombined`).
- `scripts/check_http.sh`: all pass (37 checks). New cases `route-get`, `route-query`, `route-404`,
  `route-405`, `route-head`, `route-echo`, `route-close`, `route-survival`, plus curl checks for
  404, 405 + `Allow`, `/echo` and the query string. WEB-8 body cases moved to `POST /echo`.
- `check_args.sh`, `check_connection_limit.sh`, `check_reactor.sh`, `check_scale.sh`,
  `check_write_path.sh`: all pass.
