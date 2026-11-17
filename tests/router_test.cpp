#include <gtest/gtest.h>

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
