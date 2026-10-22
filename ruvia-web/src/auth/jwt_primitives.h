#pragma once

#ifdef RUVIA_ENABLE_JWT

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/auth/jwt.h"

// Shared between jwt.cpp (token signing and verification) and jwt_json.cpp
// (JOSE header and claims JSON).
namespace ruvia::detail {

void jwt_append_json_member(
    std::pmr::string& out, bool& first, std::string_view name, std::string_view value);
void jwt_append_json_member(
    std::pmr::string& out, bool& first, std::string_view name, std::int64_t value);
[[nodiscard]] std::pmr::string jwt_parse_jose_algorithm(
    std::string_view json, std::pmr::memory_resource* resource);

// Saturates at the system clock's range instead of overflowing its ticks.
[[nodiscard]] std::chrono::system_clock::time_point jwt_from_epoch_seconds(std::int64_t value);

struct jwt_payload_access final {
    [[nodiscard]] static jwt_claim claim(std::pmr::string name, std::pmr::string value) noexcept {
        return jwt_claim(std::move(name), std::move(value));
    }

    [[nodiscard]] static bool decode_jwt_payload_claim(jwt_payload& payload_value, std::string_view key,
        std::string_view value, std::pmr::memory_resource* resource);
    static jwt_payload decode_payload_json(std::string_view json, std::pmr::memory_resource* resource);
};

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_JWT
