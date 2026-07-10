#pragma once

#include "ruvia/http/Error.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia::detail {

[[nodiscard]] inline HttpErrorInfo normalizeHttpErrorInfo(HttpErrorInfo error) noexcept {
    // makeErrorResponse must never throw: the transport layer calls it outside its
    // try-guard and only closes the socket if an exception escapes (dropping the
    // connection with no response). Coerce invalid status metadata to safe values.
    auto status = error.status();
    if (status < 100 || status > 999) {
        status = 500;
    }
    auto statusText = error.statusText();
    if (statusText.empty() || !isValidHttpStatusText(statusText)) {
        statusText = defaultStatusText(status);
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
