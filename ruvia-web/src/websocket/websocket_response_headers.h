#pragma once

#include <memory_resource>
#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/web/context.h"

#include "context/context_access.h"

namespace ruvia::detail {

// The context retains header storage through handshake construction. The
// protocol plan copies the views before this temporary pointer table is freed.
[[nodiscard]] inline std::pmr::vector<http_header_view> websocket_response_headers(context& context_value) {
    const auto& headers = context_access::response_storage(context_value).headers();
    std::pmr::vector<http_header_view> result_value(context_value.pool());
    result_value.reserve(headers.size());
    for (const auto& header : headers) {
        result_value.emplace_back(header.name(), header.value());
    }
    return result_value;
}

}  // namespace ruvia::detail
