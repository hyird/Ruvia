#pragma once

#include <memory_resource>
#include <string>

#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia::detail {

class Http2StreamHeaderBlocks final {
public:
    explicit Http2StreamHeaderBlocks(std::pmr::memory_resource* resource = nullptr)
        : Http2StreamHeaderBlocks(HttpResolvedPmrResourceTag{}, httpPmrResourceOrDefault(resource)) {}

    [[nodiscard]] std::pmr::string& request() & noexcept {
        return request_;
    }
    [[nodiscard]] std::pmr::string& request() && = delete;

    [[nodiscard]] const std::pmr::string& request() const & noexcept {
        return request_;
    }
    [[nodiscard]] const std::pmr::string& request() const && = delete;

    [[nodiscard]] std::pmr::string& response() & noexcept {
        return response_;
    }
    [[nodiscard]] std::pmr::string& response() && = delete;

    [[nodiscard]] const std::pmr::string& response() const & noexcept {
        return response_;
    }
    [[nodiscard]] const std::pmr::string& response() const && = delete;

private:
    Http2StreamHeaderBlocks(HttpResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : request_(resource),
          response_(resource) {}

    std::pmr::string request_;
    std::pmr::string response_;
};

}  // namespace ruvia::detail
