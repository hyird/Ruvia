#pragma once

#include <cstddef>
#include <cstring>

namespace ruvia::detail {

template <typename Vector>
inline void http2CompactOffsetVector(
    Vector& values,
    std::size_t& offset,
    std::size_t threshold) noexcept {
    if (offset == 0) {
        return;
    }
    if (offset == values.size()) {
        values.clear();
        offset = 0;
        return;
    }
    if (offset < threshold && offset * 2 < values.size()) {
        return;
    }
    const auto remaining = values.size() - offset;
    std::memmove(
        values.data(),
        values.data() + offset,
        remaining * sizeof(typename Vector::value_type));
    values.resize(remaining);
    offset = 0;
}

}  // namespace ruvia::detail
