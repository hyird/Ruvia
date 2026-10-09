#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {

enum class cookie_prefix : std::uint8_t {
    secure,  // serializes the name as "__Secure-<name>"; requires secure
    host,    // serializes the name as "__Host-<name>"; requires secure, Path=/, no Domain
};

enum class cookie_same_site : std::uint8_t {
    strict,
    lax,
    none,  // the literal SameSite=None attribute; requires Secure
};

enum class cookie_priority : std::uint8_t {
    low,
    medium,
    high,
};

enum class cookie_attribute_policy : std::uint8_t {
    omit,
    emit,
};

// RFC 6265bis: cookie lifetimes SHOULD NOT exceed 400 days.
inline constexpr std::int64_t max_cookie_age_seconds = 34560000;

// Cookie request pairs always include '=', including for an empty name.
template <typename string>
void append_cookie_request_pair(string& header_value, std::string_view name, std::string_view value) {
    if (!header_value.empty()) {
        header_value.append("; ", 2);
    }
    header_value.append(name.data(), name.size());
    header_value.push_back('=');
    header_value.append(value.data(), value.size());
}

// Cookie octet validation and name-prefix parsing are protocol rules shared by
// the Set-Cookie writer and outbound cookie jar.
[[nodiscard]] bool is_valid_cookie_value(std::string_view value) noexcept;
[[nodiscard]] bool cookie_name_starts_with_ignore_case(
    std::string_view name, std::string_view prefix) noexcept;
[[nodiscard]] std::string_view http_cookie_prefix_text(cookie_prefix prefix) noexcept;

struct cookie_options final {
    // Cookie attributes are retained by set_cookie_plan until serialization.
    // Keep their zero-copy representation, but reject owning-string rvalues so
    // a stored options value cannot silently contain an already-dangling view.
    ::ruvia::borrowed_text path_{"/"};
    ::ruvia::borrowed_text domain_{};
    std::optional<cookie_same_site> same_site_{};
    std::optional<cookie_priority> priority_{};
    // UTC, rounded down to whole seconds; year >= 1601 and at most 400 days ahead.
    std::optional<std::chrono::system_clock::time_point> expires_{};
    std::optional<std::chrono::seconds> max_age_{};
    std::optional<cookie_prefix> prefix_{};
    cookie_attribute_policy http_only_{cookie_attribute_policy::omit};
    cookie_attribute_policy secure_{cookie_attribute_policy::omit};
    cookie_attribute_policy partitioned_{cookie_attribute_policy::omit};
};

}  // namespace ruvia
