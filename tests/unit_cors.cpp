#include "test_harness.h"

#include <chrono>
#include <stdexcept>
#include <string_view>

#include "http/HttpCorsConfigValidation.h"

namespace {

bool corsThrows(std::string_view allowOrigin, bool allowCredentials) {
    try {
        ruvia::detail::validateCorsFields(
            /*enabled=*/true,
            allowOrigin,
            /*allowHeaders=*/"",
            /*exposeHeaders=*/"",
            std::chrono::seconds(0),
            allowCredentials);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(cors_wildcard_with_credentials_rejected) {
    // "*" + credentials would force reflecting arbitrary origins with credentials.
    RUVIA_CHECK(corsThrows("*", /*allowCredentials=*/true));
}

RUVIA_TEST(cors_wildcard_without_credentials_allowed) {
    RUVIA_CHECK(!corsThrows("*", /*allowCredentials=*/false));
}

RUVIA_TEST(cors_explicit_origin_with_credentials_allowed) {
    RUVIA_CHECK(!corsThrows("https://app.example.com", /*allowCredentials=*/true));
}

namespace {

bool corsFieldsThrow(
    bool enabled,
    std::string_view allowOrigin,
    std::string_view allowHeaders,
    std::string_view exposeHeaders,
    std::chrono::seconds maxAge,
    bool allowCredentials) {
    try {
        ruvia::detail::validateCorsFields(
            enabled, allowOrigin, allowHeaders, exposeHeaders, maxAge, allowCredentials);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(cors_empty_origin_rejected_only_when_enabled) {
    RUVIA_CHECK(corsFieldsThrow(true, "", "", "", std::chrono::seconds(0), false));    // enabled + empty
    RUVIA_CHECK(!corsFieldsThrow(false, "", "", "", std::chrono::seconds(0), false));  // disabled: empty ok
}

RUVIA_TEST(cors_header_values_reject_crlf_injection) {
    const std::string_view origin = "https://app.example.com";
    // A CRLF in any emitted CORS header value would enable response header injection.
    RUVIA_CHECK(corsFieldsThrow(true, "https://a\r\nX: y", "", "", std::chrono::seconds(0), false));
    RUVIA_CHECK(corsFieldsThrow(true, origin, "X-Foo\r\nX: y", "", std::chrono::seconds(0), false));
    RUVIA_CHECK(corsFieldsThrow(true, origin, "", "X-Bar\r\nX: y", std::chrono::seconds(0), false));
    // Clean header values are accepted.
    RUVIA_CHECK(!corsFieldsThrow(true, origin, "X-Foo, X-Bar", "X-Exposed", std::chrono::seconds(0), false));
}

RUVIA_TEST(cors_negative_max_age_rejected) {
    const std::string_view origin = "https://app.example.com";
    RUVIA_CHECK(corsFieldsThrow(true, origin, "", "", std::chrono::seconds(-1), false));
    RUVIA_CHECK(!corsFieldsThrow(true, origin, "", "", std::chrono::seconds(0), false));
    RUVIA_CHECK(!corsFieldsThrow(true, origin, "", "", std::chrono::seconds(3600), false));
}
