#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/detail/http2/hpack/Http2HeaderContinuation.h"
#include "ruvia/http/detail/http2/hpack/Http2HeaderDecode.h"
#include "ruvia/web/detail/http2/CleartextUpgrade.h"

namespace {

using ruvia::detail::HeaderDecodeStatus;
using ruvia::detail::Http1ServerRequestParseFailureSource;
using ruvia::detail::HpackDecoder;
using ruvia::detail::CleartextHttp2Probe;
using ruvia::detail::http2ClassifyHeaderDecodeResult;
using ruvia::detail::kHttp2ClientPreface;
using ruvia::detail::probeCleartextHttp2Preface;
using ruvia::detail::shouldDropInvalidCleartextHttp1Input;

bool rejectHeader(void*, std::string_view, std::string_view) {
    return false;
}

}  // namespace

RUVIA_TEST(http2_cleartext_startup_accepts_only_prior_knowledge_preface) {
    RUVIA_CHECK(
        probeCleartextHttp2Preface("GET / HTTP/1.1\r\n", false) ==
        CleartextHttp2Probe::kHttp1);
    RUVIA_CHECK(
        probeCleartextHttp2Preface("PRI * HT", false) ==
        CleartextHttp2Probe::kNeedMorePreface);
    RUVIA_CHECK(
        probeCleartextHttp2Preface(kHttp2ClientPreface, false) ==
        CleartextHttp2Probe::kCompletePreface);
    RUVIA_CHECK(
        probeCleartextHttp2Preface("PRI /not-http2\r\n", false) ==
        CleartextHttp2Probe::kDropConnection);

    // AutoHTTPS deliberately reserves the cleartext listener for HTTP/1 redirect
    // handling instead of accepting a prior-knowledge HTTP/2 connection.
    RUVIA_CHECK(
        probeCleartextHttp2Preface(kHttp2ClientPreface, true) ==
        CleartextHttp2Probe::kHttp1);
}

RUVIA_TEST(classify_header_decode_result) {
    HpackDecoder decoder(std::pmr::get_default_resource());
    // A clean decode is OK.
    RUVIA_CHECK(http2ClassifyHeaderDecodeResult(decoder.decode({}, nullptr, nullptr)) ==
                HeaderDecodeStatus::kOk);
    // A header-validation callback rejection is a protocol error.
    RUVIA_CHECK(http2ClassifyHeaderDecodeResult(decoder.decode(
                    std::string_view("\x82", 1), nullptr, &rejectHeader)) ==
                HeaderDecodeStatus::kProtocolError);
    // Any HPACK decoding fault is a compression error (RFC 7541 4.1).
    RUVIA_CHECK(http2ClassifyHeaderDecodeResult(decoder.decode(
                    std::string_view("\x80", 1), nullptr, nullptr)) ==
                HeaderDecodeStatus::kCompressionError);
}

RUVIA_TEST(drop_invalid_cleartext_only_for_request_line_errors) {
    // Only request-line-shaped parse failures are candidates for a silent drop.
    RUVIA_CHECK(!shouldDropInvalidCleartextHttp1Input(
        "blah blah\r\n",
        Http1ServerRequestParseFailureSource::kMessage));
}

RUVIA_TEST(drop_invalid_cleartext_by_version_token) {
    // A request line whose final token is not an HTTP version looks like non-HTTP
    // traffic (a port scan, TLS on a cleartext port) and is dropped.
    RUVIA_CHECK(shouldDropInvalidCleartextHttp1Input(
        "FOO /path GARBAGE\r\n",
        Http1ServerRequestParseFailureSource::kRequestLine));
    RUVIA_CHECK(shouldDropInvalidCleartextHttp1Input(
        "random bytes here\r\n",
        Http1ServerRequestParseFailureSource::kRequestLine));

    // A well-formed HTTP-version token means a real client request -> keep. Its
    // extension method is valid syntax and is handled as a Web-level 501 later.
    RUVIA_CHECK(!shouldDropInvalidCleartextHttp1Input(
        "XX /p HTTP/1.1\r\n",
        Http1ServerRequestParseFailureSource::kRequestLine));
    RUVIA_CHECK(!shouldDropInvalidCleartextHttp1Input(
        "PRI * HTTP/2.0\r\n",
        Http1ServerRequestParseFailureSource::kRequestLine));

    // No line terminator, or a single token, cannot be classified -> keep.
    RUVIA_CHECK(!shouldDropInvalidCleartextHttp1Input(
        "no-line-break",
        Http1ServerRequestParseFailureSource::kRequestLine));
    RUVIA_CHECK(!shouldDropInvalidCleartextHttp1Input(
        "singletoken\r\n",
        Http1ServerRequestParseFailureSource::kRequestLine));
}
