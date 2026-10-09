#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"

#include "response/http_response_header_access.h"

namespace ruvia::detail {

using response_header_index_slot_type = std::int16_t;

inline constexpr response_header_index_slot_type missing_response_header_index_slot = 0;
inline constexpr response_header_index_slot_type overflow_response_header_index_slot = -1;

template <std::size_t count>
using response_header_index_cache_type = std::array<response_header_index_slot_type, count>;

[[nodiscard]] inline bool response_header_index_slot_has_value(response_header_index_slot_type slot) noexcept {
    return slot > 0;
}

[[nodiscard]] inline bool response_header_index_slot_overflowed(response_header_index_slot_type slot) noexcept {
    return slot == overflow_response_header_index_slot;
}

[[nodiscard]] inline std::size_t response_header_index_slot_value(
    response_header_index_slot_type slot) noexcept {
    return static_cast<std::size_t>(slot - 1);
}

template <std::size_t count>
inline void record_response_header_index(
    response_header_index_cache_type<count>& cache, std::size_t slot, std::size_t index) noexcept {
    if (slot >= count) {
        return;
    }
    if (cache[slot] != missing_response_header_index_slot) {
        return;
    }
    if (index < static_cast<std::size_t>(std::numeric_limits<response_header_index_slot_type>::max())) {
        cache[slot] = static_cast<response_header_index_slot_type>(index + 1);
    } else {
        cache[slot] = overflow_response_header_index_slot;
    }
}

template <typename header_pointer_type, std::size_t count>
[[nodiscard]] inline header_pointer_type find_response_header_indexed(header_pointer_type begin, header_pointer_type end,
    const response_header_index_cache_type<count>& cache, std::size_t slot, std::string_view name,
    std::uint32_t known_bit) noexcept {
    if (slot < count) {
        const auto index = cache[slot];
        if (response_header_index_slot_has_value(index)) {
            return begin + response_header_index_slot_value(index);
        }
        if (!response_header_index_slot_overflowed(index)) {
            return end;
        }
    }

    for (auto cursor_value = begin; cursor_value != end; ++cursor_value) {
        const auto header_known_bit = response_header_known_bit(*cursor_value);
        if ((known_bit != 0 && header_known_bit == known_bit) ||
            (known_bit == 0 && http_ascii_equals_ignore_case(cursor_value->name(), name))) {
            return cursor_value;
        }
    }
    return end;
}

}  // namespace ruvia::detail
