#pragma once

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/error.h"

namespace ruvia::detail {

[[nodiscard]] inline http_error_info normalize_http_error_info(http_error_info error) noexcept {
    auto status = error.status();
    if (!status.is_error()) {
        status = http_status::internal_server_error;
    }
    auto status_text = error.status_text();
    if (status_text.empty() || !is_valid_http_status_text(status_text)) {
        status_text = http_reason_phrase(status);
        if (status_text.empty()) {
            // Application error presentation remains a Web concern. Do not
            // invent an HTTP/1 reason phrase for an extension status code.
            status_text = "HTTP Error";
        }
    }
    auto code = error.code();
    if (code.empty()) {
        code = default_error_code(status);
    }
    auto message = error.message();
    if (message.empty()) {
        message = status_text;
    }
    return http_error_info({.status_ = status,
        .code_ = code,
        .message_ = message,
        .status_text_ = status_text,
        .validation_issues_ = error.validation_issues()});
}

}  // namespace ruvia::detail
