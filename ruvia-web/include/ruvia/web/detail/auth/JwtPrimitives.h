#pragma once

#include "ruvia/web/auth/Jwt.h"

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia::detail {

[[nodiscard]] bool jwtIsReservedClaim(std::string_view name) noexcept;

void jwtAppendJsonEscaped(std::pmr::string& out, std::string_view value);
void jwtAppendJsonMember(std::pmr::string& out, bool& first, std::string_view name, std::string_view value);
void jwtAppendJsonMember(std::pmr::string& out, bool& first, std::string_view name, std::int64_t value);
[[nodiscard]] std::pmr::string jwtParseJoseAlgorithm(std::string_view json, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::string jwtBase64UrlEncode(std::string_view input, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string jwtBase64UrlDecode(std::string_view input, std::pmr::memory_resource* resource);

[[nodiscard]] std::string_view jwtAlgorithmName(JwtAlgorithm algorithm);
[[nodiscard]] std::pmr::string jwtHmacSign(JwtAlgorithm algorithm, std::string_view secret, std::string_view data, std::pmr::memory_resource* resource);
[[nodiscard]] bool jwtConstantTimeEquals(std::string_view left, std::string_view right) noexcept;

[[nodiscard]] std::int64_t jwtEpochSeconds(std::chrono::system_clock::time_point value);
[[nodiscard]] std::chrono::system_clock::time_point jwtFromEpochSeconds(std::int64_t value);
[[nodiscard]] std::chrono::system_clock::time_point jwtTimeWithOffset(std::chrono::system_clock::time_point value, std::chrono::seconds offset) noexcept;

// RFC 7519 §4.1.4: a token is valid only while the current time is *before*
// "exp", so at now == exp (no leeway) it MUST be rejected. leeway widens the
// accepted window past exp. Split out as a pure predicate so the exact boundary
// is deterministically testable without a live-clock dependency.
[[nodiscard]] inline bool jwtTokenExpired(std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point expiresAt, std::chrono::seconds leeway) noexcept {
    const auto nowSeconds = std::chrono::duration<long double>(now.time_since_epoch()).count();
    const auto expiresSeconds = std::chrono::duration<long double>(expiresAt.time_since_epoch()).count();
    return nowSeconds - expiresSeconds >= static_cast<long double>(leeway.count());
}

// RFC 7519 §4.1.5: a token is valid only when the current time is *after or
// equal to* "nbf"; leeway widens the accepted window earlier. Rejected while
// now (plus leeway) is still strictly before nbf.
[[nodiscard]] inline bool jwtTokenNotYetValid(std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point notBefore, std::chrono::seconds leeway) noexcept {
    const auto nowSeconds = std::chrono::duration<long double>(now.time_since_epoch()).count();
    const auto notBeforeSeconds = std::chrono::duration<long double>(notBefore.time_since_epoch()).count();
    return notBeforeSeconds - nowSeconds > static_cast<long double>(leeway.count());
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
