#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

// Address intervals compare as unsigned integers: views into unrelated buffers
// are unrelated pointers, and subtraction avoids forming an overflowing end.
[[nodiscard]] inline bool http_text_overlaps(std::string_view left, std::string_view right) noexcept {
    if (left.empty() || right.empty()) {
        return false;
    }
    const auto left_address = reinterpret_cast<std::uintptr_t>(left.data());
    const auto right_address = reinterpret_cast<std::uintptr_t>(right.data());
    return left_address >= right_address ? left_address - right_address < right.size()
                                         : right_address - left_address < left.size();
}

// Whether `text` views bytes `storage` owns, its terminator included: growing or
// rewriting `storage` would destroy that input, so the caller owns a copy first.
[[nodiscard]] inline bool http_text_aliases_string(std::string_view text, const std::pmr::string& storage) noexcept {
    return http_text_overlaps(text, std::string_view(storage.data(), storage.capacity() + 1));
}

}  // namespace ruvia::detail
