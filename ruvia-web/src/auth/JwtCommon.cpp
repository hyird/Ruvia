#include "ruvia/web/detail/auth/JwtPrimitives.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace ruvia::detail {

bool jwtIsReservedClaim(std::string_view name) noexcept {
    return name == "iss" || name == "sub" || name == "aud" || name == "exp" || name == "nbf" || name == "iat" || name == "jti";
}

std::int64_t jwtEpochSeconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point jwtFromEpochSeconds(std::int64_t value) {
    // Building a time_point converts the seconds count into the clock's finer
    // duration (nanoseconds on libstdc++), multiplying by 1e9 and overflowing
    // int64 for |value| beyond ~9.2e9 (≈ year 2262) -- undefined behaviour on an
    // attacker-controlled exp/nbf/iat. Saturate to the representable range so a
    // huge exp reads as "far future" (never expired) and a huge nbf as "far
    // future" (not yet valid); both stay fail-closed and free of UB.
    using Clock = std::chrono::system_clock;
    constexpr std::int64_t kMaxSeconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::max()).count();
    constexpr std::int64_t kMinSeconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::min()).count();
    return Clock::time_point(std::chrono::seconds(std::clamp(value, kMinSeconds, kMaxSeconds)));
}

std::chrono::system_clock::time_point jwtTimeWithOffset(std::chrono::system_clock::time_point value, std::chrono::seconds offset) noexcept {
    using Clock = std::chrono::system_clock;
    const auto valueSeconds = std::chrono::duration<long double>(value.time_since_epoch()).count();
    const auto targetSeconds = valueSeconds + static_cast<long double>(offset.count());
    const auto maxSeconds = std::chrono::duration<long double>(Clock::duration::max()).count();
    const auto minSeconds = std::chrono::duration<long double>(Clock::duration::min()).count();
    if (targetSeconds >= maxSeconds) {
        return Clock::time_point::max();
    }
    if (targetSeconds <= minSeconds) {
        return Clock::time_point::min();
    }
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<long double>(targetSeconds)));
}

JwtTokenParts jwtSplitToken(std::string_view token) {
    const auto first = token.find('.');
    const auto second = first == std::string_view::npos ? std::string_view::npos : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos) {
        throw std::invalid_argument("JWT token must have three sections");
    }
    return JwtTokenParts{token.substr(0, first), token.substr(first + 1, second - first - 1), token.substr(second + 1), token.substr(0, second)};
}

}  // namespace ruvia::detail
