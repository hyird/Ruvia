#pragma once

#include <string_view>

#include "ruvia/web/detail/http/ContextServices.h"
#include "ruvia/web/detail/server/Http2SansIoSession.h"

namespace ruvia::test {

// Test-only owner for the production session's mandatory connection wiring.
// Keeping these defaults here prevents test convenience from weakening the
// installed runtime contract.
class Http2SansIoSessionFixture final {
public:
    [[nodiscard]] detail::Http2SansIoSessionContext context(
        detail::ContextServices services = {}) noexcept {
        return detail::Http2SansIoSessionContext(
            services,
            options,
            scannerEntry,
            workerRunning);
    }

    detail::HttpServerOptions options;
    detail::ConnectionScanner::Entry scannerEntry;
    bool workerRunning{true};
};

template <typename Stream>
Task<void> runBareHttp2SansIoSession(
    Stream& stream,
    const detail::RouteTable& routes,
    WorkerMemory& worker,
    detail::ContextServices services,
    std::string_view initialBytes = {}) {
    Http2SansIoSessionFixture fixture;
    co_await detail::runHttp2SansIoSession(
        stream,
        routes,
        worker,
        fixture.context(services),
        initialBytes);
}

// Convenience for the many cleartext socket tests. TLS tests must call the
// typed helper above so the stream type cannot silently manufacture identity.
template <typename Stream>
Task<void> runBarePlainHttp2SansIoSession(
    Stream& stream,
    const detail::RouteTable& routes,
    WorkerMemory& worker,
    std::string_view remoteAddress,
    std::string_view initialBytes = {}) {
    co_await runBareHttp2SansIoSession(
        stream,
        routes,
        worker,
        detail::ContextServices{}.withPlainTransport(remoteAddress),
        initialBytes);
}

}  // namespace ruvia::test
