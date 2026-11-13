#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
namespace ruvia {

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

class http_request;

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
    http_request_header_block(http_request_header_block&& other) noexcept
        : fields_(std::exchange(other.fields_, nullptr)),
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
        auto* memory = static_cast<std::byte*>(
            ::operator new(bytes_for(count), std::align_val_t{alignof(http_header_view)}));
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
            reserve((std::min)(static_cast<std::size_t>((std::numeric_limits<std::uint16_t>::max)()),
                (std::max)(std::size_t{1}, std::size_t(capacity_) * 2)));
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
    [[nodiscard]] std::optional<std::string_view> last_raw_query_value(
        std::string_view raw_name) const noexcept RUVIA_LIFETIMEBOUND;
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
[[nodiscard]] http_request make_parsed_http_request(
    borrowed_text method, borrowed_text target, std::span<const http_header_view> headers,
    borrowed_span<std::byte> body = {});
}  // namespace ruvia
