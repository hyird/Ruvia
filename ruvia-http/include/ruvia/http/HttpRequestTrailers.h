#pragma once

#include <cstddef>
#include <expected>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/HttpHeader.h"

namespace ruvia {
enum class HttpRequestTrailerError : unsigned char { kInvalidField,
    kForbiddenField,
    kSectionTooLarge };

// Owns decoded terminal request fields, separately from the initial head.
// Drivers populate this object before reporting successful body completion.
// resource must outlive this object and every borrowed field view.
class HttpRequestTrailers final {
public:
    explicit HttpRequestTrailers(std::pmr::memory_resource* resource);
    HttpRequestTrailers(const HttpRequestTrailers&) = delete;
    HttpRequestTrailers& operator=(const HttpRequestTrailers&) = delete;
    [[nodiscard]] std::expected<void, HttpRequestTrailerError> append(std::string_view name, std::string_view value);
    [[nodiscard]] std::expected<void, HttpRequestTrailerError> appendHttp1(std::string_view block);
    [[nodiscard]] std::pmr::vector<HttpHeader> takeFields() && noexcept {
        bytes_ = 0;
        return std::move(fields_);
    }
    [[nodiscard]] std::span<const HttpHeader> fields() const& noexcept {
        return fields_;
    }
    std::span<const HttpHeader> fields() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const& noexcept;
    std::optional<std::string_view> field(std::string_view) const&& = delete;

private:
    std::pmr::vector<HttpHeader> fields_;
    std::size_t bytes_{};
};
}  // namespace ruvia
