#pragma once

#include <cstdint>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia::detail {

// One protocol-level classification shared by response writers and HTTP/1 +
// HTTP/2 response parsers. It deliberately distinguishes a successful CONNECT
// tunnel and 101 protocol switch from ordinary content framing, while preserving
// the RFC 9110 Section 6.4.1 distinction between a response with zero-length
// content and a response that is defined to have no content at all.
enum class HttpResponseContentSemantics : std::uint8_t {
    kInformational,
    kProtocolSwitch,
    kConnectTunnel,
    kWithoutContent,
    kWithContent,
};

[[nodiscard]] constexpr HttpResponseContentSemantics httpResponseContentSemantics(HttpKnownMethod requestMethod, HttpStatusCode statusCode) noexcept {
    if (statusCode == http_status::kSwitchingProtocols) {
        return HttpResponseContentSemantics::kProtocolSwitch;
    }
    if (statusCode.isInformational()) {
        return HttpResponseContentSemantics::kInformational;
    }
    if (requestMethod == HttpKnownMethod::kConnect && statusCode.isSuccessful()) {
        return HttpResponseContentSemantics::kConnectTunnel;
    }
    if (requestMethod == HttpKnownMethod::kHead || statusCode == http_status::kNoContent || statusCode == http_status::kNotModified) {
        return HttpResponseContentSemantics::kWithoutContent;
    }
    return HttpResponseContentSemantics::kWithContent;
}

[[nodiscard]] inline HttpResponseContentSemantics httpResponseContentSemantics(std::string_view requestMethod, HttpStatusCode statusCode) noexcept {
    return httpResponseContentSemantics(classifyHttpMethod(requestMethod), statusCode);
}

}  // namespace ruvia::detail
