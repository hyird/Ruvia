#pragma once

#include <span>

#include "ruvia/http/http_header.h"

namespace ruvia {

class http_response_trailer_section;
[[nodiscard]] http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers);

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
    friend http_response_trailer_section validate_http_response_trailers(
        std::span<const http_header_view>);
    explicit http_response_trailer_section(std::span<const http_header_view> fields_value) noexcept
        : fields_(fields_value) {}

    std::span<const http_header_view> fields_;
};

}  // namespace ruvia
