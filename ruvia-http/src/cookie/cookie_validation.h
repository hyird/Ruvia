#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/cookies.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_whitespace.h"
#include "ruvia/http/http_header.h"

namespace ruvia::detail {

// RFC 6265bis section 5.6 storage limits. The sender rejects anything the
// receiving parser would truncate or drop, so set_cookie can find and replace
// cookies it emitted earlier by re-parsing them.
inline constexpr std::size_t max_cookie_name_value_bytes = 4096;
inline constexpr std::size_t max_cookie_attribute_value_bytes = 1024;

[[nodiscard]] inline bool is_valid_cookie_value(std::string_view value) noexcept {
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7f || c == '"' || c == ',' || c == ';' || c == '\\') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool is_valid_cookie_attribute(std::string_view value) noexcept {
    // RFC 6265bis path-value is *av-octet: ASCII %x20-3A / %x3C-7E.
    // HTTP field values can carry HTAB and obs-text, but emitting either inside
    // Path produces a Set-Cookie value outside the server grammar.
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7e || c == ';') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool cookie_name_starts_with_ignore_case(
    std::string_view name, std::string_view prefix) noexcept {
    return name.size() >= prefix.size() &&
           http_ascii_equals_ignore_case(name.substr(0, prefix.size()), prefix);
}

[[nodiscard]] inline bool is_valid_cookie_domain(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }

    // RFC 6265bis domain-value is an RFC 1034 subdomain as relaxed by RFC 1123:
    // ASCII LDH labels, each at most 63 octets, with a letter or digit at both
    // ends. The root separators consume the remaining two octets of DNS's
    // 255-octet wire limit, so an unqualified textual name is at most 253.
    if (value.size() > 253) {
        return false;
    }

    std::size_t label_length = 0;
    bool label_ends_with_alnum = false;
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        const bool alnum = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9');
        if (c == '.') {
            if (label_length == 0 || !label_ends_with_alnum) {
                return false;
            }
            label_length = 0;
            label_ends_with_alnum = false;
            continue;
        }
        if ((!alnum && c != '-') || label_length == 63 || (label_length == 0 && !alnum)) {
            return false;
        }
        ++label_length;
        label_ends_with_alnum = alnum;
    }
    return label_length != 0 && label_ends_with_alnum;
}

[[nodiscard]] inline std::string_view cookie_priority_token(cookie_priority priority) noexcept {
    switch (priority) {
        case cookie_priority::low:
            return "Low";
        case cookie_priority::medium:
            return "Medium";
        case cookie_priority::high:
            return "High";
    }
    return {};
}

[[nodiscard]] inline std::string_view cookie_same_site_token(cookie_same_site same_site) noexcept {
    switch (same_site) {
        case cookie_same_site::strict:
            return "Strict";
        case cookie_same_site::lax:
            return "Lax";
        case cookie_same_site::none:
            return "None";
    }
    return {};
}

[[nodiscard]] inline std::string_view cookie_prefix_text(cookie_prefix prefix) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return "__Secure-";
        case cookie_prefix::host:
            return "__Host-";
    }
    return {};
}

[[nodiscard]] inline std::optional<cookie_prefix> cookie_name_prefix(std::string_view name) noexcept {
    if (cookie_name_starts_with_ignore_case(name, "__Host-")) {
        return cookie_prefix::host;
    }
    if (cookie_name_starts_with_ignore_case(name, "__Secure-")) {
        return cookie_prefix::secure;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool cookie_prefix_requirements_met(
    cookie_prefix prefix, bool secure, std::string_view path, std::string_view domain) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return secure;
        case cookie_prefix::host:
            return secure && path == "/" && domain.empty();
    }
    return false;
}

[[nodiscard]] inline bool cookie_attribute_emitted(cookie_attribute_policy policy) {
    switch (policy) {
        case cookie_attribute_policy::omit:
            return false;
        case cookie_attribute_policy::emit:
            return true;
    }
    throw std::invalid_argument("invalid cookie attribute policy");
}

inline void validate_cookie(
    std::string_view name, std::string_view value, const cookie_options& options) {
    const auto http_only = cookie_attribute_emitted(options.http_only_);
    const auto secure = cookie_attribute_emitted(options.secure_);
    const auto partitioned = cookie_attribute_emitted(options.partitioned_);
    (void)http_only;
    if (!is_valid_http_header_name(name)) {
        throw std::invalid_argument("invalid cookie name");
    }
    if (!is_valid_cookie_value(value)) {
        throw std::invalid_argument("invalid cookie value");
    }
    const auto prefix_size = options.prefix_ ? cookie_prefix_text(*options.prefix_).size() : 0;
    if (prefix_size + name.size() + value.size() > max_cookie_name_value_bytes) {
        throw std::invalid_argument("cookie name and value exceed 4096 bytes");
    }
    // Receivers trim OWS around Path, so edge spaces would not round-trip and
    // trailing SP would end the Set-Cookie field value in whitespace.
    const std::string_view path = options.path_;
    if (!is_valid_cookie_attribute(path) || path.size() > max_cookie_attribute_value_bytes ||
        http_trim_ows(path).size() != path.size()) {
        throw std::invalid_argument("invalid cookie attribute");
    }
    if (!is_valid_cookie_domain(options.domain_)) {
        throw std::invalid_argument("invalid cookie domain");
    }
    if (options.max_age_.has_value()) {
        if (options.max_age_->count() < 0) {
            throw std::invalid_argument("cookie Max-Age must not be negative");
        }
        if (options.max_age_->count() > max_cookie_age_seconds) {
            throw std::invalid_argument("cookie Max-Age must not exceed 400 days");
        }
    }
    if (options.expires_.has_value()) {
        using namespace std::chrono;
        constexpr auto minimum = duration_cast<seconds>(sys_days{year{1601} / January / 1}.time_since_epoch());
        const auto expires = floor<seconds>(options.expires_->time_since_epoch());
        if (expires < minimum) {
            throw std::invalid_argument("cookie Expires must not precede year 1601");
        }
        // Compare at the wire's second resolution, without adding a large
        // offset to the native clock duration near its representable limit.
        const auto now = floor<seconds>(system_clock::now().time_since_epoch());
        if (expires > now && expires - now > seconds(max_cookie_age_seconds)) {
            throw std::invalid_argument("cookie Expires must not exceed 400 days ahead");
        }
    }
    if (options.priority_ && cookie_priority_token(*options.priority_).empty()) {
        throw std::invalid_argument("invalid cookie Priority");
    }
    if (options.same_site_ && cookie_same_site_token(*options.same_site_).empty()) {
        throw std::invalid_argument("invalid cookie SameSite");
    }
    if (options.same_site_ == cookie_same_site::none && !secure) {
        throw std::invalid_argument("SameSite=None cookie requires Secure");
    }
    if (options.prefix_ && cookie_prefix_text(*options.prefix_).empty()) {
        throw std::invalid_argument("invalid cookie prefix");
    }
    if (partitioned && !secure) {
        throw std::invalid_argument("partitioned cookie requires Secure");
    }
    // User agents apply __Host-/__Secure- constraints case-insensitively. Mirror
    // that receive-side rule for literal wire names so every cookie emitted by
    // this sender can actually be stored. A typed prefix is canonical and is the
    // outermost wire prefix, so it takes precedence over bytes in `name`.
    const auto prefix = options.prefix_ ? options.prefix_ : detail::cookie_name_prefix(name);
    if (prefix && !detail::cookie_prefix_requirements_met(*prefix, secure, options.path_, options.domain_)) {
        throw std::invalid_argument(*prefix == cookie_prefix::host
                                        ? "__Host- cookie requires Secure, Path=/, and no Domain"
                                        : "__Secure- cookie requires Secure");
    }
}

}  // namespace ruvia::detail
