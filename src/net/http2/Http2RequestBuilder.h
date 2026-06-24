#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "../../http/HttpRequestInternal.h"
#include "Http2StreamState.h"
#include "../../http/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

class Http2RequestBuilder final {
public:
    [[nodiscard]] static HttpMethod requestMethod(const Http2StreamState& stream) noexcept {
        return stream.extendedConnectWebSocket ? HttpMethod::kGet : stream.method;
    }

    [[nodiscard]] static std::string_view requestTarget(const Http2StreamState& stream) noexcept {
        return stream.standardConnect
            ? std::string_view(stream.authority)
            : std::string_view(stream.path);
    }

    [[nodiscard]] static std::string_view requestPath(std::string_view target) noexcept {
        if (target == "*") {
            return "*";
        }
        const auto query = target.find('?');
        return query == std::string_view::npos
            ? target
            : target.substr(0, query);
    }

    [[nodiscard]] static std::string_view requestQueryString(std::string_view target) noexcept {
        if (target == "*") {
            return {};
        }
        const auto query = target.find('?');
        return query == std::string_view::npos
            ? std::string_view{}
            : target.substr(query + 1);
    }

    [[nodiscard]] static std::string_view requestPath(const Http2StreamState& stream) noexcept {
        return requestPath(requestTarget(stream));
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

        HttpRequestAccess::setMethod(request, method);
        HttpRequestAccess::setHttpVersion(request, "HTTP/2");
        HttpRequestAccess::setTarget(request, target);
        HttpRequestAccess::setPath(request, requestPath(target));
        HttpRequestAccess::setQueryString(request, requestQueryString(target));
        HttpRequestAccess::setBody(request, stream.body);

        for (std::size_t i = 0; i < stream.headers.size(); ++i) {
            const auto header = stream.headers.at(i);
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
        if (!HttpRequestAccess::addHeader(request, HttpHeaderView{name, value})) {
            return false;
        }
        cacheKnownHeader(request, kind, value);
        return true;
    }

    static void cacheKnownHeader(
        HttpRequest& request,
        RequestHeaderKind kind,
        std::string_view value) noexcept {
        HttpRequestAccess::setKnownHeaderSlot(request, requestHeaderKindKnownSlot(kind), value);
    }
};

}  // namespace ruvia::detail
