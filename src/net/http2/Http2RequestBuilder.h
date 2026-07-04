#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "../../http/HttpRequestInternal.h"
#include "Http2StreamState.h"
#include "../../http/parser/HttpRequestTarget.h"
#include "../../http/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

class Http2RequestBuilder final {
public:
    [[nodiscard]] static HttpMethod requestMethod(const Http2StreamState& stream) noexcept {
        return stream.extendedConnectWebSocket()
            ? HttpMethod::kGet
            : stream.requestMethod();
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
        const auto method = requestMethod(stream);
        if (method == HttpMethod::kUnknown) {
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
            if (!parseRequestTarget(method, target, targetView)) {
                return false;
            }
            targetParts = RequestTargetParts{.path = targetView.path, .queryString = targetView.query};
        }

        HttpRequestAccess::setMethod(request, method);
        HttpRequestAccess::setHttpVersion(request, "HTTP/2");
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
