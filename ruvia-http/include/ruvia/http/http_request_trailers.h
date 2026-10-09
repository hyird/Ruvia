#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http_header.h"

namespace ruvia {
enum class http_request_trailer_error : unsigned char { invalid_field,
    forbidden_field,
    section_too_large };

// Owns decoded terminal request fields, separately from the initial head.
// Drivers populate this object before reporting successful body completion.
// resource must outlive this object and every borrowed field view.
class http_request_trailers final {
public:
    explicit http_request_trailers(std::pmr::memory_resource* resource);
    http_request_trailers(const http_request_trailers&) = delete;
    http_request_trailers& operator=(const http_request_trailers&) = delete;
    [[nodiscard]] std::variant<std::monostate, http_request_trailer_error> append(std::string_view name, std::string_view value);
    [[nodiscard]] std::variant<std::monostate, http_request_trailer_error> append_http1(std::string_view block);
    [[nodiscard]] std::pmr::vector<http_header> take_fields() && noexcept {
        bytes_ = 0;
        return std::move(fields_);
    }
    [[nodiscard]] std::span<const http_header> fields() const& noexcept {
        return fields_;
    }
    std::span<const http_header> fields() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const& noexcept;
    std::optional<std::string_view> field(std::string_view) const&& = delete;

private:
    std::pmr::vector<http_header> fields_;
    std::size_t bytes_{};
};
}  // namespace ruvia
