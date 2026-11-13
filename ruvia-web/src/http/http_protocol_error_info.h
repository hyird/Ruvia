#pragma once

#include <string_view>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/web/error.h"

namespace ruvia::detail {

// http_error_info borrows its fields. The caller's http_protocol_error lvalue
// must outlive any suspension of the error handler that reads this view.
[[nodiscard]] inline http_error_info copy_http_protocol_error_info(
    const http_protocol_error& error RUVIA_LIFETIMEBOUND) noexcept {
    return http_error_info({.status_ = error.status(),
        .code_ = error.code(),
        .message_ = std::string_view(error.what())});
}
http_error_info copy_http_protocol_error_info(const http_protocol_error&&) = delete;

}  // namespace ruvia::detail
