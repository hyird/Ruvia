#pragma once

#include <memory>
#include <string_view>
#include <utility>

#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"

namespace ruvia::test {

// Test-only owner for the production session's mandatory connection wiring.
// Keeping these defaults here prevents test convenience from weakening the
// installed runtime contract.
class Http2SansIoSessionFixture final {
public:
    [[nodiscard]] detail::ContextServices services(const WorkerHandle& worker) const {
        return detail::ContextServices(worker, stopToken_);
    }

    [[nodiscard]] detail::Http2SansIoSessionContext context(detail::ContextServices services) {
        return detail::Http2SansIoSessionContext(
            std::move(services), options, scannerEntry, workerState);
    }

    detail::HttpServerOptions options;
    detail::ConnectionScanner::Entry scannerEntry;
    detail::HttpServerWorkerState workerState{detail::HttpServerWorkerState::kRunning};

private:
    StopToken stopToken_;
};

template <typename Stream, typename BindTransport>
Task<void> runBareHttp2SansIoSessionWith(Stream& stream, const detail::RouteTable& routes,
    WorkerMemory& worker, BindTransport bindTransport, std::string_view initialBytes) {
    Http2SansIoSessionFixture fixture;
    auto dispatcher = std::make_shared<detail::WorkerDispatcher>(
        static_cast<asio::io_context&>(stream.get_executor().context()), 64);
    const auto workerHandle = detail::WorkerHandleAccess::make(dispatcher);
    auto services = bindTransport(fixture.services(workerHandle));
    co_await detail::runHttp2SansIoSession(
        stream, routes, worker, fixture.context(services), initialBytes);
}

// Convenience for the many cleartext socket tests. TLS tests must call the
// typed helper above so the stream type cannot silently manufacture identity.
template <typename Stream>
Task<void> runBarePlainHttp2SansIoSession(Stream& stream, const detail::RouteTable& routes,
    WorkerMemory& worker, std::string_view remoteAddress, std::string_view initialBytes = {}) {
    co_await runBareHttp2SansIoSessionWith(
        stream, routes, worker,
        [remoteAddress](detail::ContextServices services) {
            return services.withPlainTransport(remoteAddress);
        },
        initialBytes);
}

template <typename Stream>
Task<void> runBareTlsHttp2SansIoSession(Stream& stream, const detail::RouteTable& routes,
    WorkerMemory& worker, std::string_view remoteAddress,
    std::string_view clientCertificateSubject = {}, std::string_view initialBytes = {}) {
    co_await runBareHttp2SansIoSessionWith(
        stream, routes, worker,
        [remoteAddress, clientCertificateSubject](detail::ContextServices services) {
            return services.withTlsTransport(remoteAddress, clientCertificateSubject);
        },
        initialBytes);
}

}  // namespace ruvia::test
