#pragma once

#include <expected>

#include "ruvia/http/Http1ResponseHeadPlan.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

// The successful head ends HTTP framing and transfers the connection to a
// duplex byte stream. Payload belongs to that stream, not to an HTTP response.
[[nodiscard]] std::expected<Http1ResponseHeadPlan, HttpProtocolError> prepareHttp1ConnectResponseHead(
    const HttpResponse& response, HttpProtocolVersion version) noexcept;

}  // namespace ruvia
