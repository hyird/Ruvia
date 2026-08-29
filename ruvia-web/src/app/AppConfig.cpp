#include "ruvia/web/detail/app/AppConfigMutation.h"
#include "ruvia/web/detail/app/EnvState.h"
#include "ruvia/web/detail/app/AppListenerOptions.h"

#include <bit>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <utility>

namespace ruvia {

namespace detail {

void applyServerConfig(AppState& state, const ServerConfig& config) {
    ensurePositiveSize(config.workerCount, "worker count must be greater than zero");
    if (config.processSignalHandlers != ProcessSignalHandlerPolicy::kExternalOwner &&
        config.processSignalHandlers != ProcessSignalHandlerPolicy::kInstall) {
        throw std::invalid_argument("process signal handler policy is invalid");
    }
    ensurePositiveSize(
        config.workerMailboxCapacity, "worker mailbox capacity must be greater than zero");
    ensurePositiveOptionalDuration(
        config.idleTimeout, "configured idle timeout must be greater than zero");
    ensurePositiveDuration(
        config.connectionScanInterval, "connection scan interval must be greater than zero");
    ensurePositiveOptionalDuration(
        config.requestHeaderTimeout, "configured request header timeout must be greater than zero");
    ensurePositiveOptionalDuration(
        config.requestBodyTimeout, "configured request body timeout must be greater than zero");
    ensurePositiveOptionalDuration(
        config.writeTimeout, "configured write timeout must be greater than zero");
    if (config.maxConnectionsPerWorker) {
        ensurePositiveSize(*config.maxConnectionsPerWorker,
            "configured connection limit must be greater than zero");
    }
    if (config.maxRequestsPerConnection) {
        ensurePositiveSize(*config.maxRequestsPerConnection,
            "configured requests-per-connection limit must be greater than zero");
    }
    ensurePositiveSize(
        config.maxBufferedBodyBytes, "buffered body limit must be greater than zero");
    if (config.maxStreamBodyBytes) {
        ensurePositiveSize(
            *config.maxStreamBodyBytes, "configured stream body limit must be greater than zero");
    }
    ensurePositiveSize(
        config.maxWebSocketMessageBytes, "websocket message limit must be greater than zero");
    ensurePositiveSize(config.memoryPool.requestInitialBufferBytes,
        "memory pool config values must be greater than zero");

    state.workerCount = config.workerCount;
    state.processSignalHandlers = config.processSignalHandlers;
    state.options.workerMailboxCapacity = config.workerMailboxCapacity;
    state.options.idleTimeout = config.idleTimeout;
    state.options.scanInterval = config.connectionScanInterval;
    state.options.requestHeaderTimeout = config.requestHeaderTimeout;
    state.options.requestBodyTimeout = config.requestBodyTimeout;
    state.options.writeTimeout = config.writeTimeout;
    state.options.maxConnections = config.maxConnectionsPerWorker;
    state.options.maxRequestsPerConnection = config.maxRequestsPerConnection;
    state.options.maxBufferedBodyBytes = config.maxBufferedBodyBytes;
    state.options.maxStreamBodyBytes = config.maxStreamBodyBytes;
    state.options.maxWebSocketMessageBytes = config.maxWebSocketMessageBytes;
    state.options.memoryConfig = config.memoryPool;
}

}  // namespace detail

App& App::loadDotenv(DotenvOptions options) {
    return detail::mutateStoppedApp(
        *this, *state_, "cannot load dotenv while app is running", [&](detail::AppState& state) {
            (void)detail::loadEnvFromExecutableDirectory(state.env, options);
        });
}

App& App::loadDotenv(const std::filesystem::path& path, DotenvOptions options) {
    return detail::mutateStoppedApp(*this, *state_, "cannot load dotenv while app is running",
        [&](detail::AppState& state) { (void)detail::loadEnvFromFile(state.env, path, options); });
}

App& App::listen(ListenConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change listeners while app is running",
        [&config](detail::AppState& state) {
            const auto address = detail::normalizeListenAddress(config.address);
            if (!config.http.has_value() && !config.https.has_value()) {
                throw std::invalid_argument("listen config must enable HTTP, HTTPS, or both");
            }
            detail::ensureNonZeroOptionalPort(config.http, "HTTP listen port must not be zero");
            detail::ensureNonZeroOptionalPort(config.https, "HTTPS listen port must not be zero");
            if (config.http.has_value() && config.http == config.https) {
                throw std::invalid_argument("HTTP and HTTPS listen ports must be different");
            }
            if (config.autoHttpsRedirect &&
                (!config.http.has_value() || !config.https.has_value())) {
                throw std::invalid_argument(
                    "automatic HTTPS redirect requires both HTTP and HTTPS ports");
            }

            if (!config.https.has_value() && detail::hasTlsConfiguration(config.tls)) {
                throw std::invalid_argument("TLS config requires an HTTPS listen port");
            }

            auto* const resource = detail::appResource();
            std::pmr::vector<detail::HttpServerListenerDefinition> replacement(resource);
            replacement.reserve(config.http.has_value() && config.https.has_value() ? 2 : 1);
            if (config.http.has_value()) {
                const auto endpoint = asio::ip::tcp::endpoint(address, *config.http);
                if (config.autoHttpsRedirect) {
                    replacement.emplace_back(endpoint,
                        detail::HttpServerListenerDefinition::RedirectHttpToHttps{*config.https});
                } else {
                    replacement.emplace_back(endpoint);
                }
            }
            if (config.https.has_value()) {
                replacement.emplace_back(asio::ip::tcp::endpoint(address, *config.https),
                    detail::normalizeTlsOptions(config.tls, resource));
            }
            state.listeners = std::move(replacement);
        });
}

App& App::server(ServerConfig config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change server config while app is running",
        [config](detail::AppState& state) { detail::applyServerConfig(state, config); });
}

App& App::deadline(DeadlineConfig config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change the deadline while app is running", [config](detail::AppState& state) {
            detail::ensurePositiveDuration(
                config.handler, "handler deadline must be greater than zero");
            state.options.deadline = config;
        });
}

App& App::deadline(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change the deadline while app is running",
        [](detail::AppState& state) { state.options.deadline.reset(); });
}

App& App::onStart(AppHook hook) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot register onStart hook while app is running",
        [&hook](detail::AppState& state) { state.onStartHooks.push_back(std::move(hook)); });
}

App& App::onStop(AppHook hook) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot register onStop hook while app is running",
        [&hook](detail::AppState& state) { state.onStopHooks.push_back(std::move(hook)); });
}

App& App::rateLimit(RateLimitConfig config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change the default rate limit per worker while app is running",
        [config](detail::AppState& state) mutable {
            detail::validateRateLimitRule(config.rule);
            if (!std::has_single_bit(config.capacityPerWorker)) {
                throw std::invalid_argument(
                    "rate-limit capacity per worker must be a power of two");
            }
            state.options.defaultRateLimitPerWorker = config.rule;
            state.options.rateLimitCapacityPerWorker = config.capacityPerWorker;
        });
}

App& App::rateLimit(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change the default rate limit per worker while app is running",
        [](detail::AppState& state) { state.options.defaultRateLimitPerWorker.reset(); });
}

App& App::trustedProxies(TrustedProxyConfig config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change trusted proxies while app is running",
        [config = std::move(config)](detail::AppState& state) {
            detail::TrustedProxySet parsed(detail::appResource());
            for (const auto& cidr : config.cidrs) {
                detail::TrustedProxyBlock block;
                if (!detail::parseTrustedProxyBlock(cidr, block)) {
                    // A typo here would silently trust nothing and leave every
                    // client identified as the proxy, so it fails startup instead.
                    throw std::invalid_argument(
                        "trusted proxy must be an IP address or CIDR block");
                }
                parsed.add(block);
            }
            state.options.trustedProxies = std::move(parsed);
        });
}

App& App::trustedProxies(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change trusted proxies while app is running", [](detail::AppState& state) {
            state.options.trustedProxies = detail::TrustedProxySet(detail::appResource());
        });
}

App& App::onAccess(AccessLogCallback callback) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot register access-log hook while app is running",
        [callback = std::move(callback)](detail::AppState& state) mutable {
            state.accessLogCallback = std::move(callback);
            state.options.accessLog.callback = detail::CallbackAccess::ref(state.accessLogCallback);
        });
}

App& App::onConnectionFailure(ConnectionFailureCallback callback) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot register connection-failure hook while app is running",
        [callback = std::move(callback)](detail::AppState& state) mutable {
            state.connectionFailureCallback = std::move(callback);
            state.options.connectionFailure.callback =
                detail::CallbackAccess::ref(state.connectionFailureCallback);
        });
}

}  // namespace ruvia
