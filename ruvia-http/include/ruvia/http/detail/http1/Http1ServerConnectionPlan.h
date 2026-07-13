#pragma once

#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/detail/HttpConnectionFields.h"

#include <cstdint>

namespace ruvia::detail {

enum class Http1ConnectionDisposition : std::uint8_t {
    kReuse,
    kClose
};

// Immutable server-side connection contract. The exact request protocol version
// and disposition are deliberately inseparable: reducing the former to a
// keep-alive signal loses the version needed by the response status-line and
// final control validation. Transformations can only tighten the plan to kClose.
class Http1ServerConnectionPlan final {
public:
    // Errors produced before a valid request version exists use the server's
    // native HTTP/1.1 response version and always close. Once parsing succeeds,
    // callers must preserve the parsed plan and use requireClose().
    [[nodiscard]] static constexpr Http1ServerConnectionPlan
    http11Close() noexcept {
        return Http1ServerConnectionPlan(
            HttpProtocolVersion::kHttp11,
            Http1ConnectionDisposition::kClose);
    }

    [[nodiscard]] constexpr HttpProtocolVersion
    protocolVersion() const noexcept {
        return protocolVersion_;
    }

    [[nodiscard]] constexpr Http1ConnectionDisposition disposition() const noexcept {
        return disposition_;
    }

    [[nodiscard]] constexpr Http1ServerConnectionPlan requireClose() const noexcept {
        return Http1ServerConnectionPlan(
            protocolVersion_,
            Http1ConnectionDisposition::kClose);
    }

private:
    friend Http1ServerConnectionPlan http1PlanHttp10RequestConnection(
        const HttpConnectionOptions&) noexcept;
    friend Http1ServerConnectionPlan http1PlanHttp11RequestConnection(
        const HttpConnectionOptions&) noexcept;

    constexpr Http1ServerConnectionPlan(
        HttpProtocolVersion protocolVersion,
        Http1ConnectionDisposition disposition) noexcept
        : protocolVersion_(protocolVersion), disposition_(disposition) {}

    HttpProtocolVersion protocolVersion_;
    Http1ConnectionDisposition disposition_;
};

[[nodiscard]] inline Http1ServerConnectionPlan
http1PlanHttp10RequestConnection(
    const HttpConnectionOptions& options) noexcept {
    return Http1ServerConnectionPlan(
        HttpProtocolVersion::kHttp10,
        !options.close() && options.keepAlive()
            ? Http1ConnectionDisposition::kReuse
            : Http1ConnectionDisposition::kClose);
}

[[nodiscard]] inline Http1ServerConnectionPlan
http1PlanHttp11RequestConnection(
    const HttpConnectionOptions& options) noexcept {
    return Http1ServerConnectionPlan(
        HttpProtocolVersion::kHttp11,
        options.close()
            ? Http1ConnectionDisposition::kClose
            : Http1ConnectionDisposition::kReuse);
}

}  // namespace ruvia::detail
