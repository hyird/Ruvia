#pragma once

#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/http_client.h"
#include "ruvia/web/websocket.h"

namespace ruvia {

// Every input is owned before connect() returns its cold operation. The builder
// is consumed once; headers remain application fields, never handshake fields.
class http_client_websocket_request final {
public:
    http_client_websocket_request(const http_client_websocket_request&) = delete;
    http_client_websocket_request& operator=(const http_client_websocket_request&) = delete;
    http_client_websocket_request(http_client_websocket_request&&) = default;
    http_client_websocket_request& operator=(http_client_websocket_request&&) = delete;
    http_client_websocket_request& headers(std::span<const http_header_view> fields);
    http_client_websocket_request& header(std::string_view name, std::string_view value);
    http_client_websocket_request& configure(http_client_request_options options);
    http_client_websocket_request& subprotocol(std::string_view value);
    http_client_websocket_request& deflate(websocket_client_deflate_offer offer);
    // Pong expiry runs only while receive can observe it; blocked writes retain
    // their independent liveness bound. An absent ping interval disables probing.
    http_client_websocket_request& lifecycle(websocket_lifecycle_options options);
    // Applies to incoming messages and submitted application frames; zero is invalid.
    http_client_websocket_request& max_message_bytes(std::size_t limit);
    // Connect and request deadlines bound opening only, not the session lifetime.
    [[nodiscard]] task<websocket> connect();

private:
    friend class http_client_ref;
    http_client_websocket_request(const http_client_ref& client, std::string_view path);
    [[nodiscard]] static task<websocket> connect_owned(http_client_websocket_request request);
    void require_open() const;
    http_client_ref client_;
    std::string path_;
    std::vector<http_header> headers_;
    std::vector<std::string> subprotocols_;
    websocket_client_deflate_offer deflate_{};
    websocket_lifecycle_options lifecycle_{};
    std::size_t max_message_bytes_{default_max_websocket_message_bytes};
    http_client_request_options request_options_;
    bool consumed_{false};
};

}  // namespace ruvia
