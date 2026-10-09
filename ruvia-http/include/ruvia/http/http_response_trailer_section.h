#pragma once

#include <span>

#include "ruvia/http/http_header.h"

namespace ruvia::detail {
class http_response_trailer_section_result;
[[nodiscard]] http_response_trailer_section_result check_http_response_trailer_section(
    std::span<const http_header_view>) noexcept;
}  // namespace ruvia::detail

namespace ruvia {

// Borrowed proof that the complete terminal section passed the shared response-
// trailer rules. The source span must outlive its synchronous consumption.
class http_response_trailer_section final {
public:
    [[nodiscard]] std::span<const http_header_view> fields() const noexcept {
        return fields_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return fields_.empty();
    }

private:
    friend class detail::http_response_trailer_section_result;
    friend detail::http_response_trailer_section_result detail::check_http_response_trailer_section(
        std::span<const http_header_view>) noexcept;

    explicit http_response_trailer_section(std::span<const http_header_view> fields_value) noexcept
        : fields_(fields_value) {}

    std::span<const http_header_view> fields_;
};

}  // namespace ruvia
