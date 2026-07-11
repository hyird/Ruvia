#pragma once

#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/detail/HttpConnectionFields.h"

#include <cstdint>

namespace ruvia::detail {

enum class Http1ConnectionDisposition : std::uint8_t {
    kReuse,
    kClose
};

// The response-side wire signal needed to make the request's persistence
// semantics unambiguous to the peer. HTTP/1.1 persistence is implicit, whereas
// an HTTP/1.0 connection can only be reused after an explicit keep-alive option.
enum class Http1ResponseConnectionSignal : std::uint8_t {
    kImplicitPersistence,
    kExplicitKeepAlive
};

// Immutable server-side connection contract. The disposition and the response
// signal are deliberately inseparable: carrying only the former through a
// runtime branch loses the HTTP-version information needed to emit a correct
// persistent response. Transformations can only tighten the plan to kClose.
class Http1ServerConnectionPlan final {
public:
    [[nodiscard]] static constexpr Http1ServerConnectionPlan close() noexcept {
        return Http1ServerConnectionPlan(
            Http1ConnectionDisposition::kClose,
            Http1ResponseConnectionSignal::kImplicitPersistence);
    }

    [[nodiscard]] constexpr Http1ConnectionDisposition disposition() const noexcept {
        return disposition_;
    }

    [[nodiscard]] constexpr Http1ResponseConnectionSignal responseSignal() const noexcept {
        return responseSignal_;
    }

    [[nodiscard]] constexpr Http1ServerConnectionPlan requireClose() const noexcept {
        return Http1ServerConnectionPlan(
            Http1ConnectionDisposition::kClose,
            responseSignal_);
    }

private:
    friend Http1ServerConnectionPlan http1PlanRequestConnection(
        HttpProtocolVersion, const HttpConnectionOptions&) noexcept;

    constexpr Http1ServerConnectionPlan(
        Http1ConnectionDisposition disposition,
        Http1ResponseConnectionSignal responseSignal) noexcept
        : disposition_(disposition), responseSignal_(responseSignal) {}

    Http1ConnectionDisposition disposition_{Http1ConnectionDisposition::kClose};
    Http1ResponseConnectionSignal responseSignal_{
        Http1ResponseConnectionSignal::kImplicitPersistence};
};

[[nodiscard]] inline Http1ServerConnectionPlan http1PlanRequestConnection(
    HttpProtocolVersion protocolVersion,
    const HttpConnectionOptions& options) noexcept {
    const bool http11 = protocolVersion == HttpProtocolVersion::kHttp11;
    const auto responseSignal = http11
        ? Http1ResponseConnectionSignal::kImplicitPersistence
        : Http1ResponseConnectionSignal::kExplicitKeepAlive;
    Http1ConnectionDisposition disposition;
    if (options.close()) {
        disposition = Http1ConnectionDisposition::kClose;
    } else if (options.keepAlive()) {
        disposition = Http1ConnectionDisposition::kReuse;
    } else {
        disposition = http11
            ? Http1ConnectionDisposition::kReuse
            : Http1ConnectionDisposition::kClose;
    }
    return Http1ServerConnectionPlan(disposition, responseSignal);
}

}  // namespace ruvia::detail
