#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "http_client_protocol.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "http_response_server.h"
#include "websocket_connection.h"

namespace ruvia {

inline constexpr std::size_t max_websocket_subprotocols = 64;

// Allocation-free validated RFC 6455 subprotocol list, suitable for server
// route configuration and parsing offered protocol lists. Capacity is a product
// limit of max_websocket_subprotocols distinct protocols across all list elements;
// a handshake offering more is rejected as invalid rather than truncated.
class websocket_subprotocol_set final {
public:
    // Strict: every protocol must be a new token (configuration and the
    // offers a client sends, which RFC 6455 section 4.1 requires to be unique).
    [[nodiscard]] bool append(borrowed_text protocol) noexcept {
        const auto protocol_view = protocol.view();
        if (!is_valid_http_header_name(protocol_view) || size_ == protocols_.size() || contains(protocol_view)) {
            return false;
        }
        protocols_[size_++] = protocol_view;
        return true;
    }

    // Strict list form of append(); empty list elements are ignored.
    [[nodiscard]] bool append_list(borrowed_text value) noexcept {
        return append_elements(value.view(), false);
    }

    // A received Sec-WebSocket-Protocol offer. Uniqueness binds the sending
    // client only (RFC 6455 section 4.1); a server receiving a repeated
    // protocol skips it (section 4.2.1 imposes no recipient check).
    [[nodiscard]] bool append_offered_list(borrowed_text value) noexcept {
        return append_elements(value.view(), true);
    }

    [[nodiscard]] bool contains(std::string_view protocol) const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            if (protocols_[i] == protocol) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

private:
    [[nodiscard]] bool append_elements(std::string_view value, bool skip_duplicates) noexcept {
        return detail::http_visit_token_list(value, detail::http_field_list_role::recipient, [this, skip_duplicates](std::string_view protocol) noexcept {
            return (skip_duplicates && contains(protocol)) || append(protocol);
        }) == detail::http_field_list_parse_status::ok;
    }

    std::array<std::string_view, max_websocket_subprotocols> protocols_{};
    std::size_t size_{0};
};

// Server handshake policy shared by HTTP/1.1 Upgrade and RFC 8441 Extended
// CONNECT.
struct websocket_server_handshake_options final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    borrowed_span<std::string_view> supported_subprotocols_{};
    // Additional response fields, copied before the response plan is returned.
    // Framing, connection, and Sec-websocket fields belong to the handshake
    // itself. Names/values must be valid fields; values cannot start or end
    // with SP/HTAB.
    borrowed_http_headers response_headers_{};
    websocket_deflate_config deflate_{};
};

struct websocket_handshake_validation_options final {
    // RFC 6455 4.2.2 lets a server that only speaks particular subprotocols
    // refuse a client offering none of them, instead of accepting without a
    // Sec-WebSocket-Protocol response field. When set, a request whose offer
    // shares no entry with supported_subprotocols_ fails validation.
    borrowed_span<std::string_view> supported_subprotocols_{};
    bool require_subprotocol_{false};
};

class websocket_handshake_accepted final {
private:
    friend class websocket_handshake_validation_result;

    constexpr websocket_handshake_accepted() noexcept = default;
};

class websocket_handshake_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept;

    // RFC 6455 requires a server rejecting an unsupported version to advertise
    // every supported version. Apply this after any application error handler.
    void apply_required_response_headers(http_response& response) const;

private:
    friend class websocket_handshake_validation_result;

    enum class kind_type : std::uint8_t { invalid_request,
        unsupported_version,
        unsupported_subprotocol };

    explicit constexpr websocket_handshake_failure(kind_type kind) noexcept :
        kind_(kind) {}

    kind_type kind_;
};

class websocket_handshake_validation_result;
class http2_connection;
class http3_connection;
class http3_request_head;
namespace detail {
class websocket_compression_extension final {
public:
    explicit constexpr websocket_compression_extension(websocket_compression compression) noexcept {
        if (!compression.enabled_) {
            return;
        }
        append("permessage-deflate");
        if (compression.server_no_context_takeover_) {
            append("; server_no_context_takeover");
        }
        if (compression.client_no_context_takeover_) {
            append("; client_no_context_takeover");
        }
        if (compression.server_max_window_bits_) {
            append("; server_max_window_bits=");
            number(*compression.server_max_window_bits_);
        }
        if (compression.client_max_window_bits_) {
            append("; client_max_window_bits=");
            number(*compression.client_max_window_bits_);
        }
    }
    [[nodiscard]] constexpr std::string_view view() const& noexcept {
        return {bytes_.data(), size_};
    }
    std::string_view view() const&& = delete;
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

private:
    constexpr void append(std::string_view text) noexcept {
        for (char ch : text) {
            bytes_[size_++] = ch;
        }
    }
    constexpr void number(int value) noexcept {
        if (value >= 10) {
            bytes_[size_++] = '1';
        }
        bytes_[size_++] = static_cast<char>('0' + value % 10);
    }
    std::array<char, 160> bytes_{};
    std::size_t size_{0};
};
class http2_connection;
class http2_stream_state;
class websocket_request_header_validator;
inline constexpr std::string_view websocket_switching_protocols_head_prefix = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
[[nodiscard]] websocket_handshake_validation_result validate_http2_websocket_handshake(const http2_stream_state&, const http_request&, websocket_handshake_validation_options) noexcept;
}  // namespace detail

class websocket_handshake_validation_result final {
public:
    [[nodiscard]] constexpr const websocket_handshake_accepted* accepted() const& noexcept RUVIA_LIFETIMEBOUND {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const websocket_handshake_accepted* accepted() const&& = delete;

    [[nodiscard]] constexpr const websocket_handshake_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const websocket_handshake_failure* failure() const&& = delete;

private:
    friend class detail::websocket_request_header_validator;
    friend websocket_handshake_validation_result validate_http2_websocket_handshake(http2_connection&, std::uint32_t, const http_request&, websocket_handshake_validation_options) noexcept;
    friend websocket_handshake_validation_result detail::validate_http2_websocket_handshake(const detail::http2_stream_state&, const http_request&, websocket_handshake_validation_options) noexcept;
    friend websocket_handshake_validation_result validate_http3_websocket_handshake(const http3_request_head&, const http_request&, websocket_handshake_validation_options) noexcept;
    friend websocket_handshake_validation_result validate_websocket_handshake(const http_request&, const http1_request_body_plan&, websocket_handshake_validation_options) noexcept;

    using value_type = std::variant<websocket_handshake_accepted, websocket_handshake_failure>;

    explicit constexpr websocket_handshake_validation_result(websocket_handshake_accepted accepted) noexcept :
        value_(accepted) {}

    explicit constexpr websocket_handshake_validation_result(websocket_handshake_failure failure) noexcept :
        value_(failure) {}

    [[nodiscard]] static constexpr websocket_handshake_validation_result make_accepted() noexcept {
        return websocket_handshake_validation_result(websocket_handshake_accepted());
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result make_invalid_request() noexcept {
        return websocket_handshake_validation_result(websocket_handshake_failure(websocket_handshake_failure::kind_type::invalid_request));
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result make_unsupported_version() noexcept {
        return websocket_handshake_validation_result(websocket_handshake_failure(websocket_handshake_failure::kind_type::unsupported_version));
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result make_unsupported_subprotocol() noexcept {
        return websocket_handshake_validation_result(websocket_handshake_failure(websocket_handshake_failure::kind_type::unsupported_subprotocol));
    }

    value_type value_;
};

// The negotiation an RFC 8441 (HTTP/2) or RFC 9220 (HTTP/3) extended CONNECT
// 200 response committed on the wire. The runtime configures its websocket
// connection from this value, never from a separately recomputed
// compression/subprotocol tuple.
class websocket_extended_connect_negotiation final {
public:
    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

private:
    friend class http2_connection;
    friend class detail::http2_connection;
    friend class http3_connection;
    websocket_extended_connect_negotiation(std::string subprotocol, websocket_compression compression) noexcept :
        subprotocol_(std::move(subprotocol)),
        compression_(compression) {}
    std::string subprotocol_;
    websocket_compression compression_;
};

// Validates an RFC 6455 HTTP/1.1 opening handshake, including the parser-owned
// request-body framing plan. It does not create or write a response.
[[nodiscard]] websocket_handshake_validation_result validate_websocket_handshake(const http_request& request, const http1_request_body_plan& body_plan, websocket_handshake_validation_options options = {}) noexcept;

// Owned HTTP/1.1 101 response plan. for_each_response_part emits stable views for
// scatter-gather I/O; compression() configures the subsequent websocket driver
// from the exact negotiation encoded in this response.
class websocket_server_handshake final {
public:
    websocket_server_handshake(const websocket_server_handshake&) = delete;
    websocket_server_handshake& operator=(const websocket_server_handshake&) = delete;
    websocket_server_handshake(websocket_server_handshake&&) noexcept = default;
    websocket_server_handshake& operator=(websocket_server_handshake&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] constexpr websocket_compression compression() const noexcept {
        return compression_;
    }

    template <typename visitor_type>
    void for_each_response_part(visitor_type&& visitor) const& {
        visitor(detail::websocket_switching_protocols_head_prefix);
        visitor(std::string_view(accept_.data(), accept_.size()));
        visitor(crlf);
        if (!subprotocol_.empty()) {
            visitor(subprotocol_header_prefix);
            visitor(std::string_view(subprotocol_));
            visitor(crlf);
        }
        if (!compression_extension_.empty()) {
            visitor(extensions_header_prefix);
            visitor(compression_extension_.view());
            visitor(crlf);
        }
        for (const auto& header : response_headers_) {
            visitor(header.name());
            visitor(std::string_view(": "));
            visitor(header.value());
            visitor(crlf);
        }
        visitor(crlf);
    }

    template <typename visitor_type>
    void for_each_response_part(visitor_type&&) const&& = delete;

private:
    friend websocket_server_handshake make_websocket_server_handshake(const http_request&, websocket_server_handshake_options);

    inline static constexpr std::string_view subprotocol_header_prefix = "Sec-WebSocket-Protocol: ";
    inline static constexpr std::string_view extensions_header_prefix = "Sec-WebSocket-Extensions: ";
    inline static constexpr std::string_view crlf = "\r\n";

    websocket_server_handshake(std::array<char, 28> accept, std::string subprotocol, websocket_compression compression, std::vector<http_header> response_headers_value) noexcept :
        accept_(accept),
        subprotocol_(std::move(subprotocol)),
        compression_(compression),
        compression_extension_(compression),
        response_headers_(std::move(response_headers_value)) {}

    std::array<char, 28> accept_;
    std::string subprotocol_;
    websocket_compression compression_;
    detail::websocket_compression_extension compression_extension_;
    std::vector<http_header> response_headers_;
};

// Call after validate_websocket_handshake() succeeds. The selected subprotocol
// and response fields are copied; the response plan borrows no
// request or server-preference storage.
[[nodiscard]] websocket_server_handshake make_websocket_server_handshake(const http_request& request, websocket_server_handshake_options options = {});

struct websocket_client_negotiation_config_view final {
    borrowed_span<std::string_view> subprotocols_{};
    websocket_client_deflate_offer deflate_{};
};
// The one failure shape of a client opening handshake over HTTP/1.1 Upgrade
// (RFC 6455 section 4.1) or Extended CONNECT (RFC 8441 over HTTP/2, RFC 9220
// over HTTP/3).
enum class websocket_client_handshake_error : std::uint8_t {
    // Not 101 over HTTP/1.1, not 2xx over Extended CONNECT.
    response_status,
    // Extended CONNECT: the 2xx HEADERS ended the stream.
    stream_closed,
    // HTTP/1.1: Sec-WebSocket-Accept is missing, repeated or wrong.
    accept,
    // HTTP/1.1: no Upgrade: websocket.
    upgrade,
    // HTTP/1.1: no Connection: Upgrade.
    connection,
    subprotocol,
    extensions,
    // Extended CONNECT: a connection-specific response field (RFC 9113
    // section 8.2.2, RFC 9114 section 4.2).
    forbidden_field,
};
struct websocket_client_negotiation_result_view final {
    // Borrows the supplied response headers.
    std::string_view selected_subprotocol_{};
    websocket_compression compression_{};
};

class websocket_client_negotiation;
namespace detail {
// The server's Sec-WebSocket-Protocol and Sec-WebSocket-Extensions selection
// against the offer; `extended` (RFC 8441) also rejects connection-specific
// fields. The result view borrows `fields`.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_websocket_client_selection(const websocket_client_negotiation& negotiation, std::span<const http_header> fields, bool extended);
}  // namespace detail

// HTTP-version-independent RFC 6455/7692 client negotiation owner. Configuration
// is copied once. It supplies request fields to HTTP/1 Upgrade or
// RFC 8441 Extended CONNECT, and validates the server's exact selection; the
// version-specific response validators check the same selection after their
// own status checks.
// All offer views expire with this owner; response views expire with the head.
class websocket_client_negotiation final {
public:
    // Validates the subprotocols and the deflate offer, including its deflater.
    static void validate_configuration(websocket_client_negotiation_config_view config);
    explicit websocket_client_negotiation(websocket_client_negotiation_config_view config);
    websocket_client_negotiation(websocket_client_negotiation&&) noexcept = default;
    websocket_client_negotiation& operator=(websocket_client_negotiation&&) = delete;
    websocket_client_negotiation(const websocket_client_negotiation&) = delete;
    websocket_client_negotiation& operator=(const websocket_client_negotiation&) = delete;
    [[nodiscard]] std::span<const http_header> request_fields() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::span<const http_header>(fields_.data(), field_count_);
    }
    std::span<const http_header> request_fields() const&& = delete;
    // The server's Sec-WebSocket-Protocol and Sec-WebSocket-Extensions selection
    // in `fields` alone, without status or connection-specific field checks.
    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_fields(std::span<const http_header> fields) const;
    template <detail::http_temporary_owning_range<http_header> fields_type>
    std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_fields(fields_type&&) const = delete;

private:
    friend std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> detail::validate_websocket_client_selection(const websocket_client_negotiation&, std::span<const http_header>, bool);

    websocket_client_deflate_offer deflate_;
    std::array<http_header, 3> fields_{};
    std::size_t field_count_{0};
    // The owned Sec-WebSocket-Protocol request value; empty when none is offered.
    std::string_view offered_subprotocols_{};
};

class http1_parsed_client_response_head;

// RFC 6455 section 4.1 opening handshake over HTTP/1.1 Upgrade. The caller
// owns the base64-encoded 16-byte nonce (`encoded_key`), the negotiation and the
// application headers for the whole exchange; subprotocols and the deflate offer
// are validated by websocket_client_negotiation.

// Rejects application headers the handshake or request writer generates itself
// (Host, framing, Expect, Upgrade/Connection and every Sec-WebSocket-* field)
// and any header the request writer would refuse.
void validate_http1_websocket_client_handshake_configuration(std::span<const http_header_view> headers);

[[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target, std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation, std::span<const http_header> headers, std::span<char> head_buffer, http_request_field_source source = http_request_field_source::generated);
[[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target, std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation, std::span<const http_header_view> headers, std::span<char> head_buffer, http_request_field_source source = http_request_field_source::generated);

// Validates the 101 response against the key, then the server's selection
// against the negotiation. The result view borrows the response head.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_http1_websocket_client_response(std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation, const http1_parsed_client_response_head& response RUVIA_LIFETIMEBOUND);
std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_http1_websocket_client_response(std::span<const char, 24>, const websocket_client_negotiation&, const http1_parsed_client_response_head&&) = delete;

// Extended CONNECT over HTTP/2 or HTTP/3: validates the server's 2xx final
// head on a still-open stream, then its selection against the negotiation.
// The result view borrows the response head.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_extended_connect_websocket_client_response(const websocket_client_negotiation& negotiation, const http_client_response_head& head, bool stream_open);
std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_extended_connect_websocket_client_response(const websocket_client_negotiation&, const http_client_response_head&&, bool) = delete;
}  // namespace ruvia
