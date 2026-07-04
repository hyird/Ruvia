#include "test_harness.h"

#include <cstdint>
#include <string_view>

#include "ruvia/http/Error.h"
#include "ruvia/http/HttpStatus.h"
#include "net/body/HttpTransferCodingDecoder.h"

namespace {

using ruvia::defaultErrorCode;
using ruvia::defaultStatusText;
using ruvia::HttpError;

}  // namespace

RUVIA_TEST(default_status_text_known_and_fallback) {
    RUVIA_CHECK_EQ(defaultStatusText(200), std::string_view("OK"));
    RUVIA_CHECK_EQ(defaultStatusText(404), std::string_view("Not Found"));
    RUVIA_CHECK_EQ(defaultStatusText(413), std::string_view("Payload Too Large"));
    RUVIA_CHECK_EQ(defaultStatusText(500), std::string_view("Internal Server Error"));
    // Unknown codes fall back by class: >= 500 is a server error, else bad request.
    RUVIA_CHECK_EQ(defaultStatusText(599), std::string_view("Internal Server Error"));
    RUVIA_CHECK_EQ(defaultStatusText(499), std::string_view("Bad Request"));
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
    } catch (const HttpError& error) {
        caught = true;
        const auto info = error.info();
        RUVIA_CHECK_EQ(info.status(), std::uint16_t{413});
        RUVIA_CHECK_EQ(info.code(), std::string_view("payload_too_large"));
    }
    RUVIA_CHECK(caught);
}

RUVIA_TEST(http_error_info_round_trips) {
    const HttpError error(422, "unprocessable", "bad fields");
    const auto info = error.info();
    RUVIA_CHECK_EQ(info.status(), std::uint16_t{422});
    RUVIA_CHECK_EQ(info.code(), std::string_view("unprocessable"));
    RUVIA_CHECK_EQ(info.message(), std::string_view("bad fields"));
}
