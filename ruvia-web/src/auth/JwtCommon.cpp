#include <chrono>
#include <stdexcept>

#include "ruvia/web/detail/auth/JwtPrimitives.h"

namespace ruvia::detail {

bool jwtIsReservedClaim(std::string_view name) noexcept {
    return name == "iss" || name == "sub" || name == "aud" || name == "exp" || name == "nbf" ||
           name == "iat" || name == "jti";
}

std::int64_t jwtEpochSeconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point jwtFromEpochSeconds(std::int64_t value) {
    // Use the same saturating conversion for integer NumericDates and offsets.
    // Converting unbounded seconds directly to clock ticks would overflow.
    return jwtTimeWithOffset(std::chrono::system_clock::time_point{}, std::chrono::seconds{value});
}

std::chrono::system_clock::time_point jwtTimeWithOffset(
    std::chrono::system_clock::time_point value, std::chrono::seconds offset) noexcept {
    using Clock = std::chrono::system_clock;
    const auto parts = jwtSplitClockTime(value);
    const auto minimum = jwtSplitClockTime(Clock::time_point::min());
    const auto maximum = jwtSplitClockTime(Clock::time_point::max());
    if (offset > maximum.wholeSeconds - parts.wholeSeconds) {
        return Clock::time_point::max();
    }
    if (offset < minimum.wholeSeconds - parts.wholeSeconds) {
        return Clock::time_point::min();
    }
    const auto target = parts.wholeSeconds + offset;
    if (target == minimum.wholeSeconds) {
        if (parts.fraction < minimum.fraction) {
            return Clock::time_point::min();
        }
        // Do not convert the minimum's floored seconds back into clock ticks:
        // that intermediate can underflow even when the final value fits.
        return Clock::time_point::min() + (parts.fraction - minimum.fraction);
    }
    if (target == maximum.wholeSeconds && parts.fraction > maximum.fraction) {
        return Clock::time_point::max();
    }
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(target) + parts.fraction);
}

JwtTokenParts jwtSplitToken(std::string_view token) {
    const auto first = token.find('.');
    const auto second =
        first == std::string_view::npos ? std::string_view::npos : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        token.find('.', second + 1) != std::string_view::npos) {
        throw std::invalid_argument("JWT token must have three sections");
    }
    return JwtTokenParts{token.substr(0, first), token.substr(first + 1, second - first - 1),
        token.substr(second + 1), token.substr(0, second)};
}

}  // namespace ruvia::detail
