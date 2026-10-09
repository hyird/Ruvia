#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_protocol.h"

namespace ruvia {

struct http3_websocket_handshake_options final {
    std::span<const std::string_view> supported_subprotocols_{};
    std::span<const http_header_view> response_headers_{};
    std::pmr::memory_resource* resource_{};
    websocket_deflate_config deflate_{};
    std::string_view date_{};
};

class http3_websocket_handshake;
class http_response;

class http3_websocket_handshake_failure final {
public:
    enum class kind_type : unsigned char {
        invalid_request,
        unsupported_version,
    };

    [[nodiscard]] constexpr kind_type kind() const noexcept {
        return kind_;
    }
    [[nodiscard]] http_protocol_error protocol_error() const noexcept;
    void apply_required_response_headers(http_response& response) const;

private:
    friend std::variant<std::monostate, http3_websocket_handshake_failure>
    validate_http3_websocket_handshake(const http_request&, std::string_view, bool) noexcept;
    friend std::variant<http3_websocket_handshake, http3_websocket_handshake_failure>
    make_http3_websocket_handshake(const http_request&, std::string_view, bool,
        http3_websocket_handshake_options);

    explicit constexpr http3_websocket_handshake_failure(kind_type kind) noexcept
        : kind_(kind) {}

    kind_type kind_;
};

// The RFC 9220 request checks are deliberately distinct from HTTP/1.1 Upgrade
// checks. `stream_open` must be false once the peer has sent FIN or RESET.
[[nodiscard]] std::variant<std::monostate, http3_websocket_handshake_failure>
validate_http3_websocket_handshake(const http_request& request,
    std::string_view protocol, bool stream_open) noexcept;

// Owns a canonical HTTP/3 HEADERS frame and the exact compression result to
// use for subsequent websocket frames. All dynamic storage belongs to resource.
class http3_websocket_handshake final {
public:
    http3_websocket_handshake(const http3_websocket_handshake&) = delete;
    http3_websocket_handshake& operator=(const http3_websocket_handshake&) = delete;
    http3_websocket_handshake(http3_websocket_handshake&&) noexcept = default;
    http3_websocket_handshake& operator=(http3_websocket_handshake&&) = delete;

    [[nodiscard]] std::span<const char> headers_frame() const& noexcept {
        return headers_frame_;
    }
    std::span<const char> headers_frame() const&& = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

private:
    friend std::variant<http3_websocket_handshake, http3_websocket_handshake_failure>
    make_http3_websocket_handshake(const http_request&, std::string_view, bool,
        http3_websocket_handshake_options);

    explicit http3_websocket_handshake(std::pmr::memory_resource* resource)
        : subprotocol_(resource),
          headers_frame_(resource) {}

    std::pmr::string subprotocol_;
    websocket_compression compression_{(websocket_compression{})};
    std::pmr::vector<char> headers_frame_;
};

[[nodiscard]] std::variant<http3_websocket_handshake, http3_websocket_handshake_failure>
make_http3_websocket_handshake(const http_request& request, std::string_view protocol,
    bool stream_open, http3_websocket_handshake_options options = {});

}  // namespace ruvia
