#include "ruvia/web/detail/app/AppConfigMutation.h"
#include "ruvia/web/detail/app/EnvState.h"
#include "ruvia/web/detail/app/AppListenerOptions.h"
#include "ruvia/http/detail/util/AsciiCase.h"

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
    ensurePositiveSize(config.workerMailboxCapacity, "worker mailbox capacity must be greater than zero");
    ensurePositiveOptionalDuration(config.idleTimeout, "configured idle timeout must be greater than zero");
    ensurePositiveDuration(config.connectionScanInterval, "connection scan interval must be greater than zero");
    ensurePositiveOptionalDuration(config.requestHeaderTimeout, "configured request header timeout must be greater than zero");
    ensurePositiveOptionalDuration(config.requestBodyTimeout, "configured request body timeout must be greater than zero");
    ensurePositiveOptionalDuration(config.writeTimeout, "configured write timeout must be greater than zero");
    if (config.maxConnectionsPerWorker) ensurePositiveSize(*config.maxConnectionsPerWorker, "configured connection limit must be greater than zero");
    if (config.maxRequestsPerConnection) ensurePositiveSize(*config.maxRequestsPerConnection, "configured requests-per-connection limit must be greater than zero");
    ensurePositiveSize(config.maxBufferedBodyBytes, "buffered body limit must be greater than zero");
    if (config.maxStreamBodyBytes) ensurePositiveSize(*config.maxStreamBodyBytes, "configured stream body limit must be greater than zero");
    ensurePositiveSize(config.maxWebSocketMessageBytes, "websocket message limit must be greater than zero");
    ensurePositiveSize(config.memoryPool.requestInitialBufferBytes, "memory pool config values must be greater than zero");

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
    return detail::mutateStoppedApp(*this, *state_, "cannot load dotenv while app is running", [&](detail::AppState& state) { (void)detail::loadEnvFromExecutableDirectory(state.env, options); });
}

App& App::loadDotenv(const std::filesystem::path& path, DotenvOptions options) {
    return detail::mutateStoppedApp(*this, *state_, "cannot load dotenv while app is running", [&](detail::AppState& state) { (void)detail::loadEnvFromFile(state.env, path, options); });
}

App& App::listen(ListenConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change listeners while app is running", [&config](detail::AppState& state) {
        if (config.address.empty()) {
            throw std::invalid_argument("listen address must not be empty");
        }
        if (!config.http.has_value() && !config.https.has_value()) {
            throw std::invalid_argument("listen config must enable HTTP, HTTPS, or both");
        }
        detail::ensureNonZeroOptionalPort(config.http, "HTTP listen port must not be zero");
        detail::ensureNonZeroOptionalPort(config.https, "HTTPS listen port must not be zero");
        if (config.http.has_value() && config.http == config.https) {
            throw std::invalid_argument("HTTP and HTTPS listen ports must be different");
        }
        if (config.autoHttpsRedirect && (!config.http.has_value() || !config.https.has_value())) {
            throw std::invalid_argument("automatic HTTPS redirect requires both HTTP and HTTPS ports");
        }

        const bool tlsConfigured = !config.tls.certificateChainFile.empty() ||
                                   !config.tls.privateKeyFile.empty() ||
                                   !config.tls.privateKeyPassword.empty() ||
                                   config.tls.clientCertificates.verifyFile.has_value() ||
                                   config.tls.clientCertificates.requirement != TlsClientCertificateRequirement::kOptional ||
                                   !config.tls.sni.empty();
        if (!config.https.has_value()) {
            if (tlsConfigured) {
                throw std::invalid_argument("TLS config requires an HTTPS listen port");
            }
        } else {
            if (config.tls.certificateChainFile.empty()) {
                throw std::invalid_argument("TLS certificate chain file must not be empty");
            }
            if (config.tls.privateKeyFile.empty()) {
                throw std::invalid_argument("TLS private key file must not be empty");
            }
            if (config.tls.clientCertificates.verifyFile.has_value()) {
                if (config.tls.clientCertificates.verifyFile->empty()) {
                    throw std::invalid_argument("TLS client certificate CA bundle must not be empty");
                }
                const auto requirement = config.tls.clientCertificates.requirement;
                if (requirement != TlsClientCertificateRequirement::kOptional &&
                    requirement != TlsClientCertificateRequirement::kRequired) {
                    throw std::invalid_argument("TLS client certificate requirement is invalid");
                }
            } else if (config.tls.clientCertificates.requirement != TlsClientCertificateRequirement::kOptional) {
                throw std::invalid_argument("TLS client certificate requirement needs a CA bundle");
            }
            for (std::size_t i = 0; i < config.tls.sni.size(); ++i) {
                const auto& sni = config.tls.sni[i];
                detail::ensureSniHost(sni.host, "SNI host must not be empty", "SNI host is invalid");
                if (sni.certificateChainFile.empty() || sni.privateKeyFile.empty()) {
                    throw std::invalid_argument("SNI TLS identity requires certificate chain and private key files");
                }
                for (std::size_t j = i + 1; j < config.tls.sni.size(); ++j) {
                    if (detail::httpAsciiEqualsIgnoreCase(config.tls.sni[j].host, sni.host)) {
                        throw std::invalid_argument("SNI hosts must be unique");
                    }
                }
            }
        }

        auto* const resource = detail::appResource();
        std::pmr::vector<detail::AppListenerConfig> replacement(resource);
        replacement.reserve(config.http.has_value() && config.https.has_value() ? 2 : 1);
        if (config.http.has_value()) {
            if (config.autoHttpsRedirect) {
                replacement.emplace_back(resource, config.address, *config.http, detail::HttpServerListenerDefinition::RedirectHttpToHttps{*config.https});
            } else {
                replacement.emplace_back(resource, config.address, *config.http, detail::HttpServerListenerDefinition::PlainHttp{});
            }
        }
        if (config.https.has_value()) {
            replacement.emplace_back(resource, config.address, *config.https, detail::makeTlsOptions(config.tls, resource));
        }
        state.listeners = std::move(replacement);
    });
}

App& App::server(ServerConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change server config while app is running", [config = std::move(config)](detail::AppState& state) {
        detail::applyServerConfig(state, config);
    });
}

App& App::deadline(DeadlineConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change the deadline while app is running", [config](detail::AppState& state) {
        detail::ensurePositiveDuration(config.handler, "handler deadline must be greater than zero");
        state.options.deadline = config;
    });
}

App& App::deadline(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change the deadline while app is running", [](detail::AppState& state) { state.options.deadline.reset(); });
}

App& App::onStart(AppHook hook) {
    return detail::mutateStoppedApp(*this, *state_, "cannot register onStart hook while app is running", [&hook](detail::AppState& state) { state.onStartHooks.push_back(std::move(hook)); });
}

App& App::onStop(AppHook hook) {
    return detail::mutateStoppedApp(*this, *state_, "cannot register onStop hook while app is running", [&hook](detail::AppState& state) { state.onStopHooks.push_back(std::move(hook)); });
}

App& App::rateLimit(RateLimitConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change the default rate limit per worker while app is running", [config](detail::AppState& state) mutable {
        detail::validateRateLimitRule(config.rule);
        if (!std::has_single_bit(config.capacityPerWorker)) {
            throw std::invalid_argument("rate-limit capacity per worker must be a power of two");
        }
        state.options.defaultRateLimitPerWorker = std::move(config.rule);
        state.options.rateLimitCapacityPerWorker = config.capacityPerWorker;
    });
}

App& App::rateLimit(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change the default rate limit per worker while app is running", [](detail::AppState& state) { state.options.defaultRateLimitPerWorker.reset(); });
}

App& App::trustedProxies(TrustedProxyConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change trusted proxies while app is running", [config = std::move(config)](detail::AppState& state) {
        detail::TrustedProxySet parsed(detail::appResource());
        for (const auto& cidr : config.cidrs) {
            detail::TrustedProxyBlock block;
            if (!detail::parseTrustedProxyBlock(cidr, block)) {
                // A typo here would silently trust nothing and leave every
                // client identified as the proxy, so it fails startup instead.
                throw std::invalid_argument("trusted proxy must be an IP address or CIDR block");
            }
            parsed.add(block);
        }
        state.options.trustedProxies = std::move(parsed);
    });
}

App& App::trustedProxies(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change trusted proxies while app is running", [](detail::AppState& state) {
        state.options.trustedProxies = detail::TrustedProxySet(detail::appResource());
    });
}

App& App::onAccess(AccessLogCallback callback) {
    return detail::mutateStoppedApp(*this, *state_, "cannot register access-log hook while app is running", [callback = std::move(callback)](detail::AppState& state) mutable {
        state.accessLogCallback = std::move(callback);
        state.options.accessLog.callback = detail::CallbackAccess::ref(state.accessLogCallback);
    });
}

App& App::onConnectionFailure(ConnectionFailureCallback callback) {
    return detail::mutateStoppedApp(*this, *state_, "cannot register connection-failure hook while app is running", [callback = std::move(callback)](detail::AppState& state) mutable {
        state.connectionFailureCallback = std::move(callback);
        state.options.connectionFailure.callback = detail::CallbackAccess::ref(state.connectionFailureCallback);
    });
}

}  // namespace ruvia
