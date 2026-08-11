#pragma once

#include <memory_resource>
#include <string>

#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia::detail {

class Http2StreamHeaderBlocks final {
public:
    explicit Http2StreamHeaderBlocks(std::pmr::memory_resource* resource = nullptr)
        : Http2StreamHeaderBlocks(HttpResolvedPmrResourceTag{}, httpPmrResourceOrDefault(resource)) {}

    [[nodiscard]] std::pmr::string& remote() & noexcept {
        return remote_;
    }
    [[nodiscard]] std::pmr::string& remote() && = delete;

    [[nodiscard]] const std::pmr::string& remote() const& noexcept {
        return remote_;
    }
    [[nodiscard]] const std::pmr::string& remote() const&& = delete;

    [[nodiscard]] std::pmr::string& local() & noexcept {
        return local_;
    }
    [[nodiscard]] std::pmr::string& local() && = delete;

    [[nodiscard]] const std::pmr::string& local() const& noexcept {
        return local_;
    }
    [[nodiscard]] const std::pmr::string& local() const&& = delete;

private:
    Http2StreamHeaderBlocks(HttpResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : remote_(resource),
          local_(resource) {}

    std::pmr::string remote_;
    std::pmr::string local_;
};

}  // namespace ruvia::detail
