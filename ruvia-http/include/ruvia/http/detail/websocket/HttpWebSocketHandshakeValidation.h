#pragma once

#include <cstdint>
#include <variant>

#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"

namespace ruvia::detail {

class Http1RequestBodyPlan;
class Http2StreamState;
class HttpWebSocketHandshakeValidationResult;

[[nodiscard]] HttpWebSocketHandshakeValidationResult
validateHttp1WebSocketHandshake(
    const HttpRequest& request,
    const Http1RequestBodyPlan& bodyPlan) noexcept;

[[nodiscard]] HttpWebSocketHandshakeValidationResult
validateHttp2WebSocketHandshake(
    const Http2StreamState& stream,
    const HttpRequest& request) noexcept;

class HttpWebSocketHandshakeAccepted final {
private:
    friend class HttpWebSocketHandshakeValidationResult;

    constexpr HttpWebSocketHandshakeAccepted() noexcept = default;
};

class HttpWebSocketHandshakeFailure final {
public:
    [[nodiscard]] HttpProtocolError protocolError() const noexcept {
        switch (kind_) {
            case Kind::kInvalidRequest:
                return HttpProtocolError(http_status::kBadRequest, "invalid WebSocket handshake");
            case Kind::kUnsupportedVersion:
                return HttpProtocolError(http_status::kBadRequest, "unsupported WebSocket version");
        }
        return HttpProtocolError(http_status::kBadRequest, "invalid WebSocket handshake");
    }

    // RFC 6455 section 4.4 requires a server to advertise every supported
    // version when it rejects the client's requested version. Apply this
    // after the Web error handler so product customization cannot omit it.
    void applyRequiredResponseHeaders(HttpResponse& response) const {
        if (kind_ == Kind::kUnsupportedVersion) {
            setResponseHeaderStableView(
                response,
                "Sec-WebSocket-Version",
                "13");
        }
    }

private:
    friend class HttpWebSocketHandshakeValidationResult;
    friend HttpWebSocketHandshakeValidationResult
    validateHttp1WebSocketHandshake(
        const HttpRequest&,
        const Http1RequestBodyPlan&) noexcept;
    friend HttpWebSocketHandshakeValidationResult
    validateHttp2WebSocketHandshake(
        const Http2StreamState&,
        const HttpRequest&) noexcept;

    enum class Kind : std::uint8_t {
        kInvalidRequest,
        kUnsupportedVersion
    };

    explicit constexpr HttpWebSocketHandshakeFailure(Kind kind) noexcept
        : kind_(kind) {}

    Kind kind_;
};

// Acceptance and rejection are exclusive. Only rejection exposes the HTTP
// protocol error and the mandatory response-header finalization contract.
class HttpWebSocketHandshakeValidationResult final {
public:
    [[nodiscard]] constexpr const HttpWebSocketHandshakeAccepted*
    accepted() const & noexcept {
        return std::get_if<HttpWebSocketHandshakeAccepted>(&value_);
    }
    const HttpWebSocketHandshakeAccepted* accepted() const && = delete;

    [[nodiscard]] constexpr const HttpWebSocketHandshakeFailure*
    failure() const & noexcept {
        return std::get_if<HttpWebSocketHandshakeFailure>(&value_);
    }
    const HttpWebSocketHandshakeFailure* failure() const && = delete;

private:
    friend HttpWebSocketHandshakeValidationResult
    validateHttp1WebSocketHandshake(
        const HttpRequest&,
        const Http1RequestBodyPlan&) noexcept;
    friend HttpWebSocketHandshakeValidationResult
    validateHttp2WebSocketHandshake(
        const Http2StreamState&,
        const HttpRequest&) noexcept;

    using Value = std::variant<
        HttpWebSocketHandshakeAccepted,
        HttpWebSocketHandshakeFailure>;

    explicit constexpr HttpWebSocketHandshakeValidationResult(
        HttpWebSocketHandshakeAccepted accepted) noexcept
        : value_(accepted) {}

    explicit constexpr HttpWebSocketHandshakeValidationResult(
        HttpWebSocketHandshakeFailure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static constexpr HttpWebSocketHandshakeValidationResult
    makeAccepted() noexcept {
        return HttpWebSocketHandshakeValidationResult(
            HttpWebSocketHandshakeAccepted());
    }

    [[nodiscard]] static constexpr HttpWebSocketHandshakeValidationResult
    makeInvalidRequest() noexcept {
        return HttpWebSocketHandshakeValidationResult(
            HttpWebSocketHandshakeFailure(
                HttpWebSocketHandshakeFailure::Kind::kInvalidRequest));
    }

    [[nodiscard]] static constexpr HttpWebSocketHandshakeValidationResult
    makeUnsupportedVersion() noexcept {
        return HttpWebSocketHandshakeValidationResult(
            HttpWebSocketHandshakeFailure(
                HttpWebSocketHandshakeFailure::Kind::kUnsupportedVersion));
    }

    Value value_;
};

}  // namespace ruvia::detail
