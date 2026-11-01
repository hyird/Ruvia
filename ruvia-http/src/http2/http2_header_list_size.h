#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/http_limits.h"

namespace ruvia::detail {

// HTTP/2 header-list size (RFC 9113 section 6.5.2): each field, pseudo-fields
// included, costs its name and value plus 32 bytes, charged against
// max_http_header_bytes. This deliberately differs from the HTTP/1 field-line
// accounting of http_field_section_budget. A failed add leaves the size unchanged.
class http2_header_list_size final {
public:
    [[nodiscard]] bool add(std::string_view name, std::string_view value) noexcept {
        constexpr std::size_t field_metadata_bytes = 32;
        const auto remaining = max_http_header_bytes - bytes_;
        if (remaining < field_metadata_bytes || name.size() > remaining - field_metadata_bytes ||
            value.size() > remaining - field_metadata_bytes - name.size()) {
            return false;
        }
        bytes_ += name.size() + value.size() + field_metadata_bytes;
        return true;
    }

private:
    std::size_t bytes_{0};
};

}  // namespace ruvia::detail
