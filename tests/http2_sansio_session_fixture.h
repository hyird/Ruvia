#pragma once

#include <atomic>
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
        std::string_view remoteAddress,
        detail::ContextServices services = {},
        std::string_view clientCertificate = {}) noexcept {
        return detail::Http2SansIoSessionContext(
            services,
            options,
            scannerEntry,
            serverStarted,
            remoteAddress,
            clientCertificate);
    }

    HttpServerOptions options;
    detail::ConnectionScanner::Entry scannerEntry;
    std::atomic_bool serverStarted{true};
};

template <typename Stream>
Task<void> runBareHttp2SansIoSession(
    Stream& stream,
    const detail::RouteTable& routes,
    WorkerMemory& worker,
    std::string_view remoteAddress,
    std::string_view initialBytes = {}) {
    Http2SansIoSessionFixture fixture;
    co_await detail::runHttp2SansIoSession(
        stream,
        routes,
        worker,
        fixture.context(remoteAddress),
        initialBytes);
}

}  // namespace ruvia::test
