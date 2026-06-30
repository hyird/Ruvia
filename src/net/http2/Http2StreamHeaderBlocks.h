#pragma once

#include <memory_resource>
#include <string>

#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

class Http2StreamHeaderBlocks final {
    struct ResolvedResourceTag final {};

public:
    explicit Http2StreamHeaderBlocks(std::pmr::memory_resource* resource = nullptr)
        : Http2StreamHeaderBlocks(ResolvedResourceTag{}, pmrResourceOrDefault(resource)) {}

    [[nodiscard]] std::pmr::string& request() noexcept {
        return request_;
    }

    [[nodiscard]] const std::pmr::string& request() const noexcept {
        return request_;
    }

    [[nodiscard]] std::pmr::string& response() noexcept {
        return response_;
    }

    [[nodiscard]] const std::pmr::string& response() const noexcept {
        return response_;
    }

private:
    Http2StreamHeaderBlocks(ResolvedResourceTag, std::pmr::memory_resource* resource)
        : request_(resource),
          response_(resource) {}

    std::pmr::string request_;
    std::pmr::string response_;
};

}  // namespace ruvia::detail
