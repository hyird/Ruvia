#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace ruvia::detail {

struct Http2PayloadSlice final {
    std::string_view first;
    std::string_view second;
};

[[nodiscard]] inline Http2PayloadSlice http2SliceTwoPartPayload(
    std::string_view first,
    std::string_view second,
    std::size_t offset,
    std::size_t size) noexcept {
    if (offset < first.size()) {
        const auto firstSize = std::min(size, first.size() - offset);
        const auto firstSlice = first.substr(offset, firstSize);
        size -= firstSize;
        return Http2PayloadSlice{
            .first = firstSlice,
            .second = size == 0 ? std::string_view{} : second.substr(0, size)};
    }
    return Http2PayloadSlice{
        .first = second.substr(offset - first.size(), size),
        .second = {}};
}

}  // namespace ruvia::detail
