#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/Error.h"
#include "ruvia/http/HttpResponse.h"
#include "http/HttpResponseBodyAccess.h"

namespace {

using ruvia::HttpErrorInfo;
using ruvia::makeErrorResponse;

}  // namespace

RUVIA_TEST(make_error_response_escapes_message_in_json_body) {
    auto* resource = std::pmr::new_delete_resource();
    // A custom message carrying quotes must be JSON-escaped in the error body so
    // it cannot break out of the string and corrupt the response.
    HttpErrorInfo error(400, "bad_request", "invalid \"input\"");
    const auto response = makeErrorResponse(resource, error);

    RUVIA_CHECK_EQ(response.status(), std::uint16_t{400});
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));

    const auto body = ruvia::detail::responseBodyBytes(response);
    RUVIA_CHECK(body.starts_with("{") && body.ends_with("}"));
    RUVIA_CHECK(body.find(R"("code":"bad_request")") != std::string_view::npos);
    RUVIA_CHECK(body.find(R"("message":"invalid \"input\"")") != std::string_view::npos);
}

RUVIA_TEST(make_error_response_embeds_details_json) {
    auto* resource = std::pmr::new_delete_resource();
    HttpErrorInfo error(422, "validation_failed", "failed", "Unprocessable Entity",
                        R"([{"field":"x","code":"required","message":"m"}])");
    const auto response = makeErrorResponse(resource, error);

    const auto body = ruvia::detail::responseBodyBytes(response);
    // The already-valid details JSON is embedded verbatim under "details".
    RUVIA_CHECK(body.find(R"("details":[{"field":"x","code":"required","message":"m"}])") !=
                std::string_view::npos);
}

RUVIA_TEST(make_error_response_close_connection_sets_header) {
    auto* resource = std::pmr::new_delete_resource();
    HttpErrorInfo error(500, "internal", "boom");
    const auto response = makeErrorResponse(resource, error, /*closeConnection=*/true);
    RUVIA_CHECK_EQ(response.header("Connection"), std::string_view("close"));
}

RUVIA_TEST(make_error_response_coerces_invalid_status_and_status_text) {
    auto* resource = std::pmr::new_delete_resource();

    // An out-of-range status (a typo like 4004, or a code copied from an upstream
    // response) must NOT throw on the never-throws error path: HttpResponse::status
    // rejects it, and makeErrorResponse is called outside the transport try-guard,
    // so the throw would drop the connection with no body. It is coerced to 500.
    {
        const auto response = makeErrorResponse(resource, HttpErrorInfo(4004, "x", "y"));
        RUVIA_CHECK_EQ(response.status(), std::uint16_t{500});
    }
    // A status text carrying CR/LF (which HttpResponse::status also rejects) is
    // replaced with the default reason phrase rather than throwing.
    {
        HttpErrorInfo error(400, "bad", "msg", std::string_view("Bad\r\nRequest", 12));
        const auto response = makeErrorResponse(resource, error);
        RUVIA_CHECK_EQ(response.status(), std::uint16_t{400});
        RUVIA_CHECK(response.statusText().find('\r') == std::string_view::npos);
        RUVIA_CHECK(response.statusText().find('\n') == std::string_view::npos);
    }
    // A valid in-range status is preserved unchanged.
    {
        const auto response = makeErrorResponse(resource, HttpErrorInfo(404, "not_found", "nope"));
        RUVIA_CHECK_EQ(response.status(), std::uint16_t{404});
    }
}
