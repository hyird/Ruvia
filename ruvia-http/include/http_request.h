#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "http_field_section_budget.h"
#include "http_header.h"
#include "http_message.h"
#include "http_protocol.h"
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
    template <std::same_as<std::vector<http_header>> fields_type>
    [[nodiscard]] std::optional<http_request_trailer_error> append_received(fields_type&& fields) {
        return append_received_owned(std::move(fields));
    }
    template <detail::http_temporary_owning_range<http_header> fields_type>
        requires(!std::same_as<fields_type, std::vector<http_header>>)
    std::optional<http_request_trailer_error> append_received(fields_type&&) = delete;
    [[nodiscard]] std::vector<http_header> take_fields() && noexcept {
        budget_ = http_trailer_section_budget();
        return std::move(fields_);
    }
    [[nodiscard]] std::span<const http_header> fields() const& noexcept RUVIA_LIFETIMEBOUND {
        return fields_;
    }
    std::span<const http_header> fields() const&& = delete;
    // Case-insensitive; the first matching field line wins.
    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> field(std::string_view) const&& = delete;

private:
    [[nodiscard]] std::optional<http_request_trailer_error> append_received_owned(std::vector<http_header>&& fields);

    std::vector<http_header> fields_;
    http_field_section_budget budget_{http_trailer_section_budget()};
};

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

enum class http_field_kind : std::uint8_t;

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

    [[nodiscard]] std::string_view method() const& noexcept RUVIA_LIFETIMEBOUND {
        return method_;
    }
    std::string_view method() const&& = delete;

    [[nodiscard]] http_known_method known_method() const noexcept {
        return known_method_;
    }

    // Returns the request to its default-constructed state: releases owned header
    // descriptors and clears all borrowed message views.
    void reset() noexcept {
        *this = http_request();
    }

    [[nodiscard]] std::string_view target() const& noexcept RUVIA_LIFETIMEBOUND {
        return target_;
    }
    std::string_view target() const&& = delete;

    [[nodiscard]] std::string_view scheme() const& noexcept RUVIA_LIFETIMEBOUND {
        return scheme_;
    }
    std::string_view scheme() const&& = delete;

    [[nodiscard]] std::string_view authority() const& noexcept RUVIA_LIFETIMEBOUND {
        return authority_;
    }
    std::string_view authority() const&& = delete;

    // The HTTP/2 :protocol pseudo-field (RFC 8441 extended CONNECT). Empty for
    // HTTP/1 requests and for HTTP/2 requests that carry no :protocol.
    [[nodiscard]] std::string_view protocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return protocol_;
    }
    std::string_view protocol() const&& = delete;

    [[nodiscard]] http_request_target_form target_form() const noexcept {
        return target_form_;
    }

    [[nodiscard]] std::string_view path() const& noexcept RUVIA_LIFETIMEBOUND {
        return path_;
    }
    std::string_view path() const&& = delete;

    [[nodiscard]] std::string_view query_string() const& noexcept RUVIA_LIFETIMEBOUND {
        return query_string_;
    }
    std::string_view query_string() const&& = delete;

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

    // Case-insensitive semantic lookup; the first field line wins. Use
    // unique_header() where a repeated field must not resolve to one line.
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> header(std::string_view) const&& = delete;
    // Case-insensitive three-state lookup: repeated when two or more field
    // lines carry `name`, found with the single value otherwise. O(1) for
    // known field kinds. The value borrows the request's header text.
    [[nodiscard]] http_unique_field_lookup unique_header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    http_unique_field_lookup unique_header(std::string_view) const&& = delete;
    // Exact raw lookup over the encoded query string. Both `raw_name` and the
    // returned value retain their percent-encoded bytes; no application/x-www-
    // form-urlencoded `+` conversion is applied. Later duplicate fields win.
    [[nodiscard]] std::optional<std::string_view> last_raw_query_value(std::string_view raw_name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> last_raw_query_value(std::string_view) const&& = delete;
    // Case-sensitive Cookie lookup; the first matching pair across field lines
    // (in wire order) wins. The returned view borrows the header text.
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> cookie(std::string_view) const&& = delete;

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
[[nodiscard]] http_request make_http_request(borrowed_text method, borrowed_text target, std::span<const http_header_view> headers, borrowed_span<std::byte> body = {});

// RFC 9110 CONNECT authority-form requires a nonempty host and explicit port.
[[nodiscard]] bool is_valid_http_connect_authority(std::string_view authority) noexcept;

// Validates an HTTP origin-form request target without allocating. The same
// protocol rule applies to server routes and outbound HTTP/websocket clients.
[[nodiscard]] bool is_valid_http_origin_form_target(std::string_view target) noexcept;

}  // namespace ruvia
