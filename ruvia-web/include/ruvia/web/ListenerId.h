#pragma once

#include <cstdint>

namespace ruvia {

class ListenerId final {
public:
    explicit constexpr ListenerId(std::uint32_t value) noexcept
        : value_(value) {}

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return value_;
    }

    friend constexpr bool operator==(ListenerId, ListenerId) noexcept = default;

private:
    std::uint32_t value_;
};

}  // namespace ruvia
