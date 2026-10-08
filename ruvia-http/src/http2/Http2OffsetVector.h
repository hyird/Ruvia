#pragma once

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

namespace ruvia::detail {

template <typename Vector>
[[nodiscard]] inline bool http2ShouldCompactOffsetVector(
    Vector& values, std::size_t& offset, std::size_t threshold) noexcept {
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

template <typename Vector>
inline void http2CompactOffsetVector(
    Vector& values, std::size_t& offset, std::size_t threshold) noexcept {
    using Value = typename Vector::value_type;
    static_assert(std::is_trivially_copyable_v<Value>,
        "http2CompactOffsetVector uses memmove; use http2CompactMovableOffsetVector for owning "
        "values");

    if (!http2ShouldCompactOffsetVector(values, offset, threshold)) {
        return;
    }
    const auto remaining = values.size() - offset;
    std::memmove(
        values.data(), values.data() + offset, remaining * sizeof(typename Vector::value_type));
    values.resize(remaining);
    offset = 0;
}

template <typename Vector>
inline void http2CompactMovableOffsetVector(
    Vector& values, std::size_t& offset, std::size_t threshold) {
    if (!http2ShouldCompactOffsetVector(values, offset, threshold)) {
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
