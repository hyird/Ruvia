#include <chrono>
#include <stdexcept>

#include "auth/jwt_primitives.h"

namespace ruvia::detail {

bool jwt_is_reserved_claim(std::string_view name) noexcept {
    return name == "iss" || name == "sub" || name == "aud" || name == "exp" || name == "nbf" ||
           name == "iat" || name == "jti";
}

std::int64_t jwt_epoch_seconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point jwt_from_epoch_seconds(std::int64_t value) {
    // Use the same saturating conversion for integer NumericDates and offsets.
    // Converting unbounded seconds directly to clock ticks would overflow.
    return jwt_time_with_offset(std::chrono::system_clock::time_point{}, std::chrono::seconds{value});
}

std::chrono::system_clock::time_point jwt_time_with_offset(
    std::chrono::system_clock::time_point value, std::chrono::seconds offset) noexcept {
    using clock_type = std::chrono::system_clock;
    const auto parts = jwt_split_clock_time(value);
    const auto minimum = jwt_split_clock_time(clock_type::time_point::min());
    const auto maximum = jwt_split_clock_time(clock_type::time_point::max());
    if (offset > maximum.whole_seconds_ - parts.whole_seconds_) {
        return clock_type::time_point::max();
    }
    if (offset < minimum.whole_seconds_ - parts.whole_seconds_) {
        return clock_type::time_point::min();
    }
    const auto target = parts.whole_seconds_ + offset;
    if (target == minimum.whole_seconds_) {
        if (parts.fraction_ < minimum.fraction_) {
            return clock_type::time_point::min();
        }
        // Do not convert the minimum's floored seconds back into clock ticks:
        // that intermediate can underflow even when the final value fits.
        return clock_type::time_point::min() + (parts.fraction_ - minimum.fraction_);
    }
    if (target == maximum.whole_seconds_ && parts.fraction_ > maximum.fraction_) {
        return clock_type::time_point::max();
    }
    return clock_type::time_point(std::chrono::duration_cast<clock_type::duration>(target) + parts.fraction_);
}

jwt_token_parts jwt_split_token(std::string_view token) {
    const auto first = token.find('.');
    const auto second =
        first == std::string_view::npos ? std::string_view::npos : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        token.find('.', second + 1) != std::string_view::npos) {
        throw std::invalid_argument("JWT token must have three sections");
    }
    return jwt_token_parts{token.substr(0, first), token.substr(first + 1, second - first - 1),
        token.substr(second + 1), token.substr(0, second)};
}

}  // namespace ruvia::detail
