#pragma once

#include "ruvia/http/HttpClientRuntime.h"

#include <cstdint>
#include <memory_resource>
#include <utility>
#include <vector>

namespace ruvia::detail {

struct FetchResponseStreamAccess final {
    [[nodiscard]] static FetchResponseStream make(
        std::uint16_t status,
        std::pmr::vector<FetchResponseHeader> headers,
        HttpBodyStream body) noexcept {
        return FetchResponseStream(status, std::move(headers), std::move(body));
    }
};

}  // namespace ruvia::detail
