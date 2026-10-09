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

#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/websocket_protocol.h"

namespace ruvia {

namespace detail {
struct websocket_handshake_validation_result_access;
}  // namespace detail

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
        unsupported_version };

    explicit constexpr websocket_handshake_failure(kind_type kind) noexcept
        : kind_(kind) {}

    kind_type kind_;
};

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
    friend struct detail::websocket_handshake_validation_result_access;
    friend websocket_handshake_validation_result validate_websocket_handshake(
        const http_request&, const http1_request_body_plan&) noexcept;

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

    value_type value_;
};

// Validates an RFC 6455 HTTP/1.1 opening handshake, including the parser-owned
// request-body framing plan. It does not create or write a response.
[[nodiscard]] websocket_handshake_validation_result validate_websocket_handshake(
    const http_request& request, const http1_request_body_plan& body_plan) noexcept;

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
        visitor(
            std::string_view(switching_protocols_prefix.data(), switching_protocols_prefix.size()));
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

    inline static constexpr auto switching_protocols_prefix = [] {
        constexpr std::string_view protocol = "HTTP/1.1 ";
        constexpr auto status = detail::http_status_code_token(http_status::switching_protocols);
        constexpr auto reason = http_reason_phrase(http_status::switching_protocols);
        constexpr std::string_view suffix =
            "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: ";
        std::array<char, protocol.size() + status.size() + 1 + reason.size() + suffix.size()>
            result_value{};
        std::size_t cursor_value = 0;
        const auto append = [&result_value, &cursor_value](std::string_view part) {
            for (const char value : part) {
                result_value[cursor_value++] = value;
            }
        };
        append(protocol);
        append(detail::http_status_code_token_view(status));
        append(" ");
        append(reason);
        append(suffix);
        return result_value;
    }();
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
