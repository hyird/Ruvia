#include "test_harness.h"

#include <memory_resource>
#include <string>
#include <string_view>

#include "net/http1/Http1Connection.h"

namespace {

using ruvia::detail::Http1Connection;
using ruvia::detail::Http1CoreConfig;
using ruvia::detail::Http1Event;
using ruvia::detail::Http1FeedStatus;

struct Drained {
    int heads{0};
    int ends{0};
    std::string body;
};

Drained drain(Http1Connection& conn) {
    Drained out;
    for (;;) {
        const auto event = conn.nextEvent();
        if (event.kind == Http1Event::Kind::kNone) {
            break;
        }
        if (event.kind == Http1Event::Kind::kMessageHead) ++out.heads;
        if (event.kind == Http1Event::Kind::kMessageEnd) ++out.ends;
        if (event.kind == Http1Event::Kind::kMessageBodyChunk) {
            out.body.append(event.bytes.data(), event.bytes.size());
        }
    }
    return out;
}

}  // namespace

// Two pipelined GETs in one feed: the core surfaces exactly ONE message, parks on the
// pipelined remainder until nextMessage(), then surfaces the second.
RUVIA_TEST(http1_connection_pipelined_gets_one_message_at_a_time) {
    std::pmr::monotonic_buffer_resource resource;
    Http1Connection conn(&resource);

    const std::string_view two =
        "GET /a HTTP/1.1\r\nHost: h\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = conn.feed(two);
    RUVIA_CHECK(result.status == Http1FeedStatus::kOk);

    auto first = drain(conn);
    RUVIA_CHECK_EQ(first.heads, 1);
    RUVIA_CHECK_EQ(first.ends, 1);
    RUVIA_CHECK(conn.messageComplete());
    RUVIA_CHECK(conn.head().request.path() == "/a");
    RUVIA_CHECK(conn.keepAlive());
    RUVIA_CHECK(conn.unconsumedInput().substr(0, 6) == "GET /b");

    conn.nextMessage();
    auto second = drain(conn);
    RUVIA_CHECK_EQ(second.heads, 1);
    RUVIA_CHECK_EQ(second.ends, 1);
    RUVIA_CHECK(conn.head().request.path() == "/b");
    RUVIA_CHECK(conn.unconsumedInput().empty());
}

// A content-length body split across three feeds arrives as measured chunks and ends
// exactly at the declared length.
RUVIA_TEST(http1_connection_content_length_body_across_feeds) {
    std::pmr::monotonic_buffer_resource resource;
    Http1Connection conn(&resource);

    (void)conn.feed("POST /u HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\nhel");
    auto part1 = drain(conn);
    RUVIA_CHECK_EQ(part1.heads, 1);
    RUVIA_CHECK(part1.body == "hel");
    RUVIA_CHECK_EQ(part1.ends, 0);

    (void)conn.feed("lo wo");
    auto part2 = drain(conn);
    RUVIA_CHECK(part2.body == "lo wo");

    (void)conn.feed("rld");
    auto part3 = drain(conn);
    RUVIA_CHECK(part3.body == "rld");
    RUVIA_CHECK_EQ(part3.ends, 1);
    RUVIA_CHECK(conn.messageComplete());
}

// Chunked framing: sizes with extensions, split at awkward boundaries, a trailer
// section -- the de-chunked payload and the message end both come out right.
RUVIA_TEST(http1_connection_chunked_body_with_trailers) {
    std::pmr::monotonic_buffer_resource resource;
    Http1Connection conn(&resource);

    (void)conn.feed("POST /c HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n");
    (void)drain(conn);
    (void)conn.feed("5;ext=1\r\nhel");
    auto a = drain(conn);
    RUVIA_CHECK(a.body == "hel");
    (void)conn.feed("lo\r\n3\r\nwor\r\n0\r\nX-Sum: ok\r\n\r\n");
    auto b = drain(conn);
    RUVIA_CHECK(b.body == "lowor");
    RUVIA_CHECK_EQ(b.ends, 1);
    RUVIA_CHECK(conn.messageComplete());
}

// Empty trailer section ("0\r\n\r\n") ends the message too.
RUVIA_TEST(http1_connection_chunked_empty_trailers) {
    std::pmr::monotonic_buffer_resource resource;
    Http1Connection conn(&resource);
    (void)conn.feed(
        "POST /c HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n");
    auto all = drain(conn);
    RUVIA_CHECK(all.body == "ok");
    RUVIA_CHECK_EQ(all.ends, 1);
}

// Framing violations fail the connection with a specific error: a bad chunk size and
// a missing chunk-data CRLF.
RUVIA_TEST(http1_connection_chunked_violations_fail) {
    std::pmr::monotonic_buffer_resource resource;
    {
        Http1Connection conn(&resource);
        (void)conn.feed("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n");
        const auto result = conn.feed("zz\r\n");
        RUVIA_CHECK(result.status == Http1FeedStatus::kError);
        RUVIA_CHECK(conn.error() == ruvia::HttpParseError::kInvalidChunkSize);
    }
    {
        Http1Connection conn(&resource);
        (void)conn.feed("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n");
        const auto result = conn.feed("2\r\nokXX");
        RUVIA_CHECK(result.status == Http1FeedStatus::kError);
        RUVIA_CHECK(conn.error() == ruvia::HttpParseError::kInvalidChunkCrlf);
    }
}

// Protocol-level caps: an oversized header fails with kHeaderTooLarge; a decoded body
// beyond maxBodyBytes fails with kBodyTooLarge.
RUVIA_TEST(http1_connection_caps_enforced) {
    std::pmr::monotonic_buffer_resource resource;
    {
        Http1Connection conn(&resource, Http1CoreConfig{.maxHeaderBytes = 32});
        const auto result = conn.feed("GET /aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa HTTP/1.1\r\n");
        RUVIA_CHECK(result.status == Http1FeedStatus::kError);
        RUVIA_CHECK(conn.error() == ruvia::HttpParseError::kHeaderTooLarge);
    }
    {
        Http1Connection conn(&resource, Http1CoreConfig{.maxBodyBytes = 4});
        const auto result =
            conn.feed("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 8\r\n\r\n12345678");
        RUVIA_CHECK(result.status == Http1FeedStatus::kError);
        RUVIA_CHECK(conn.error() == ruvia::HttpParseError::kBodyTooLarge);
    }
}

// keepAlive() follows RFC 9112 §9.3: 1.1 defaults on (explicit close wins), 1.0
// defaults off (explicit keep-alive wins).
RUVIA_TEST(http1_connection_keep_alive_semantics) {
    std::pmr::monotonic_buffer_resource resource;
    {
        Http1Connection conn(&resource);
        (void)conn.feed("GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
        (void)drain(conn);
        RUVIA_CHECK(!conn.keepAlive());
    }
    {
        Http1Connection conn(&resource);
        (void)conn.feed("GET / HTTP/1.0\r\nHost: h\r\n\r\n");
        (void)drain(conn);
        RUVIA_CHECK(!conn.keepAlive());
    }
    {
        Http1Connection conn(&resource);
        (void)conn.feed("GET / HTTP/1.0\r\nHost: h\r\nConnection: keep-alive\r\n\r\n");
        (void)drain(conn);
        RUVIA_CHECK(conn.keepAlive());
    }
}

// Upgrade hand-off shape: after an upgrade-style request completes, bytes the peer
// pipelined behind it (e.g. an h2 client preface) sit intact in unconsumedInput().
RUVIA_TEST(http1_connection_unconsumed_input_for_upgrade_handoff) {
    std::pmr::monotonic_buffer_resource resource;
    Http1Connection conn(&resource);
    (void)conn.feed(
        "GET / HTTP/1.1\r\nHost: h\r\nConnection: Upgrade, HTTP2-Settings\r\n"
        "Upgrade: h2c\r\nHTTP2-Settings: AAMAAABkAAQAAQAAAAIAAAAA\r\n\r\n"
        "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
    auto all = drain(conn);
    RUVIA_CHECK_EQ(all.heads, 1);
    RUVIA_CHECK_EQ(all.ends, 1);
    RUVIA_CHECK(conn.head().flags.upgrade);
    RUVIA_CHECK(conn.unconsumedInput() == "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
}
