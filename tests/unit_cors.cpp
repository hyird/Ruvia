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
