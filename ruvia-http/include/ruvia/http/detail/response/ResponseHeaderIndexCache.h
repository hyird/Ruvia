#pragma once

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace ruvia::detail {

using ResponseHeaderIndexSlot = std::int16_t;

inline constexpr ResponseHeaderIndexSlot kMissingResponseHeaderIndexSlot = 0;
inline constexpr ResponseHeaderIndexSlot kOverflowResponseHeaderIndexSlot = -1;

template <std::size_t Count>
using ResponseHeaderIndexCache = std::array<ResponseHeaderIndexSlot, Count>;

[[nodiscard]] inline bool responseHeaderIndexSlotHasValue(ResponseHeaderIndexSlot slot) noexcept {
    return slot > 0;
}

[[nodiscard]] inline bool responseHeaderIndexSlotOverflowed(ResponseHeaderIndexSlot slot) noexcept {
    return slot == kOverflowResponseHeaderIndexSlot;
}

[[nodiscard]] inline std::size_t responseHeaderIndexSlotValue(ResponseHeaderIndexSlot slot) noexcept {
    return static_cast<std::size_t>(slot - 1);
}

template <std::size_t Count>
inline void recordResponseHeaderIndex(ResponseHeaderIndexCache<Count>& cache, std::size_t slot, std::size_t index) noexcept {
    if (slot >= Count) {
        return;
    }
    if (cache[slot] != kMissingResponseHeaderIndexSlot) {
        return;
    }
    if (index < static_cast<std::size_t>(std::numeric_limits<ResponseHeaderIndexSlot>::max())) {
        cache[slot] = static_cast<ResponseHeaderIndexSlot>(index + 1);
    } else {
        cache[slot] = kOverflowResponseHeaderIndexSlot;
    }
}

template <typename HeaderPointer, std::size_t Count>
[[nodiscard]] inline HeaderPointer findResponseHeaderIndexed(HeaderPointer begin, HeaderPointer end, const ResponseHeaderIndexCache<Count>& cache, std::size_t slot, std::string_view name, std::uint32_t knownBit) noexcept {
    if (slot < Count) {
        const auto index = cache[slot];
        if (responseHeaderIndexSlotHasValue(index)) {
            return begin + responseHeaderIndexSlotValue(index);
        }
        if (!responseHeaderIndexSlotOverflowed(index)) {
            return end;
        }
    }

    for (auto cursor = begin; cursor != end; ++cursor) {
        const auto headerKnownBit = responseHeaderKnownBit(*cursor);
        if ((knownBit != 0 && headerKnownBit == knownBit) || (knownBit == 0 && httpAsciiEqualsIgnoreCase(cursor->name(), name))) {
            return cursor;
        }
    }
    return end;
}

}  // namespace ruvia::detail
