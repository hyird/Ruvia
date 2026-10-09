#pragma once

#include <exception>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <variant>

#include <asio.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/Http1Connect.h"
#include "ruvia/http/HttpConnectUdp.h"
#include "ruvia/http/HttpResponseServer.h"
#include "ruvia/web/detail/util/CallableRef.h"

#include "http/HttpSocketTunnelTransport.h"
#include "http/HttpTunnelSession.h"
#include "http/TlsTunnelOutput.h"
#include "server/Http1RouteDispatch.h"
#include "server/Http1SessionRequestCompletion.h"
#include "server/HttpServerResponseState.h"

namespace ruvia::detail {

template <typename Stream>
[[nodiscard]] Task<std::optional<Http1SessionRequestCompletion>> dispatchHttpTunnelRoute(
    Http1RouteDispatch<Stream> d, const ResolvedRoute& resolved, std::string_view pending) {
    const auto& endpoint = *resolved.route().endpoint().tunnel();
    const bool udp = endpoint.protocol() == "connect-udp";
    if (d.parsed.bodyPlan.requiresConsumption() || (udp && (validateHttpConnectUdpRequest(d.parsed.request).index() != 0))) {
        d.response = co_await d.routes.handleError(d.parsed.request, d.requestMemory,
            HttpErrorInfo({.status = http_status::kBadRequest, .message = "CONNECT route does not accept HTTP request content"}), d.baseRouteServices);
        co_return Http1SessionRequestCompletion::makeBufferedClosing(
            requireHttp1FinalResponseCommit(d.response, d.parsed.connectionPlan.requireClose()));
    }
    std::optional<TlsTunnelOutput> tlsOutput;
    std::optional<HttpTunnelSession<HttpSocketTunnelTransport<Stream>>> tunnel;
    auto establishAndRun = [&](Context& context) -> Task<void> {
        auto headResponse = ContextAccess::streamingHead(context);
        if (udp) {
            auto negotiated = prepareHttpConnectUdpResponse(std::move(headResponse), d.parsed.request.protocolVersion());
            if ((negotiated.index() != 0)) {
                throw std::invalid_argument("invalid CONNECT-UDP response metadata");
            }
            headResponse = std::move(std::get<0>(negotiated));
        }
        const auto plan = [&]() -> std::variant<Http1ResponseHeadPlan, HttpProtocolError> {
            if (!udp) {
                return prepareHttp1ConnectResponseHead(headResponse, d.parsed.request.protocolVersion());
            }
            const auto upgrade = prepareHttp1ConnectUdpResponseHead(headResponse);
            if ((upgrade.index() != 0)) {
                return HttpProtocolError(http_status::kInternalServerError, "invalid CONNECT-UDP response head");
            }
            return std::get<0>(upgrade);
        }();
        if (plan.index() != 0) {
            throw std::get<1>(plan);
        }
        HttpResponseHeadBuffer head(std::pmr::polymorphic_allocator<char>(d.memory.resource()));
        appendHttp1ResponseHead(headResponse, head, std::get<0>(plan));
        ContextAccess::markTunnelHandshakeStarted(context);
        const auto written = co_await asyncAsio<std::size_t>([&](auto handler) {
            asio::async_write(d.stream, asio::buffer(head.view()), std::move(handler));
        });
        if (written.errorCode()) {
            throw std::system_error(written.errorCode(), "CONNECT head write");
        }
        if constexpr (requires { d.stream.next_layer(); }) {
            tlsOutput.emplace(*d.stream.native_handle(), d.stream.next_layer(), d.baseRouteServices.worker(), *d.memory.resource());
            tlsOutput->start();
        }
        tunnel.emplace(HttpSocketTunnelTransport<Stream>(d.stream, tlsOutput ? &*tlsOutput : nullptr),
            d.baseRouteServices.worker(), *d.memory.resource(), pending);
        co_await invokeTunnelHandler(*tunnel, d.scannerEntry, endpoint.handler(), context);
    };
    std::optional<HttpResponse> buffered;
    std::exception_ptr failure;
    try {
        buffered = co_await d.routes.dispatchTunnel(d.parsed.request, resolved, d.requestMemory,
            makeCallableRef<void, Context&>(establishAndRun), d.baseRouteServices);
    } catch (...) {
        failure = std::current_exception();
    }
    if (tunnel) {
        co_await finishTunnelSession(*tunnel, failure, d.options.connectionFailure, d.baseRouteServices.connInfo().remote().address(), d.scannerEntry, endpoint.config().peerTransportFinTimeout);
    } else if (tlsOutput) {
        tlsOutput->abort();
    }
    if (tlsOutput) {
        co_await tlsOutput->join();
    }
    if (tunnel) {
        co_return std::nullopt;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (buffered) {
        d.response = std::move(*buffered);
        co_return Http1SessionRequestCompletion::makeBufferedClosing(
            requireHttp1FinalResponseCommit(d.response, d.parsed.connectionPlan.requireClose()));
    }
    co_return std::nullopt;
}

}  // namespace ruvia::detail
