#pragma once

#include <expected>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/WebSocketProtocol.h"

namespace ruvia {

struct Http3WebSocketHandshakeOptions final {
    std::span<const std::string_view> supportedSubprotocols{};
    std::span<const HttpHeaderView> responseHeaders{};
    std::pmr::memory_resource* resource{};
    WebSocketDeflateConfig deflate{};
    std::string_view date{};
};

class Http3WebSocketHandshake;
class HttpResponse;

class Http3WebSocketHandshakeFailure final {
public:
    enum class Kind : unsigned char {
        kInvalidRequest,
        kUnsupportedVersion,
    };

    [[nodiscard]] constexpr Kind kind() const noexcept {
        return kind_;
    }
    [[nodiscard]] HttpProtocolError protocolError() const noexcept;
    void applyRequiredResponseHeaders(HttpResponse& response) const;

private:
    friend std::expected<void, Http3WebSocketHandshakeFailure>
    validateHttp3WebSocketHandshake(const HttpRequest&, std::string_view, bool) noexcept;
    friend std::expected<Http3WebSocketHandshake, Http3WebSocketHandshakeFailure>
    makeHttp3WebSocketHandshake(const HttpRequest&, std::string_view, bool,
        Http3WebSocketHandshakeOptions);

    explicit constexpr Http3WebSocketHandshakeFailure(Kind kind) noexcept
        : kind_(kind) {}

    Kind kind_;
};

// The RFC 9220 request checks are deliberately distinct from HTTP/1.1 Upgrade
// checks. `streamOpen` must be false once the peer has sent FIN or RESET.
[[nodiscard]] std::expected<void, Http3WebSocketHandshakeFailure>
validateHttp3WebSocketHandshake(const HttpRequest& request,
    std::string_view protocol, bool streamOpen) noexcept;

// Owns a canonical HTTP/3 HEADERS frame and the exact compression result to
// use for subsequent WebSocket frames. All dynamic storage belongs to resource.
class Http3WebSocketHandshake final {
public:
    Http3WebSocketHandshake(const Http3WebSocketHandshake&) = delete;
    Http3WebSocketHandshake& operator=(const Http3WebSocketHandshake&) = delete;
    Http3WebSocketHandshake(Http3WebSocketHandshake&&) noexcept = default;
    Http3WebSocketHandshake& operator=(Http3WebSocketHandshake&&) = delete;

    [[nodiscard]] std::span<const char> headersFrame() const& noexcept {
        return headersFrame_;
    }
    std::span<const char> headersFrame() const&& = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] WebSocketCompression compression() const noexcept {
        return compression_;
    }

private:
    friend std::expected<Http3WebSocketHandshake, Http3WebSocketHandshakeFailure>
    makeHttp3WebSocketHandshake(const HttpRequest&, std::string_view, bool,
        Http3WebSocketHandshakeOptions);

    explicit Http3WebSocketHandshake(std::pmr::memory_resource* resource)
        : subprotocol_(resource),
          headersFrame_(resource) {}

    std::pmr::string subprotocol_;
    WebSocketCompression compression_{WebSocketCompression::kDisabled};
    std::pmr::vector<char> headersFrame_;
};

[[nodiscard]] std::expected<Http3WebSocketHandshake, Http3WebSocketHandshakeFailure>
makeHttp3WebSocketHandshake(const HttpRequest& request, std::string_view protocol,
    bool streamOpen, Http3WebSocketHandshakeOptions options = {});

}  // namespace ruvia
