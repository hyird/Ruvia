#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

#include "ruvia/web/Attributes.h"

namespace ruvia {

template <std::size_t N>
struct FixedString {
    char value[N]{};

    constexpr FixedString(const char (&text)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = text[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::string_view(value, N - 1);
    }
    [[nodiscard]] constexpr std::string_view view() const&& = delete;
};

template <std::size_t N>
FixedString(const char (&)[N]) -> FixedString<N>;

template <std::size_t LeftN, std::size_t RightN>
[[nodiscard]] constexpr bool operator==(
    const FixedString<LeftN>& left, const FixedString<RightN>& right) noexcept {
    if constexpr (LeftN != RightN) {
        return false;
    } else {
        return std::ranges::equal(left.value, right.value);
    }
}

}  // namespace ruvia
