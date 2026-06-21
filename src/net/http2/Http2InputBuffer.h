#pragma once

#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

inline constexpr std::size_t kHttp2InputCompactThresholdBytes = 64 * 1024;

[[nodiscard]] inline std::size_t http2AvailableInput(
    const std::pmr::string& input,
    std::size_t offset) noexcept {
    return input.size() - offset;
}

[[nodiscard]] inline std::string_view http2InputView(
    const std::pmr::string& input,
    std::size_t offset,
    std::size_t size) noexcept {
    return std::string_view(input.data() + offset, size);
}

inline void http2ConsumeInput(
    std::pmr::string& input,
    std::size_t& offset,
    std::size_t size) {
    offset += size;
    if (offset == input.size()) {
        input.clear();
        offset = 0;
        return;
    }
    if (offset >= kHttp2InputCompactThresholdBytes) {
        const auto remaining = input.size() - offset;
        std::memmove(input.data(), input.data() + offset, remaining);
        input.resize(remaining);
        offset = 0;
    }
}

}  // namespace ruvia::detail
