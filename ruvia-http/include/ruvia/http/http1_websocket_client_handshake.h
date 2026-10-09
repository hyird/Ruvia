#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/websocket_client_negotiation.h"

namespace ruvia {

class http1_parsed_client_response_head;

struct http1_websocket_client_handshake_config_view final {
    // Supply fresh cryptographically random bytes for each opening handshake.
    std::span<const std::uint8_t, 16> nonce_;
    std::span<const http_header_view> headers_{};
    std::span<const std::string_view> subprotocols_{};
    std::string_view user_agent_{};
    websocket_client_deflate_offer deflate_{};
};

enum class http1_websocket_client_handshake_error : std::uint8_t {
    response_status,
    accept,
    upgrade,
    connection,
    subprotocol,
    extensions,
};

struct http1_websocket_client_handshake_result_view final {
    // Borrows the parsed response head passed to validate_response().
    std::string_view selected_subprotocol_{};
    websocket_compression compression_{};
};

// Owns one opening handshake's key and configured fields in the supplied
// resource; configuration views need only survive construction. The resource
// must outlive this object and every prepared request's exchange state.
class http1_websocket_client_handshake final {
public:
    http1_websocket_client_handshake(http1_websocket_client_handshake_config_view options,
        std::pmr::memory_resource* resource = nullptr);

    http1_websocket_client_handshake(const http1_websocket_client_handshake&) = delete;
    http1_websocket_client_handshake& operator=(const http1_websocket_client_handshake&) = delete;
    http1_websocket_client_handshake(http1_websocket_client_handshake&&) noexcept = default;
    http1_websocket_client_handshake& operator=(http1_websocket_client_handshake&&) = delete;

    static void validate_configuration(
        std::span<const http_header_view> headers,
        std::span<const std::string_view> subprotocols,
        std::string_view user_agent = {});

    [[nodiscard]] http1_client_request_prepare_result prepare_request(
        const http_origin_view& origin, std::string_view target, std::span<char> head_buffer) const;

    [[nodiscard]] std::variant<http1_websocket_client_handshake_result_view,
        http1_websocket_client_handshake_error>
    validate_response(const http1_parsed_client_response_head& response) const;

private:
    std::pmr::memory_resource* resource_;
    std::pmr::string key_;
    websocket_client_negotiation negotiation_;

    struct stored_header_type final {
        std::pmr::string name_;
        std::pmr::string value_;
        stored_header_type(std::string_view n, std::string_view v, std::pmr::memory_resource* r)
            : name_(n, r),
              value_(v, r) {}
    };
    std::pmr::vector<stored_header_type> headers_;
    std::pmr::string user_agent_;
};

}  // namespace ruvia
