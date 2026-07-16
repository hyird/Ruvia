#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>

namespace ruvia {

// A protocol byte ceiling with no numeric sentinel: the default is unlimited,
// while a configured limit is always strictly positive.
class ProtocolByteLimit final {
public:
    ProtocolByteLimit() noexcept = default;

    [[nodiscard]] static ProtocolByteLimit unlimited() noexcept {
        return ProtocolByteLimit();
    }

    [[nodiscard]] static ProtocolByteLimit limited(std::size_t bytes) {
        if (bytes == 0) {
            throw std::invalid_argument(
                "protocol byte limit must be greater than zero");
        }
        return ProtocolByteLimit(bytes);
    }

    [[nodiscard]] bool isLimited() const noexcept {
        return maximum_.has_value();
    }

    [[nodiscard]] std::optional<std::size_t> maximum() const noexcept {
        return maximum_;
    }

    [[nodiscard]] bool exceeds(std::size_t bytes) const noexcept {
        return maximum_.has_value() && bytes > *maximum_;
    }

    [[nodiscard]] bool additionExceeds(
        std::size_t current,
        std::size_t added) const noexcept {
        if (added > (std::numeric_limits<std::size_t>::max)() - current) {
            return true;
        }
        return maximum_.has_value() &&
            (current > *maximum_ || added > *maximum_ - current);
    }

    [[nodiscard]] std::size_t readCeiling() const noexcept {
        return maximum_.value_or((std::numeric_limits<std::size_t>::max)());
    }

private:
    explicit ProtocolByteLimit(std::size_t bytes) noexcept
        : maximum_(bytes) {}

    std::optional<std::size_t> maximum_;
};

}  // namespace ruvia
