#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"

namespace ruvia {

class http_origin_view;
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

// One Cookie request cookie-pair, "name=value". The '=' is always written,
// including for an empty name, so a nameless value that itself looks like a
// cookie-pair stays behind '='. Callers join pairs with "; " (RFC 6265
// section 4.2.1).
[[nodiscard]] constexpr std::size_t cookie_request_pair_size(std::string_view name, std::string_view value) noexcept {
    return name.size() + 1U + value.size();
}

// Writes cookie_request_pair_size(name, value) bytes at out and returns the
// end of the written pair. Only inputs overlapping the written bytes need a
// snapshot: short snapshots use the stack, otherwise heap storage.
// Allocation failure leaves the output untouched.
char* write_cookie_request_pair(char* out, std::string_view name, std::string_view value);

// Cookie request pairs always include '=', including for an empty name.
// Inputs may borrow header_value, including its terminating NUL.
// Failure preserves the original contents; capacity may grow.
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
        if (input.empty()) {
            return std::nullopt;
        }
        const auto first = reinterpret_cast<std::uintptr_t>(header_value.data());
        const auto input_address = reinterpret_cast<std::uintptr_t>(input.data());
        if (input_address < first || input_address - first > start) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(input_address - first);
    };
    const auto name_offset = borrowed_offset(name);
    const auto value_offset = borrowed_offset(value);
    // Growing may reallocate; the value-initialized bytes keep a borrowed
    // terminator readable as NUL until the writer consumes it.
    header_value.resize(start + separator_bytes + cookie_request_pair_size(name, value));
    auto* const bytes = header_value.data();
    if (name_offset) {
        name = std::string_view(bytes + *name_offset, name.size());
    }
    if (value_offset) {
        value = std::string_view(bytes + *value_offset, value.size());
    }
    // The pair is written before the separator, which would otherwise
    // overwrite a borrowed terminator first.
    try {
        (void)write_cookie_request_pair(bytes + start + separator_bytes, name, value);
    } catch (...) {
        header_value.resize(start);
        throw;
    }
    if (separator_bytes != 0) {
        bytes[start] = ';';
        bytes[start + 1] = ' ';
    }
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
    bool http_only_{false};
    bool secure_{false};
    bool partitioned_{false};
};

struct http_cookie_jar_options final {
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
};

// Sans-I/O RFC 6265 client cookie jar for a fixed origin. Validates Domain,
// Path, Secure, SameSite=None, Partitioned, and __Secure-/__Host- prefixes,
// caps Max-Age/Expires at 400 days, evicts expired entries, and enforces
// count and byte budgets.
class http_cookie_jar final {
public:
    explicit http_cookie_jar(const http_origin_view& origin, http_cookie_jar_options options = {});
    http_cookie_jar(http_scheme scheme, std::string_view host, http_cookie_jar_options options = {});

    void add_configured_cookie(std::string_view name, std::string_view value);

    bool absorb_set_cookie(std::string_view request_target, std::string_view set_cookie_value,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    void retain_response_cookies(std::string_view request_target,
        std::span<const http_header> headers,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
    void retain_response_cookies(std::string_view request_target,
        std::span<const http_header_view> headers,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    [[nodiscard]] std::string cookie_header(std::string_view request_target,
        bool include_received = true,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    template <typename fn_type>
    void visit_matching_cookies(std::string_view request_target, bool include_received,
        std::chrono::system_clock::time_point now, fn_type&& fn) {
        if (cookies_.empty()) {
            return;
        }
        if (next_expiry_.has_value() && now >= *next_expiry_) {
            discard_expired(now);
        }
        if (cookies_.empty()) {
            return;
        }
        const auto path = request_path_only(request_target);
        for (const auto& cookie : cookies_) {
            if (!cookie.configured_ && !include_received) {
                continue;
            }
            if (cookie.secure_ && scheme_ != http_scheme::https) {
                continue;
            }
            if (!cookie_path_matches(path, cookie.path_)) {
                continue;
            }
            fn(std::string_view(cookie.name_), std::string_view(cookie.value_));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return cookies_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return cookies_.empty();
    }
    [[nodiscard]] std::size_t storage_bytes() const noexcept {
        return cookie_bytes_;
    }
    void clear() noexcept;

    [[nodiscard]] static bool cookie_domain_matches(
        std::string_view host, std::string_view domain) noexcept;
    [[nodiscard]] static bool cookie_path_matches(
        std::string_view request_path, std::string_view cookie_path) noexcept;
    [[nodiscard]] static std::string_view request_path_only(std::string_view target) noexcept;
    [[nodiscard]] static std::string_view default_cookie_path(std::string_view target) noexcept;

private:
    struct stored_cookie final {
        stored_cookie(std::string_view name, std::string_view value)
            : stored_cookie(name, value, "/", {}) {}
        stored_cookie(std::string_view name, std::string_view value,
            std::string_view path, std::string_view domain)
            : name_(name),
              value_(value),
              path_(path),
              domain_(domain) {}

        std::string name_;
        std::string value_;
        std::string path_;
        std::string domain_;
        std::optional<std::chrono::system_clock::time_point> expires_;
        bool secure_{false};
        bool partitioned_{false};
        bool configured_{true};
    };

    struct received_cookie final {
        std::string_view name_;
        std::string_view value_;
        std::string_view path_;
        std::string_view domain_;
        std::optional<std::chrono::system_clock::time_point> expires_;
        bool secure_{false};
        bool partitioned_{false};
        bool expired_{false};
    };

    [[nodiscard]] std::optional<received_cookie> accept_set_cookie_view(
        std::string_view request_target, std::string_view set_cookie_value,
        std::chrono::system_clock::time_point now) const;
    bool store_received_cookie(const received_cookie& cookie);
    void reserve_cookie_slot();
    void discard_expired(std::chrono::system_clock::time_point now);
    [[nodiscard]] static std::size_t entry_storage_bytes(std::string_view name,
        std::string_view value, std::string_view path, std::string_view domain) noexcept;
    [[nodiscard]] bool has_capacity(
        std::size_t replaced_bytes, std::size_t replacement_bytes, bool adding) const noexcept;
    void recompute_next_expiry() noexcept;

    std::string host_;
    http_scheme scheme_{http_scheme::http};
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
    std::vector<stored_cookie> cookies_;
    std::optional<std::chrono::system_clock::time_point> next_expiry_{};
    std::size_t cookie_bytes_{0};
};

}  // namespace ruvia

namespace ruvia {

enum class http_set_cookie_attribute : std::uint8_t {
    secure = 1U << 0,
    partitioned = 1U << 1,
    http_only = 1U << 2,
};

// Borrowed, allocation-free Set-Cookie fields for outbound client runtimes,
// following the RFC 6265 section 5.2 user-agent algorithm. A cookie without '=',
// with an empty name, over 4096 name+value bytes, or with a control byte other
// than HTAB is rejected. Unknown and oversized attributes are ignored. path() is
// empty when the last Path value is
// empty or does not start with '/', meaning the caller's default-path. An empty
// Domain value is ignored; a leading '.' is removed, so domain() may be empty
// (host-only) and is not validated beyond domain-matching by the caller. Expires follows RFC 6265 cookie-date token grammar, including its
// permitted suffixes; this does not relax the separate HTTP-date grammar.
// Partitioned is reported independently of Secure; callers own origin policy.
class http_set_cookie_view final {
public:
    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] constexpr std::string_view domain() const noexcept {
        return domain_;
    }

    [[nodiscard]] constexpr std::optional<std::time_t> expires() const noexcept {
        return expires_;
    }

    [[nodiscard]] constexpr std::optional<std::int64_t> max_age_seconds() const noexcept {
        return max_age_seconds_;
    }

    [[nodiscard]] constexpr bool has(http_set_cookie_attribute attribute) const noexcept {
        const auto mask = static_cast<std::uint8_t>(attribute);
        return mask != 0U && (mask & (mask - 1U)) == 0U && (attributes_ & mask) != 0U;
    }

    [[nodiscard]] constexpr bool http_only() const noexcept {
        return has(http_set_cookie_attribute::http_only);
    }

    [[nodiscard]] constexpr bool same_site_none() const noexcept {
        return same_site_ == cookie_same_site::none;
    }

    [[nodiscard]] constexpr std::optional<cookie_same_site> same_site() const noexcept {
        return same_site_;
    }

private:
    friend std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept;

    constexpr http_set_cookie_view(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    constexpr void set(http_set_cookie_attribute attribute) noexcept {
        attributes_ |= static_cast<std::uint8_t>(attribute);
    }

    std::string_view name_;
    std::string_view value_;
    std::string_view path_;
    std::string_view domain_;
    std::optional<std::time_t> expires_;
    std::optional<std::int64_t> max_age_seconds_;
    std::optional<cookie_same_site> same_site_{};
    std::uint8_t attributes_{0};
};

[[nodiscard]] std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept;

template <detail::http_temporary_owning_char_string value_type>
std::optional<http_set_cookie_view> parse_set_cookie(value_type&&) = delete;

}  // namespace ruvia
