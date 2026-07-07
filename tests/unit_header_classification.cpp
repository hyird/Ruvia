#include "test_harness.h"

#include <string_view>

#include "parser/HttpParserSyntax.h"

namespace {

using ruvia::detail::classifyRequestHeader;
using ruvia::detail::singletonRequestHeaderBit;
using ruvia::detail::RequestHeaderKind;

struct Case final {
    std::string_view name;
    RequestHeaderKind kind;
};

}  // namespace

// classifyRequestHeader drives the known-header fast paths, including the
// smuggling-sensitive Host / Content-Length / Transfer-Encoding / Connection
// detection. Lock the whole name->kind table so a typo can't silently
// misclassify a security-relevant header down to kOther.
RUVIA_TEST(request_header_classification_table) {
    const Case cases[] = {
        {"Accept", RequestHeaderKind::kAccept},
        {"Accept-Encoding", RequestHeaderKind::kAcceptEncoding},
        {"Access-Control-Request-Headers", RequestHeaderKind::kAccessControlRequestHeaders},
        {"Access-Control-Request-Method", RequestHeaderKind::kAccessControlRequestMethod},
        {"Authorization", RequestHeaderKind::kAuthorization},
        {"Connection", RequestHeaderKind::kConnection},
        {"Content-Encoding", RequestHeaderKind::kContentEncoding},
        {"Content-Length", RequestHeaderKind::kContentLength},
        {"Content-Type", RequestHeaderKind::kContentType},
        {"Cookie", RequestHeaderKind::kCookie},
        {"Expect", RequestHeaderKind::kExpect},
        {"Host", RequestHeaderKind::kHost},
        {"If-Match", RequestHeaderKind::kIfMatch},
        {"If-Modified-Since", RequestHeaderKind::kIfModifiedSince},
        {"If-None-Match", RequestHeaderKind::kIfNoneMatch},
        {"If-Range", RequestHeaderKind::kIfRange},
        {"If-Unmodified-Since", RequestHeaderKind::kIfUnmodifiedSince},
        {"Origin", RequestHeaderKind::kOrigin},
        {"Range", RequestHeaderKind::kRange},
        {"Sec-WebSocket-Key", RequestHeaderKind::kSecWebSocketKey},
        {"Sec-WebSocket-Protocol", RequestHeaderKind::kSecWebSocketProtocol},
        {"Sec-WebSocket-Version", RequestHeaderKind::kSecWebSocketVersion},
        {"Transfer-Encoding", RequestHeaderKind::kTransferEncoding},
        {"Upgrade", RequestHeaderKind::kUpgrade},
        {"User-Agent", RequestHeaderKind::kUserAgent},
    };
    for (const auto& entry : cases) {
        RUVIA_CHECK(classifyRequestHeader(entry.name) == entry.kind);
    }
}

RUVIA_TEST(request_header_classification_is_case_insensitive) {
    // Field names are case-insensitive (RFC 7230 §3.2); the security-sensitive
    // ones must classify regardless of case.
    RUVIA_CHECK(classifyRequestHeader("host") == RequestHeaderKind::kHost);
    RUVIA_CHECK(classifyRequestHeader("HOST") == RequestHeaderKind::kHost);
    RUVIA_CHECK(classifyRequestHeader("content-length") == RequestHeaderKind::kContentLength);
    RUVIA_CHECK(classifyRequestHeader("CONTENT-LENGTH") == RequestHeaderKind::kContentLength);
    RUVIA_CHECK(classifyRequestHeader("Transfer-ENCODING") == RequestHeaderKind::kTransferEncoding);
    RUVIA_CHECK(classifyRequestHeader("cOnNeCtIoN") == RequestHeaderKind::kConnection);
}

RUVIA_TEST(request_header_classification_unknown_is_other) {
    RUVIA_CHECK(classifyRequestHeader("") == RequestHeaderKind::kOther);
    RUVIA_CHECK(classifyRequestHeader("X-Custom-Header") == RequestHeaderKind::kOther);
    RUVIA_CHECK(classifyRequestHeader("Accept-Language") == RequestHeaderKind::kOther);
    RUVIA_CHECK(classifyRequestHeader("Content-Disposition") == RequestHeaderKind::kOther);
    // Same length and first byte as a known header but a different name.
    RUVIA_CHECK(classifyRequestHeader("Hosx") == RequestHeaderKind::kOther);   // 4 bytes, not "Host"
    RUVIA_CHECK(classifyRequestHeader("Hosts") == RequestHeaderKind::kOther);  // 5 bytes, not "Range"
}

RUVIA_TEST(request_header_singleton_policy_table) {
    const RequestHeaderKind singleton[] = {
        RequestHeaderKind::kAccessControlRequestMethod,
        RequestHeaderKind::kAuthorization,
        RequestHeaderKind::kContentType,
        RequestHeaderKind::kIfMatch,
        RequestHeaderKind::kIfModifiedSince,
        RequestHeaderKind::kIfNoneMatch,
        RequestHeaderKind::kIfRange,
        RequestHeaderKind::kIfUnmodifiedSince,
        RequestHeaderKind::kOrigin,
        RequestHeaderKind::kRange,
    };
    for (const auto kind : singleton) {
        RUVIA_CHECK(singletonRequestHeaderBit(kind) == (1U << static_cast<unsigned>(kind)));
    }

    const RequestHeaderKind repeatableOrSpecial[] = {
        RequestHeaderKind::kOther,
        RequestHeaderKind::kAccept,
        RequestHeaderKind::kAcceptEncoding,
        RequestHeaderKind::kAccessControlRequestHeaders,
        RequestHeaderKind::kConnection,
        RequestHeaderKind::kContentEncoding,
        RequestHeaderKind::kContentLength,
        RequestHeaderKind::kCookie,
        RequestHeaderKind::kExpect,
        RequestHeaderKind::kHost,
        RequestHeaderKind::kSecWebSocketKey,
        RequestHeaderKind::kSecWebSocketProtocol,
        RequestHeaderKind::kSecWebSocketVersion,
        RequestHeaderKind::kTransferEncoding,
        RequestHeaderKind::kUpgrade,
        RequestHeaderKind::kUserAgent,
    };
    for (const auto kind : repeatableOrSpecial) {
        RUVIA_CHECK(singletonRequestHeaderBit(kind) == 0U);
    }
}
