#include <string_view>

#include "ruvia/http/detail/field/HttpMediaType.h"

#include "test_harness.h"

namespace {

using ruvia::detail::httpMediaTypeOnly;

}  // namespace

RUVIA_TEST(http_media_type_only_strips_parameters_and_ows) {
    RUVIA_CHECK_EQ(httpMediaTypeOnly("application/json"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(
        httpMediaTypeOnly("application/json; charset=utf-8"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(httpMediaTypeOnly("application/json ; charset=utf-8"),
        std::string_view("application/json"));
    RUVIA_CHECK_EQ(httpMediaTypeOnly(""), std::string_view(""));
}
