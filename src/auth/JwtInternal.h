#pragma once

#include "ruvia/auth/Jwt.h"

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

[[nodiscard]] bool jwtIsReservedClaim(std::string_view name) noexcept;

void jwtAppendJsonEscaped(std::pmr::string& out, std::string_view value);
void jwtAppendJsonMember(std::pmr::string& out, bool& first, std::string_view name, std::string_view value);
void jwtAppendJsonMember(std::pmr::string& out, bool& first, std::string_view name, std::int64_t value);
[[nodiscard]] std::string_view jwtFindJsonString(std::string_view json, std::string_view key);

[[nodiscard]] std::pmr::string jwtBase64UrlEncode(
    std::string_view input,
    std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string jwtBase64UrlDecode(
    std::string_view input,
    std::pmr::memory_resource* resource);

[[nodiscard]] std::string_view jwtAlgorithmName(JwtAlgorithm algorithm);
[[nodiscard]] std::pmr::string jwtHmacSign(
    JwtAlgorithm algorithm,
    std::string_view secret,
    std::string_view data,
    std::pmr::memory_resource* resource);
[[nodiscard]] bool jwtConstantTimeEquals(std::string_view left, std::string_view right) noexcept;

[[nodiscard]] std::int64_t jwtEpochSeconds(std::chrono::system_clock::time_point value);
[[nodiscard]] std::chrono::system_clock::time_point jwtFromEpochSeconds(std::int64_t value);

struct JwtTokenParts final {
    std::string_view header;
    std::string_view payload;
    std::string_view signature;
    std::string_view signingInput;
};

[[nodiscard]] JwtTokenParts jwtSplitToken(std::string_view token);

}  // namespace ruvia::detail

namespace ruvia {

struct JwtPayloadAccess final {
    static JwtPayload decodePayloadJson(std::string_view json, std::pmr::memory_resource* resource);
};

}  // namespace ruvia
