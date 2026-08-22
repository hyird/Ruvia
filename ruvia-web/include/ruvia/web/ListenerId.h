#pragma once

#include <cstdint>
#include <stdexcept>

namespace ruvia {

class ListenerId final {
public:
    explicit constexpr ListenerId(std::uint32_t value)
        : value_(value) {
        if (value == 0) {
            throw std::invalid_argument("listener ID must be greater than zero");
        }
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return value_;
    }

    friend constexpr bool operator==(ListenerId, ListenerId) noexcept = default;

private:
    std::uint32_t value_;
};

}  // namespace ruvia
