#pragma once

#include <memory_resource>
#include <string>

#include "ruvia/http/detail/util/pmr_resource.h"

namespace ruvia::detail {

class http2_stream_header_blocks final {
public:
    explicit http2_stream_header_blocks(std::pmr::memory_resource* resource = nullptr)
        : http2_stream_header_blocks(
              http_resolved_pmr_resource_tag{}, http_pmr_resource_or_default(resource)) {}

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
    http2_stream_header_blocks(http_resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
        : remote_(resource),
          local_(resource) {}

    std::pmr::string remote_;
    std::pmr::string local_;
};

}  // namespace ruvia::detail
