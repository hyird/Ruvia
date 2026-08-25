#include <ruvia/web/HttpClientTypes.h>

#include <concepts>
#include <type_traits>

static_assert(std::is_aggregate_v<ruvia::HttpClientConfig>);
static_assert(std::same_as<decltype(ruvia::HttpClientConfig{}.connectionCount), std::size_t>);

template <typename Config>
concept HasWorkerDeploymentLimits = requires(Config config) {
    config.connectionsPerWorker;
    config.maxBufferedRequestsPerWorker;
    config.maxCookiesPerWorker;
    config.maxCookieBytesPerWorker;
};

static_assert(!HasWorkerDeploymentLimits<ruvia::HttpClientConfig>);

#include <ruvia/web/HttpClientResponse.h>

static_assert(std::move_constructible<ruvia::HttpClientResponse>);

#include <ruvia/web/HttpClientHandle.h>

static_assert(std::copy_constructible<ruvia::HttpClientHandle>);

#include <ruvia/web/HttpClient.h>

static_assert(!std::copy_constructible<ruvia::HttpClient>);

int main() {
    return 0;
}
