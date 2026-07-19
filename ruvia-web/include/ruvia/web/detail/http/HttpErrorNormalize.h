#pragma once

#include "ruvia/http/HttpHeader.h"

#include "ruvia/http/HttpStatus.h"
#include "ruvia/web/Error.h"

namespace ruvia::detail {

[[nodiscard]] inline HttpErrorInfo normalizeHttpErrorInfo(HttpErrorInfo error) noexcept {
    auto status = error.status();
    if (!status.isError()) {
        status = http_status::kInternalServerError;
    }
    auto statusText = error.statusText();
    if (statusText.empty() || !isValidHttpStatusText(statusText)) {
        statusText = httpReasonPhrase(status);
        if (statusText.empty()) {
            // Application error presentation remains a Web concern. Do not
            // invent an HTTP/1 reason phrase for an extension status code.
            statusText = "HTTP Error";
        }
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
