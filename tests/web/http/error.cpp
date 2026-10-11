#include "ruvia/web/error.h"

#include <memory_resource>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_status.h"
#include "ruvia/web/validation.h"

#include "test_harness.h"

namespace {

using ruvia::default_error_code;
using ruvia::http_error;

}  // namespace

using validation_issues_owning_view_type = std::ranges::owning_view<ruvia::validation_error::issue_list_type>;

RUVIA_TEST(default_error_code_mapping) {
    RUVIA_CHECK_EQ(
        default_error_code(ruvia::http_status::bad_request), std::string_view("bad_request"));
    RUVIA_CHECK_EQ(default_error_code(ruvia::http_status::not_found), std::string_view("not_found"));
    RUVIA_CHECK_EQ(default_error_code(ruvia::http_status::method_not_allowed),
        std::string_view("method_not_allowed"));
    RUVIA_CHECK_EQ(default_error_code(ruvia::http_status::content_too_large),
        std::string_view("content_too_large"));
}

RUVIA_TEST(http_error_info_round_trips) {
    const http_error error({.status_ = ruvia::http_status::unprocessable_content,
        .code_ = "unprocessable",
        .message_ = "bad fields"});
    const auto info = error.info();
    RUVIA_CHECK_EQ(info.status(), ruvia::http_status::unprocessable_content);
    RUVIA_CHECK_EQ(info.code(), std::string_view("unprocessable"));
    RUVIA_CHECK_EQ(info.message(), std::string_view("bad fields"));
}
