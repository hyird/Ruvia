#include "test_harness.h"

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpStatus.h"
#include "ruvia/web/Error.h"

namespace {

using ruvia::defaultErrorCode;
using ruvia::HttpError;

}  // namespace

template <typename T>
concept ExposesRvalueHttpErrorInfo = requires {
    std::declval<const T&&>().info();
};

template <typename String>
concept AcceptsAnyRvalueHttpErrorInfoText =
    requires(String&& value) {
        ruvia::HttpErrorInfo(ruvia::http_status::kBadRequest, std::forward<String>(value));
    } ||
    requires(String&& value) {
        ruvia::HttpErrorInfo(ruvia::http_status::kBadRequest, {}, std::forward<String>(value));
    } ||
    requires(String&& value) {
        ruvia::HttpErrorInfo(ruvia::http_status::kBadRequest, {}, {}, std::forward<String>(value));
    } ||
    requires(String&& value) {
        ruvia::HttpErrorInfo(ruvia::http_status::kBadRequest, {}, {}, {}, std::forward<String>(value));
    };

template <typename String>
concept AcceptsLvalueHttpErrorInfoText = requires(String& value) {
    ruvia::HttpErrorInfo(ruvia::http_status::kBadRequest, value, value, value, value);
};

static_assert(!ExposesRvalueHttpErrorInfo<ruvia::HttpError>);
static_assert(!AcceptsAnyRvalueHttpErrorInfoText<std::string>);
static_assert(!AcceptsAnyRvalueHttpErrorInfoText<const std::string>);
static_assert(!AcceptsAnyRvalueHttpErrorInfoText<std::pmr::string>);
static_assert(AcceptsLvalueHttpErrorInfoText<std::string>);

RUVIA_TEST(default_error_code_mapping) {
    RUVIA_CHECK_EQ(
        defaultErrorCode(ruvia::http_status::kBadRequest),
        std::string_view("bad_request"));
    RUVIA_CHECK_EQ(
        defaultErrorCode(ruvia::http_status::kNotFound),
        std::string_view("not_found"));
    RUVIA_CHECK_EQ(
        defaultErrorCode(ruvia::http_status::kMethodNotAllowed),
        std::string_view("method_not_allowed"));
    RUVIA_CHECK_EQ(
        defaultErrorCode(ruvia::http_status::kContentTooLarge),
        std::string_view("content_too_large"));
}

RUVIA_TEST(http_error_info_round_trips) {
    const HttpError error(
        ruvia::http_status::kUnprocessableContent,
        "unprocessable",
        "bad fields");
    const auto info = error.info();
    RUVIA_CHECK_EQ(info.status(), ruvia::http_status::kUnprocessableContent);
    RUVIA_CHECK_EQ(info.code(), std::string_view("unprocessable"));
    RUVIA_CHECK_EQ(info.message(), std::string_view("bad fields"));
}
