#include "ruvia/web/error.h"

#include <gtest/gtest.h>

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

#include "memory_resource_fixture.h"

namespace {

using ruvia::default_error_code;
using ruvia::http_error;

}  // namespace

TEST(Ruvia, default_error_code_mapping) {
    EXPECT_EQ(
        default_error_code(ruvia::http_status::bad_request), std::string_view("bad_request"));
    EXPECT_EQ(default_error_code(ruvia::http_status::not_found), std::string_view("not_found"));
    EXPECT_EQ(default_error_code(ruvia::http_status::method_not_allowed),
        std::string_view("method_not_allowed"));
    EXPECT_EQ(default_error_code(ruvia::http_status::content_too_large),
        std::string_view("content_too_large"));
}

TEST(Ruvia, http_error_info_round_trips) {
    const http_error error({.status_ = ruvia::http_status::unprocessable_content,
        .code_ = "unprocessable",
        .message_ = "bad fields"});
    const auto info = error.info();
    EXPECT_EQ(info.status(), ruvia::http_status::unprocessable_content);
    EXPECT_EQ(info.code(), std::string_view("unprocessable"));
    EXPECT_EQ(info.message(), std::string_view("bad fields"));
}

TEST(Ruvia, exception_text_describes_any_exception) {
    EXPECT_TRUE(ruvia::exception_text(nullptr).empty());
    EXPECT_EQ(ruvia::exception_text(std::make_exception_ptr(std::runtime_error("plain failure"))),
        std::string("plain failure"));
    EXPECT_EQ(ruvia::exception_text(std::make_exception_ptr(42)), std::string("non-standard exception"));

    const std::system_error generic(std::make_error_code(std::errc::timed_out), "upstream");
    EXPECT_EQ(ruvia::exception_text(std::make_exception_ptr(generic)), std::string(generic.what()));

    // A system-category message is the OS text (UTF-8 on Windows, where what()
    // uses the ANSI code page); the context before it is kept.
    const std::system_error system(std::error_code(1, std::system_category()), "bind");
    const auto text = ruvia::exception_text(std::make_exception_ptr(system));
    EXPECT_TRUE(text.starts_with("bind: "));
    EXPECT_TRUE(text.size() > std::string_view("bind: ").size());
}

TEST(Ruvia, http_error_cross_resource_move_assignment_propagates_bad_alloc) {
    ruvia::test::rejecting_memory_resource resource;
    http_error target({}, &resource);
    const std::string long_message(1024, 'x');
    http_error source({.status_ = ruvia::http_status::bad_request, .message_ = long_message});
    resource.reject_allocations();
    bool caught = false;
    try {
        target = std::move(source);
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    EXPECT_TRUE(caught);
    EXPECT_EQ(source.info().message(), std::string_view(long_message));
}

TEST(Ruvia, http_error_preserves_all_validation_issues_beyond_64) {
    ruvia::validator validator_value;
    constexpr std::size_t issue_count = 80;
    for (std::size_t index = 0; index < issue_count; ++index) {
        validator_value.add("field_" + std::to_string(index), "invalid", "msg_" + std::to_string(index));
    }
    EXPECT_EQ(validator_value.issues().size(), issue_count);

    const http_error error({.status_ = ruvia::http_status::bad_request,
        .code_ = "validation_failed",
        .message_ = "request validation failed",
        .validation_issues_ = validator_value.issues()});
    EXPECT_EQ(error.validation_issues().size(), issue_count);
    EXPECT_EQ(error.info().validation_issues().size(), issue_count);
    if (error.validation_issues().size() == issue_count) {
        EXPECT_EQ(error.validation_issues().front().field(), std::string_view("field_0"));
        EXPECT_EQ(error.validation_issues().back().field(), std::string_view("field_79"));
    }
}
