#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/http2/Http2Hpack.h"
#include "net/http2/Http2ResponseHeaders.h"
#include "net/http2/Http2StreamState.h"
#include "ruvia/http/HttpResponse.h"

namespace {

using ruvia::HttpResponse;
using ruvia::detail::appendHttp2ResponseHeaders;
using ruvia::detail::HpackDecoder;
using ruvia::detail::Http2StreamState;

struct Collector final {
    std::vector<std::pair<std::string, std::string>> headers;
};

bool collect(void* target, std::string_view name, std::string_view value) {
    static_cast<Collector*>(target)->headers.emplace_back(std::string(name), std::string(value));
    return true;
}

bool decodeResponseHeaders(
    const HttpResponse& response,
    std::uint64_t autoContentLength,
    Collector& out) {
    Http2StreamState stream(1, std::pmr::get_default_resource());
    appendHttp2ResponseHeaders(stream, response, autoContentLength);

    HpackDecoder decoder(std::pmr::get_default_resource());
    return decoder.decode(stream.responseHeaderBlock(), &out, &collect).ok();
}

bool hasHeader(const Collector& headers, std::string_view name, std::string_view value) {
    for (const auto& header : headers.headers) {
        if (header.first == name && header.second == value) {
            return true;
        }
    }
    return false;
}

bool hasHeaderName(const Collector& headers, std::string_view name) {
    for (const auto& header : headers.headers) {
        if (header.first == name) {
            return true;
        }
    }
    return false;
}

}  // namespace

RUVIA_TEST(http2_response_headers_omit_content_length_for_204) {
    HttpResponse response(std::pmr::get_default_resource());
    response.status(204);
    response.header("Content-Length", "12");

    Collector headers;
    RUVIA_CHECK(decodeResponseHeaders(response, 12, headers));
    RUVIA_CHECK(hasHeader(headers, ":status", "204"));
    RUVIA_CHECK(!hasHeaderName(headers, "content-length"));
}

RUVIA_TEST(http2_response_headers_keep_explicit_content_length_for_304) {
    HttpResponse response(std::pmr::get_default_resource());
    response.status(304);
    response.header("Content-Length", "12");

    Collector headers;
    RUVIA_CHECK(decodeResponseHeaders(response, 99, headers));
    RUVIA_CHECK(hasHeader(headers, ":status", "304"));
    RUVIA_CHECK(hasHeader(headers, "content-length", "12"));
}

RUVIA_TEST(http2_response_headers_do_not_auto_content_length_for_304) {
    HttpResponse response(std::pmr::get_default_resource());
    response.status(304);

    Collector headers;
    RUVIA_CHECK(decodeResponseHeaders(response, 12, headers));
    RUVIA_CHECK(hasHeader(headers, ":status", "304"));
    RUVIA_CHECK(!hasHeaderName(headers, "content-length"));
}

RUVIA_TEST(http2_response_headers_set_cookie_uses_never_indexed_literal) {
    // RFC 7541 §7.1.3: Set-Cookie carries session credentials and must be emitted
    // as a never-indexed literal (0x10 prefix) so an intermediary never places it
    // in a shared HPACK dynamic table.
    HttpResponse response(std::pmr::get_default_resource());
    response.status(200);
    response.header("Set-Cookie", "sid=secret; HttpOnly");

    Http2StreamState stream(1, std::pmr::get_default_resource());
    appendHttp2ResponseHeaders(stream, response, 0);
    const auto& block = stream.responseHeaderBlock();

    // Byte 0 is the indexed :status 200 (0x88); Set-Cookie is the next field and
    // its representation prefix must be the never-indexed literal nibble (0x10).
    RUVIA_CHECK(block.size() >= 2);
    RUVIA_CHECK(static_cast<unsigned char>(block[0]) == 0x88);
    RUVIA_CHECK((static_cast<unsigned char>(block[1]) & 0xF0U) == 0x10U);

    // The never-indexed hint must not corrupt the round-trip value.
    Collector headers;
    RUVIA_CHECK(decodeResponseHeaders(response, 0, headers));
    RUVIA_CHECK(hasHeader(headers, "set-cookie", "sid=secret; HttpOnly"));
}

RUVIA_TEST(http2_response_headers_non_sensitive_uses_without_indexing) {
    // A non-credential field (content-type) must stay a plain without-indexing
    // literal (0x00 nibble) -- confirms the never-indexed choice discriminates by
    // header name rather than marking everything.
    HttpResponse response(std::pmr::get_default_resource());
    response.status(200);
    response.header("Content-Type", "text/plain");

    Http2StreamState stream(1, std::pmr::get_default_resource());
    appendHttp2ResponseHeaders(stream, response, 0);
    const auto& block = stream.responseHeaderBlock();

    RUVIA_CHECK(block.size() >= 2);
    RUVIA_CHECK(static_cast<unsigned char>(block[0]) == 0x88);
    RUVIA_CHECK((static_cast<unsigned char>(block[1]) & 0xF0U) == 0x00U);
}

RUVIA_TEST(http2_response_headers_omit_hop_by_hop_fields) {
    HttpResponse response(std::pmr::get_default_resource());
    response.header("Connection", "close");
    response.header("Keep-Alive", "timeout=5");
    response.header("Proxy-Connection", "keep-alive");
    response.header("TE", "trailers");
    response.header("Transfer-Encoding", "chunked");
    response.header("Upgrade", "websocket");
    response.header("X-Ok", "yes");

    Collector headers;
    RUVIA_CHECK(decodeResponseHeaders(response, 0, headers));
    RUVIA_CHECK(!hasHeaderName(headers, "connection"));
    RUVIA_CHECK(!hasHeaderName(headers, "keep-alive"));
    RUVIA_CHECK(!hasHeaderName(headers, "proxy-connection"));
    RUVIA_CHECK(!hasHeaderName(headers, "te"));
    RUVIA_CHECK(!hasHeaderName(headers, "transfer-encoding"));
    RUVIA_CHECK(!hasHeaderName(headers, "upgrade"));
    RUVIA_CHECK(hasHeader(headers, "x-ok", "yes"));
}
