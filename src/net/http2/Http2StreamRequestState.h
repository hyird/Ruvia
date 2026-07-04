#pragma once

#include <cstdint>

namespace ruvia::detail {

class Http2StreamRequestState final {
public:
    [[nodiscard]] bool hasMethod() const noexcept {
        return hasMethod_;
    }

    void markMethod() noexcept {
        hasMethod_ = true;
    }

    [[nodiscard]] bool hasProtocol() const noexcept {
        return hasProtocol_;
    }

    [[nodiscard]] bool protocolIsWebSocket() const noexcept {
        return protocolIsWebSocket_;
    }

    void setProtocol(bool isWebSocket) noexcept {
        hasProtocol_ = true;
        protocolIsWebSocket_ = isWebSocket;
    }

    [[nodiscard]] bool hasScheme() const noexcept {
        return hasScheme_;
    }

    void markScheme(std::uint16_t defaultPort) noexcept {
        hasScheme_ = true;
        schemeDefaultPort_ = defaultPort;
    }

    [[nodiscard]] std::uint16_t schemeDefaultPort() const noexcept {
        return schemeDefaultPort_;
    }

    [[nodiscard]] bool hasAuthority() const noexcept {
        return hasAuthority_;
    }

    void markAuthority() noexcept {
        hasAuthority_ = true;
    }

    [[nodiscard]] bool hasPath() const noexcept {
        return hasPath_;
    }

    void markPath() noexcept {
        hasPath_ = true;
    }

    [[nodiscard]] bool hasHost() const noexcept {
        return hasHost_;
    }

    void markHost() noexcept {
        hasHost_ = true;
    }

    [[nodiscard]] bool hasCookie() const noexcept {
        return hasCookie_;
    }

    void markCookie() noexcept {
        hasCookie_ = true;
    }

    [[nodiscard]] bool regularHeaderSeen() const noexcept {
        return regularHeaderSeen_;
    }

    void markRegularHeaderSeen() noexcept {
        regularHeaderSeen_ = true;
    }

    [[nodiscard]] bool headersDecoded() const noexcept {
        return headersDecoded_;
    }

    void markHeadersDecoded() noexcept {
        headersDecoded_ = true;
    }

    [[nodiscard]] bool standardConnect() const noexcept {
        return standardConnect_;
    }

    void markStandardConnect() noexcept {
        standardConnect_ = true;
    }

    [[nodiscard]] bool extendedConnectWebSocket() const noexcept {
        return extendedConnectWebSocket_;
    }

    void markExtendedConnectWebSocket() noexcept {
        extendedConnectWebSocket_ = true;
    }

    [[nodiscard]] bool webSocketTunnel() const noexcept {
        return webSocketTunnel_;
    }

    void markWebSocketTunnel() noexcept {
        webSocketTunnel_ = true;
    }

private:
    bool hasMethod_ : 1 {false};
    bool hasProtocol_ : 1 {false};
    bool protocolIsWebSocket_ : 1 {false};
    bool hasScheme_ : 1 {false};
    bool hasAuthority_ : 1 {false};
    bool hasPath_ : 1 {false};
    bool hasHost_ : 1 {false};
    bool hasCookie_ : 1 {false};
    bool regularHeaderSeen_ : 1 {false};
    bool headersDecoded_ : 1 {false};
    bool standardConnect_ : 1 {false};
    bool extendedConnectWebSocket_ : 1 {false};
    bool webSocketTunnel_ : 1 {false};
    std::uint16_t schemeDefaultPort_{0};
};

}  // namespace ruvia::detail
