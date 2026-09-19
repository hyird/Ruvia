#pragma once

#include <memory_resource>
#include <vector>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"

namespace ruvia::detail {

// The Context retains header storage through handshake construction. The
// protocol plan copies the views before this temporary pointer table is freed.
[[nodiscard]] inline std::pmr::vector<HttpHeaderView> webSocketResponseHeaders(Context& context) {
    const auto& headers = ContextAccess::responseStorage(context).headers();
    std::pmr::vector<HttpHeaderView> result(context.pool());
    result.reserve(headers.size());
    for (const auto& header : headers) {
        result.emplace_back(header.name(), header.value());
    }
    return result;
}

}  // namespace ruvia::detail
