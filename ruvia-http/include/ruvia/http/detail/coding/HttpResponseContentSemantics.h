#pragma once

#include <string_view>

#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

// Keep protocol implementation code on the public response contract's single
// authoritative classification.
using HttpResponseContentSemantics = ::ruvia::HttpResponseContentSemantics;

[[nodiscard]] constexpr HttpResponseContentSemantics httpResponseContentSemantics(
    HttpKnownMethod requestMethod, HttpStatusCode statusCode) noexcept {
    if (statusCode == http_status::kSwitchingProtocols) {
        return HttpResponseContentSemantics::kProtocolSwitch;
    }
    if (statusCode.isInformational()) {
        return HttpResponseContentSemantics::kInformational;
    }
    if (requestMethod == HttpKnownMethod::kConnect && statusCode.isSuccessful()) {
        return HttpResponseContentSemantics::kConnectTunnel;
    }
    if (requestMethod == HttpKnownMethod::kHead || statusCode == http_status::kNoContent ||
        statusCode == http_status::kNotModified) {
        return HttpResponseContentSemantics::kWithoutContent;
    }
    return HttpResponseContentSemantics::kWithContent;
}

[[nodiscard]] inline HttpResponseContentSemantics httpResponseContentSemantics(
    std::string_view requestMethod, HttpStatusCode statusCode) noexcept {
    return httpResponseContentSemantics(classifyHttpMethod(requestMethod), statusCode);
}

}  // namespace ruvia::detail
