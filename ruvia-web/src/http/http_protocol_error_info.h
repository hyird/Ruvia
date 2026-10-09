#pragma once

#include <cstring>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/http_protocol_error.h"
#include "ruvia/web/error.h"

namespace ruvia::detail {

// http_error_info borrows its fields. Copy a protocol diagnostic into the
// request lifetime domain before an application error handler may suspend.
[[nodiscard]] inline http_error_info copy_http_protocol_error_info(
    std::pmr::memory_resource* resource, const http_protocol_error& error) {
    const std::string_view message(error.what());
    auto* storage = static_cast<char*>(resource->allocate(message.size(), alignof(char)));
    std::memcpy(storage, message.data(), message.size());
    return http_error_info(
        {.status_ = error.status(), .message_ = std::string_view(storage, message.size())});
}

}  // namespace ruvia::detail
