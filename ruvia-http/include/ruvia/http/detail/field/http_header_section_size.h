#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/http_limits.h"

namespace ruvia::detail {

// Representation-independent field-section accounting. The fixed 32-byte
// charge matches HTTP/2's decoded header-list definition and also reserves a
// conservative metadata budget for protocols whose wire syntax is smaller.
// Pseudo-fields participate in the byte budget but are counted separately from
// max_http_header_fields by the protocol state that owns them.
class http_header_section_size final {
public:
    explicit http_header_section_size(std::size_t limit = max_http_header_bytes) noexcept
        : limit_(limit) {}

    [[nodiscard]] bool add(std::string_view name, std::string_view value) noexcept {
        constexpr std::size_t field_metadata_bytes = 32;
        const auto remaining = limit_ - bytes_;
        if (remaining < field_metadata_bytes || name.size() > remaining - field_metadata_bytes ||
            value.size() > remaining - field_metadata_bytes - name.size()) {
            return false;
        }
        const auto field_bytes = name.size() + value.size() + field_metadata_bytes;
        bytes_ += field_bytes;
        return true;
    }

    [[nodiscard]] std::size_t bytes() const noexcept {
        return bytes_;
    }

private:
    std::size_t bytes_{0};
    std::size_t limit_;
};

}  // namespace ruvia::detail
