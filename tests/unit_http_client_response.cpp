#include "test_harness.h"

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <memory_resource>
#include <string_view>

#include "http/client/HttpClientContentEncoding.h"
#include "http/client/HttpClientRedirect.h"
#include "http/client/HttpClientAccess.h"
#include "http/client/HttpClientResponseParser.h"

namespace {

using ruvia::FetchResponse;
using ruvia::detail::HttpClientResponseHead;
using ruvia::detail::isValidHttpClientOriginTarget;
using ruvia::detail::parseHttpClientResponseHead;

// Parse a header block (as produced by the pool: status line + headers joined by
// CRLF, WITHOUT the terminating CRLFCRLF).
HttpClientResponseHead parseHead(std::string_view method, std::string_view headerSection) {
    FetchResponse response = ruvia::detail::FetchResponseAccess::make(std::pmr::get_default_resource());
    return parseHttpClientResponseHead(
        method, headerSection, response, std::pmr::get_default_resource());
}

// Returns true if parsing the given header block throws (malformed / rejected framing).
bool parseThrows(std::string_view method, std::string_view headerSection) {
    try {
        (void)parseHead(method, headerSection);
        return false;
    } catch (...) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(http_client_origin_target_validation) {
    RUVIA_CHECK(isValidHttpClientOriginTarget("/ok%2F?q=%7B%7D"));
    RUVIA_CHECK(isValidHttpClientOriginTarget("*"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget(""));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("relative"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad#fragment"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad\\path"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%zz"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%2"));
}

// --- Content-Length framing ----------------------------------------------
RUVIA_TEST(http_client_content_length_body) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5");
    RUVIA_CHECK(head.responseMayHaveBody);
    RUVIA_CHECK(head.hasContentLength);
    RUVIA_CHECK(!head.hasTransferEncoding);
    RUVIA_CHECK(!head.isChunked);
    RUVIA_CHECK_EQ(head.contentLength, std::size_t{5});
    RUVIA_CHECK_EQ(head.bodyOffset, std::string_view("HTTP/1.1 200 OK\r\nContent-Length: 5").size() + 4);
}

// --- Chunked framing is decodable and connection-reusable ----------------
RUVIA_TEST(http_client_chunked_is_reusable) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    RUVIA_CHECK(head.responseMayHaveBody);
    RUVIA_CHECK(head.hasTransferEncoding);
    RUVIA_CHECK(head.isChunked);
    RUVIA_CHECK(!head.hasContentLength);
    // A self-delimiting chunked body must not force the connection closed.
    RUVIA_CHECK(!head.closeAfterResponse);
}

RUVIA_TEST(http_client_chunked_case_insensitive) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\ntransfer-encoding: Chunked");
    RUVIA_CHECK(head.isChunked);
}

RUVIA_TEST(http_client_chunked_transfer_parameter_is_reusable) {
    const auto head = parseHead(
        "GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;note=\"a,b\";foo=bar");
    RUVIA_CHECK(head.hasTransferEncoding);
    RUVIA_CHECK(head.isChunked);
    RUVIA_CHECK(!head.closeAfterResponse);
}

RUVIA_TEST(http_client_transfer_encoding_empty_item_is_unsupported) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: , chunked");
    RUVIA_CHECK(head.hasTransferEncoding);
    RUVIA_CHECK(!head.isChunked);
    RUVIA_CHECK(head.closeAfterResponse);
}

// --- Undecodable transfer coding: body delimited by close ----------------
RUVIA_TEST(http_client_non_chunked_te_closes) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    RUVIA_CHECK(head.hasTransferEncoding);
    RUVIA_CHECK(!head.isChunked);
    RUVIA_CHECK(head.closeAfterResponse);
}

RUVIA_TEST(http_client_te_list_with_chunked_is_unsupported) {
    // "gzip, chunked" is a coding list we cannot decode (no gzip transfer-decoder).
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    RUVIA_CHECK(head.hasTransferEncoding);
    RUVIA_CHECK(!head.isChunked);
}

RUVIA_TEST(http_client_multiple_te_headers_unsupported) {
    RUVIA_CHECK(parseThrows(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked"));
}

// --- Request-smuggling guard: both CL and TE must be rejected -------------
RUVIA_TEST(http_client_content_length_and_te_rejected) {
    RUVIA_CHECK(parseThrows(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked"));
    // Order should not matter.
    RUVIA_CHECK(parseThrows(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5"));
}

// --- Bodiless responses ---------------------------------------------------
RUVIA_TEST(http_client_204_has_no_body) {
    const auto head = parseHead("GET", "HTTP/1.1 204 No Content");
    RUVIA_CHECK(!head.responseMayHaveBody);
}

RUVIA_TEST(http_client_head_cl_and_te_not_rejected) {
    // A bodiless response (HEAD) has no framing to exploit, so carrying both
    // Content-Length and Transfer-Encoding must stay interoperable, not throw.
    const auto head = parseHead(
        "HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nTransfer-Encoding: chunked");
    RUVIA_CHECK(!head.responseMayHaveBody);
}

RUVIA_TEST(http_client_304_cl_and_te_not_rejected) {
    const auto head = parseHead(
        "GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\nTransfer-Encoding: chunked");
    RUVIA_CHECK(!head.responseMayHaveBody);
}

RUVIA_TEST(http_client_304_has_no_body) {
    const auto head = parseHead("GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 10");
    RUVIA_CHECK(!head.responseMayHaveBody);
}

RUVIA_TEST(http_client_head_has_no_body) {
    // A response to HEAD never carries a body even with Content-Length.
    const auto head = parseHead("HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 100");
    RUVIA_CHECK(!head.responseMayHaveBody);
    RUVIA_CHECK(head.hasContentLength);
}

RUVIA_TEST(http_client_205_and_1xx_are_bodiless) {
    // The no-body status set is 204/205/304 plus every 1xx (status < 200). Only 204
    // and 304 were pinned; 205 Reset Content (RFC 7231 6.3.6) and interim 1xx
    // responses are equally bodiless -- a framed body there would desync the stream.
    RUVIA_CHECK(!parseHead("GET", "HTTP/1.1 205 Reset Content").responseMayHaveBody);
    RUVIA_CHECK(!parseHead("GET", "HTTP/1.1 100 Continue").responseMayHaveBody);
    RUVIA_CHECK(!parseHead("GET", "HTTP/1.1 103 Early Hints").responseMayHaveBody);
    // Positive control: a normal 200 GET response does expect a body.
    RUVIA_CHECK(parseHead("GET", "HTTP/1.1 200 OK").responseMayHaveBody);
}

// --- Content-Encoding detection ------------------------------------------
RUVIA_TEST(http_client_content_encoding_gzip) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 3");
    RUVIA_CHECK(head.hasContentEncoding);
    RUVIA_CHECK(head.contentCoding == ruvia::detail::HttpContentCoding::kGzip);
}

RUVIA_TEST(http_client_content_encoding_br) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 3");
    RUVIA_CHECK(head.contentCoding == ruvia::detail::HttpContentCoding::kBrotli);
}

RUVIA_TEST(http_client_content_encoding_identity_is_none) {
    const auto head = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Length: 3");
    RUVIA_CHECK(head.hasContentEncoding);
    RUVIA_CHECK(head.contentCoding == ruvia::detail::HttpContentCoding::kNone);
}

RUVIA_TEST(http_client_content_encoding_list_is_none) {
    // A multi-coding list is not decoded (delivered as received).
    const auto head = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nContent-Encoding: gzip, br\r\nContent-Length: 3");
    RUVIA_CHECK(head.contentCoding == ruvia::detail::HttpContentCoding::kNone);
}

RUVIA_TEST(http_client_content_encoding_multiple_headers_is_none) {
    const auto head = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Encoding: br\r\nContent-Length: 3");
    RUVIA_CHECK(head.contentCoding == ruvia::detail::HttpContentCoding::kNone);
}

// --- Connection: close ----------------------------------------------------
RUVIA_TEST(http_client_connection_close) {
    const auto head = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3");
    RUVIA_CHECK(head.closeAfterResponse);
}

// --- Malformed status lines / headers are rejected ------------------------
RUVIA_TEST(http_client_rejects_bad_version) {
    RUVIA_CHECK(parseThrows("GET", "HTTP/2 200 OK"));
}

RUVIA_TEST(http_client_rejects_bad_status_code) {
    RUVIA_CHECK(parseThrows("GET", "HTTP/1.1 99 Too Small"));
    RUVIA_CHECK(parseThrows("GET", "HTTP/1.1 abc Bad"));
}

RUVIA_TEST(http_client_rejects_switching_protocols_status) {
    RUVIA_CHECK(parseThrows("GET", "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket"));
}

RUVIA_TEST(http_client_rejects_bad_content_length) {
    RUVIA_CHECK(parseThrows("GET", "HTTP/1.1 200 OK\r\nContent-Length: notanumber"));
}

RUVIA_TEST(http_client_rejects_conflicting_content_length) {
    RUVIA_CHECK(parseThrows(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"));
}

RUVIA_TEST(http_client_duplicate_content_length_same_value_ok) {
    const auto head = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5");
    RUVIA_CHECK(head.hasContentLength);
    RUVIA_CHECK_EQ(head.contentLength, std::size_t{5});
}

#endif  // RUVIA_ENABLE_HTTP_CLIENT

// --- Response Content-Encoding: parser vs. decode-path must not drift --------
// The response content-coding decision is computed in TWO places: the parser
// fills HttpClientResponseHead::contentCoding, and httpClientResponseContentCoding
// re-derives the same decision from the stored headers for the actual decode.
// They apply identical rules (a single known token decodes; identity, an unknown
// coding, a comma list, or a repeated Content-Encoding header all yield kNone so
// the body is delivered as received). Pin the two paths to agree so a fix to one
// can never silently diverge from the other -- e.g. wrongly decoding a layered
// or attacker-supplied multi-coding response.
RUVIA_TEST(http_client_response_content_coding_paths_agree) {
    using ruvia::detail::HttpContentCoding;
    using ruvia::detail::httpClientResponseContentCoding;

    struct Case final {
        std::string_view headers;
        HttpContentCoding expected;
    };
    const Case cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip", HttpContentCoding::kGzip},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: x-gzip", HttpContentCoding::kGzip},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: GZIP", HttpContentCoding::kGzip},   // case-insensitive
        {"HTTP/1.1 200 OK\r\nContent-Encoding: br", HttpContentCoding::kBrotli},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: zstd", HttpContentCoding::kZstd},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity", HttpContentCoding::kNone},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: deflate", HttpContentCoding::kNone},  // unsupported
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip, br", HttpContentCoding::kNone}, // a list is not decoded
        // A repeated Content-Encoding header is a coding list split across lines:
        // both paths must refuse it (kNone), never decode only the first layer.
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Encoding: br", HttpContentCoding::kNone},
        {"HTTP/1.1 200 OK\r\nContent-Length: 0", HttpContentCoding::kNone},         // header absent
    };

    for (const auto& c : cases) {
        FetchResponse response =
            ruvia::detail::FetchResponseAccess::make(std::pmr::get_default_resource());
        const auto head = ruvia::detail::parseHttpClientResponseHead(
            "GET", c.headers, response, std::pmr::get_default_resource());
        const auto rederived = httpClientResponseContentCoding(response);
        RUVIA_CHECK(head.contentCoding == c.expected);
        RUVIA_CHECK(rederived == c.expected);
        RUVIA_CHECK(head.contentCoding == rederived);  // the two paths agree
    }
}
