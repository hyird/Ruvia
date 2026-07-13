#pragma once

#include <string_view>

#include "ruvia/http/detail/HttpContentCoding.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

[[nodiscard]] inline HttpContentCodingFieldResult requestContentCoding(
    const HttpRequest& request) noexcept {
    return httpContentCodingFromHeaders(request.headers());
}

}  // namespace ruvia::detail
