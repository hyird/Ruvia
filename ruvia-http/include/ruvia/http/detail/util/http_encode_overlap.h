#pragma once

#include <cstddef>
#include <cstdint>

namespace ruvia::detail {

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
