#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/http_limits.h"

namespace ruvia {

// Bytes a field costs beyond its name and value: the ": " separator and the
// CRLF of its normalized HTTP/1 field line "name: value\r\n".
inline constexpr std::size_t http_field_line_overhead = 4;

// The single byte rule for an HTTP field section, for headers and trailers,
// requests and responses, received and generated: the section must fit
// max_http_header_bytes, each field costing name.size() + value.size() +
// http_field_line_overhead (independent of received optional whitespace).
// Fixed head bytes such as a start line or the terminating empty line are
// charged with add(); a trailer section starts from
// http_trailer_section_budget(). A failed charge leaves the budget unchanged.
class http_field_section_budget final {
public:
    [[nodiscard]] constexpr bool add(std::size_t bytes_value) noexcept {
        if (bytes_value > max_http_header_bytes - bytes_) {
            return false;
        }
        bytes_ += bytes_value;
        return true;
    }

    [[nodiscard]] constexpr bool add_field(std::size_t name_bytes, std::size_t value_bytes) noexcept {
        const auto remaining = max_http_header_bytes - bytes_;
        if (remaining < http_field_line_overhead || name_bytes > remaining - http_field_line_overhead ||
            value_bytes > remaining - http_field_line_overhead - name_bytes) {
            return false;
        }
        bytes_ += name_bytes + value_bytes + http_field_line_overhead;
        return true;
    }

    [[nodiscard]] constexpr bool add_field(std::string_view name, std::string_view value) noexcept {
        return add_field(name.size(), value.size());
    }

    [[nodiscard]] constexpr std::size_t bytes() const noexcept {
        return bytes_;
    }

private:
    std::size_t bytes_{0};
};

// The budget of a trailer section before its first field. Like a head, the
// section is charged its terminating empty line, so every section a writer
// generates also fits the decoders' limit on the complete encoded section.
[[nodiscard]] constexpr http_field_section_budget http_trailer_section_budget() noexcept {
    http_field_section_budget budget;
    (void)budget.add(std::string_view("\r\n").size());
    return budget;
}

}  // namespace ruvia
