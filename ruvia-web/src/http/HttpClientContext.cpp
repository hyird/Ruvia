
#include <span>
#include <string_view>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include "ruvia/http/Context.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/web/detail/client/HttpClientInternal.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/http/detail/AsciiCase.h"

namespace ruvia {

namespace {

// Request headers the outbound client manages itself (or rejects), so a proxy must not forward them.
[[nodiscard]] bool isProxyStrippedRequestHeader(std::string_view name) noexcept {
    static constexpr std::string_view kDrop[] = {
        "host", "connection", "keep-alive", "proxy-connection", "te", "trailer",
        "transfer-encoding", "upgrade", "content-length"};
    for (const auto drop : kDrop) {
        if (detail::httpAsciiEqualsIgnoreCase(name, drop)) {
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
        if (detail::httpAsciiEqualsIgnoreCase(name, drop)) {
            return true;
        }
    }
    return false;
}

}  // namespace

Task<FetchResponse> ContextClient::fetch(
    std::string_view alias,
    std::string_view path,
    FetchOptions options) const {
    if (context_.httpClients_ == nullptr) {
        throw std::logic_error(
            "no http client registered; call App::useHttpClient before run()");
    }
    auto* pool = context_.httpClients_->get(alias);
    if (pool == nullptr) {
        throw std::logic_error("http client alias not found");
    }
    co_return co_await pool->fetch(path, options, context_.resource());
}

Task<FetchResponseStream> ContextClient::fetchStream(
    std::string_view alias,
    std::string_view path,
    FetchOptions options) const {
    if (context_.httpClients_ == nullptr) {
        throw std::logic_error(
            "no http client registered; call App::useHttpClient before run()");
    }
    auto* pool = context_.httpClients_->get(alias);
    if (pool == nullptr) {
        throw std::logic_error("http client alias not found");
    }
    co_return co_await pool->fetchStream(path, options, context_.resource());
}

Task<FetchResponseStream> ContextClient::fetchStream(
    const HttpClientConfig& config,
    std::string_view target,
    FetchOptions options) const {
    if (context_.httpClients_ == nullptr) {
        throw std::logic_error("no http client subsystem; run() must have started an http client");
    }
    // Get-or-create a pooled client for this origin (LRU-bounded, reused across requests).
    auto* backend = context_.httpClients_->getOrCreate(config);
    co_return co_await backend->fetchStream(target, options, context_.resource());
}

namespace {

// Fill the outbound fetch options' method + timeouts + forwarded request headers (minus
// hop-by-hop). `forwarded` backs the borrowed header span, so it must outlive the fetch.
void fillProxyRequest(
    Context& context,
    std::pmr::vector<HttpHeaderView>& forwarded,
    FetchOptions& fetchOptions,
    const ProxyOptions& options) {
    fetchOptions.method = context.req().method();
    fetchOptions.maxRedirects = options.maxRedirects;
    fetchOptions.timeout = options.timeout;
    if (options.forwardRequestHeaders) {
        for (const auto& header : context.req().raw().headers()) {
            if (isProxyStrippedRequestHeader(header.name())) {
                continue;
            }
            forwarded.emplace_back(header.name(), header.value());
        }
        fetchOptions.headers = std::span<const HttpHeaderView>(forwarded.data(), forwarded.size());
    }
}

// Build the streaming HttpResponse from the upstream stream: status + headers (minus hop-by-hop /
// framing) passed through, body streamed as received.
[[nodiscard]] HttpResponse buildProxyResponse(
    std::pmr::memory_resource* resource, FetchResponseStream& upstream) {
    HttpResponse response(resource);
    response.status(upstream.status());
    for (const auto& header : upstream.headers()) {
        if (isProxyStrippedResponseHeader(header.name())) {
            continue;
        }
        response.header(header.name(), header.value(), HttpResponse::HeaderOptions{.append = true});
    }
    detail::setResponseStreamBody(response, upstream.takeBody());
    return response;
}

}  // namespace

Task<HttpResponse> ContextClient::proxy(
    std::string_view alias,
    std::string_view target,
    ProxyOptions options) const {
    FetchOptions fetchOptions;
    std::pmr::vector<HttpHeaderView> forwarded(context_.allocator<HttpHeaderView>());
    fillProxyRequest(context_, forwarded, fetchOptions, options);
    const auto bodyBytes = co_await context_.req().bytes();
    if (!bodyBytes.empty()) {
        fetchOptions.body = std::string_view(
            reinterpret_cast<const char*>(bodyBytes.data()), bodyBytes.size());
    }
    auto upstream = co_await fetchStream(alias, target, fetchOptions);
    co_return buildProxyResponse(context_.resource(), upstream);
}

Task<HttpResponse> ContextClient::proxy(
    const HttpClientConfig& config,
    std::string_view target,
    ProxyOptions options) const {
    FetchOptions fetchOptions;
    std::pmr::vector<HttpHeaderView> forwarded(context_.allocator<HttpHeaderView>());
    fillProxyRequest(context_, forwarded, fetchOptions, options);
    const auto bodyBytes = co_await context_.req().bytes();
    if (!bodyBytes.empty()) {
        fetchOptions.body = std::string_view(
            reinterpret_cast<const char*>(bodyBytes.data()), bodyBytes.size());
    }
    auto upstream = co_await fetchStream(config, target, fetchOptions);
    co_return buildProxyResponse(context_.resource(), upstream);
}

void Context::defer(Task<void> task) {
    if (httpClients_ == nullptr) {
        throw std::logic_error(
            "Context::defer requires an http client subsystem; call App::useHttpClient before run()");
    }
    // Spawn detached on the worker's single-threaded executor. asio::detached swallows any exception
    // that escapes the task, which is the intended fire-and-forget semantics.
    asio::co_spawn(
        httpClients_->ioContext().get_executor(),
        detail::taskAsAwaitable(std::move(task)),
        asio::detached);
}

}  // namespace ruvia
