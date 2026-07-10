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
using ruvia::httpStatusText;

}  // namespace

RUVIA_TEST(http_status_text_known_and_fallback) {
    RUVIA_CHECK_EQ(httpStatusText(200), std::string_view("OK"));
    RUVIA_CHECK_EQ(httpStatusText(404), std::string_view("Not Found"));
    RUVIA_CHECK_EQ(httpStatusText(413), std::string_view("Payload Too Large"));
    RUVIA_CHECK_EQ(httpStatusText(500), std::string_view("Internal Server Error"));
    // Unknown codes fall back by class: >= 500 is a server error, else bad request.
    RUVIA_CHECK_EQ(httpStatusText(599), std::string_view("Internal Server Error"));
    RUVIA_CHECK_EQ(httpStatusText(499), std::string_view("Bad Request"));
}

RUVIA_TEST(http_status_text_covers_reset_content_and_gateway_statuses) {
    using ruvia::detail::httpCachedStatusLine;
    // 205 Reset Content is a real bodyless status the response-head policy already
    // recognizes, so it must carry its own reason phrase rather than the <500
    // "Bad Request" fallback (an internal inconsistency before this entry existed).
    // 502/504 are the common gateway statuses a proxy deployment emits.
    RUVIA_CHECK_EQ(httpStatusText(205), std::string_view("Reset Content"));
    RUVIA_CHECK_EQ(httpStatusText(502), std::string_view("Bad Gateway"));
    RUVIA_CHECK_EQ(httpStatusText(504), std::string_view("Gateway Timeout"));
    // Each also gets a pre-baked status line, used when the reason phrase matches.
    RUVIA_CHECK_EQ(httpCachedStatusLine(205, "Reset Content"),
                   std::string_view("HTTP/1.1 205 Reset Content\r\n"));
    RUVIA_CHECK_EQ(httpCachedStatusLine(502, "Bad Gateway"),
                   std::string_view("HTTP/1.1 502 Bad Gateway\r\n"));
    RUVIA_CHECK_EQ(httpCachedStatusLine(504, "Gateway Timeout"),
                   std::string_view("HTTP/1.1 504 Gateway Timeout\r\n"));
}

RUVIA_TEST(default_error_code_mapping) {
    RUVIA_CHECK_EQ(defaultErrorCode(400), std::string_view("bad_request"));
    RUVIA_CHECK_EQ(defaultErrorCode(404), std::string_view("not_found"));
    RUVIA_CHECK_EQ(defaultErrorCode(405), std::string_view("method_not_allowed"));
    RUVIA_CHECK_EQ(defaultErrorCode(413), std::string_view("payload_too_large"));
}

RUVIA_TEST(cached_status_line_match_and_mismatch) {
    using ruvia::detail::httpCachedStatusLine;
    // The pre-baked status line is used only when the reason phrase matches.
    RUVIA_CHECK_EQ(httpCachedStatusLine(404, "Not Found"),
                   std::string_view("HTTP/1.1 404 Not Found\r\n"));
    RUVIA_CHECK(httpCachedStatusLine(404, "Nope").empty());       // custom phrase
    RUVIA_CHECK(httpCachedStatusLine(499, "Bad Request").empty());  // unknown code
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
