#pragma once

#include <span>

#include "ruvia/http/HttpHeader.h"

namespace ruvia::detail {
class HttpResponseTrailerSectionResult;
[[nodiscard]] HttpResponseTrailerSectionResult httpResponseTrailerSection(
    std::span<const HttpHeaderView>) noexcept;
}  // namespace ruvia::detail

namespace ruvia {

// Borrowed proof that the complete terminal section passed the shared response-
// trailer rules. The source span must outlive its synchronous consumption.
class HttpResponseTrailerSection final {
public:
    [[nodiscard]] std::span<const HttpHeaderView> fields() const noexcept {
        return fields_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return fields_.empty();
    }

private:
    friend class detail::HttpResponseTrailerSectionResult;
    friend detail::HttpResponseTrailerSectionResult detail::httpResponseTrailerSection(
        std::span<const HttpHeaderView>) noexcept;

    explicit HttpResponseTrailerSection(std::span<const HttpHeaderView> fields) noexcept
        : fields_(fields) {}

    std::span<const HttpHeaderView> fields_;
};

}  // namespace ruvia
