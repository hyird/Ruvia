#pragma once

#include "ruvia/web/WebSocketClient.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

namespace ruvia::detail {

[[nodiscard]] inline bool isReservedWebSocketHandshakeHeader(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 8> reserved{
        "host",
        "connection",
        "upgrade",
        "sec-websocket-key",
        "sec-websocket-version",
        "sec-websocket-protocol",
        "sec-websocket-extensions",
        "content-length",
    };
    return std::ranges::any_of(reserved,
        [name](std::string_view candidate) { return httpAsciiEqualsIgnoreCase(name, candidate); });
}

inline void validateWebSocketClientConfig(const WebSocketClientConfig& config) {
    if (config.scheme != WebSocketScheme::kWs && config.scheme != WebSocketScheme::kWss) {
        throw std::invalid_argument("WebSocket client scheme is invalid");
    }
    validateClientOriginHost(
        config.host, "WebSocket client host must not be empty", "WebSocket client host is invalid");
    if (config.port.has_value() && config.port.value() == 0) {
        throw std::invalid_argument("WebSocket client port must be greater than zero");
    }
    if (config.target.empty() || config.target.front() != '/') {
        throw std::invalid_argument("WebSocket client target must use origin-form");
    }
    if (config.maxMessageBytes == 0) {
        throw std::invalid_argument("WebSocket client max message bytes must be greater than zero");
    }
    if (config.connectTimeout.count() <= 0) {
        throw std::invalid_argument("WebSocket client connect timeout must be greater than zero");
    }
    for (const std::optional<std::chrono::milliseconds> timeout :
        {config.readTimeout, config.writeTimeout, config.closeHandshakeTimeout}) {
        if (timeout.has_value() && timeout->count() <= 0) {
            throw std::invalid_argument("WebSocket client timeout must be greater than zero");
        }
    }
    validateClientTransportConfig(clientTransportConfigView(config));

    for (const auto& [name, value] : config.headers) {
        if (!isValidHttpHeaderName(name) || !isValidHttpHeaderValue(value) ||
            isReservedWebSocketHandshakeHeader(name)) {
            throw std::invalid_argument("invalid or reserved WebSocket client handshake header");
        }
    }

    if (config.subprotocols.empty()) {
        return;
    }
    bool valid = true;
    std::size_t count = 0;
    httpVisitCommaSeparatedQuotedItems(config.subprotocols, [&](std::string_view token) {
        valid = valid && !token.empty() && std::ranges::all_of(token, [](char ch) {
            return isHttpTokenChar(static_cast<unsigned char>(ch));
        });
        ++count;
        return valid;
    });
    if (!valid || count == 0) {
        throw std::invalid_argument("invalid WebSocket client subprotocol list");
    }
}

}  // namespace ruvia::detail
