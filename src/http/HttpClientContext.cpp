#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <span>
#include <string_view>
#include <vector>

#include "ruvia/http/Context.h"
#include "HeaderTokenUtils.h"
#include "HttpResponseBodyAccess.h"
#include "client/HttpClientInternal.h"
#include "client/HttpClientPool.h"

namespace ruvia {

namespace {

// Request headers the outbound client manages itself (or rejects), so a proxy must not forward them.
[[nodiscard]] bool isProxyStrippedRequestHeader(std::string_view name) noexcept {
    static constexpr std::string_view kDrop[] = {
        "host", "connection", "keep-alive", "proxy-connection", "te", "trailer",
        "transfer-encoding", "upgrade", "content-length"};
    for (const auto drop : kDrop) {
        if (detail::asciiEqualsIgnoreCase(name, drop)) {
            return true;
        }
    }
    return false;
}

// Response hop-by-hop / framing headers: the framing (chunked / Content-Length) is recomputed for
// the streamed response, and connection-management headers are per-hop. Content-Encoding is kept
// (the body is streamed through unchanged, not decoded).
[[nodiscard]] bool isProxyStrippedResponseHeader(std::string_view name) noexcept {
    static constexpr std::string_view kDrop[] = {
        "connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade",
        "content-length"};
    for (const auto drop : kDrop) {
        if (detail::asciiEqualsIgnoreCase(name, drop)) {
            return true;
        }
    }
    return false;
}

}  // namespace

Task<FetchResponse> Context::fetch(
    std::string_view alias,
    std::string_view path,
    FetchOptions options) {
    if (httpClients_ == nullptr) {
        throw std::logic_error(
            "no http client registered; call App::useHttpClient before run()");
    }
    auto* pool = httpClients_->get(alias);
    if (pool == nullptr) {
        throw std::logic_error("http client alias not found");
    }
    co_return co_await pool->fetch(path, options, resource());
}

Task<FetchResponseStream> Context::fetchStream(
    std::string_view alias,
    std::string_view path,
    FetchOptions options) {
    if (httpClients_ == nullptr) {
        throw std::logic_error(
            "no http client registered; call App::useHttpClient before run()");
    }
    auto* pool = httpClients_->get(alias);
    if (pool == nullptr) {
        throw std::logic_error("http client alias not found");
    }
    co_return co_await pool->fetchStream(path, options, resource());
}

Task<HttpResponse> Context::proxy(
    std::string_view alias,
    std::string_view target,
    ProxyOptions options) {
    FetchOptions fetchOptions;
    fetchOptions.method = req().method();
    fetchOptions.maxRedirects = options.maxRedirects;
    fetchOptions.timeout = options.timeout;

    // Forward the incoming request headers (minus hop-by-hop / client-managed). The vector backs
    // the borrowed span and the views point into the request buffer, both stable across the fetch.
    std::pmr::vector<HttpHeaderView> forwarded(allocator<HttpHeaderView>());
    if (options.forwardRequestHeaders) {
        for (const auto& header : req().raw().headers()) {
            if (isProxyStrippedRequestHeader(header.name())) {
                continue;
            }
            forwarded.emplace_back(header.name(), header.value());
        }
        fetchOptions.headers = std::span<const HttpHeaderView>(forwarded.data(), forwarded.size());
    }

    // Forward the (buffered) request body if any.
    const auto bodyBytes = co_await req().bytes();
    if (!bodyBytes.empty()) {
        fetchOptions.body = std::string_view(
            reinterpret_cast<const char*>(bodyBytes.data()), bodyBytes.size());
    }

    auto upstream = co_await fetchStream(alias, target, fetchOptions);

    HttpResponse response(resource());
    response.status(upstream.status());
    for (const auto& header : upstream.headers()) {
        if (isProxyStrippedResponseHeader(header.name())) {
            continue;
        }
        response.header(header.name(), header.value(), HttpResponse::HeaderOptions{.append = true});
    }
    // Stream the upstream body straight through: the framework writes it (chunked / h2 DATA).
    detail::setResponseStreamBody(response, upstream.takeBody());
    co_return response;
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_HTTP_CLIENT
