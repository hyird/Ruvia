#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/Http1RequestBodyPlan.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/WebSocketProtocol.h"

namespace ruvia {

namespace detail {
struct WebSocketHandshakeValidationResultAccess;
}  // namespace detail

struct WebSocketServerHandshakeOptions final {
    std::string_view supportedSubprotocols{};
    std::pmr::memory_resource* resource{nullptr};
};

class WebSocketHandshakeAccepted final {
private:
    friend class WebSocketHandshakeValidationResult;

    constexpr WebSocketHandshakeAccepted() noexcept = default;
};

class WebSocketHandshakeFailure final {
public:
    [[nodiscard]] HttpProtocolError protocolError() const noexcept;

    // RFC 6455 requires a server rejecting an unsupported version to advertise
    // every supported version. Apply this after any application error handler.
    void applyRequiredResponseHeaders(HttpResponse& response) const;

private:
    friend class WebSocketHandshakeValidationResult;

    enum class Kind : std::uint8_t { kInvalidRequest, kUnsupportedVersion };

    explicit constexpr WebSocketHandshakeFailure(Kind kind) noexcept
        : kind_(kind) {}

    Kind kind_;
};

class WebSocketHandshakeValidationResult final {
public:
    [[nodiscard]] constexpr const WebSocketHandshakeAccepted* accepted() const& noexcept {
        return std::get_if<WebSocketHandshakeAccepted>(&value_);
    }
    const WebSocketHandshakeAccepted* accepted() const&& = delete;

    [[nodiscard]] constexpr const WebSocketHandshakeFailure* failure() const& noexcept {
        return std::get_if<WebSocketHandshakeFailure>(&value_);
    }
    const WebSocketHandshakeFailure* failure() const&& = delete;

private:
    friend struct detail::WebSocketHandshakeValidationResultAccess;
    friend WebSocketHandshakeValidationResult validateWebSocketHandshake(const HttpRequest&, const Http1RequestBodyPlan&) noexcept;

    using Value = std::variant<WebSocketHandshakeAccepted, WebSocketHandshakeFailure>;

    explicit constexpr WebSocketHandshakeValidationResult(WebSocketHandshakeAccepted accepted) noexcept
        : value_(accepted) {}

    explicit constexpr WebSocketHandshakeValidationResult(WebSocketHandshakeFailure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static constexpr WebSocketHandshakeValidationResult makeAccepted() noexcept {
        return WebSocketHandshakeValidationResult(WebSocketHandshakeAccepted());
    }

    [[nodiscard]] static constexpr WebSocketHandshakeValidationResult makeInvalidRequest() noexcept {
        return WebSocketHandshakeValidationResult(WebSocketHandshakeFailure(WebSocketHandshakeFailure::Kind::kInvalidRequest));
    }

    [[nodiscard]] static constexpr WebSocketHandshakeValidationResult makeUnsupportedVersion() noexcept {
        return WebSocketHandshakeValidationResult(WebSocketHandshakeFailure(WebSocketHandshakeFailure::Kind::kUnsupportedVersion));
    }

    Value value_;
};

// Validates an RFC 6455 HTTP/1.1 opening handshake, including the parser-owned
// request-body framing plan. It does not create or write a response.
[[nodiscard]] WebSocketHandshakeValidationResult validateWebSocketHandshake(
    const HttpRequest& request,
    const Http1RequestBodyPlan& bodyPlan) noexcept;

// Owned HTTP/1.1 101 response plan. forEachResponsePart emits stable views for
// scatter-gather I/O; compression() configures the subsequent WebSocket driver
// from the exact negotiation encoded in this response.
class WebSocketServerHandshake final {
public:
    WebSocketServerHandshake(const WebSocketServerHandshake&) = delete;
    WebSocketServerHandshake& operator=(const WebSocketServerHandshake&) = delete;
    WebSocketServerHandshake(WebSocketServerHandshake&&) noexcept = default;
    WebSocketServerHandshake& operator=(WebSocketServerHandshake&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] constexpr WebSocketCompression compression() const noexcept {
        return compression_;
    }

    template <typename Visitor>
    void forEachResponsePart(Visitor&& visitor) const& {
        visitor(std::string_view(kSwitchingProtocolsPrefix.data(), kSwitchingProtocolsPrefix.size()));
        visitor(std::string_view(accept_.data(), accept_.size()));
        visitor(kCrlf);
        if (!subprotocol_.empty()) {
            visitor(kSubprotocolHeaderPrefix);
            visitor(std::string_view(subprotocol_));
            visitor(kCrlf);
        }
        const auto extension = detail::webSocketCompressionExtension(compression_);
        if (!extension.empty()) {
            visitor(kExtensionsHeaderPrefix);
            visitor(extension);
            visitor(kCrlf);
        }
        visitor(kCrlf);
    }

    template <typename Visitor>
    void forEachResponsePart(Visitor&&) const&& = delete;

private:
    friend WebSocketServerHandshake makeWebSocketServerHandshake(const HttpRequest&, WebSocketServerHandshakeOptions);

    inline static constexpr auto kSwitchingProtocolsPrefix = [] {
        constexpr std::string_view protocol = "HTTP/1.1 ";
        constexpr auto status = detail::httpStatusCodeToken(http_status::kSwitchingProtocols);
        constexpr auto reason = httpReasonPhrase(http_status::kSwitchingProtocols);
        constexpr std::string_view suffix =
            "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: ";
        std::array<char, protocol.size() + status.size() + 1 + reason.size() + suffix.size()> result{};
        std::size_t cursor = 0;
        const auto append = [&result, &cursor](std::string_view part) {
            for (const char value : part) {
                result[cursor++] = value;
            }
        };
        append(protocol);
        append(detail::httpStatusCodeTokenView(status));
        append(" ");
        append(reason);
        append(suffix);
        return result;
    }();
    inline static constexpr std::string_view kSubprotocolHeaderPrefix = "Sec-WebSocket-Protocol: ";
    inline static constexpr std::string_view kExtensionsHeaderPrefix = "Sec-WebSocket-Extensions: ";
    inline static constexpr std::string_view kCrlf = "\r\n";

    WebSocketServerHandshake(std::array<char, 28> accept, std::pmr::string subprotocol, WebSocketCompression compression) noexcept
        : accept_(accept),
          subprotocol_(std::move(subprotocol)),
          compression_(compression) {}

    std::array<char, 28> accept_;
    std::pmr::string subprotocol_;
    WebSocketCompression compression_;
};

// Call after validateWebSocketHandshake() succeeds. The selected subprotocol
// is copied into the requested resource; the response plan borrows no request
// or server-preference storage.
[[nodiscard]] WebSocketServerHandshake makeWebSocketServerHandshake(
    const HttpRequest& request,
    WebSocketServerHandshakeOptions options = {});

}  // namespace ruvia
