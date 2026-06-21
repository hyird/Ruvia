#pragma once

#include <memory_resource>
#include <string_view>

#include "Http2StreamState.h"
#include "../../http/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

class Http2RequestBuilder final {
public:
    static bool build(
        Http2StreamState& stream,
        HttpRequest& request,
        std::string_view remoteAddress,
        std::pmr::memory_resource* resource) noexcept {
        request.reset();
        request.setResource(resource);
        const auto method = stream.extendedConnectWebSocket ? HttpMethod::kGet : parseMethod(stream.method);
        if (method == HttpMethod::kUnknown) {
            return false;
        }
        const auto target = stream.standardConnect
            ? std::string_view(stream.authority)
            : std::string_view(stream.path);
        if (target.empty()) {
            return false;
        }

        request.setMethod(method);
        request.setHttpVersion("HTTP/2");
        request.setTarget(target);
        if (target == "*") {
            request.setPath("*");
            request.setQueryString({});
        } else {
            const auto query = target.find('?');
            if (query == std::string_view::npos) {
                request.setPath(target);
                request.setQueryString({});
            } else {
                request.setPath(target.substr(0, query));
                request.setQueryString(target.substr(query + 1));
            }
        }
        request.setBody(stream.body);
        request.setRemoteAddress(remoteAddress);

        for (const auto& header : stream.headers) {
            if (!addHeader(request, header.name, header.value, header.kind)) {
                return false;
            }
        }
        if (!stream.hasHost && !stream.authority.empty()) {
            if (!addHeader(request, "host", stream.authority, RequestHeaderKind::kHost)) {
                return false;
            }
        }
        if (stream.hasCookie) {
            if (!addHeader(request, "cookie", stream.cookie, RequestHeaderKind::kCookie)) {
                return false;
            }
        }
        return true;
    }

private:
    static bool addHeader(
        HttpRequest& request,
        std::string_view name,
        std::string_view value,
        RequestHeaderKind kind) noexcept {
        if (!request.addHeader(HttpHeaderView{name, value})) {
            return false;
        }
        cacheKnownHeader(request, kind, value);
        return true;
    }

    static void cacheKnownHeader(
        HttpRequest& request,
        RequestHeaderKind kind,
        std::string_view value) noexcept {
        switch (kind) {
            case RequestHeaderKind::kAccept:
                request.setAcceptHeader(value);
                break;
            case RequestHeaderKind::kAcceptEncoding:
                request.setAcceptEncodingHeader(value);
                break;
            case RequestHeaderKind::kAccessControlRequestHeaders:
                request.setAccessControlRequestHeadersHeader(value);
                break;
            case RequestHeaderKind::kAccessControlRequestMethod:
                request.setAccessControlRequestMethodHeader(value);
                break;
            case RequestHeaderKind::kAuthorization:
                request.setAuthorizationHeader(value);
                break;
            case RequestHeaderKind::kContentLength:
                request.setContentLengthHeader(value);
                break;
            case RequestHeaderKind::kContentType:
                request.setContentTypeHeader(value);
                break;
            case RequestHeaderKind::kCookie:
                request.setCookieHeader(value);
                break;
            case RequestHeaderKind::kHost:
                request.setHostHeader(value);
                break;
            case RequestHeaderKind::kIfMatch:
                request.setIfMatchHeader(value);
                break;
            case RequestHeaderKind::kIfModifiedSince:
                request.setIfModifiedSinceHeader(value);
                break;
            case RequestHeaderKind::kIfNoneMatch:
                request.setIfNoneMatchHeader(value);
                break;
            case RequestHeaderKind::kIfRange:
                request.setIfRangeHeader(value);
                break;
            case RequestHeaderKind::kIfUnmodifiedSince:
                request.setIfUnmodifiedSinceHeader(value);
                break;
            case RequestHeaderKind::kOrigin:
                request.setOriginHeader(value);
                break;
            case RequestHeaderKind::kRange:
                request.setRangeHeader(value);
                break;
            case RequestHeaderKind::kUserAgent:
                request.setUserAgentHeader(value);
                break;
            case RequestHeaderKind::kConnection:
                request.setConnectionHeader(value);
                break;
            case RequestHeaderKind::kExpect:
                request.setExpectHeader(value);
                break;
            case RequestHeaderKind::kSecWebSocketKey:
                request.setSecWebSocketKeyHeader(value);
                break;
            case RequestHeaderKind::kSecWebSocketProtocol:
                request.setSecWebSocketProtocolHeader(value);
                break;
            case RequestHeaderKind::kSecWebSocketVersion:
                request.setSecWebSocketVersionHeader(value);
                break;
            case RequestHeaderKind::kTransferEncoding:
                request.setTransferEncodingHeader(value);
                break;
            case RequestHeaderKind::kUpgrade:
                request.setUpgradeHeader(value);
                break;
            case RequestHeaderKind::kOther:
                break;
        }
    }
};

}  // namespace ruvia::detail
