#pragma once

#include "ruvia/http/Http1ClosePolicy.h"
#include "ruvia/http/Http1RequestBodyPlan.h"
#include "ruvia/http/HttpProtocolVersion.h"

namespace ruvia {

// Immutable request-side HTTP/1 persistence contract. Parsing establishes the
// version and initial disposition; later body-consumption policy may only close.
class Http1RequestConnectionPlan final {
public:
    [[nodiscard]] static constexpr Http1RequestConnectionPlan http11Close() noexcept {
        return {HttpProtocolVersion::kHttp11, Http1ClosePolicy::kCloseAfterResponse};
    }

    [[nodiscard]] constexpr HttpProtocolVersion protocolVersion() const noexcept {
        return version_;
    }
    [[nodiscard]] constexpr Http1ClosePolicy disposition() const noexcept {
        return disposition_;
    }
    [[nodiscard]] constexpr Http1RequestConnectionPlan requireClose() const noexcept {
        return {version_, Http1ClosePolicy::kCloseAfterResponse};
    }

private:
    friend constexpr Http1RequestConnectionPlan planHttp10RequestConnection(bool, bool) noexcept;
    friend constexpr Http1RequestConnectionPlan planHttp11RequestConnection(bool) noexcept;

    constexpr Http1RequestConnectionPlan(HttpProtocolVersion version,
        Http1ClosePolicy disposition) noexcept
        : version_(version),
          disposition_(disposition) {}

    HttpProtocolVersion version_;
    Http1ClosePolicy disposition_;
};

[[nodiscard]] inline constexpr Http1RequestConnectionPlan planHttp10RequestConnection(
    bool close, bool keepAlive) noexcept {
    return {HttpProtocolVersion::kHttp10,
        !close && keepAlive ? Http1ClosePolicy::kAllowReuse : Http1ClosePolicy::kCloseAfterResponse};
}

[[nodiscard]] inline constexpr Http1RequestConnectionPlan planHttp11RequestConnection(
    bool close) noexcept {
    return {HttpProtocolVersion::kHttp11,
        close ? Http1ClosePolicy::kCloseAfterResponse : Http1ClosePolicy::kAllowReuse};
}

[[nodiscard]] inline constexpr Http1RequestConnectionPlan applyRequestBodyConsumption(
    Http1RequestConnectionPlan plan, Http1RequestBodyConsumption consumption) noexcept {
    return consumption == Http1RequestBodyConsumption::kComplete ? plan : plan.requireClose();
}

}  // namespace ruvia
