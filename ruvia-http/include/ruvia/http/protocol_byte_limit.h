#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>

namespace ruvia {

// A protocol byte ceiling with no numeric sentinel: the default is unlimited,
// while a configured limit is always strictly positive.
class protocol_byte_limit final {
public:
    protocol_byte_limit() noexcept = default;

    [[nodiscard]] static protocol_byte_limit unlimited() noexcept {
        return protocol_byte_limit();
    }

    [[nodiscard]] static protocol_byte_limit limited(std::size_t bytes_value) {
        if (bytes_value == 0) {
            throw std::invalid_argument("protocol byte limit must be greater than zero");
        }
        return protocol_byte_limit(bytes_value);
    }

    [[nodiscard]] bool is_limited() const noexcept {
        return maximum_.has_value();
    }

    [[nodiscard]] std::optional<std::size_t> maximum() const noexcept {
        return maximum_;
    }

    [[nodiscard]] bool exceeds(std::size_t bytes_value) const noexcept {
        return maximum_.has_value() && bytes_value > *maximum_;
    }

    [[nodiscard]] bool addition_exceeds(std::size_t current, std::size_t added) const noexcept {
        if (added > (std::numeric_limits<std::size_t>::max)() - current) {
            return true;
        }
        return maximum_.has_value() && (current > *maximum_ || added > *maximum_ - current);
    }

    [[nodiscard]] std::size_t read_ceiling() const noexcept {
        return maximum_.value_or((std::numeric_limits<std::size_t>::max)());
    }

private:
    explicit protocol_byte_limit(std::size_t bytes_value) noexcept
        : maximum_(bytes_value) {}

    std::optional<std::size_t> maximum_;
};

}  // namespace ruvia
