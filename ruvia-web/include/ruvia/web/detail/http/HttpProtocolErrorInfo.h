#pragma once

#include <cstring>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/web/Error.h"

namespace ruvia::detail {

// HttpErrorInfo borrows its fields. Copy a protocol diagnostic into the
// request lifetime domain before an application error handler may suspend.
[[nodiscard]] inline HttpErrorInfo copyHttpProtocolErrorInfo(
    std::pmr::memory_resource* resource,
    const HttpProtocolError& error) {
    const std::string_view message(error.what());
    auto* storage = static_cast<char*>(resource->allocate(
        message.size(),
        alignof(char)));
    std::memcpy(storage, message.data(), message.size());
    return HttpErrorInfo(
        error.status(),
        {},
        std::string_view(storage, message.size()));
}

}  // namespace ruvia::detail
