#include <gtest/gtest.h>

#include <new>
#include <stdexcept>
#include <string>
#include <variant>

#include "http/Parser.h"
#include "http/Router.h"

// WEB-10 tests for http::Router. Requests come from http::parse so the views have the shape the server sees.

namespace
{
    using namespace http;

    // Parses `raw` (must outlive the returned Request) and returns the request.
    Request parseOk(const std::string &raw)
    {
        const ParseOutcome o = parse(raw);
        const auto *done = std::get_if<Done>(&o);
        EXPECT_NE(done, nullptr) << "request did not parse: " << raw;
        return done != nullptr ? done->request : Request{};
    }

    std::string get(const std::string &target, const std::string &extra = "")
    {
        return "GET " + target + " HTTP/1.1\r\nHost: x\r\n" + extra + "\r\n";
    }

    Response okHandler(const Request &) { return Response(StatusCode::Ok, std::string("hello"), std::string(), false); }

    Router makeRouter()
    {
        Router r;
        r.add(HttpMethod::GET, "/",
              [](const Request &) { return Response(StatusCode::Ok, std::string("root"), std::string(), false); });
        r.add(HttpMethod::POST, "/echo",
              [](const Request &req) { return Response(StatusCode::Ok, std::string(req.body), std::string(), false); });
        return r;
    }
} // namespace

// ---------- concept ----------

static_assert(RequestHandler<Response (*)(const Request &)>);
static_assert(RequestHandler<decltype([](const Request &) { return Response(StatusCode::Ok); })>);
static_assert(!RequestHandler<int>);
static_assert(!RequestHandler<decltype([](int) { return Response(StatusCode::Ok); })>);
static_assert(!RequestHandler<decltype([](const Request &) { return 42; })>); // wrong return type

// ---------- matching ----------

TEST(RouterTest, MatchesExactPath)
{
    const Router r = makeRouter();
    const std::string raw = get("/");
    const Response resp = r.route(parseOk(raw));
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, "root");
}

TEST(RouterTest, StripsQueryString)
{
    const Router r = makeRouter();
    const std::string raw = get("/?a=1&b=2");
    const Response resp = r.route(parseOk(raw));
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, "root");
}

TEST(RouterTest, HandlerReceivesRequestBody)
{
    const Router r = makeRouter();
    const std::string raw = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi";
    const Response resp = r.route(parseOk(raw));
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, "hi");
}

TEST(RouterTest, RegisterAcceptsLambdaAndFunctionPointer)
{
    Router r;
    r.add(HttpMethod::GET, "/fn", &okHandler);
    int calls = 0;
    r.add(HttpMethod::GET, "/lambda",
          [&calls](const Request &)
          {
              ++calls;
              return Response(StatusCode::Ok, std::string("l"), std::string(), false);
          });
    const std::string a = get("/fn");
    const std::string b = get("/lambda");
    EXPECT_EQ(r.route(parseOk(a)).body, "hello");
    EXPECT_EQ(r.route(parseOk(b)).body, "l");
    EXPECT_EQ(calls, 1);
}

TEST(RouterTest, PathMatchIsExactAndCaseSensitive)
{
    const Router r = makeRouter();
    for (const char *t: {"/echo/", "/ECHO", "/ech", "//"})
    {
        const std::string raw = get(t);
        EXPECT_EQ(r.route(parseOk(raw)).status_code, StatusCode::NotFound) << t;
    }
}

// ---------- 404 / 405 ----------

TEST(RouterTest, UnknownPathIs404)
{
    const Router r = makeRouter();
    const std::string raw = get("/nope");
    EXPECT_EQ(r.route(parseOk(raw)).status_code, StatusCode::NotFound);
}

TEST(RouterTest, WrongMethodIs405WithAllowHeader)
{
    const Router r = makeRouter();
    const std::string raw = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
    const Response resp = r.route(parseOk(raw));
    EXPECT_EQ(resp.status_code, StatusCode::MethodNotAllowed);
    EXPECT_NE(resp.headers.find("Allow: GET, HEAD"), std::string::npos) << "headers: " << resp.headers;
}

TEST(RouterTest, AllowHeaderListsOnlyRegisteredMethods)
{
    Router r;
    r.add(HttpMethod::POST, "/p", &okHandler);
    const std::string raw = get("/p");
    const Response resp = r.route(parseOk(raw));
    EXPECT_EQ(resp.status_code, StatusCode::MethodNotAllowed);
    EXPECT_NE(resp.headers.find("Allow: POST"), std::string::npos) << "headers: " << resp.headers;
    EXPECT_EQ(resp.headers.find("GET"), std::string::npos) << "headers: " << resp.headers;
}

// ---------- HEAD ----------

TEST(RouterTest, HeadFallsBackToGet)
{
    const Router r = makeRouter();
    const std::string raw = "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n";
    const Response resp = r.route(parseOk(raw));
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
}

TEST(RouterTest, HeadOnPostOnlyPathIs405)
{
    const Router r = makeRouter();
    const std::string raw = "HEAD /echo HTTP/1.1\r\nHost: x\r\n\r\n";
    EXPECT_EQ(r.route(parseOk(raw)).status_code, StatusCode::MethodNotAllowed);
}

// ---------- connection handling in router-generated errors ----------

TEST(RouterTest, ErrorResponseKeepsConnectionOpenWhenClientAllowsIt)
{
    const Router r = makeRouter();
    const std::string raw = get("/nope"); // HTTP/1.1 default: keep-alive
    EXPECT_FALSE(r.route(parseOk(raw)).close_connection);
}

TEST(RouterTest, ErrorResponseClosesWhenClientAsksForClose)
{
    const Router r = makeRouter();
    const std::string raw = get("/nope", "Connection: close\r\n");
    EXPECT_TRUE(r.route(parseOk(raw)).close_connection);
}

// ---------- Response constructor overloads ----------

TEST(RouterTest, ResponseStringLiteralBodyIsNotTakenAsBool)
{
    const Response resp(StatusCode::Ok, "body text");
    EXPECT_EQ(resp.body, "body text");
}

// ---------- WEB-11: prefix mounts ----------

namespace
{
    Response tagged(std::string tag)
    {
        return Response(StatusCode::Ok, std::move(tag), std::string(), false);
    }

    std::string request(const std::string &method, const std::string &target, const std::string &extra = "")
    {
        std::string raw = method + " " + target + " HTTP/1.1\r\nHost: x\r\n" + extra;
        if (method == "POST")
            raw += "Content-Length: 0\r\n";
        return raw + "\r\n";
    }

    Router makeMountRouter()
    {
        Router r = makeRouter(); // GET /, POST /echo
        r.add(HttpMethod::GET, "/static/special", [](const Request &) { return tagged("exact"); });
        r.mount("/static/", [](const Request &) { return tagged("static"); });
        r.mount("/static/img/", [](const Request &) { return tagged("img"); });
        r.mount("/assets", [](const Request &) { return tagged("assets"); }); // no trailing slash
        return r;
    }
} // namespace

TEST(RouterMountTest, PrefixMatchesNestedPaths)
{
    const Router r = makeMountRouter();
    for (const char *t: {"/static/a.css", "/static/sub/dir/file.js", "/static/"})
    {
        const std::string raw = get(t);
        const Response resp = r.route(parseOk(raw));
        EXPECT_EQ(resp.status_code, StatusCode::Ok) << t;
        EXPECT_EQ(resp.body, "static") << t;
    }
}

TEST(RouterMountTest, ExactRouteBeatsMount)
{
    const Router r = makeMountRouter();
    const std::string raw = get("/static/special");
    EXPECT_EQ(r.route(parseOk(raw)).body, "exact");
}

TEST(RouterMountTest, ExactRouteStillWinsOutsideMounts)
{
    const Router r = makeMountRouter();
    const std::string raw = get("/");
    EXPECT_EQ(r.route(parseOk(raw)).body, "root");
}

TEST(RouterMountTest, LongestPrefixWins)
{
    const Router r = makeMountRouter();
    const std::string img = get("/static/img/logo.png");
    const std::string other = get("/static/imgx/logo.png");
    EXPECT_EQ(r.route(parseOk(img)).body, "img");
    EXPECT_EQ(r.route(parseOk(other)).body, "static");
}

TEST(RouterMountTest, LongestPrefixWinsRegardlessOfRegistrationOrder)
{
    Router r;
    r.mount("/a/b/", [](const Request &) { return tagged("deep"); });
    r.mount("/a/", [](const Request &) { return tagged("shallow"); });
    const std::string raw = get("/a/b/c");
    EXPECT_EQ(r.route(parseOk(raw)).body, "deep");
}

TEST(RouterMountTest, MountWithoutSlashMatchesOnSegmentBoundary)
{
    const Router r = makeMountRouter();
    const std::string exact = get("/assets");
    const std::string nested = get("/assets/app.js");
    const std::string sibling = get("/assetsfoo");
    EXPECT_EQ(r.route(parseOk(exact)).body, "assets");
    EXPECT_EQ(r.route(parseOk(nested)).body, "assets");
    EXPECT_EQ(r.route(parseOk(sibling)).status_code, StatusCode::NotFound);
}

TEST(RouterMountTest, PathWithoutTrailingSlashDoesNotMatchSlashMount)
{
    // "/static" is not under "/static/"; it is a different resource.
    const Router r = makeMountRouter();
    const std::string raw = get("/static");
    EXPECT_EQ(r.route(parseOk(raw)).status_code, StatusCode::NotFound);
}

TEST(RouterMountTest, QueryStringIsStrippedBeforePrefixMatch)
{
    const Router r = makeMountRouter();
    const std::string raw = get("/assets?x=/static/");
    EXPECT_EQ(r.route(parseOk(raw)).body, "assets");
}

TEST(RouterMountTest, HeadIsDispatchedToMount)
{
    const Router r = makeMountRouter();
    const std::string raw = request("HEAD", "/static/a.css");
    const Response resp = r.route(parseOk(raw));
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, "static");
}

TEST(RouterMountTest, PostUnderMountIs405WithAllow)
{
    const Router r = makeMountRouter();
    const std::string raw = request("POST", "/static/a.css");
    const Response resp = r.route(parseOk(raw));
    EXPECT_EQ(resp.status_code, StatusCode::MethodNotAllowed);
    EXPECT_NE(resp.headers.find("Allow: GET, HEAD"), std::string::npos) << "headers: " << resp.headers;
    EXPECT_FALSE(resp.close_connection);
}

TEST(RouterMountTest, PostUnderMountWithConnectionCloseCloses)
{
    const Router r = makeMountRouter();
    const std::string raw = request("POST", "/static/a.css", "Connection: close\r\n");
    EXPECT_TRUE(r.route(parseOk(raw)).close_connection);
}

TEST(RouterMountTest, UnmatchedPathIsStill404)
{
    const Router r = makeMountRouter();
    const std::string raw = get("/nope");
    EXPECT_EQ(r.route(parseOk(raw)).status_code, StatusCode::NotFound);
}

TEST(RouterMountTest, RouterWithOnlyMountsHas404Fallback)
{
    Router r;
    r.mount("/static/", [](const Request &) { return tagged("static"); });
    const std::string raw = get("/");
    EXPECT_EQ(r.route(parseOk(raw)).status_code, StatusCode::NotFound);
}

// ---------- WEB-12: handler exceptions become 500 ----------

namespace
{
    Router makeThrowingRouter()
    {
        Router r;
        r.add(HttpMethod::GET, "/std", [](const Request &) -> Response { throw std::runtime_error("boom"); });
        r.add(HttpMethod::GET, "/alloc", [](const Request &) -> Response { throw std::bad_alloc(); });
        r.add(HttpMethod::GET, "/int", [](const Request &) -> Response { throw 42; });
        r.add(HttpMethod::GET, "/ok", okHandler);
        r.mount("/static/", [](const Request &) -> Response { throw std::runtime_error("mount boom"); });
        return r;
    }
} // namespace

TEST(RouterTest, ThrowingHandlerReturns500)
{
    const Router r = makeThrowingRouter();
    for (const char *target: {"/std", "/alloc"})
    {
        const std::string raw = get(target);
        const Response resp = r.route(parseOk(raw));
        EXPECT_EQ(resp.status_code, StatusCode::InternalServerError) << target;
        EXPECT_TRUE(resp.body.empty()) << target << ": exception text must not leak into the body";
    }
}

TEST(RouterTest, NonStdExceptionReturns500)
{
    const Router r = makeThrowingRouter();
    const std::string raw = get("/int");
    EXPECT_EQ(r.route(parseOk(raw)).status_code, StatusCode::InternalServerError);
}

TEST(RouterTest, ThrowingMountHandlerReturns500)
{
    const Router r = makeThrowingRouter();
    const std::string raw = get("/static/a.css");
    EXPECT_EQ(r.route(parseOk(raw)).status_code, StatusCode::InternalServerError);
}

TEST(RouterTest, ThrowingHeadFallbackReturns500)
{
    const Router r = makeThrowingRouter();
    const std::string raw = "HEAD /std HTTP/1.1\r\nHost: x\r\n\r\n";
    EXPECT_EQ(r.route(parseOk(raw)).status_code, StatusCode::InternalServerError);
}

// A 500 means the handler's state is unknown: close even when the client asked for keep-alive.
TEST(RouterTest, ThrowingHandlerRequestsClose)
{
    const Router r = makeThrowingRouter();
    for (const char *target: {"/std", "/int", "/static/a.css"})
    {
        const std::string raw = get(target); // HTTP/1.1, no Connection header: keep-alive by default
        EXPECT_TRUE(r.route(parseOk(raw)).close_connection) << target;
    }
}

TEST(RouterTest, RouterStillServesAfterAThrow)
{
    const Router r = makeThrowingRouter();
    const std::string bad = get("/std");
    static_cast<void>(r.route(parseOk(bad)));
    const std::string good = get("/ok");
    const Response resp = r.route(parseOk(good));
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, "hello");
}
