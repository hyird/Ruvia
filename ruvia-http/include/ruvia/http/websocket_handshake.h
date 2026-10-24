#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/response/http1_status_line.h"
#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/websocket_protocol.h"

namespace ruvia {

inline constexpr std::size_t max_websocket_subprotocols = 64;

// Allocation-free validated RFC 6455 subprotocol list, suitable for server
// route configuration and parsing offered protocol lists. Capacity is a product
// limit of max_websocket_subprotocols distinct protocols across all list elements;
// a handshake offering more is rejected as invalid rather than truncated.
class websocket_subprotocol_set final {
public:
    [[nodiscard]] bool append(std::string_view protocol) noexcept {
        if (!is_valid_http_header_name(protocol) || size_ == protocols_.size() || contains(protocol)) {
            return false;
        }
        protocols_[size_++] = protocol;
        return true;
    }

    [[nodiscard]] bool append_list(std::string_view value) noexcept {
        while (true) {
            const auto comma = value.find(',');
            const auto protocol = http_trim_ows(
                comma == std::string_view::npos ? value : value.substr(0, comma));
            if (!protocol.empty() && !append(protocol)) {
                return false;
            }
            if (comma == std::string_view::npos) {
                return true;
            }
            value.remove_prefix(comma + 1);
        }
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
    std::array<std::string_view, max_websocket_subprotocols> protocols_{};
    std::size_t size_{0};
};

struct websocket_server_handshake_options final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    std::span<const std::string_view> supported_subprotocols_{};
    // Additional response fields, copied before construction returns. Framing,
    // connection, and Sec-websocket fields belong to the handshake itself.
    // Names/values must be valid fields; values cannot start or end with SP/HTAB.
    std::span<const http_header_view> response_headers_{};
    std::pmr::memory_resource* resource_{nullptr};
    websocket_deflate_config deflate_{};
};

struct websocket_handshake_validation_options final {
    // RFC 6455 4.2.2 lets a server that only speaks particular subprotocols
    // refuse a client offering none of them, instead of accepting without a
    // Sec-WebSocket-Protocol response field. When set, a request whose offer
    // shares no entry with supported_subprotocols_ fails validation.
    std::span<const std::string_view> supported_subprotocols_{};
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

    explicit constexpr websocket_handshake_failure(kind_type kind) noexcept
        : kind_(kind) {}

    kind_type kind_;
};

class websocket_handshake_validation_result;
class http2_connection;
namespace detail {
class http2_stream_state;
class websocket_request_header_validator;
[[nodiscard]] websocket_handshake_validation_result validate_http2_websocket_handshake(
    const http2_stream_state&, const http_request&,
    websocket_handshake_validation_options) noexcept;
}  // namespace detail

class websocket_handshake_validation_result final {
public:
    [[nodiscard]] constexpr const websocket_handshake_accepted* accepted() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const websocket_handshake_accepted* accepted() const&& = delete;

    [[nodiscard]] constexpr const websocket_handshake_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const websocket_handshake_failure* failure() const&& = delete;

private:
    friend class detail::websocket_request_header_validator;
    friend websocket_handshake_validation_result validate_http2_websocket_handshake(
        http2_connection&, std::uint32_t, const http_request&,
        websocket_handshake_validation_options) noexcept;
    friend websocket_handshake_validation_result detail::validate_http2_websocket_handshake(
        const detail::http2_stream_state&, const http_request&,
        websocket_handshake_validation_options) noexcept;
    friend websocket_handshake_validation_result validate_websocket_handshake(
        const http_request&, const http1_request_body_plan&, websocket_handshake_validation_options) noexcept;

    using value_type = std::variant<websocket_handshake_accepted, websocket_handshake_failure>;

    explicit constexpr websocket_handshake_validation_result(
        websocket_handshake_accepted accepted) noexcept
        : value_(accepted) {}

    explicit constexpr websocket_handshake_validation_result(
        websocket_handshake_failure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static constexpr websocket_handshake_validation_result make_accepted() noexcept {
        return websocket_handshake_validation_result(websocket_handshake_accepted());
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result
    make_invalid_request() noexcept {
        return websocket_handshake_validation_result(
            websocket_handshake_failure(websocket_handshake_failure::kind_type::invalid_request));
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result
    make_unsupported_version() noexcept {
        return websocket_handshake_validation_result(
            websocket_handshake_failure(websocket_handshake_failure::kind_type::unsupported_version));
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result
    make_unsupported_subprotocol() noexcept {
        return websocket_handshake_validation_result(
            websocket_handshake_failure(websocket_handshake_failure::kind_type::unsupported_subprotocol));
    }

    value_type value_;
};

// Validates an RFC 6455 HTTP/1.1 opening handshake, including the parser-owned
// request-body framing plan. It does not create or write a response.
[[nodiscard]] websocket_handshake_validation_result validate_websocket_handshake(
    const http_request& request, const http1_request_body_plan& body_plan,
    websocket_handshake_validation_options options = {}) noexcept;

namespace detail {

// The fixed bytes of the HTTP/1.1 101 opening-handshake response (RFC 6455
// section 4.2.2) through the Sec-WebSocket-Accept field name; the accept key
// and its CRLF follow.
inline constexpr auto websocket_switching_protocols_head_prefix = [] {
    constexpr auto reason = http_reason_phrase(http_status::switching_protocols);
    constexpr std::string_view upgrade_field = "Upgrade: websocket\r\n";
    constexpr std::string_view field_separator = ": ";
    constexpr std::string_view crlf = "\r\n";
    constexpr std::string_view accept_field_name = "Sec-WebSocket-Accept: ";
    std::array<char, http1_status_line_size(reason) + upgrade_field.size() + http_connection_field_name.size() +
                         field_separator.size() + http_connection_upgrade_option.size() + crlf.size() +
                         accept_field_name.size()>
        result_value{};
    struct sink_type final {
        char* out_;

        constexpr void append(std::string_view part) noexcept {
            for (const char value : part) {
                *out_++ = value;
            }
        }

        constexpr void append(char value) noexcept {
            *out_++ = value;
        }
    } sink{result_value.data()};
    emit_http1_status_line(sink, http_status::switching_protocols, reason);
    sink.append(upgrade_field);
    sink.append(http_connection_field_name);
    sink.append(field_separator);
    sink.append(http_connection_upgrade_option);
    sink.append(crlf);
    sink.append(accept_field_name);
    return result_value;
}();

}  // namespace detail

// Owned HTTP/1.1 101 response plan. for_each_response_part emits stable views for
// scatter-gather I/O; compression() configures the subsequent websocket driver
// from the exact negotiation encoded in this response.
class websocket_server_handshake final {
public:
    websocket_server_handshake(const websocket_server_handshake&) = delete;
    websocket_server_handshake& operator=(const websocket_server_handshake&) = delete;
    websocket_server_handshake(websocket_server_handshake&&) noexcept = default;
    websocket_server_handshake& operator=(websocket_server_handshake&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] constexpr websocket_compression compression() const noexcept {
        return compression_;
    }

    template <typename visitor_type>
    void for_each_response_part(visitor_type&& visitor) const& {
        visitor(std::string_view(detail::websocket_switching_protocols_head_prefix.data(),
            detail::websocket_switching_protocols_head_prefix.size()));
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
    friend websocket_server_handshake make_websocket_server_handshake(
        const http_request&, websocket_server_handshake_options);

    inline static constexpr std::string_view subprotocol_header_prefix = "Sec-WebSocket-Protocol: ";
    inline static constexpr std::string_view extensions_header_prefix = "Sec-WebSocket-Extensions: ";
    inline static constexpr std::string_view crlf = "\r\n";

    websocket_server_handshake(std::array<char, 28> accept, std::pmr::string subprotocol,
        websocket_compression compression, std::pmr::vector<http_header> response_headers_value) noexcept
        : accept_(accept),
          subprotocol_(std::move(subprotocol)),
          compression_(compression),
          compression_extension_(compression),
          response_headers_(std::move(response_headers_value)) {}

    std::array<char, 28> accept_;
    std::pmr::string subprotocol_;
    websocket_compression compression_;
    detail::websocket_compression_extension compression_extension_;
    std::pmr::vector<http_header> response_headers_;
};

// Call after validate_websocket_handshake() succeeds. The selected subprotocol
// is copied into the requested resource; the response plan borrows no request
// or server-preference storage.
[[nodiscard]] websocket_server_handshake make_websocket_server_handshake(
    const http_request& request, websocket_server_handshake_options options = {});

}  // namespace ruvia
