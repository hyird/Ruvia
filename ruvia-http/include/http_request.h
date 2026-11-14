#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "decimal_text.h"
#include "http_header.h"
#include "http_message.h"
#include "http_protocol.h"
#include "http_status.h"
namespace ruvia {

// Outbound Content-Length in 0..UINT64_MAX. Checked before narrowing so
// negative inputs cannot wrap into huge finite lengths.
class http_content_length final {
public:
    constexpr http_content_length() noexcept = default;

    template <std::integral integer_type>
    constexpr http_content_length(integer_type value) :
        value_(is_valid_value(value) ? static_cast<std::uint64_t>(value) : throw std::invalid_argument("HTTP Content-Length must be in 0..18446744073709551615")) {}

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

    constexpr http_content_length(std::uint64_t value, validated_tag_type) noexcept :
        value_(value) {}

    std::uint64_t value_{0};
};

static_assert(std::is_trivially_copyable_v<http_content_length>);
static_assert(sizeof(http_content_length) == sizeof(std::uint64_t));

// A received Content-Length field value (RFC 9110 section 8.6): a comma list
// of decimal lengths, as left by combining repeated field lines, that must all
// agree. An empty member, a non-digit, overflow or disagreeing lengths yield
// no value. Every protocol version frames messages with this same rule.
[[nodiscard]] std::optional<std::uint64_t> http_parse_content_length(std::string_view field_value) noexcept;

// RFC 9110 sender requirements when a request carries content. Explicit
// zero-length framing does not create content; recipients frame it separately.
enum class http_request_content_semantics : std::uint8_t {
    no_additional_requirements,
    forbidden,
    content_type_required,
};

[[nodiscard]] constexpr http_request_content_semantics classify_http_request_content_semantics(std::string_view method) noexcept {
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
[[nodiscard]] constexpr bool http_method_sends_zero_content_length(std::string_view method) noexcept {
    if (method.empty() || method == "GET" || method == "HEAD" || method == "DELETE" || method == "OPTIONS" || method == "TRACE" || method == "CONNECT") {
        return false;
    }
    return true;
}

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
    explicit constexpr http_known_length_request_content(http_content_length length) noexcept :
        length_(length) {}
    http_content_length length_;
};

class http_streaming_request_content final {
public:
    [[nodiscard]] constexpr std::optional<http_content_length> expected_length() const noexcept {
        return length_;
    }

private:
    friend class http_request_content_plan;
    explicit constexpr http_streaming_request_content(std::optional<http_content_length> length) noexcept :
        length_(length) {}
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
    [[nodiscard]] static constexpr http_request_content_plan streaming(std::optional<http_content_length> length = {}) noexcept {
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
    using value_type = std::variant<http_request_without_content, http_known_length_request_content, http_streaming_request_content>;
    explicit constexpr http_request_content_plan(http_request_without_content value) noexcept :
        value_(value) {}
    explicit constexpr http_request_content_plan(http_known_length_request_content value) noexcept :
        value_(value) {}
    explicit constexpr http_request_content_plan(http_streaming_request_content value) noexcept :
        value_(value) {}
    value_type value_;
};

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
        if (fields_ == max_http_header_fields || remaining < http_field_line_overhead || name_bytes > remaining - http_field_line_overhead || value_bytes > remaining - http_field_line_overhead - name_bytes) {
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

template <typename field_span_type>
[[nodiscard]] constexpr std::size_t estimate_http1_fields_bytes_impl(const field_span_type& fields, bool include_terminating_crlf) noexcept {
    std::size_t total = include_terminating_crlf ? 2U : 0U;
    for (const auto& field : fields) {
        total = saturating_add(total, field.name().size());
        total = saturating_add(total, field.value().size());
        total = saturating_add(total, http_field_line_overhead);
    }
    return total;
}

}  // namespace detail

[[nodiscard]] constexpr std::size_t estimate_http1_field_section_bytes(std::span<const http_header> fields, bool include_terminating_crlf = true) noexcept {
    return detail::estimate_http1_fields_bytes_impl(fields, include_terminating_crlf);
}

[[nodiscard]] constexpr std::size_t estimate_http1_field_section_bytes(std::span<const http_header_view> fields, bool include_terminating_crlf = true) noexcept {
    return detail::estimate_http1_fields_bytes_impl(fields, include_terminating_crlf);
}

[[nodiscard]] std::size_t estimate_http1_field_section_bytes(const http_response_headers& fields, bool include_terminating_crlf = true) noexcept;

// Equivalent HTTP/1.1 wire byte size of a response status line ("HTTP/1.1 NNN
// <reason>\r\n"), all header field lines, and the terminating CRLF.
[[nodiscard]] constexpr std::size_t estimate_http1_response_head_bytes(http_status_code status, std::span<const http_header> headers) noexcept {
    const std::size_t status_line_bytes = 15U + http_reason_phrase(status).size();
    return saturating_add(status_line_bytes, estimate_http1_field_section_bytes(headers, true));
}

[[nodiscard]] constexpr std::size_t estimate_http1_response_head_bytes(http_status_code status, std::span<const http_header_view> headers) noexcept {
    const std::size_t status_line_bytes = 15U + http_reason_phrase(status).size();
    return saturating_add(status_line_bytes, estimate_http1_field_section_bytes(headers, true));
}

[[nodiscard]] std::size_t estimate_http1_response_head_bytes(http_status_code status, const http_response_headers& headers) noexcept;

namespace detail {
// Repeated fields retain their wire order; singular convenience accessors
// select the last matching field without copying its borrowed value.
template <typename field_type>
[[nodiscard]] std::optional<std::string_view> find_last_http_header(std::span<const field_type> fields, std::string_view name) noexcept {
    for (auto field = fields.rbegin(); field != fields.rend(); ++field) {
        if (http_ascii_equals_ignore_case(field->name(), name)) {
            return field->value();
        }
    }
    return std::nullopt;
}
}  // namespace detail

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
[[nodiscard]] bool is_forbidden_common_trailer_name(std::string_view name, http_field_kind kind) noexcept;

// Trailer advertisements and actual request sections use the same policy.
// Accept-Ranges is permitted in response trailers, not request trailers.
[[nodiscard]] bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept;

template <typename forbidden_name_type>
[[nodiscard]] inline bool is_valid_http_trailer_field_value(std::string_view value, http_field_list_role role, forbidden_name_type&& forbidden_name) noexcept {
    // RFC 9110 section 5.6.1.1: senders must not generate empty list elements,
    // but `#field-name` itself can represent an empty list.
    const auto list_role = http_trim_ows(value).empty() ? http_field_list_role::recipient : role;
    return http_visit_token_list(value, list_role, [&forbidden_name](std::string_view name) noexcept {
        return !forbidden_name(name);
    }) == http_field_list_parse_status::ok;
}

[[nodiscard]] inline bool is_valid_http_request_trailer_field_value(std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role, [](std::string_view name) noexcept {
        return is_forbidden_http_request_trailer_name(name);
    });
}

// The single rule for one generated request trailer field: valid field syntax
// (RFC 9110 section 5), a name a sender may place in request trailers (RFC 9110
// section 6.5.1), and a charge to the section `budget`, which is unchanged on
// failure.
[[nodiscard]] inline std::optional<http_request_trailer_error> charge_http_request_trailer_field(http_field_section_budget& budget, std::string_view name, std::string_view value) noexcept {
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
[[nodiscard]] constexpr bool http_client_expectation_is_valid(bool has_continue, http_request_content_indication content) noexcept {
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
    return http_protocol_error(http_status::expectation_failed, "unsupported_expectation", "unsupported Expect header");
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

    [[nodiscard]] http_server_expectation_plan server_plan(http_request_content_indication content, http_unsupported_expectation_policy unsupported_policy) const noexcept {
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

    [[nodiscard]] static constexpr http_target_origin ip_address(http_target_host_kind kind, std::span<const std::uint8_t> address) noexcept {
        if ((kind == http_target_host_kind::ipv4 && address.size() != 4) || (kind == http_target_host_kind::ipv6 && address.size() != 16) || (kind != http_target_host_kind::ipv4 && kind != http_target_host_kind::ipv6)) {
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
    explicit constexpr http_target_origin(http_target_origin_action action) noexcept :
        action_(action) {
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
[[nodiscard]] http_target_origin decide_http_target_origin(const http_request& request, http_target_origin_connection connection) noexcept;

enum class http_request_target_form : std::uint8_t {
    origin,
    absolute,
    authority,
    asterisk,
    // HTTP/2 and HTTP/3 carry target components as pseudo-fields rather
    // than one HTTP/1 request-target token.
    http2,
    http3,
};

namespace detail {

struct http_request_access;

// Owns field descriptors and the parser's http_field_kind for each field.
// Kinds sit in the same allocation so Cookie/Accept scans do not re-classify
// names. Production parsers reserve the final size once after validating the
// head. Moving this block never allocates.
class http_request_header_block final {
public:
    http_request_header_block() noexcept = default;
    http_request_header_block(const http_request_header_block&) = delete;
    http_request_header_block& operator=(const http_request_header_block&) = delete;
    http_request_header_block(http_request_header_block&& other) noexcept :
        fields_(std::exchange(other.fields_, nullptr)),
        kinds_(std::exchange(other.kinds_, nullptr)),
        size_(std::exchange(other.size_, std::uint16_t{0})),
        capacity_(std::exchange(other.capacity_, std::uint16_t{0})),
        repeated_known_bits_(std::exchange(other.repeated_known_bits_, std::uint32_t{0})) {}
    http_request_header_block& operator=(http_request_header_block&& other) noexcept {
        if (this != &other) {
            release();
            fields_ = std::exchange(other.fields_, nullptr);
            kinds_ = std::exchange(other.kinds_, nullptr);
            size_ = std::exchange(other.size_, std::uint16_t{0});
            capacity_ = std::exchange(other.capacity_, std::uint16_t{0});
            repeated_known_bits_ = std::exchange(other.repeated_known_bits_, std::uint32_t{0});
        }
        return *this;
    }
    ~http_request_header_block() {
        release();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] const http_header_view& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return fields_[index];
    }
    const http_header_view& operator[](std::size_t) const&& = delete;
    [[nodiscard]] http_field_kind kind_at(std::size_t index) const noexcept {
        return kinds_[index];
    }
    [[nodiscard]] std::span<const http_header_view> fields() const& noexcept RUVIA_LIFETIMEBOUND {
        return {fields_, size_};
    }
    std::span<const http_header_view> fields() const&& = delete;
    void mark_repeated_known_slot(std::size_t slot) noexcept {
        if (slot < 32U) {
            repeated_known_bits_ |= (std::uint32_t{1} << slot);
        }
    }
    [[nodiscard]] bool has_repeated_known_slot(std::size_t slot) const noexcept {
        return slot < 32U && (repeated_known_bits_ & (std::uint32_t{1} << slot)) != 0;
    }

    void reserve(std::size_t count) {
        if (count > (std::numeric_limits<std::uint16_t>::max)()) {
            throw std::logic_error("request header block exceeds field limit");
        }
        if (count <= capacity_) {
            return;
        }
        auto* memory = static_cast<std::byte*>(::operator new(bytes_for(count), std::align_val_t{alignof(http_header_view)}));
        auto* replacement = reinterpret_cast<http_header_view*>(memory);
        auto* replacement_kinds = reinterpret_cast<http_field_kind*>(replacement + count);
        for (std::size_t i = 0; i < size_; ++i) {
            std::construct_at(replacement + i, fields_[i]);
            replacement_kinds[i] = kinds_[i];
        }
        const auto size = size_;
        const auto repeated_known_bits = repeated_known_bits_;
        release();
        fields_ = replacement;
        kinds_ = replacement_kinds;
        size_ = size;
        capacity_ = static_cast<std::uint16_t>(count);
        repeated_known_bits_ = repeated_known_bits;
    }

    void append(http_header_view field, http_field_kind kind) {
        if (size_ == (std::numeric_limits<std::uint16_t>::max)()) {
            throw std::logic_error("request header block is full");
        }
        if (size_ == capacity_) {
            reserve((std::min)(static_cast<std::size_t>((std::numeric_limits<std::uint16_t>::max)()), (std::max)(std::size_t{1}, std::size_t(capacity_) * 2)));
        }
        std::construct_at(fields_ + size_, field);
        kinds_[size_] = kind;
        ++size_;
    }

private:
    [[nodiscard]] static std::size_t bytes_for(std::size_t count) noexcept {
        return count * sizeof(http_header_view) + count * sizeof(http_field_kind);
    }

    void release() noexcept {
        if (fields_ != nullptr) {
            std::destroy_n(fields_, size_);
            ::operator delete(fields_, bytes_for(capacity_), std::align_val_t{alignof(http_header_view)});
        }
        fields_ = nullptr;
        kinds_ = nullptr;
        size_ = 0;
        capacity_ = 0;
        repeated_known_bits_ = 0;
    }

    http_header_view* fields_{nullptr};
    http_field_kind* kinds_{nullptr};
    std::uint16_t size_{0};
    std::uint16_t capacity_{0};
    std::uint32_t repeated_known_bits_{0};
};

}  // namespace detail

// RFC 9110 section 7.8 matches protocol names case-insensitively, while RFC
// 8441 section 4 and RFC 9220 section 3 register the lowercase "websocket"
// :protocol token. Any spelling of websocket maps to that token; other
// protocols are returned as is, borrowing `protocol`.
[[nodiscard]] constexpr std::string_view http_canonical_extended_connect_protocol(borrowed_text protocol) noexcept {
    return http_ascii_equals_ignore_case(protocol.view(), "websocket") ? std::string_view("websocket") : protocol.view();
}

class http_request final {
public:
    // Owns the compact descriptor block; field text still borrows protocol input.
    // Moving transfers it.
    http_request(const http_request&) = delete;
    http_request& operator=(const http_request&) = delete;
    http_request(http_request&&) noexcept = default;
    http_request& operator=(http_request&& other) noexcept;

    [[nodiscard]] std::string_view method() const noexcept RUVIA_LIFETIMEBOUND {
        return method_;
    }

    [[nodiscard]] http_known_method known_method() const noexcept {
        return known_method_;
    }

    // Returns the request to its default-constructed state: releases owned header
    // descriptors and clears all borrowed message views.
    void reset() noexcept {
        *this = http_request();
    }

    [[nodiscard]] std::string_view target() const noexcept RUVIA_LIFETIMEBOUND {
        return target_;
    }

    [[nodiscard]] std::string_view scheme() const noexcept RUVIA_LIFETIMEBOUND {
        return scheme_;
    }

    [[nodiscard]] std::string_view authority() const noexcept RUVIA_LIFETIMEBOUND {
        return authority_;
    }

    // The HTTP/2 :protocol pseudo-field (RFC 8441 extended CONNECT). Empty for
    // HTTP/1 requests and for HTTP/2 requests that carry no :protocol.
    [[nodiscard]] std::string_view protocol() const noexcept RUVIA_LIFETIMEBOUND {
        return protocol_;
    }

    [[nodiscard]] http_request_target_form target_form() const noexcept {
        return target_form_;
    }

    [[nodiscard]] std::string_view path() const noexcept RUVIA_LIFETIMEBOUND {
        return path_;
    }

    [[nodiscard]] std::string_view query_string() const noexcept RUVIA_LIFETIMEBOUND {
        return query_string_;
    }

    [[nodiscard]] http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

    // Semantic regular fields, not a byte-for-byte copy of the wire header
    // block. HTTP/1 preserves accepted field-name spelling but removes field
    // framing/OWS and replaces a conflicting Host value for absolute- or
    // authority-form targets. HTTP/2 names are lowercase, pseudo-fields are
    // exposed through method()/scheme()/authority()/protocol()/target(), split Cookie
    // fields are coalesced, and Host is synthesized from a valid :authority
    // when absent. Repeated regular fields retain wire order.
    // Content-Type is retained as metadata without parsing media-type syntax;
    // interpreting or rejecting that media type belongs to the application.
    // Content-Encoding likewise remains metadata at admission; content-coding
    // parsers validate the complete stack before a decoder is selected.
    // Origin and CORS preflight fields are metadata, not transport framing.
    // Applications validate them before deriving any cross-origin permission.
    [[nodiscard]] std::span<const http_header_view> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_.fields();
    }
    [[nodiscard]] std::span<const http_header_view> headers() const&& = delete;

    [[nodiscard]] std::string_view body() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::string_view(reinterpret_cast<const char*>(body_.data()), body_.size());
    }
    std::string_view body() const&& = delete;

    [[nodiscard]] std::span<const std::byte> body_bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    [[nodiscard]] std::span<const std::byte> body_bytes() const&& = delete;

    // Case-insensitive semantic lookup; the last repeated field wins.
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept RUVIA_LIFETIMEBOUND;
    // Exact raw lookup over the encoded query string. Both `raw_name` and the
    // returned value retain their percent-encoded bytes; no application/x-www-
    // form-urlencoded `+` conversion is applied. Later duplicate fields win.
    [[nodiscard]] std::optional<std::string_view> last_raw_query_value(std::string_view raw_name) const noexcept RUVIA_LIFETIMEBOUND;
    // Case-sensitive Cookie lookup; the last matching pair across field lines wins.
    // The returned view borrows the corresponding header text.
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept RUVIA_LIFETIMEBOUND;

private:
    friend struct detail::http_request_access;

    static constexpr std::size_t cached_header_slots = 32;

    http_request() noexcept = default;

    std::string_view method_;
    http_known_method known_method_{http_known_method::unknown};
    std::string_view target_;
    std::string_view scheme_;
    std::string_view authority_;
    std::string_view protocol_;
    std::string_view path_;
    std::string_view query_string_;
    http_protocol_version protocol_version_{http_protocol_version::http11};
    http_request_target_form target_form_{http_request_target_form::origin};
    detail::http_request_header_block headers_{};
    // One-based indices; zero means absent. No duplicated string views.
    std::array<std::uint16_t, cached_header_slots> cached_headers_{};
    std::span<const std::byte> body_{};
};

// Header descriptors live on the heap rather than the request's coroutine frame.
// The protocol field-count limit is independent of this object's layout.

// Constructs a semantic request directly, without parsing a wire-format head.
// Header names/values and body bytes are borrowed from the caller; the exact
// header span count is used to allocate descriptors. The caller must keep all
// supplied text/body storage alive while the returned request is in use.
// Invalid method, target, or header fields throw http_protocol_error. Header
// fields pass the same recipient field rules as the HTTP/1 and HTTP/2 parsers,
// so a repeated singleton field such as Range or If-Range throws instead of
// resolving to one of its lines. A header section outside the
// http_field_section_budget rule (max_http_header_bytes, max_http_header_fields)
// throws the 431 header_too_large error. Body framing fields are intentionally
// not interpreted because this API represents an already-parsed request.
[[nodiscard]] http_request make_parsed_http_request(borrowed_text method, borrowed_text target, std::span<const http_header_view> headers, borrowed_span<std::byte> body = {});

}  // namespace ruvia
