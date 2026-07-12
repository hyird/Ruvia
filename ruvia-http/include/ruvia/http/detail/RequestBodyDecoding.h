#pragma once

#include <string_view>

#include "ruvia/http/detail/HttpContentCoding.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

[[nodiscard]] inline HttpContentCoding requestContentCoding(const HttpRequest& request) noexcept {
    bool seen = false;
    std::string_view value;
    for (const auto& header : request.headers()) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "Content-Encoding")) {
            continue;
        }
        if (seen) {
            return HttpContentCoding::kNone;
        }
        seen = true;
        value = header.value();
    }
    return seen
        ? httpContentCodingFromFieldValue(value)
        : HttpContentCoding::kNone;
}

}  // namespace ruvia::detail
