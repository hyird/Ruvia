#include <iterator>
#include <string_view>

#include "ruvia/http/detail/parser/http_parser_syntax.h"

#include "test_harness.h"

namespace {

using ruvia::detail::classify_request_header;
using ruvia::detail::request_header_kind;
using ruvia::detail::singleton_request_header_bit;

struct case_value final {
    std::string_view name_;
    request_header_kind kind_;
};

}  // namespace

// classify_request_header drives the known-header fast paths, including the
// smuggling-sensitive Host / Content-Length / Transfer-Encoding / Connection
// detection. Lock the whole name->kind table so a typo can't silently
// misclassify a security-relevant header down to other.
RUVIA_TEST(request_header_classification_table) {
    const case_value cases[] = {
        {"Accept", request_header_kind::accept},
        {"Accept-Encoding", request_header_kind::accept_encoding},
        {"Access-Control-Request-Headers", request_header_kind::access_control_request_headers},
        {"Access-Control-Request-Method", request_header_kind::access_control_request_method},
        {"Authorization", request_header_kind::authorization},
        {"Connection", request_header_kind::connection},
        {"Content-Encoding", request_header_kind::content_encoding},
        {"Content-Length", request_header_kind::content_length},
        {"Content-Type", request_header_kind::content_type},
        {"Cookie", request_header_kind::cookie},
        {"Expect", request_header_kind::expect},
        {"Host", request_header_kind::host},
        {"If-Match", request_header_kind::if_match},
        {"If-Modified-Since", request_header_kind::if_modified_since},
        {"If-None-Match", request_header_kind::if_none_match},
        {"If-Range", request_header_kind::if_range},
        {"If-Unmodified-Since", request_header_kind::if_unmodified_since},
        {"Origin", request_header_kind::origin},
        {"Range", request_header_kind::range},
        {"Sec-WebSocket-Key", request_header_kind::sec_websocket_key},
        {"Sec-WebSocket-Protocol", request_header_kind::sec_websocket_protocol},
        {"Sec-WebSocket-Version", request_header_kind::sec_websocket_version},
        {"Transfer-Encoding", request_header_kind::transfer_encoding},
        {"Upgrade", request_header_kind::upgrade},
        {"User-Agent", request_header_kind::user_agent},
        {"Forwarded", request_header_kind::forwarded},
        {"X-Forwarded-For", request_header_kind::x_forwarded_for},
        {"X-Forwarded-Proto", request_header_kind::x_forwarded_proto},
        {"Sec-WebSocket-Extensions", request_header_kind::sec_websocket_extensions},
    };
    for (const auto& entry : cases) {
        RUVIA_CHECK(classify_request_header(entry.name_) == entry.kind_);
    }
}

RUVIA_TEST(request_header_classification_is_case_insensitive) {
    // Field names are case-insensitive (RFC 7230 §3.2); the security-sensitive
    // ones must classify regardless of case.
    RUVIA_CHECK(classify_request_header("host") == request_header_kind::host);
    RUVIA_CHECK(classify_request_header("HOST") == request_header_kind::host);
    RUVIA_CHECK(classify_request_header("content-length") == request_header_kind::content_length);
    RUVIA_CHECK(classify_request_header("CONTENT-LENGTH") == request_header_kind::content_length);
    RUVIA_CHECK(classify_request_header("Transfer-ENCODING") == request_header_kind::transfer_encoding);
    RUVIA_CHECK(classify_request_header("cOnNeCtIoN") == request_header_kind::connection);
}

RUVIA_TEST(request_header_classification_unknown_is_other) {
    RUVIA_CHECK(classify_request_header("") == request_header_kind::other);
    RUVIA_CHECK(classify_request_header("X-Custom-Header") == request_header_kind::other);
    RUVIA_CHECK(classify_request_header("Accept-Language") == request_header_kind::other);
    RUVIA_CHECK(classify_request_header("Content-Disposition") == request_header_kind::other);
    // Same length and first byte as a known header but a different name.
    RUVIA_CHECK(classify_request_header("Hosx") == request_header_kind::other);  // 4 bytes, not "Host"
    RUVIA_CHECK(
        classify_request_header("Hosts") == request_header_kind::other);  // 5 bytes, not "Range"
}

RUVIA_TEST(request_header_singleton_policy_table) {
    const request_header_kind singleton[] = {
        request_header_kind::access_control_request_method,
        request_header_kind::authorization,
        request_header_kind::content_type,
        request_header_kind::if_modified_since,
        request_header_kind::if_range,
        request_header_kind::if_unmodified_since,
        request_header_kind::origin,
        request_header_kind::range,
        request_header_kind::sec_websocket_key,
        request_header_kind::sec_websocket_version,
        request_header_kind::user_agent,
    };
    for (const auto kind : singleton) {
        RUVIA_CHECK(singleton_request_header_bit(kind) == (1U << static_cast<unsigned>(kind)));
    }

    const request_header_kind repeatable_or_special[] = {
        request_header_kind::other,
        request_header_kind::accept,
        request_header_kind::accept_encoding,
        request_header_kind::access_control_request_headers,
        request_header_kind::connection,
        request_header_kind::content_encoding,
        request_header_kind::content_length,
        request_header_kind::cookie,
        request_header_kind::expect,
        request_header_kind::host,
        request_header_kind::if_match,
        request_header_kind::if_none_match,
        request_header_kind::sec_websocket_protocol,
        request_header_kind::transfer_encoding,
        request_header_kind::upgrade,
        request_header_kind::forwarded,
        request_header_kind::x_forwarded_for,
        request_header_kind::x_forwarded_proto,
        request_header_kind::sec_websocket_extensions,
    };
    for (const auto kind : repeatable_or_special) {
        RUVIA_CHECK(singleton_request_header_bit(kind) == 0U);
    }
    RUVIA_CHECK(std::size(singleton) + std::size(repeatable_or_special) ==
                ruvia::detail::request_header_kind_count);
}
