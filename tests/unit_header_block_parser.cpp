#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/parser/HttpHeaderBlockParser.h"
#include "ruvia/http/HttpParseError.h"

namespace {

using ruvia::HttpParseError;
using ruvia::detail::findHttpHeaderEnd;
using ruvia::detail::HttpContentLengthParseStatus;
using ruvia::detail::HttpContentLengthState;
using ruvia::detail::HttpTransferCoding;
using ruvia::detail::HttpTransferEncodingParseStatus;
using ruvia::detail::HttpTransferEncodingState;

template <typename T>
concept HasValueSemanticTransferCodings =
    requires(const T& value) {
        { value.transferCodings() } ->
            std::same_as<ruvia::detail::HttpTransferCodings>;
    } &&
    requires(const T&& value) {
        { std::move(value).transferCodings() } ->
            std::same_as<ruvia::detail::HttpTransferCodings>;
    };

static_assert(HasValueSemanticTransferCodings<
    ruvia::detail::HttpNonChunkedTransferEncoding>);
static_assert(HasValueSemanticTransferCodings<
    ruvia::detail::HttpFinalChunkedTransferEncoding>);
using ruvia::detail::ParsedRequestHeaderBlock;
using ruvia::detail::parseHttpHeaderBlock;

struct Parsed final {
    std::optional<HttpParseError> error;
    bool hasHost;
    bool sawChunked;
    bool hasContentLength;
    std::size_t contentLength;
    std::size_t transferCodingCount;
    ruvia::detail::HttpTransferCoding firstTransferCoding;
};

Parsed parse(std::string_view head) {
    ParsedRequestHeaderBlock block{};
    const auto headerBytes = findHttpHeaderEnd(head, 0);
    const auto error = parseHttpHeaderBlock(head, headerBytes, block);
    const auto contentLength = block.contentLength.value();
    const auto transferEncoding = block.transferEncoding.value();
    const auto* finalChunked = transferEncoding.has_value()
        ? transferEncoding->finalChunked()
        : nullptr;
    const auto* nonChunked = transferEncoding.has_value()
        ? transferEncoding->nonChunked()
        : nullptr;
    ruvia::detail::HttpTransferCodings transferCodings;
    if (finalChunked != nullptr) {
        transferCodings = finalChunked->transferCodings();
    } else if (nonChunked != nullptr) {
        transferCodings = nonChunked->transferCodings();
    }
    return {error, block.hostHeaderIndex >= 0, finalChunked != nullptr,
            contentLength.has_value(), contentLength.value_or(0),
            transferCodings.count,
            transferCodings.values[0]};
}

RUVIA_TEST(content_length_field_updates_are_transactional) {
    HttpContentLengthState state;
    RUVIA_CHECK(
        state.parseField("5") == HttpContentLengthParseStatus::kOk);
    RUVIA_CHECK(state.value() == std::optional<std::size_t>(5));

    RUVIA_CHECK(
        state.parseField("5, invalid") ==
        HttpContentLengthParseStatus::kInvalid);
    RUVIA_CHECK(state.value() == std::optional<std::size_t>(5));

    RUVIA_CHECK(
        state.parseField("6, 6") ==
        HttpContentLengthParseStatus::kConflicting);
    RUVIA_CHECK(state.value() == std::optional<std::size_t>(5));
}

RUVIA_TEST(transfer_encoding_field_updates_are_transactional_and_discriminated) {
    HttpTransferEncodingState state;
    RUVIA_CHECK(!state.value().has_value());
    RUVIA_CHECK(
        state.parseField("gzip") == HttpTransferEncodingParseStatus::kOk);

    auto value = state.value();
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK(value->nonChunked() != nullptr);
    RUVIA_CHECK(value->finalChunked() == nullptr);
    if (const auto* nonChunked = value->nonChunked()) {
        RUVIA_CHECK_EQ(nonChunked->transferCodings().count, std::size_t{1});
        RUVIA_CHECK(
            nonChunked->transferCodings().values[0] ==
            HttpTransferCoding::kGzip);
    }

    RUVIA_CHECK(
        state.parseField("chunked, deflate") ==
        HttpTransferEncodingParseStatus::kMalformed);
    value = state.value();
    RUVIA_CHECK(value->nonChunked() != nullptr);
    RUVIA_CHECK(value->finalChunked() == nullptr);

    RUVIA_CHECK(
        state.parseField("g@zip") ==
        HttpTransferEncodingParseStatus::kMalformed);
    RUVIA_CHECK(
        state.parseField(R"(custom; level="a\"b")") ==
        HttpTransferEncodingParseStatus::kUnsupported);
    RUVIA_CHECK(
        state.parseField("custom; level") ==
        HttpTransferEncodingParseStatus::kMalformed);
    value = state.value();
    RUVIA_CHECK(value->nonChunked() != nullptr);
    RUVIA_CHECK(value->finalChunked() == nullptr);

    RUVIA_CHECK(
        state.parseField("chunked") == HttpTransferEncodingParseStatus::kOk);
    value = state.value();
    RUVIA_CHECK(value->nonChunked() == nullptr);
    RUVIA_CHECK(value->finalChunked() != nullptr);
    if (const auto* finalChunked = value->finalChunked()) {
        RUVIA_CHECK_EQ(finalChunked->transferCodings().count, std::size_t{1});
    }
}

}  // namespace

RUVIA_TEST(header_block_parses_valid_request) {
    const auto result = parse("GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\n");
    RUVIA_CHECK(!result.error.has_value());
    RUVIA_CHECK(result.hasHost);
    RUVIA_CHECK(result.hasContentLength);
    RUVIA_CHECK_EQ(result.contentLength, std::size_t{5});
}

RUVIA_TEST(header_block_rejects_conflicting_content_length) {
    // Two Content-Length values that disagree is a classic request-smuggling
    // vector and must be rejected.
    const auto result = parse(
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kConflictingContentLength);
}

RUVIA_TEST(header_block_allows_repeated_equal_content_length) {
    const auto result = parse(
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n");
    RUVIA_CHECK(!result.error.has_value());
    RUVIA_CHECK_EQ(result.contentLength, std::size_t{5});
}

RUVIA_TEST(header_block_allows_combined_equal_content_length) {
    const auto result = parse(
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5, 5\r\n\r\n");
    RUVIA_CHECK(!result.error.has_value());
    RUVIA_CHECK(result.hasContentLength);
    RUVIA_CHECK_EQ(result.contentLength, std::size_t{5});

    const auto conflict = parse(
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5, 6\r\n\r\n");
    RUVIA_CHECK(conflict.error == HttpParseError::kConflictingContentLength);
}

RUVIA_TEST(header_block_rejects_invalid_content_length) {
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n").error ==
                HttpParseError::kInvalidContentLength);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: -5\r\n\r\n").error ==
                HttpParseError::kInvalidContentLength);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5x\r\n\r\n").error ==
                HttpParseError::kInvalidContentLength);
}

RUVIA_TEST(header_block_rejects_duplicate_host) {
    // A duplicate Host header is ambiguous (host confusion) and must be rejected.
    const auto result = parse("GET / HTTP/1.1\r\nHost: a.com\r\nHost: b.com\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kInvalidHost);
}

RUVIA_TEST(header_block_uses_recipient_connection_and_upgrade_list_rules) {
    const auto tolerant = parse(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Connection: , keep-alive,\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: , custom/1, websocket,\r\n\r\n");
    RUVIA_CHECK(!tolerant.error.has_value());

    RUVIA_CHECK(
        parse(
            "GET / HTTP/1.1\r\nHost: x\r\n"
            "Connection: close;invalid\r\n\r\n")
            .error == HttpParseError::kInvalidConnection);
    RUVIA_CHECK(
        parse(
            "GET / HTTP/1.1\r\nHost: x\r\n"
            "Upgrade: websocket/\r\n\r\n")
            .error == HttpParseError::kInvalidUpgrade);
}

RUVIA_TEST(header_block_rejects_duplicate_content_type) {
    // Content-Type is a singleton representation header; accepting duplicates
    // lets body helpers observe only the cached last value.
    const auto result = parse(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Type: application/json\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kInvalidHeader);
}

RUVIA_TEST(header_block_rejects_duplicate_range) {
    // Ruvia file responses only support a single byte-range set. Repeated Range
    // fields must not degrade to cached last-value behavior.
    const auto result = parse(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "Range: bytes=0-99\r\n"
        "Range: bytes=200-299\r\n\r\n");
    RUVIA_CHECK(result.error == HttpParseError::kInvalidHeader);
}

RUVIA_TEST(header_block_accepts_repeated_etag_list_fields) {
    // If-None-Match is a list field, so repeated lines are equivalent to a
    // comma-joined value (RFC 9110 §5.3). The execution layer folds all lines.
    const auto result = parse(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "If-None-Match: \"old\"\r\n"
        "If-None-Match: \"new\"\r\n\r\n");
    RUVIA_CHECK(!result.error.has_value());
}

RUVIA_TEST(header_block_rejects_duplicate_auth_and_cors_singletons) {
    RUVIA_CHECK(parse(
                    "GET / HTTP/1.1\r\nHost: x\r\n"
                    "Authorization: Bearer first\r\n"
                    "Authorization: Bearer second\r\n\r\n")
                    .error == HttpParseError::kInvalidHeader);
    RUVIA_CHECK(parse(
                    "GET / HTTP/1.1\r\nHost: x\r\n"
                    "Origin: https://a.example\r\n"
                    "Origin: https://b.example\r\n\r\n")
                    .error == HttpParseError::kInvalidHeader);
    RUVIA_CHECK(parse(
                    "OPTIONS / HTTP/1.1\r\nHost: x\r\n"
                    "Access-Control-Request-Method: GET\r\n"
                    "Access-Control-Request-Method: POST\r\n\r\n")
                    .error == HttpParseError::kInvalidHeader);
}

RUVIA_TEST(header_block_enforces_cors_request_field_grammar) {
    for (const auto value : {
             std::string_view(""),
             std::string_view("POST, DELETE"),
             std::string_view("POST /admin")}) {
        const auto result = parse(
            std::string("OPTIONS / HTTP/1.1\r\nHost: x\r\n") +
            "Access-Control-Request-Method: " + std::string(value) +
            "\r\n\r\n");
        RUVIA_CHECK(result.error == HttpParseError::kInvalidHeader);
    }

    for (const auto value : {
             std::string_view(""),
             std::string_view(", ,"),
             std::string_view("X-Good, X Bad")}) {
        const auto result = parse(
            std::string("OPTIONS / HTTP/1.1\r\nHost: x\r\n") +
            "Access-Control-Request-Headers: " + std::string(value) +
            "\r\n\r\n");
        RUVIA_CHECK(result.error == HttpParseError::kInvalidHeader);
    }

    for (const auto value : {
             std::string_view("*"),
             std::string_view("https://app.example/"),
             std::string_view("https://APP.example")}) {
        const auto result = parse(
            std::string("GET / HTTP/1.1\r\nHost: x\r\nOrigin: ") +
            std::string(value) + "\r\n\r\n");
        RUVIA_CHECK(result.error == HttpParseError::kInvalidHeader);
    }

    RUVIA_CHECK(!parse(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\n"
        "Origin: https://first.example https://second.example\r\n"
        "Access-Control-Request-Method: PATCH\r\n"
        "Access-Control-Request-Headers: , X-One,, X-Two,\r\n\r\n")
        .error.has_value());
    RUVIA_CHECK(!parse(
        "GET / HTTP/1.1\r\nHost: x\r\nOrigin: null\r\n\r\n")
        .error.has_value());
}

RUVIA_TEST(header_block_rejects_invalid_bracketed_host_literal) {
    RUVIA_CHECK(!parse(
        "GET / HTTP/1.1\r\nHost: [::1]\r\n\r\n").error.has_value());
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: [::::]\r\n\r\n").error == HttpParseError::kInvalidHost);
}

RUVIA_TEST(header_block_accepts_transfer_encoding_chunked) {
    const auto result = parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(!result.error.has_value());
    RUVIA_CHECK(result.sawChunked);
}

RUVIA_TEST(header_block_rejects_transfer_coding_parameters) {
    for (const auto value : {
             std::string_view("chunked;note=\"a,b\""),
             std::string_view("gzip;level=9, chunked")}) {
        const auto result = parse(
            std::string("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ") +
            std::string(value) + "\r\n\r\n");
        RUVIA_CHECK(result.error == HttpParseError::kInvalidTransferEncoding);
    }
}

RUVIA_TEST(header_block_accepts_one_transfer_coding_before_final_chunked) {
    // RFC 9112 §6.1 explicitly defines gzip, chunked: the content is compressed
    // first and chunked last for message framing.
    const auto gzipChunked = parse(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n");
    RUVIA_CHECK(!gzipChunked.error.has_value());
    RUVIA_CHECK(gzipChunked.sawChunked);
    RUVIA_CHECK_EQ(gzipChunked.transferCodingCount, std::size_t{1});
    RUVIA_CHECK(
        gzipChunked.firstTransferCoding ==
        ruvia::detail::HttpTransferCoding::kGzip);

    const auto split = parse(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: deflate\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(!split.error.has_value());
    RUVIA_CHECK(split.sawChunked);
    RUVIA_CHECK_EQ(split.transferCodingCount, std::size_t{1});
    RUVIA_CHECK(
        split.firstTransferCoding ==
        ruvia::detail::HttpTransferCoding::kDeflate);

    // A lone non-chunked coding is recorded here, then rejected by the request-level
    // body planner because request Transfer-Encoding requires final chunked framing.
    const auto lone = parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n");
    RUVIA_CHECK(!lone.error.has_value());
    RUVIA_CHECK(!lone.sawChunked);
    RUVIA_CHECK_EQ(lone.transferCodingCount, std::size_t{1});
    RUVIA_CHECK(lone.firstTransferCoding == ruvia::detail::HttpTransferCoding::kGzip);
}

RUVIA_TEST(header_block_rejects_smuggling_transfer_encodings) {
    // "chunked" must be the final coding.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, gzip\r\n\r\n").error ==
                HttpParseError::kInvalidTransferEncoding);
    // A second coding after chunked (here a repeated header) is malformed.
    RUVIA_CHECK(parse(
                    "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n")
                    .error == HttpParseError::kInvalidTransferEncoding);
    // An unknown coding is unsupported.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: bogus\r\n\r\n").error ==
                HttpParseError::kUnsupportedTransferEncoding);
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: bogus; level=1\r\n\r\n").error ==
                HttpParseError::kUnsupportedTransferEncoding);
    // Invalid transfer-coding grammar is a malformed request, not an unknown
    // extension that merits 501.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: g@zip\r\n\r\n").error ==
                HttpParseError::kInvalidTransferEncoding);
    // More than one non-chunked coding exceeds the single-coding limit.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, deflate, chunked\r\n\r\n").error ==
                HttpParseError::kUnsupportedTransferEncoding);
    // Empty transfer-coding list items are malformed in this framing-sensitive header.
    RUVIA_CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ,chunked\r\n\r\n").error ==
                HttpParseError::kInvalidTransferEncoding);
}

RUVIA_TEST(header_block_content_length_edge_cases) {
    // OWS around the value is trimmed.
    const auto ows = parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length:   42  \r\n\r\n");
    RUVIA_CHECK(!ows.error.has_value());
    RUVIA_CHECK_EQ(ows.contentLength, std::size_t{42});
    // Leading zeros parse to the same numeric value (no desync).
    const auto zeros = parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 007\r\n\r\n");
    RUVIA_CHECK(!zeros.error.has_value());
    RUVIA_CHECK_EQ(zeros.contentLength, std::size_t{7});
    // A '+' sign, a hex form, and overflow are all rejected rather than wrapped.
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\n").error ==
                HttpParseError::kInvalidContentLength);
    RUVIA_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0x10\r\n\r\n").error ==
                HttpParseError::kInvalidContentLength);
    RUVIA_CHECK(parse(
                    "GET / HTTP/1.1\r\nHost: x\r\n"
                    "Content-Length: 99999999999999999999999999\r\n\r\n")
                    .error == HttpParseError::kInvalidContentLength);
}
