#include "test_harness.h"

#include "ruvia/http/Http1RequestParser.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/HttpStatus.h"

namespace {

using ruvia::HttpParseError;
using ruvia::httpParseProtocolError;
using ruvia::HttpProtocolError;
using ruvia::httpReasonPhrase;

inline constexpr auto kOkStatusToken =
    ruvia::detail::httpStatusCodeToken(ruvia::http_status::kOk);

}  // namespace

static_assert(std::is_trivially_copyable_v<ruvia::HttpStatusCode>);
static_assert(sizeof(ruvia::HttpStatusCode) == sizeof(std::uint16_t));
static_assert(!std::is_constructible_v<ruvia::HttpStatusCode, std::uint16_t>);
static_assert(!std::is_convertible_v<ruvia::HttpStatusCode, std::uint16_t>);
static_assert(!std::is_convertible_v<std::uint16_t, ruvia::HttpStatusCode>);
static_assert(ruvia::http_status::kContinue.isInformational());
static_assert(ruvia::http_status::kOk.isSuccessful());
static_assert(ruvia::http_status::kTemporaryRedirect.isRedirection());
static_assert(ruvia::http_status::kBadRequest.isClientError());
static_assert(ruvia::http_status::kInternalServerError.isServerError());
static_assert(ruvia::http_status::kBadRequest.isError());
static_assert(!ruvia::http_status::kOk.isError());
static_assert(
    ruvia::detail::httpStatusCodeTokenView(kOkStatusToken) ==
    std::string_view("200"));

RUVIA_TEST(http_status_code_validates_the_wire_value_boundary) {
    RUVIA_CHECK(
        ruvia::HttpStatusCode::tryFromValue(100) ==
        ruvia::http_status::kContinue);
    RUVIA_CHECK(
        ruvia::HttpStatusCode::tryFromValue(599) ==
        ruvia::HttpStatusCode::fromValue(599));
    RUVIA_CHECK(!ruvia::HttpStatusCode::tryFromValue(99).has_value());
    RUVIA_CHECK(!ruvia::HttpStatusCode::tryFromValue(600).has_value());

    bool threw = false;
    try {
        (void)ruvia::HttpStatusCode::fromValue(600);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(http_status_code_wire_token_is_derived_from_the_strong_type) {
    const auto token = ruvia::detail::httpStatusCodeToken(
        ruvia::HttpStatusCode::fromValue(599));
    RUVIA_CHECK_EQ(
        ruvia::detail::httpStatusCodeTokenView(token),
        std::string_view("599"));
}

RUVIA_TEST(http_reason_phrase_is_conventional_http1_presentation) {
    RUVIA_CHECK_EQ(
        httpReasonPhrase(ruvia::http_status::kOk), std::string_view("OK"));
    RUVIA_CHECK_EQ(
        httpReasonPhrase(ruvia::http_status::kNotFound),
        std::string_view("Not Found"));
    RUVIA_CHECK_EQ(
        httpReasonPhrase(ruvia::http_status::kContentTooLarge),
        std::string_view("Content Too Large"));
    RUVIA_CHECK_EQ(
        httpReasonPhrase(ruvia::http_status::kInternalServerError),
        std::string_view("Internal Server Error"));
    RUVIA_CHECK_EQ(
        httpReasonPhrase(ruvia::http_status::kResetContent),
        std::string_view("Reset Content"));
    RUVIA_CHECK_EQ(
        httpReasonPhrase(ruvia::http_status::kBadGateway),
        std::string_view("Bad Gateway"));
    RUVIA_CHECK_EQ(
        httpReasonPhrase(ruvia::http_status::kGatewayTimeout),
        std::string_view("Gateway Timeout"));
}

RUVIA_TEST(http_reason_phrase_does_not_mislabel_extension_statuses) {
    // 104 is still a temporary draft registration, so its unstable name is not
    // promoted into the framework's stable public vocabulary.
    RUVIA_CHECK(
        httpReasonPhrase(ruvia::HttpStatusCode::fromValue(104)).empty());
    RUVIA_CHECK(
        httpReasonPhrase(ruvia::HttpStatusCode::fromValue(299)).empty());
    RUVIA_CHECK(
        httpReasonPhrase(ruvia::HttpStatusCode::fromValue(499)).empty());
    RUVIA_CHECK(
        httpReasonPhrase(ruvia::HttpStatusCode::fromValue(599)).empty());
}

RUVIA_TEST(http_protocol_error_owns_bounded_diagnostic_without_allocation) {
    std::string source(200, 'x');
    const HttpProtocolError error(ruvia::http_status::kContentTooLarge, source);
    source.assign(200, 'y');

    RUVIA_CHECK_EQ(error.status(), ruvia::http_status::kContentTooLarge);
    const auto diagnostic = std::string_view(error.what());
    RUVIA_CHECK_EQ(diagnostic.size(), std::size_t{127});
    RUVIA_CHECK(diagnostic.find_first_not_of('x') == std::string_view::npos);
}

RUVIA_TEST(parse_error_status_mapping) {
    // Size limits map to their specific statuses.
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kHeaderTooLarge).status(), ruvia::http_status::kRequestHeaderFieldsTooLarge);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kTooManyHeaders).status(), ruvia::http_status::kRequestHeaderFieldsTooLarge);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kBodyTooLarge).status(), ruvia::http_status::kContentTooLarge);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kUnsupportedTransferEncoding).status(), ruvia::http_status::kNotImplemented);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kUnsupportedHttpVersion).status(), ruvia::http_status::kHttpVersionNotSupported);
    // Everything else is a 400 Bad Request.
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kMissingHost).status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kInvalidConnection).status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kInvalidUpgrade).status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kInvalidChunkSize).status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(httpParseProtocolError(HttpParseError::kConflictingContentLength).status(), ruvia::http_status::kBadRequest);
}
