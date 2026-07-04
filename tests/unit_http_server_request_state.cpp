#include "test_harness.h"

#include <cstddef>

#include "http/HttpParserInternal.h"
#include "net/server/HttpServerRequestState.h"

namespace {

using ruvia::detail::contentLengthExceedsLimit;
using ruvia::detail::HttpServerParser;
using ruvia::detail::shouldKeepAlive;
using ruvia::detail::wantsContinue;

}  // namespace

RUVIA_TEST(request_state_content_length_exceeds_limit) {
    RUVIA_CHECK(contentLengthExceedsLimit(101, 100));
    RUVIA_CHECK(!contentLengthExceedsLimit(100, 100));       // exact fit is allowed
    RUVIA_CHECK(!contentLengthExceedsLimit(1'000'000, 0));   // a 0 limit means unlimited
}

RUVIA_TEST(request_state_keep_alive_by_connection_header) {
    HttpServerParser parser;
    RUVIA_CHECK(!shouldKeepAlive(
        parser.parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")));
    RUVIA_CHECK(shouldKeepAlive(
        parser.parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n")));
}

RUVIA_TEST(request_state_keep_alive_default_by_version) {
    HttpServerParser parser;
    // HTTP/1.1 defaults to persistent; HTTP/1.0 defaults to close.
    RUVIA_CHECK(shouldKeepAlive(parser.parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n")));
    RUVIA_CHECK(!shouldKeepAlive(parser.parse("GET / HTTP/1.0\r\n\r\n")));
    // HTTP/1.0 can still opt in with an explicit keep-alive.
    RUVIA_CHECK(shouldKeepAlive(parser.parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")));
}

RUVIA_TEST(request_state_wants_continue) {
    HttpServerParser parser;
    RUVIA_CHECK(wantsContinue(parser.parse(
        "POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 0\r\n\r\n")));
    RUVIA_CHECK(!wantsContinue(parser.parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n")));
}
