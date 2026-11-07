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

// Expanding encoders read group k of an input at input + n * k and write its
// at most n + 1 characters at output + (n + 1) * k, so with d = input - output, group
// k's output starts k - d bytes past its own input. Groups k < d write below
// the next group's input and must run forwards; groups k >= d write at or above
// their own input, which every earlier group lies below, and must run
// backwards. Encoding the backward groups first, then the forward groups,
// never overwrites unread input for any placement of output, in place included.
// Returns the number of leading groups to encode forwards. Addresses compare as
// integers because disjoint buffers are unrelated pointers.
[[nodiscard]] inline std::size_t http_forward_encoded_groups(
    const void* output, const void* input, std::size_t groups) noexcept {
    const auto output_address = reinterpret_cast<std::uintptr_t>(output);
    const auto input_address = reinterpret_cast<std::uintptr_t>(input);
    if (output_address >= input_address) {
        return 0;
    }
    const auto distance = input_address - output_address;
    return distance < groups ? static_cast<std::size_t>(distance) : groups;
}

}  // namespace ruvia::detail
