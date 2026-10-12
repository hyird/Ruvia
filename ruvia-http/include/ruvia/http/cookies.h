#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
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
// Inputs may borrow header_value, including its terminating NUL.
template <typename string>
void append_cookie_request_pair(string& header_value, std::string_view name, std::string_view value) {
    const auto start = header_value.size();
    const std::size_t separator_bytes = start == 0 ? 0 : 2;
    const auto remaining = header_value.max_size() - start;
    if (remaining < separator_bytes + 1 || name.size() > remaining - separator_bytes - 1 ||
        value.size() > remaining - separator_bytes - 1 - name.size()) {
        throw std::length_error("cookie request header length is not representable");
    }
    const auto borrowed_offset = [&](std::string_view input) -> std::optional<std::size_t> {
        const auto* const first = header_value.data();
        const std::less<const char*> before;
        if (input.empty() || before(input.data(), first) || before(first + start, input.data())) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(input.data() - first);
    };
    const auto name_offset = borrowed_offset(name);
    const auto value_offset = borrowed_offset(value);
    header_value.reserve(start + separator_bytes + name.size() + 1 + value.size());
    const auto append_input = [&](std::string_view input, std::optional<std::size_t> offset) {
        const bool borrows_nul = offset && input.size() == start - *offset + 1;
        if (offset) {
            input = std::string_view(header_value.data() + *offset, input.size());
        }
        // Punctuation overwrites the old terminator; copy its original NUL
        // explicitly, while every borrowed prefix remains unchanged.
        const auto prefix_size = input.size() - static_cast<std::size_t>(borrows_nul);
        if (prefix_size != 0) {
            header_value.append(input.data(), prefix_size);
        }
        if (borrows_nul) {
            header_value.push_back('\0');
        }
    };
    if (separator_bytes != 0) {
        header_value.append("; ", 2);
    }
    append_input(name, name_offset);
    header_value.push_back('=');
    append_input(value, value_offset);
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
