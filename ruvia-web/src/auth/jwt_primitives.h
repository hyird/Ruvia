#pragma once

#ifdef RUVIA_ENABLE_JWT

#include <chrono>
#include <concepts>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <ratio>
#include <string_view>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/web/auth/jwt.h"

namespace ruvia::detail {

[[nodiscard]] bool jwt_is_reserved_claim(std::string_view name) noexcept;

void jwt_append_json_escaped(std::pmr::string& out, std::string_view value);
void jwt_append_json_member(
    std::pmr::string& out, bool& first, std::string_view name, std::string_view value);
void jwt_append_json_member(
    std::pmr::string& out, bool& first, std::string_view name, std::int64_t value);
[[nodiscard]] std::pmr::string jwt_parse_jose_algorithm(
    std::string_view json, std::pmr::memory_resource* resource);

// Throws std::length_error when the encoded or decoded capacity hint cannot be
// represented by std::size_t before allocating the result.
[[nodiscard]] std::pmr::string jwt_base64_url_encode(
    std::string_view input, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string jwt_base64_url_decode(
    std::string_view input, std::pmr::memory_resource* resource);

[[nodiscard]] std::string_view jwt_algorithm_name(jwt_algorithm algorithm);
// Throws std::invalid_argument for a key shorter than the selected digest,
// and std::length_error when
// the secret or signing input cannot be represented by OpenSSL's HMAC length
// parameters.
[[nodiscard]] std::pmr::string jwt_hmac_sign(jwt_algorithm algorithm, std::string_view secret,
    std::string_view data, std::pmr::memory_resource* resource);
[[nodiscard]] bool jwt_constant_time_equals(std::string_view left, std::string_view right) noexcept;

[[nodiscard]] std::int64_t jwt_epoch_seconds(std::chrono::system_clock::time_point value);
[[nodiscard]] std::chrono::system_clock::time_point jwt_from_epoch_seconds(std::int64_t value);
[[nodiscard]] std::chrono::system_clock::time_point jwt_time_with_offset(
    std::chrono::system_clock::time_point value, std::chrono::seconds offset) noexcept;

// Split without subtracting a rounded time_point: floor(seconds) at the
// clock's minimum can itself lie below the representable clock range.
using jwt_clock_second_ratio_type = std::ratio_divide<std::chrono::seconds::period,
    std::chrono::system_clock::period>;
static_assert(jwt_clock_second_ratio_type::den == 1 && jwt_clock_second_ratio_type::num > 1,
    "JWT time arithmetic requires an integral subsecond system clock");
static_assert(std::numeric_limits<std::chrono::system_clock::rep>::is_integer &&
                  std::numeric_limits<std::chrono::system_clock::rep>::is_signed &&
                  std::numeric_limits<std::chrono::system_clock::rep>::digits <=
                      std::numeric_limits<std::chrono::seconds::rep>::digits,
    "JWT whole-second distances must fit in the signed seconds representation");

struct jwt_clock_parts final {
    std::chrono::seconds whole_seconds_{};
    std::chrono::system_clock::duration fraction_{};
};

[[nodiscard]] inline jwt_clock_parts jwt_split_clock_time(
    std::chrono::system_clock::time_point value) noexcept {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch());
    auto remainder = value.time_since_epoch().count() % jwt_clock_second_ratio_type::num;
    if (remainder < 0) {
        seconds -= std::chrono::seconds{1};
        remainder += jwt_clock_second_ratio_type::num;
    }
    return {seconds, std::chrono::system_clock::duration{remainder}};
}

// RFC 7519 §4.1.4: a token is valid only while the current time is *before*
// "exp", so at now == exp (no leeway) it MUST be rejected. leeway widens the
// accepted window past exp. Split out as a pure predicate so the exact boundary
// is deterministically testable without a live-clock dependency.
[[nodiscard]] inline bool jwt_token_expired(std::chrono::system_clock::time_point now,
    std::chrono::system_clock::time_point expires_at, std::chrono::seconds leeway) noexcept {
    const auto now_parts = jwt_split_clock_time(now);
    const auto expires_parts = jwt_split_clock_time(expires_at);
    // Subsecond clock resolution keeps the whole-second distance representable
    // even when subtracting time_points directly would overflow their duration.
    const auto distance = now_parts.whole_seconds_ - expires_parts.whole_seconds_;
    return distance > leeway ||
           (distance == leeway && now_parts.fraction_ >= expires_parts.fraction_);
}

// RFC 7519 §4.1.5: a token is valid only when the current time is *after or
// equal to* "nbf"; leeway widens the accepted window earlier. Rejected while
// now (plus leeway) is still strictly before nbf.
[[nodiscard]] inline bool jwt_token_not_yet_valid(std::chrono::system_clock::time_point now,
    std::chrono::system_clock::time_point not_before, std::chrono::seconds leeway) noexcept {
    const auto now_parts = jwt_split_clock_time(now);
    const auto not_before_parts = jwt_split_clock_time(not_before);
    const auto distance = not_before_parts.whole_seconds_ - now_parts.whole_seconds_;
    return distance > leeway ||
           (distance == leeway && not_before_parts.fraction_ > now_parts.fraction_);
}

struct jwt_token_parts final {
    std::string_view header_;
    std::string_view payload_;
    std::string_view signature_;
    std::string_view signing_input_;
};

[[nodiscard]] jwt_token_parts jwt_split_token(std::string_view token);

template <typename token_type>
    requires(std::convertible_to<token_type &&, std::string_view> &&
                !std::constructible_from<borrowed_text, token_type &&>)
jwt_token_parts jwt_split_token(token_type&&) = delete;

}  // namespace ruvia::detail

namespace ruvia::detail {

struct jwt_payload_access final {
    [[nodiscard]] static jwt_claim claim(std::pmr::string name, std::pmr::string value) {
        return jwt_claim(jwt_claim::owned_tag_type{}, std::move(name), std::move(value));
    }

    static jwt_payload decode_payload_json(std::string_view json, std::pmr::memory_resource* resource);
};

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_JWT
