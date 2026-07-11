#include "test_harness.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/body/HttpTransferCodingDecoder.h"
#include "ruvia/web/Error.h"

namespace {

using ruvia::defaultErrorCode;
using ruvia::HttpError;
using ruvia::HttpProtocolError;
using ruvia::httpReasonPhrase;

}  // namespace

RUVIA_TEST(http_reason_phrase_is_conventional_http1_presentation) {
    RUVIA_CHECK_EQ(httpReasonPhrase(200), std::string_view("OK"));
    RUVIA_CHECK_EQ(httpReasonPhrase(404), std::string_view("Not Found"));
    RUVIA_CHECK_EQ(httpReasonPhrase(413), std::string_view("Payload Too Large"));
    RUVIA_CHECK_EQ(httpReasonPhrase(500), std::string_view("Internal Server Error"));
    RUVIA_CHECK_EQ(httpReasonPhrase(205), std::string_view("Reset Content"));
    RUVIA_CHECK_EQ(httpReasonPhrase(502), std::string_view("Bad Gateway"));
    RUVIA_CHECK_EQ(httpReasonPhrase(504), std::string_view("Gateway Timeout"));
}

RUVIA_TEST(http_reason_phrase_does_not_mislabel_extension_statuses) {
    RUVIA_CHECK(httpReasonPhrase(299).empty());
    RUVIA_CHECK(httpReasonPhrase(499).empty());
    RUVIA_CHECK(httpReasonPhrase(599).empty());
}

RUVIA_TEST(default_error_code_mapping) {
    RUVIA_CHECK_EQ(defaultErrorCode(400), std::string_view("bad_request"));
    RUVIA_CHECK_EQ(defaultErrorCode(404), std::string_view("not_found"));
    RUVIA_CHECK_EQ(defaultErrorCode(405), std::string_view("method_not_allowed"));
    RUVIA_CHECK_EQ(defaultErrorCode(413), std::string_view("payload_too_large"));
}

RUVIA_TEST(throw_request_body_too_large_is_413) {
    using ruvia::detail::throwRequestBodyTooLarge;
    bool caught = false;
    try {
        throwRequestBodyTooLarge();
    } catch (const HttpProtocolError& error) {
        caught = true;
        RUVIA_CHECK_EQ(error.status(), std::uint16_t{413});
        RUVIA_CHECK_EQ(std::string_view(error.what()), std::string_view("request body is too large"));
    }
    RUVIA_CHECK(caught);
}

RUVIA_TEST(http_protocol_error_owns_bounded_diagnostic_without_allocation) {
    std::string source(200, 'x');
    const HttpProtocolError error(413, source);
    source.assign(200, 'y');

    RUVIA_CHECK_EQ(error.status(), std::uint16_t{413});
    const auto diagnostic = std::string_view(error.what());
    RUVIA_CHECK_EQ(diagnostic.size(), std::size_t{127});
    RUVIA_CHECK(diagnostic.find_first_not_of('x') == std::string_view::npos);
}

RUVIA_TEST(http_error_info_round_trips) {
    const HttpError error(422, "unprocessable", "bad fields");
    const auto info = error.info();
    RUVIA_CHECK_EQ(info.status(), std::uint16_t{422});
    RUVIA_CHECK_EQ(info.code(), std::string_view("unprocessable"));
    RUVIA_CHECK_EQ(info.message(), std::string_view("bad fields"));
}
