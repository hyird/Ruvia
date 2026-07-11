#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/http2/Http2StreamState.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

class Http2RequestBuilder final {
public:
    // RFC 8441 WebSocket CONNECT binds to the framework's GET route shape, but this
    // is route selection only. The HttpRequest built below preserves wire CONNECT.
    [[nodiscard]] static HttpKnownMethod routeMethod(const Http2StreamState& stream) noexcept {
        return stream.extendedConnectWebSocket()
            ? HttpKnownMethod::kGet
            : stream.requestKnownMethod();
    }

    [[nodiscard]] static std::string_view requestTarget(const Http2StreamState& stream) noexcept {
        return stream.standardConnect()
            ? stream.requestAuthority()
            : stream.requestPath();
    }

    [[nodiscard]] static std::string_view requestPath(const Http2StreamState& stream) noexcept {
        return splitRequestTarget(requestTarget(stream)).path;
    }

    static bool build(
        Http2StreamState& stream,
        HttpRequest& request,
        std::pmr::memory_resource* resource) noexcept {
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setResource(request, resource);
        const auto method = stream.requestMethod();
        if (method.empty()) {
            return false;
        }
        const auto target = requestTarget(stream);
        if (target.empty()) {
            return false;
        }
        RequestTargetParts targetParts;
        if (stream.standardConnect()) {
            targetParts = splitRequestTarget(target);
        } else {
            RequestTargetView targetView;
            // Extended CONNECT retains normal :scheme/:path target components. GET
            // is used only to select the origin-form target grammar; it does not
            // overwrite the wire method stored on HttpRequest.
            const auto targetMethod = stream.extendedConnect()
                ? HttpKnownMethod::kGet
                : stream.requestKnownMethod();
            if (!parseRequestTarget(targetMethod, target, targetView)) {
                return false;
            }
            targetParts = RequestTargetParts{.path = targetView.path, .queryString = targetView.query};
        }

        HttpRequestAccess::setMethod(request, method);
        HttpRequestAccess::setProtocolVersion(
            request, HttpProtocolVersion::kHttp2);
        HttpRequestAccess::setTarget(request, target);
        HttpRequestAccess::setPath(request, targetParts.path);
        HttpRequestAccess::setQueryString(request, targetParts.queryString);
        HttpRequestAccess::setBody(request, stream.requestBodyView());

        for (std::size_t i = 0; i < stream.requestHeaderCount(); ++i) {
            const auto header = stream.requestHeaderAt(i);
            if (!addHeader(request, header.name, header.value, header.kind)) {
                return false;
            }
        }
        const auto authority = stream.requestAuthority();
        if (!stream.hasHost() && !authority.empty()) {
            if (!addHeader(request, "host", authority, RequestHeaderKind::kHost)) {
                return false;
            }
        }
        if (stream.hasCookie()) {
            if (!addHeader(request, "cookie", stream.requestCookie(), RequestHeaderKind::kCookie)) {
                return false;
            }
        }
        return true;
    }

private:
    struct RequestTargetParts final {
        std::string_view path;
        std::string_view queryString;
    };

    [[nodiscard]] static RequestTargetParts splitRequestTarget(std::string_view target) noexcept {
        if (target == "*") {
            return RequestTargetParts{.path = "*", .queryString = {}};
        }
        const auto query = target.find('?');
        if (query == std::string_view::npos) {
            return RequestTargetParts{.path = target, .queryString = {}};
        }
        return RequestTargetParts{.path = target.substr(0, query), .queryString = target.substr(query + 1)};
    }

    static bool addHeader(
        HttpRequest& request,
        std::string_view name,
        std::string_view value,
        RequestHeaderKind kind) noexcept {
        return HttpRequestAccess::addHeader(
            request,
            HttpHeaderView{name, value},
            requestHeaderKindKnownSlot(kind));
    }
};

}  // namespace ruvia::detail
