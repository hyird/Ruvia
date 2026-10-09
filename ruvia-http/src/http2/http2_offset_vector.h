#pragma once

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

namespace ruvia::detail {

template <typename vector_type>
[[nodiscard]] inline bool http2_should_compact_offset_vector(
    vector_type& values, std::size_t& offset, std::size_t threshold) noexcept {
    if (offset == 0) {
        return false;
    }
    if (offset == values.size()) {
        values.clear();
        offset = 0;
        return false;
    }
    return offset >= threshold || offset >= values.size() - offset;
}

template <typename vector_type>
inline void http2_compact_offset_vector(
    vector_type& values, std::size_t& offset, std::size_t threshold) noexcept {
    using value_type = typename vector_type::value_type;
    static_assert(std::is_trivially_copyable_v<value_type>,
        "http2_compact_offset_vector uses memmove; use http2_compact_movable_offset_vector for owning "
        "values");

    if (!http2_should_compact_offset_vector(values, offset, threshold)) {
        return;
    }
    const auto remaining = values.size() - offset;
    std::memmove(
        values.data(), values.data() + offset, remaining * sizeof(typename vector_type::value_type));
    values.resize(remaining);
    offset = 0;
}

template <typename vector_type>
inline void http2_compact_movable_offset_vector(
    vector_type& values, std::size_t& offset, std::size_t threshold) {
    if (!http2_should_compact_offset_vector(values, offset, threshold)) {
        return;
    }
    const auto remaining = values.size() - offset;
    for (std::size_t i = 0; i < remaining; ++i) {
        values[i] = std::move(values[offset + i]);
    }
    values.resize(remaining);
    offset = 0;
}

}  // namespace ruvia::detail
