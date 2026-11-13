#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/ip_address.h"

namespace ruvia {

// Outbound Content-Length in 0..UINT64_MAX. Checked before narrowing so
// negative inputs cannot wrap into huge finite lengths.
class http_content_length final {
public:
    constexpr http_content_length() noexcept = default;

    template <std::integral integer_type>
    constexpr http_content_length(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint64_t>(value)
                     : throw std::invalid_argument("HTTP Content-Length must be in 0..18446744073709551615")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::in_range<std::uint64_t>(+value);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_content_length> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return http_content_length(static_cast<std::uint64_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(http_content_length, http_content_length) = default;

private:
    struct validated_tag_type {};

    constexpr http_content_length(std::uint64_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint64_t value_{0};
};

static_assert(std::is_trivially_copyable_v<http_content_length>);
static_assert(sizeof(http_content_length) == sizeof(std::uint64_t));

// RFC 9110 sender requirements when a request carries content. Explicit
// zero-length framing does not create content; recipients frame it separately.
enum class http_request_content_semantics : std::uint8_t {
    no_additional_requirements,
    forbidden,
    content_type_required,
};

[[nodiscard]] constexpr http_request_content_semantics classify_http_request_content_semantics(
    std::string_view method) noexcept {
    if (method == "CONNECT" || method == "TRACE") {
        return http_request_content_semantics::forbidden;
    }
    if (method == "OPTIONS") {
        return http_request_content_semantics::content_type_required;
    }
    return http_request_content_semantics::no_additional_requirements;
}

// Whether a request method should explicitly send Content-Length: 0 when
// forwarded or generated with no body (false for GET, HEAD, DELETE, OPTIONS,
// TRACE, CONNECT; true for POST, PUT, PATCH and extension methods).
[[nodiscard]] constexpr bool http_method_sends_zero_content_length(
    std::string_view method) noexcept {
    if (method.empty() || method == "GET" || method == "HEAD" || method == "DELETE" ||
        method == "OPTIONS" || method == "TRACE" || method == "CONNECT") {
        return false;
    }
    return true;
}

}  // namespace ruvia

namespace ruvia {

// One outbound content contract for an HTTP/2 or HTTP/3 request head: it
// determines both the generated Content-Length and whether the head ends the
// request stream. Public callers and both protocol engines use this value.
class http_request_content_plan;

class http_request_without_content final {
private:
    friend class http_request_content_plan;
    constexpr http_request_without_content() noexcept = default;
};

class http_known_length_request_content final {
public:
    [[nodiscard]] constexpr http_content_length length() const noexcept {
        return length_;
    }

private:
    friend class http_request_content_plan;
    explicit constexpr http_known_length_request_content(http_content_length length) noexcept
        : length_(length) {}
    http_content_length length_;
};

class http_streaming_request_content final {
public:
    [[nodiscard]] constexpr std::optional<http_content_length> expected_length() const noexcept {
        return length_;
    }

private:
    friend class http_request_content_plan;
    explicit constexpr http_streaming_request_content(std::optional<http_content_length> length) noexcept
        : length_(length) {}
    std::optional<http_content_length> length_{};
};

class http_request_content_plan final {
public:
    [[nodiscard]] static constexpr http_request_content_plan none() noexcept {
        return http_request_content_plan(http_request_without_content());
    }
    // Zero describes no content: it does not require OPTIONS Content-Type and
    // cannot use Expect: 100-continue. TRACE omits the zero Content-Length.
    // The content is submitted after the head, so the head never ends the
    // stream unless the length is zero.
    [[nodiscard]] static constexpr http_request_content_plan known_length(http_content_length length) noexcept {
        return http_request_content_plan(http_known_length_request_content(length));
    }
    // A known zero keeps the send half open until the request is finished; it
    // cannot send content or wait for 100 Continue.
    [[nodiscard]] static constexpr http_request_content_plan streaming(
        std::optional<http_content_length> length = {}) noexcept {
        return http_request_content_plan(http_streaming_request_content(length));
    }
    [[nodiscard]] constexpr const http_request_without_content* without_content() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_request_without_content>(&value_);
    }
    const http_request_without_content* without_content() const&& = delete;
    [[nodiscard]] constexpr const http_known_length_request_content* known_length_content() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_known_length_request_content>(&value_);
    }
    const http_known_length_request_content* known_length_content() const&& = delete;
    [[nodiscard]] constexpr const http_streaming_request_content* streaming_content() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_streaming_request_content>(&value_);
    }
    const http_streaming_request_content* streaming_content() const&& = delete;
    // The Content-Length the head declares, if any.
    [[nodiscard]] constexpr std::optional<http_content_length> declared_length() const noexcept {
        if (const auto* known = std::get_if<http_known_length_request_content>(&value_)) {
            return known->length();
        }
        if (const auto* streaming = std::get_if<http_streaming_request_content>(&value_)) {
            return streaming->expected_length();
        }
        return std::nullopt;
    }

private:
    using value_type = std::variant<http_request_without_content, http_known_length_request_content,
        http_streaming_request_content>;
    explicit constexpr http_request_content_plan(http_request_without_content value) noexcept
        : value_(value) {}
    explicit constexpr http_request_content_plan(http_known_length_request_content value) noexcept
        : value_(value) {}
    explicit constexpr http_request_content_plan(http_streaming_request_content value) noexcept
        : value_(value) {}
    value_type value_;
};

}  // namespace ruvia

namespace ruvia {

// Request fields the HTTP protocol layer generates or interprets itself when it
// frames and transports a request: Host (RFC 9110 section 7.2), message framing
// (Content-Length, Transfer-Encoding, Trailer; RFC 9110 sections 6.5 and 8.6),
// Expect (RFC 9110 section 10.1.1), and connection management (Connection and
// the connection-specific Keep-Alive, Proxy-Connection, TE and Upgrade; RFC
// 9110 section 7.6.1). A component that builds a request on an application's
// behalf owns these fields and must not accept them from the application.
// Field names are matched case-insensitively.
[[nodiscard]] bool is_http_protocol_managed_request_field(std::string_view name) noexcept;

// 校验应用提供的单个出站请求首部，不分配内存。字段名须为 token，
// 字段值须符合 RFC 9110 §5.5 的规范化语法（不接受首尾 SP/HTAB），
// 并通过请求 writer 的已知字段发送方语义校验；未知合法扩展允许空值。
// 拒绝上面的协议托管字段，名称按大小写不敏感匹配；因此无需 HTTP 版本参数。
// 这不是响应首部或接收方校验器。调用方负责字段间重复及完整消息约束；
// 返回 true 不代表整个请求可发送，不保留输入或跨调用状态。
[[nodiscard]] bool is_valid_http_request_application_field(std::string_view name, std::string_view value) noexcept;

}  // namespace ruvia

namespace ruvia {

class http_response_headers;

// Bytes a field costs beyond its name and value: the ": " separator and the
// CRLF of its normalized HTTP/1 field line "name: value\r\n".
inline constexpr std::size_t http_field_line_overhead = 4;

// The byte and count rule for an HTTP field section, for headers and trailers,
// requests and responses, received and generated: the section must fit
// max_http_header_bytes, each field costing name.size() + value.size() +
// http_field_line_overhead (independent of received optional whitespace), and
// hold at most max_http_header_fields fields.
// HTTP/2 header lists are the deliberate exception: they are charged by the
// RFC 9113 section 6.5.2 SETTINGS_MAX_HEADER_LIST_SIZE accounting (name and
// value plus 32 bytes per field, pseudo-fields included) against the same limit.
// Fixed head bytes such as a start line or the terminating empty line are
// charged with add(); a trailer section starts from
// http_trailer_section_budget(). A failed charge leaves the budget unchanged.
class http_field_section_budget final {
public:
    [[nodiscard]] constexpr bool add(std::size_t bytes_value) noexcept {
        if (bytes_value > max_http_header_bytes - bytes_) {
            return false;
        }
        bytes_ += bytes_value;
        return true;
    }

    [[nodiscard]] constexpr bool add_field(std::size_t name_bytes, std::size_t value_bytes) noexcept {
        const auto remaining = max_http_header_bytes - bytes_;
        if (fields_ == max_http_header_fields || remaining < http_field_line_overhead ||
            name_bytes > remaining - http_field_line_overhead ||
            value_bytes > remaining - http_field_line_overhead - name_bytes) {
            return false;
        }
        bytes_ += name_bytes + value_bytes + http_field_line_overhead;
        ++fields_;
        return true;
    }

    [[nodiscard]] constexpr bool add_field(std::string_view name, std::string_view value) noexcept {
        return add_field(name.size(), value.size());
    }

    [[nodiscard]] constexpr std::size_t bytes() const noexcept {
        return bytes_;
    }

private:
    std::size_t bytes_{0};
    std::size_t fields_{0};
};

// The budget of a trailer section before its first field. Like a head, the
// section is charged its terminating empty line, so every section a writer
// generates also fits the decoders' limit on the complete encoded section.
[[nodiscard]] constexpr http_field_section_budget http_trailer_section_budget() noexcept {
    http_field_section_budget budget;
    (void)budget.add(std::string_view("\r\n").size());
    return budget;
}

namespace detail {

[[nodiscard]] constexpr std::size_t saturating_add_size(
    std::size_t left, std::size_t right) noexcept {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    return left > maximum - right ? maximum : left + right;
}

template <typename field_span_type>
[[nodiscard]] constexpr std::size_t estimate_http1_fields_bytes_impl(
    const field_span_type& fields, bool include_terminating_crlf) noexcept {
    std::size_t total = include_terminating_crlf ? 2U : 0U;
    for (const auto& field : fields) {
        total = saturating_add_size(total, field.name().size());
        total = saturating_add_size(total, field.value().size());
        total = saturating_add_size(total, http_field_line_overhead);
    }
    return total;
}

}  // namespace detail

[[nodiscard]] constexpr std::size_t estimate_http1_field_section_bytes(
    std::span<const http_header> fields, bool include_terminating_crlf = true) noexcept {
    return detail::estimate_http1_fields_bytes_impl(fields, include_terminating_crlf);
}

[[nodiscard]] constexpr std::size_t estimate_http1_field_section_bytes(
    std::span<const http_header_view> fields, bool include_terminating_crlf = true) noexcept {
    return detail::estimate_http1_fields_bytes_impl(fields, include_terminating_crlf);
}

[[nodiscard]] std::size_t estimate_http1_field_section_bytes(
    const http_response_headers& fields, bool include_terminating_crlf = true) noexcept;

// Equivalent HTTP/1.1 wire byte size of a response status line ("HTTP/1.1 NNN
// <reason>\r\n"), all header field lines, and the terminating CRLF.
[[nodiscard]] constexpr std::size_t estimate_http1_response_head_bytes(
    http_status_code status, std::span<const http_header> headers) noexcept {
    const std::size_t status_line_bytes = 15U + http_reason_phrase(status).size();
    return detail::saturating_add_size(
        status_line_bytes, estimate_http1_field_section_bytes(headers, true));
}

[[nodiscard]] constexpr std::size_t estimate_http1_response_head_bytes(
    http_status_code status, std::span<const http_header_view> headers) noexcept {
    const std::size_t status_line_bytes = 15U + http_reason_phrase(status).size();
    return detail::saturating_add_size(
        status_line_bytes, estimate_http1_field_section_bytes(headers, true));
}

[[nodiscard]] std::size_t estimate_http1_response_head_bytes(
    http_status_code status, const http_response_headers& headers) noexcept;

namespace detail {
// Repeated fields retain their wire order; singular convenience accessors
// select the last matching field without copying its borrowed value.
template <typename field_type>
[[nodiscard]] std::optional<std::string_view> find_last_http_header(
    std::span<const field_type> fields, std::string_view name) noexcept {
    for (auto field = fields.rbegin(); field != fields.rend(); ++field) {
        if (http_ascii_equals_ignore_case(field->name(), name)) {
            return field->value();
        }
    }
    return std::nullopt;
}
}  // namespace detail

}  // namespace ruvia

#pragma once

namespace ruvia {
enum class http_request_trailer_error : unsigned char { invalid_field,
    forbidden_field,
    section_too_large };

// Owns decoded terminal request fields, separately from the initial head.
// Drivers populate this object before reporting successful body completion.
// The section must fit the http_field_section_budget byte and field-count rule,
// starting from http_trailer_section_budget(). Retained field names are
// lowercase whichever append overload or protocol supplied them.
class http_request_trailers final {
public:
    http_request_trailers() noexcept = default;
    http_request_trailers(const http_request_trailers&) = delete;
    http_request_trailers& operator=(const http_request_trailers&) = delete;
    // Appends one field or a span of fields all-or-nothing. A name not permitted
    // in request trailers (RFC 9110 section 6.5.1) reports forbidden_field.
    [[nodiscard]] std::optional<http_request_trailer_error> append(std::string_view name, std::string_view value);
    [[nodiscard]] std::optional<http_request_trailer_error> append(std::span<const http_header_view> fields);
    template <detail::http_temporary_owning_range<http_header_view> fields_type>
    std::optional<http_request_trailer_error> append(fields_type&&) = delete;
    [[nodiscard]] std::optional<http_request_trailer_error> append(std::span<const http_header> fields);
    template <detail::http_temporary_owning_range<http_header> fields_type>
    std::optional<http_request_trailer_error> append(fields_type&&) = delete;
    // Appends a received trailer section all-or-nothing, as decoded by any
    // protocol version (http1_chunked_body_decoder, HTTP/2 or HTTP/3 message
    // end events). Every field must be a valid regular field with a lowercase
    // name; a pseudo-header field makes the section malformed (RFC 9113
    // section 8.1, RFC 9114 section 4.3) and reports invalid_field. Every
    // received field is charged to the byte budget; fields not permitted in
    // request trailers are discarded rather than retained (RFC 9110 section
    // 6.5.1, RFC 9112 section 7.1.2).
    [[nodiscard]] std::optional<http_request_trailer_error> append_received(std::span<const http_header> fields);
    template <detail::http_temporary_owning_range<http_header> fields_type>
    std::optional<http_request_trailer_error> append_received(fields_type&&) = delete;
    [[nodiscard]] std::vector<http_header> take_fields() && noexcept {
        budget_ = http_trailer_section_budget();
        return std::move(fields_);
    }
    [[nodiscard]] std::span<const http_header> fields() const& noexcept RUVIA_LIFETIMEBOUND {
        return fields_;
    }
    std::span<const http_header> fields() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> field(std::string_view) const&& = delete;

private:
    std::vector<http_header> fields_;
    http_field_section_budget budget_{http_trailer_section_budget()};
};
}  // namespace ruvia

namespace ruvia::detail {

enum class http_field_kind : std::uint8_t;

// Shared request/response restrictions (RFC 9110 section 6.5.1): fields that
// control framing, routing, authentication or representation interpretation.
// Direction-specific permissions are composed below and by response policy.
[[nodiscard]] bool is_forbidden_common_trailer_name(
    std::string_view name, http_field_kind kind) noexcept;

// Trailer advertisements and actual request sections use the same policy.
// Accept-Ranges is permitted in response trailers, not request trailers.
[[nodiscard]] bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept;

template <typename forbidden_name_type>
[[nodiscard]] inline bool is_valid_http_trailer_field_value(
    std::string_view value, http_field_list_role role, forbidden_name_type&& forbidden_name) noexcept {
    // RFC 9110 section 5.6.1.1: senders must not generate empty list elements,
    // but `#field-name` itself can represent an empty list.
    const auto list_role = http_trim_ows(value).empty() ? http_field_list_role::recipient : role;
    return http_visit_token_list(value, list_role, [&forbidden_name](std::string_view name) noexcept {
        return !forbidden_name(name);
    }) == http_field_list_parse_status::ok;
}

[[nodiscard]] inline bool is_valid_http_request_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_http_request_trailer_name(name); });
}

// The single rule for one generated request trailer field: valid field syntax
// (RFC 9110 section 5), a name a sender may place in request trailers (RFC 9110
// section 6.5.1), and a charge to the section `budget`, which is unchanged on
// failure.
[[nodiscard]] inline std::optional<http_request_trailer_error> charge_http_request_trailer_field(
    http_field_section_budget& budget, std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return http_request_trailer_error::invalid_field;
    }
    if (is_forbidden_http_request_trailer_name(name)) {
        return http_request_trailer_error::forbidden_field;
    }
    if (!budget.add_field(name, value)) {
        return http_request_trailer_error::section_too_large;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail

namespace ruvia {

// Whether the framing/lifecycle owner has established that request content will
// follow the initial head. Keep this typed: HTTP/1 derives it from its body plan,
// while HTTP/2 combines its receive-half and remaining-content states so an open
// metadata-only or known-empty stream cannot masquerade as pending content.
enum class http_request_content_indication : std::uint8_t { no_content,
    will_follow };

// RFC 9110 Section 10.1.1 forbids a client from generating 100-continue when
// the request has no content. Keep this sender check next to the recipient-side
// expectation state so HTTP/1 and HTTP/2 cannot derive different answers.
[[nodiscard]] constexpr bool http_client_expectation_is_valid(
    bool has_continue, http_request_content_indication content) noexcept {
    return !has_continue || content == http_request_content_indication::will_follow;
}

// Whether the product accepts unknown expectation extensions. Expect remains
// valid syntax either way; the HTTP contract owns the protocol response chosen
// by the explicit rejection policy.
enum class http_unsupported_expectation_policy : std::uint8_t { ignore,
    reject };

enum class http_server_expectation_plan : std::uint8_t {
    no_action,
    send_continue,
    reject_unsupported,
};

[[nodiscard]] inline http_protocol_error http_unsupported_expectation_error() noexcept {
    return http_protocol_error(
        http_status::expectation_failed, "unsupported_expectation", "unsupported Expect header");
}

// Incremental recipient-side state for the RFC 9110 Expect #list. Repeated field
// lines extend the same logical list, empty members are ignored, and all state is
// fixed-size. The only standardized member is 100-continue; every other non-empty
// member is retained as the single semantic fact "unsupported" for product policy.
class http_request_expectations final {
public:
    void parse_field(std::string_view value) noexcept;

    [[nodiscard]] bool has_continue() const noexcept {
        return (flags_ & continue_value) != 0;
    }

    [[nodiscard]] bool has_unsupported() const noexcept {
        return (flags_ & unsupported) != 0;
    }

    // RFC 9110 requires an HTTP/1.0 recipient to ignore 100-continue. Preserve
    // the independent unsupported-member fact so the Web product can still apply
    // its chosen extension-support policy.
    void ignore_continue() noexcept {
        flags_ &= static_cast<std::uint8_t>(~continue_value);
    }

    [[nodiscard]] http_server_expectation_plan server_plan(http_request_content_indication content,
        http_unsupported_expectation_policy unsupported_policy) const noexcept {
        if (has_unsupported() && unsupported_policy == http_unsupported_expectation_policy::reject) {
            return http_server_expectation_plan::reject_unsupported;
        }
        if (has_continue() && content == http_request_content_indication::will_follow) {
            return http_server_expectation_plan::send_continue;
        }
        return http_server_expectation_plan::no_action;
    }

    // RFC 9110 section 7.8: a server that receives both Upgrade and an Expect
    // field with 100-continue MUST send 100 (Continue) before 101 (Switching
    // Protocols), even when no content follows the head. Unsupported members
    // are planned by server_plan() before the upgrade is accepted.
    [[nodiscard]] bool continue_before_switching_protocols() const noexcept {
        return has_continue();
    }

private:
    static constexpr std::uint8_t continue_value = 1U << 0;
    static constexpr std::uint8_t unsupported = 1U << 1;

    std::uint8_t flags_{0};
};

static_assert(std::is_trivially_copyable_v<http_request_expectations>);
static_assert(sizeof(http_request_expectations) <= 1);
static_assert(std::is_trivially_copyable_v<http_server_expectation_plan>);
static_assert(sizeof(http_server_expectation_plan) <= 2);

}  // namespace ruvia

namespace ruvia {

class http_request;

// Validates an HTTP origin-form request target without allocating. The same
// protocol rule applies to server routes and outbound HTTP/websocket clients.
[[nodiscard]] bool is_valid_http_origin_form_target(std::string_view target) noexcept;

// RFC 9110 CONNECT authority-form requires a nonempty host and explicit port.
[[nodiscard]] bool is_valid_http_connect_authority(std::string_view authority) noexcept;

// Numeric IP host syntax without URI brackets.
[[nodiscard]] bool is_valid_http_ipv4_literal(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_http_ipv6_literal(std::string_view value) noexcept;

namespace detail {
struct http_authority_view_access;
}

enum class http_authority_port_kind : std::uint8_t {
    absent,
    empty,
    value,
};

// A validated, borrowed RFC 3986 authority without userinfo; IP-literal hosts
// keep their brackets. An explicit empty port stays distinct from an absent one
// for syntax-preserving parsing; both map to the scheme default when an HTTP
// origin is compared.
class http_authority_view final {
public:
    [[nodiscard]] constexpr std::string_view host() const noexcept {
        return host_;
    }

    [[nodiscard]] constexpr http_authority_port_kind port_kind() const noexcept {
        return port_kind_;
    }

    [[nodiscard]] constexpr std::optional<std::uint16_t> port() const noexcept {
        return port_kind_ == http_authority_port_kind::value ? std::optional<std::uint16_t>(port_)
                                                             : std::nullopt;
    }

    // Returns the explicit port or caller fallback. This numeric observation
    // alone does not preserve the absent/empty versus explicit-zero distinction.
    [[nodiscard]] constexpr std::uint16_t effective_port(port_number default_port) const noexcept {
        return port_kind_ == http_authority_port_kind::value ? port_ : default_port.value();
    }

private:
    friend struct detail::http_authority_view_access;

    constexpr http_authority_view(
        std::string_view host, http_authority_port_kind port_kind, std::uint16_t port) noexcept
        : host_(host),
          port_(port),
          port_kind_(port_kind) {}

    std::string_view host_;
    std::uint16_t port_{0};
    http_authority_port_kind port_kind_{http_authority_port_kind::absent};
};

[[nodiscard]] std::optional<http_authority_view> parse_http_authority(borrowed_text value) noexcept;

// Parses a Host-field authority without userinfo and borrows its host text.
// The host retains brackets around an IP literal. An invalid authority has no
// value; a syntactically valid empty host remains an engaged empty view.
[[nodiscard]] std::optional<std::string_view> parse_http_authority_host(borrowed_text value) noexcept;

// Compares HTTP authorities using case-insensitive host syntax and the supplied
// scheme default port, checked before narrowing like other port inputs.
// Zero means no known scheme default: absent/empty ports then compare equal
// only to absent/empty ports, never to an explicitly supplied zero.
// Invalid authorities never compare equal.
[[nodiscard]] bool http_authorities_equal(borrowed_text left, borrowed_text right, port_number default_port) noexcept;

// Validates a WHATWG serialized origin, not a general URI or origin list.
[[nodiscard]] bool is_valid_http_serialized_origin(std::string_view value) noexcept;

// Normalizes an incoming request target for proxy forwarding: converts
// absolute-form targets to origin-form (with "/" when the path is empty and
// preserving any query), converts an OPTIONS target with an empty path and no
// query to "*", and preserves all other forms verbatim.
[[nodiscard]] std::string normalize_http_proxy_request_target(
    std::string_view method, std::string_view target);
[[nodiscard]] std::string normalize_http_proxy_request_target(
    http_known_method method, std::string_view target);
[[nodiscard]] std::string normalize_http_proxy_request_target(
    const http_request& request);

enum class http_max_forwards_status : std::uint8_t {
    absent,
    valid,
    invalid,
};

// Parsed Max-Forwards hop count (RFC 9110 section 7.6.2).
class http_max_forwards_result final {
public:
    [[nodiscard]] static constexpr http_max_forwards_result absent() noexcept {
        return http_max_forwards_result(http_max_forwards_status::absent, 0);
    }
    [[nodiscard]] static constexpr http_max_forwards_result invalid() noexcept {
        return http_max_forwards_result(http_max_forwards_status::invalid, 0);
    }
    [[nodiscard]] static constexpr http_max_forwards_result valid(std::uint64_t current) noexcept {
        return http_max_forwards_result(http_max_forwards_status::valid, current);
    }

    [[nodiscard]] constexpr http_max_forwards_status status() const noexcept {
        return status_;
    }
    [[nodiscard]] constexpr bool is_absent() const noexcept {
        return status_ == http_max_forwards_status::absent;
    }
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return status_ == http_max_forwards_status::valid;
    }
    [[nodiscard]] constexpr bool is_invalid() const noexcept {
        return status_ == http_max_forwards_status::invalid;
    }
    [[nodiscard]] constexpr std::optional<std::uint64_t> current() const noexcept {
        return is_valid() ? std::optional<std::uint64_t>(current_) : std::nullopt;
    }
    [[nodiscard]] constexpr bool exhausted() const noexcept {
        return is_valid() && current_ == 0;
    }
    [[nodiscard]] constexpr std::optional<std::uint64_t> decremented() const noexcept {
        return (is_valid() && current_ > 0) ? std::optional<std::uint64_t>(current_ - 1)
                                            : std::nullopt;
    }
    [[nodiscard]] std::string format_decremented() const;

private:
    constexpr http_max_forwards_result(
        http_max_forwards_status status, std::uint64_t current) noexcept
        : current_(current),
          status_(status) {}

    std::uint64_t current_{0};
    http_max_forwards_status status_{http_max_forwards_status::absent};
};

[[nodiscard]] std::optional<std::uint64_t> parse_http_max_forwards_value(
    std::string_view field_value,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    std::string_view field_value,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    std::span<const http_header> headers,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    std::span<const http_header_view> headers,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    const http_request& request,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
}  // namespace ruvia

namespace ruvia {

class http_request;

// The connection facts RFC 9110 §7.4 weighs against a request's target URI.
// The server runtime supplies them; the request alone cannot know them.
struct http_target_origin_connection final {
    // The request arrived over a TLS-secured connection.
    bool secured_{false};
    // The peer is a trusted gateway (a configured reverse proxy). It, not this
    // connection, vouches for the target's scheme and authority, so §7.4
    // exempts the request from the connection checks.
    bool trusted_gateway_{false};
};

enum class http_target_origin_action : std::uint8_t {
    // Nothing on this connection must be verified before processing.
    accept,
    // An "https" target on a secured connection: process it only when the
    // certificate this connection presented is valid for host().
    verify_certificate,
    // The connection cannot carry this target: answer 421 (Misdirected
    // Request, RFC 9110 §15.5.20).
    misdirected,
};

enum class http_target_host_kind : std::uint8_t {
    // A reg-name, without the trailing dot of an absolute domain name.
    dns_name,
    ipv4,
    ipv6,
};

// What an origin server must establish about one request's target origin.
// Meaningful host data only for verify_certificate.
class http_target_origin final {
public:
    static constexpr std::size_t max_dns_name_size = 253;

    [[nodiscard]] constexpr http_target_origin_action action() const noexcept {
        return action_;
    }

    [[nodiscard]] constexpr http_target_host_kind host_kind() const noexcept {
        return host_kind_;
    }

    // dns_name: the host text (percent-encoded unreserved octets decoded, a
    // single trailing dot stripped, letter case preserved). ipv4/ipv6: empty.
    [[nodiscard]] constexpr std::string_view host() const& noexcept RUVIA_LIFETIMEBOUND {
        return decoded_host_size_ != 0 ? std::string_view(decoded_host_.data(), decoded_host_size_) : host_;
    }
    std::string_view host() const&& = delete;

    // ipv4/ipv6: the address in network byte order (4 or 16 bytes).
    // dns_name: empty.
    [[nodiscard]] std::span<const std::uint8_t> address() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::span(address_.data(), address_size_);
    }
    std::span<const std::uint8_t> address() const&& = delete;

    [[nodiscard]] static constexpr http_target_origin accepted() noexcept {
        return http_target_origin(http_target_origin_action::accept);
    }

    [[nodiscard]] static constexpr http_target_origin misdirected() noexcept {
        return http_target_origin(http_target_origin_action::misdirected);
    }

    [[nodiscard]] static constexpr http_target_origin dns_name(std::string_view host) noexcept {
        if (host.empty() || host.size() > max_dns_name_size) {
            return misdirected();
        }
        http_target_origin origin(http_target_origin_action::verify_certificate);
        origin.host_ = host;
        return origin;
    }

    template <detail::http_temporary_owning_char_string host_type>
    static void dns_name(host_type&&) = delete;

    [[nodiscard]] static constexpr http_target_origin decoded_dns_name(std::string_view host) noexcept {
        if (host.empty() || host.size() > max_dns_name_size) {
            return misdirected();
        }
        http_target_origin origin(http_target_origin_action::verify_certificate);
        origin.decoded_host_size_ = static_cast<std::uint8_t>(host.size());
        for (std::size_t i = 0; i < host.size(); ++i) {
            origin.decoded_host_[i] = host[i];
        }
        return origin;
    }

    [[nodiscard]] static constexpr http_target_origin ip_address(
        http_target_host_kind kind, std::span<const std::uint8_t> address) noexcept {
        if ((kind == http_target_host_kind::ipv4 && address.size() != 4) ||
            (kind == http_target_host_kind::ipv6 && address.size() != 16) ||
            (kind != http_target_host_kind::ipv4 && kind != http_target_host_kind::ipv6)) {
            return misdirected();
        }
        http_target_origin origin(http_target_origin_action::verify_certificate);
        origin.host_kind_ = kind;
        origin.address_size_ = static_cast<std::uint8_t>(address.size());
        for (std::size_t i = 0; i < address.size(); ++i) {
            origin.address_[i] = address[i];
        }
        return origin;
    }

private:
    explicit constexpr http_target_origin(http_target_origin_action action) noexcept
        : action_(action) {
        if (std::is_constant_evaluated()) {
            decoded_host_.fill('\0');
            address_.fill(0);
        }
    }

    std::string_view host_;
    std::array<char, max_dns_name_size> decoded_host_;
    std::array<std::uint8_t, 16> address_;
    std::uint8_t decoded_host_size_{0};
    std::uint8_t address_size_{0};
    http_target_origin_action action_;
    http_target_host_kind host_kind_{http_target_host_kind::dns_name};
};

// RFC 9110 §7.4: unless the connection is from a trusted gateway, an origin
// server MUST reject a request whose target URI's scheme-specific requirements
// the connection does not meet; an "https" resource only over a connection
// secured by a certificate valid for the target's origin. Decides, for a
// parsed request of any HTTP version:
// - trusted gateway: accept;
// - an "https" target (absolute-form scheme or HTTP/2 :scheme, compared
//   case-insensitively) on a cleartext connection: misdirected;
// - a secured connection whose target scheme is "https" -- explicit, or
//   implied for origin-form and asterisk-form (RFC 9112 §3.3): verify the
//   target host (:authority, else Host) against the certificate. An
//   IPvFuture host, which no certificate names, is misdirected;
// - everything else accepts: "http" targets on either transport (RFC 9110
//   imposes no certificate requirement on them), CONNECT tunnel targets
//   (authority-form, or HTTP/2 CONNECT without :scheme), and targets with an
//   empty authority, which RFC 9112 §3.3 completes with the server's
//   configured default rather than a client-chosen name.
// The port is not compared: certificates name hosts, not ports. Allocation free.
[[nodiscard]] http_target_origin decide_http_target_origin(
    const http_request& request, http_target_origin_connection connection) noexcept;

}  // namespace ruvia
