#pragma once

#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/web/Error.h"

namespace ruvia::detail {

[[nodiscard]] inline HttpErrorInfo normalizeHttpErrorInfo(HttpErrorInfo error) noexcept {
    auto status = error.status();
    if (status < 100 || status > 999) {
        status = 500;
    }
    auto statusText = error.statusText();
    if (statusText.empty() || !isValidHttpStatusText(statusText)) {
        statusText = httpStatusText(status);
    }
    auto code = error.code();
    if (code.empty()) {
        code = defaultErrorCode(status);
    }
    auto message = error.message();
    if (message.empty()) {
        message = statusText;
    }
    return HttpErrorInfo(status, code, message, statusText, error.detailsJson());
}

}  // namespace ruvia::detail
