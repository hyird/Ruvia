#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/util/borrowed_view.h"

namespace ruvia::detail {

struct http2_payload_slice final {
    std::string_view first_;
    std::string_view second_;
};

[[nodiscard]] inline http2_payload_slice http2_slice_two_part_payload(std::string_view first,
    std::string_view second, std::size_t offset, std::size_t size) noexcept {
    if (offset < first.size()) {
        const auto first_size = std::min(size, first.size() - offset);
        const auto first_slice = first.substr(offset, first_size);
        size -= first_size;
        return http2_payload_slice{
            .first_ = first_slice, .second_ = size == 0 ? std::string_view{} : second.substr(0, size)};
    }
    return http2_payload_slice{.first_ = second.substr(offset - first.size(), size), .second_ = {}};
}

template <http_temporary_owning_char_string first_type>
http2_payload_slice http2_slice_two_part_payload(
    first_type&&, std::string_view, std::size_t, std::size_t) = delete;

template <http_temporary_owning_char_string second_type>
http2_payload_slice http2_slice_two_part_payload(
    std::string_view, second_type&&, std::size_t, std::size_t) = delete;

}  // namespace ruvia::detail
