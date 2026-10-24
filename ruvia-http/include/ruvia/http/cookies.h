#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_field_syntax.h"
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

// Expires value for deleting a cookie (Thu, 01 Jan 1970 00:00:00 GMT). RFC 6265
// section 4.1.1 limits max-age-av to non-zero digits, so deletion uses an
// Expires date in the past (RFC 6265 section 3.1) instead of Max-Age=0.
inline constexpr std::chrono::system_clock::time_point cookie_deletion_expires{};

// RFC 6265bis user-agent cookie-age-limit (400 days): the outbound cookie jar caps
// received lifetimes at this value. The Set-Cookie writer does not enforce it,
// since the limit binds user agents, not servers.
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

// Cookie octet validation and name-prefix rules are protocol rules shared by
// the Set-Cookie writer and outbound cookie jar. is_valid_cookie_value accepts
// the RFC 6265 cookie-value grammar, including one DQUOTE pair around the octets.
[[nodiscard]] bool is_valid_cookie_value(std::string_view value) noexcept;
// The prefix a wire cookie name starts with, matched ASCII case-insensitively
// as user agents do (RFC 6265bis 4.1.3); nullopt when it carries none.
[[nodiscard]] std::optional<cookie_prefix> cookie_name_prefix(std::string_view name) noexcept;
// RFC 6265bis 4.1.3: __Secure- requires Secure; __Host- also requires Path=/
// and no Domain. `path` is the Path attribute value, empty when absent.
[[nodiscard]] bool cookie_prefix_requirements_met(
    cookie_prefix prefix, bool secure, std::string_view path, std::string_view domain) noexcept;
[[nodiscard]] std::string_view http_cookie_prefix_text(cookie_prefix prefix) noexcept;

struct cookie_options final {
    // Cookie attributes are retained by set_cookie_plan until serialization.
    // Keep their zero-copy representation, but reject owning-string rvalues so
    // a stored options value cannot silently contain an already-dangling view.
    ::ruvia::borrowed_text path_{"/"};
    ::ruvia::borrowed_text domain_{};
    std::optional<cookie_same_site> same_site_{};
    std::optional<cookie_priority> priority_{};
    // UTC, rounded down to whole seconds; year >= 1601. Lifetime caps belong to user agents.
    std::optional<std::chrono::system_clock::time_point> expires_{};
    // Zero means expire now: written as Expires=cookie_deletion_expires (replacing
    // expires_), since max-age-av requires a non-zero first digit (RFC 6265 4.1.1).
    std::optional<std::chrono::seconds> max_age_{};
    std::optional<cookie_prefix> prefix_{};
    cookie_attribute_policy http_only_{cookie_attribute_policy::omit};
    cookie_attribute_policy secure_{cookie_attribute_policy::omit};
    cookie_attribute_policy partitioned_{cookie_attribute_policy::omit};
};

// Visits literal semicolon-separated Cookie name/value pairs. A quote has no
// special meaning in a Cookie field. Segments without '=' are skipped, names
// and values are OWS-trimmed, and returning false stops traversal. Views borrow
// the input field.
template <typename visitor_type>
inline void http_visit_cookie_pairs(std::string_view value, visitor_type&& visitor) {
    while (!value.empty()) {
        const auto semicolon = value.find(';');
        const auto part = http_trim_ows(
            semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos &&
            !visitor(http_trim_ows(part.substr(0, equals)), http_trim_ows(part.substr(equals + 1)))) {
            return;
        }
        if (semicolon == std::string_view::npos) {
            return;
        }
        value.remove_prefix(semicolon + 1);
    }
}

class http_response_headers;

// Validates and fixes the exact Set-Cookie field-value shape before a runtime
// allocates its output buffer. The plan borrows name, value, path and domain
// unchanged until write() completes, so owning-string and cookie_options
// temporaries are rejected at construction. Construction throws std::length_error if the wire
// length cannot be represented by std::size_t.
class set_cookie_plan final {
public:
    set_cookie_plan(std::string_view name, std::string_view value, const cookie_options& options);

    template <typename name_type, typename value_type>
        requires(detail::http_temporary_owning_char_string<name_type> ||
                    detail::http_temporary_owning_char_string<value_type>)
    set_cookie_plan(name_type&&, value_type&&, const cookie_options&) = delete;

    set_cookie_plan(std::string_view, std::string_view, cookie_options&&) = delete;
    set_cookie_plan(std::string_view, std::string_view, const cookie_options&&) = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view wire_prefix() const noexcept {
        return prefix_text_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::string_view domain() const noexcept {
        return domain_;
    }

    // output must cover size() bytes and not overlap the borrowed inputs.
    void write(char* output) const;

private:
    friend class http_response_headers;
    struct written_fields final {
        std::string_view wire_name_;
        std::string_view path_;
        std::string_view domain_;
    };
    [[nodiscard]] written_fields write_fields(char* output) const noexcept;
    std::array<char, 32> expires_buffer_{};
    std::string_view name_;
    std::string_view value_;
    std::string_view path_;
    std::string_view domain_;
    std::string_view prefix_text_;
    std::string_view priority_text_;
    std::string_view same_site_text_;
    std::uint64_t max_age_value_{0};
    std::size_t expires_size_{0};
    std::size_t size_{0};
    bool has_max_age_{false};
    bool http_only_{false};
    bool secure_{false};
    bool partitioned_{false};
};

}  // namespace ruvia
