#include "ruvia/edge/EdgeServer.h"
#include "ruvia/edge/EdgeTypes.h"
#include "ruvia/http/HttpStatus.h"

#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>

static_assert(!std::copy_constructible<ruvia::edge::EdgeServer>);
static_assert(!std::is_copy_assignable_v<ruvia::edge::EdgeServer>);
static_assert(!std::move_constructible<ruvia::edge::EdgeServer>);
static_assert(!std::is_move_assignable_v<ruvia::edge::EdgeServer>);

static_assert(std::same_as<
    decltype(ruvia::edge::EdgeServerOptions{}.tls),
    std::optional<ruvia::edge::EdgeTlsConfig>>);
static_assert(std::same_as<
    decltype(ruvia::edge::EdgeServerOptions{}.accessLog),
    std::function<void(const ruvia::edge::AccessLogEntry&)>>);
static_assert(std::same_as<
    decltype(ruvia::edge::EdgeServerOptions{}.taskFailure),
    std::function<void(const ruvia::edge::EdgeTaskFailure&)>>);

int main() {
    ruvia::edge::EdgeEndpoint endpoint{"127.0.0.1", 0};
    ruvia::edge::OriginSettings origin{"origin.internal", 8443, true};
    ruvia::edge::EdgeServerOptions options;
    options.maxConnections = std::size_t{512};
    options.tls = ruvia::edge::EdgeTlsConfig{
        "certificate-chain-pem",
        "private-key-pem"};

    ruvia::edge::AccessLogEntry log{
        .clientAddress = "127.0.0.1",
        .method = "GET",
        .host = "front.local",
        .target = "/asset.js",
        .status = ruvia::http_status::kOk.value(),
        .cacheResult = "MISS",
        .bytesToClient = 42};
    ruvia::edge::EdgeTaskFailure failure{
        .kind = ruvia::edge::EdgeTaskKind::kControl,
        .exception = std::make_exception_ptr(std::runtime_error("probe"))};

    return endpoint.port != 0 ||
            !origin.https ||
            origin.upstreamPort != 8443 ||
            !options.maxConnections.has_value() ||
            !options.tls.has_value() ||
            options.fetch.verifyOriginCertificate != true ||
            options.fetch.circuitFailureThreshold == 0 ||
            log.status != ruvia::http_status::kOk.value() ||
            log.bytesToClient != 42 ||
            failure.kind != ruvia::edge::EdgeTaskKind::kControl ||
            !failure.exception ||
            ruvia::edge::kEdgeTaskKindCount == 0
        ? 1
        : 0;
}
