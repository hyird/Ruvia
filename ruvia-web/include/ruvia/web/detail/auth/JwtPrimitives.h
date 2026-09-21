#pragma once

#ifdef RUVIA_ENABLE_JWT

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <ratio>
#include <string_view>

#include "ruvia/http/detail/util/BorrowedView.h"
#include "ruvia/web/auth/Jwt.h"

namespace ruvia::detail {

[[nodiscard]] bool jwtIsReservedClaim(std::string_view name) noexcept;

void jwtAppendJsonEscaped(std::pmr::string& out, std::string_view value);
void jwtAppendJsonMember(
    std::pmr::string& out, bool& first, std::string_view name, std::string_view value);
void jwtAppendJsonMember(
    std::pmr::string& out, bool& first, std::string_view name, std::int64_t value);
[[nodiscard]] std::pmr::string jwtParseJoseAlgorithm(
    std::string_view json, std::pmr::memory_resource* resource);

// Throws std::length_error when the encoded or decoded capacity hint cannot be
// represented by std::size_t before allocating the result.
[[nodiscard]] std::pmr::string jwtBase64UrlEncode(
    std::string_view input, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string jwtBase64UrlDecode(
    std::string_view input, std::pmr::memory_resource* resource);

[[nodiscard]] std::string_view jwtAlgorithmName(JwtAlgorithm algorithm);
// Throws std::invalid_argument for an empty secret and std::length_error when
// the secret or signing input cannot be represented by OpenSSL's HMAC length
// parameters.
[[nodiscard]] std::pmr::string jwtHmacSign(JwtAlgorithm algorithm, std::string_view secret,
    std::string_view data, std::pmr::memory_resource* resource);
[[nodiscard]] bool jwtConstantTimeEquals(std::string_view left, std::string_view right) noexcept;

[[nodiscard]] std::int64_t jwtEpochSeconds(std::chrono::system_clock::time_point value);
[[nodiscard]] std::chrono::system_clock::time_point jwtFromEpochSeconds(std::int64_t value);
[[nodiscard]] std::chrono::system_clock::time_point jwtTimeWithOffset(
    std::chrono::system_clock::time_point value, std::chrono::seconds offset) noexcept;

// Split without subtracting a rounded time_point: floor(seconds) at the
// clock's minimum can itself lie below the representable clock range.
using JwtClockSecondRatio = std::ratio_divide<std::chrono::seconds::period,
    std::chrono::system_clock::period>;
static_assert(JwtClockSecondRatio::den == 1 && JwtClockSecondRatio::num > 1,
    "JWT time arithmetic requires an integral subsecond system clock");
static_assert(std::numeric_limits<std::chrono::system_clock::rep>::is_integer &&
                  std::numeric_limits<std::chrono::system_clock::rep>::is_signed &&
                  std::numeric_limits<std::chrono::system_clock::rep>::digits <=
                      std::numeric_limits<std::chrono::seconds::rep>::digits,
    "JWT whole-second distances must fit in the signed seconds representation");

struct JwtClockParts final {
    std::chrono::seconds wholeSeconds{};
    std::chrono::system_clock::duration fraction{};
};

[[nodiscard]] inline JwtClockParts jwtSplitClockTime(
    std::chrono::system_clock::time_point value) noexcept {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch());
    auto remainder = value.time_since_epoch().count() % JwtClockSecondRatio::num;
    if (remainder < 0) {
        seconds -= std::chrono::seconds{1};
        remainder += JwtClockSecondRatio::num;
    }
    return {seconds, std::chrono::system_clock::duration{remainder}};
}

// RFC 7519 §4.1.4: a token is valid only while the current time is *before*
// "exp", so at now == exp (no leeway) it MUST be rejected. leeway widens the
// accepted window past exp. Split out as a pure predicate so the exact boundary
// is deterministically testable without a live-clock dependency.
[[nodiscard]] inline bool jwtTokenExpired(std::chrono::system_clock::time_point now,
    std::chrono::system_clock::time_point expiresAt, std::chrono::seconds leeway) noexcept {
    const auto nowParts = jwtSplitClockTime(now);
    const auto expiresParts = jwtSplitClockTime(expiresAt);
    // Subsecond clock resolution keeps the whole-second distance representable
    // even when subtracting time_points directly would overflow their duration.
    const auto distance = nowParts.wholeSeconds - expiresParts.wholeSeconds;
    return distance > leeway ||
           (distance == leeway && nowParts.fraction >= expiresParts.fraction);
}

// RFC 7519 §4.1.5: a token is valid only when the current time is *after or
// equal to* "nbf"; leeway widens the accepted window earlier. Rejected while
// now (plus leeway) is still strictly before nbf.
[[nodiscard]] inline bool jwtTokenNotYetValid(std::chrono::system_clock::time_point now,
    std::chrono::system_clock::time_point notBefore, std::chrono::seconds leeway) noexcept {
    const auto nowParts = jwtSplitClockTime(now);
    const auto notBeforeParts = jwtSplitClockTime(notBefore);
    const auto distance = notBeforeParts.wholeSeconds - nowParts.wholeSeconds;
    return distance > leeway ||
           (distance == leeway && notBeforeParts.fraction > nowParts.fraction);
}

struct JwtTokenParts final {
    std::string_view header;
    std::string_view payload;
    std::string_view signature;
    std::string_view signingInput;
};

[[nodiscard]] JwtTokenParts jwtSplitToken(std::string_view token);

template <HttpTemporaryOwningCharString Token>
JwtTokenParts jwtSplitToken(Token&&) = delete;

}  // namespace ruvia::detail

namespace ruvia::detail {

struct JwtPayloadAccess final {
    [[nodiscard]] static JwtClaim claim(std::pmr::string name, std::pmr::string value) {
        return JwtClaim(JwtClaim::OwnedTag{}, std::move(name), std::move(value));
    }

    static JwtPayload decodePayloadJson(std::string_view json, std::pmr::memory_resource* resource);
};

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_JWT
