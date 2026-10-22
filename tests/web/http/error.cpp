#include "ruvia/web/error.h"

#include <exception>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/http/http_status.h"
#include "ruvia/web/validation.h"

#include "test_harness.h"

namespace {

using ruvia::default_error_code;
using ruvia::http_error;

}  // namespace

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

RUVIA_TEST(exception_text_describes_any_exception) {
    RUVIA_CHECK(ruvia::exception_text(nullptr).empty());
    RUVIA_CHECK_EQ(ruvia::exception_text(std::make_exception_ptr(std::runtime_error("plain failure"))),
        std::string("plain failure"));
    RUVIA_CHECK_EQ(ruvia::exception_text(std::make_exception_ptr(42)), std::string("non-standard exception"));

    const std::system_error generic(std::make_error_code(std::errc::timed_out), "upstream");
    RUVIA_CHECK_EQ(ruvia::exception_text(std::make_exception_ptr(generic)), std::string(generic.what()));

    // A system-category message is the OS text (UTF-8 on Windows, where what()
    // uses the ANSI code page); the context before it is kept.
    const std::system_error system(std::error_code(1, std::system_category()), "bind");
    const auto text = ruvia::exception_text(std::make_exception_ptr(system));
    RUVIA_CHECK(text.starts_with("bind: "));
    RUVIA_CHECK(text.size() > std::string_view("bind: ").size());
}
