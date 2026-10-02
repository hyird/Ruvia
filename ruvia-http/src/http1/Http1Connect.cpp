#include "ruvia/http/Http1Connect.h"

#include "ruvia/http/HttpAscii.h"

namespace ruvia {
std::expected<Http1ResponseHeadPlan, HttpProtocolError> prepareHttp1ConnectResponseHead(
    const HttpResponse& response, HttpProtocolVersion version) noexcept {
    if ((version != HttpProtocolVersion::kHttp10 && version != HttpProtocolVersion::kHttp11) ||
        !response.status().isSuccessful() || response.fileBody() || !response.bodyBytes().empty()) {
        return std::unexpected(HttpProtocolError(http_status::kInternalServerError, "invalid HTTP/1 CONNECT response head"));
    }
    for (const auto& header : response.headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "content-length") || httpAsciiEqualsIgnoreCase(header.name(), "transfer-encoding")) {
            return std::unexpected(HttpProtocolError(http_status::kInternalServerError, "CONNECT response cannot contain message framing fields"));
        }
    }
    const auto body = planHttpResponseBody(HttpKnownMethod::kConnect, response.status());
    const auto connection = version == HttpProtocolVersion::kHttp10
                                ? planHttp10RequestConnection(true, false)
                                : Http1RequestConnectionPlan::http11Close();
    return http1CloseDelimitedResponseStreamHeadPlan(body, connection);
}
}  // namespace ruvia
