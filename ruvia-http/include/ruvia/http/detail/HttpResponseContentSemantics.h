#pragma once

#include <cstdint>
#include <string_view>
#include <variant>

#include "ruvia/http/HttpKnownMethod.h"

namespace ruvia::detail {

class HttpResponseContentSemantics;

class HttpInformationalResponseContent final {
private:
    friend class HttpResponseContentSemantics;

    constexpr HttpInformationalResponseContent() noexcept = default;
};

class HttpProtocolSwitchResponseContent final {
private:
    friend class HttpResponseContentSemantics;

    constexpr HttpProtocolSwitchResponseContent() noexcept = default;
};

class HttpConnectTunnelResponseContent final {
private:
    friend class HttpResponseContentSemantics;

    constexpr HttpConnectTunnelResponseContent() noexcept = default;
};

class HttpResponseWithoutContent final {
private:
    friend class HttpResponseContentSemantics;

    constexpr HttpResponseWithoutContent() noexcept = default;
};

class HttpResponseWithContent final {
private:
    friend class HttpResponseContentSemantics;

    constexpr HttpResponseWithContent() noexcept = default;
};

// One protocol-level classification shared by response writers and HTTP/1 +
// HTTP/2 response parsers. It deliberately distinguishes a successful CONNECT
// tunnel and 101 protocol switch from ordinary content framing, while preserving
// the RFC 9110 Section 6.4.1 distinction between a response with zero-length
// content and a response that is defined to have no content at all.
class HttpResponseContentSemantics final {
public:
    [[nodiscard]] constexpr const HttpInformationalResponseContent*
    informational() const & noexcept {
        return std::get_if<HttpInformationalResponseContent>(&state_);
    }
    [[nodiscard]] constexpr const HttpInformationalResponseContent*
    informational() const && = delete;

    [[nodiscard]] constexpr const HttpProtocolSwitchResponseContent*
    protocolSwitch() const & noexcept {
        return std::get_if<HttpProtocolSwitchResponseContent>(&state_);
    }
    [[nodiscard]] constexpr const HttpProtocolSwitchResponseContent*
    protocolSwitch() const && = delete;

    [[nodiscard]] constexpr const HttpConnectTunnelResponseContent*
    connectTunnel() const & noexcept {
        return std::get_if<HttpConnectTunnelResponseContent>(&state_);
    }
    [[nodiscard]] constexpr const HttpConnectTunnelResponseContent*
    connectTunnel() const && = delete;

    [[nodiscard]] constexpr const HttpResponseWithoutContent*
    withoutContent() const & noexcept {
        return std::get_if<HttpResponseWithoutContent>(&state_);
    }
    [[nodiscard]] constexpr const HttpResponseWithoutContent*
    withoutContent() const && = delete;

    [[nodiscard]] constexpr const HttpResponseWithContent*
    withContent() const & noexcept {
        return std::get_if<HttpResponseWithContent>(&state_);
    }
    [[nodiscard]] constexpr const HttpResponseWithContent*
    withContent() const && = delete;

private:
    friend constexpr HttpResponseContentSemantics httpResponseContentSemantics(
        HttpKnownMethod, std::uint16_t) noexcept;

    using State = std::variant<
        HttpInformationalResponseContent,
        HttpProtocolSwitchResponseContent,
        HttpConnectTunnelResponseContent,
        HttpResponseWithoutContent,
        HttpResponseWithContent>;

    [[nodiscard]] static constexpr State informationalState() noexcept {
        return State(HttpInformationalResponseContent());
    }

    [[nodiscard]] static constexpr State protocolSwitchState() noexcept {
        return State(HttpProtocolSwitchResponseContent());
    }

    [[nodiscard]] static constexpr State connectTunnelState() noexcept {
        return State(HttpConnectTunnelResponseContent());
    }

    [[nodiscard]] static constexpr State withoutContentState() noexcept {
        return State(HttpResponseWithoutContent());
    }

    [[nodiscard]] static constexpr State withContentState() noexcept {
        return State(HttpResponseWithContent());
    }

    explicit constexpr HttpResponseContentSemantics(State state) noexcept
        : state_(state) {}

    State state_;
};

[[nodiscard]] constexpr HttpResponseContentSemantics
httpResponseContentSemantics(
    HttpKnownMethod requestMethod,
    std::uint16_t statusCode) noexcept {
    if (statusCode == 101) {
        return HttpResponseContentSemantics(
            HttpResponseContentSemantics::protocolSwitchState());
    }
    if (statusCode >= 100 && statusCode < 200) {
        return HttpResponseContentSemantics(
            HttpResponseContentSemantics::informationalState());
    }
    if (requestMethod == HttpKnownMethod::kConnect &&
        statusCode >= 200 && statusCode < 300) {
        return HttpResponseContentSemantics(
            HttpResponseContentSemantics::connectTunnelState());
    }
    if (requestMethod == HttpKnownMethod::kHead ||
        statusCode == 204 || statusCode == 304) {
        return HttpResponseContentSemantics(
            HttpResponseContentSemantics::withoutContentState());
    }
    return HttpResponseContentSemantics(
        HttpResponseContentSemantics::withContentState());
}

[[nodiscard]] inline HttpResponseContentSemantics
httpResponseContentSemantics(
    std::string_view requestMethod,
    std::uint16_t statusCode) noexcept {
    return httpResponseContentSemantics(
        classifyHttpMethod(requestMethod), statusCode);
}

}  // namespace ruvia::detail
