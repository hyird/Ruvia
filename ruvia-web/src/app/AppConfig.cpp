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

App& App::setWorkerCount(std::size_t workerCount) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change worker count while app is running", [workerCount](detail::AppState& state) {
        if (workerCount == 0) {
            throw std::invalid_argument("worker count must be greater than 0");
        }

        state.workerCount = workerCount;
    });
}

App& App::setProcessSignalHandlers(ProcessSignalHandlerPolicy policy) {
    if (policy != ProcessSignalHandlerPolicy::kExternalOwner &&
        policy != ProcessSignalHandlerPolicy::kInstall) {
        throw std::invalid_argument("process signal handler policy is invalid");
    }
    return detail::mutateStoppedApp(*this, *state_, "cannot change process signal handlers while app is running", [policy](detail::AppState& state) { state.processSignalHandlers = policy; });
}

App& App::setWorkerMailboxCapacity(std::size_t capacity) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change worker mailbox capacity while app is running", [capacity](detail::AppState& state) {
        if (capacity == 0) {
            throw std::invalid_argument("worker mailbox capacity must be greater than 0");
        }
        state.options.workerMailboxCapacity = capacity;
    });
}

App& App::setHttpClientOriginCacheCapacityPerWorker(std::size_t capacityPerWorker) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change HTTP client origin cache capacity while app is running", [capacityPerWorker](detail::AppState& state) {
        if (capacityPerWorker == 0) {
            throw std::invalid_argument("HTTP client origin cache capacity must be greater than 0");
        }
        state.options.httpClientOriginCacheCapacityPerWorker = capacityPerWorker;
    });
}

App& App::setIdleTimeout(std::chrono::milliseconds timeout) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change idle timeout while app is running", [timeout](detail::AppState& state) {
        detail::ensurePositiveDuration(timeout, "configured idle timeout must be greater than zero");
        state.options.idleTimeout = timeout;
    });
}

App& App::setIdleTimeout(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change idle timeout while app is running", [](detail::AppState& state) { state.options.idleTimeout.reset(); });
}

App& App::setConnectionScanInterval(std::chrono::milliseconds interval) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change connection scan interval while app is running", [interval](detail::AppState& state) {
        detail::ensurePositiveDuration(interval, "connection scan interval must be greater than 0");
        state.options.scanInterval = interval;
    });
}

App& App::setRequestHeaderTimeout(std::chrono::milliseconds timeout) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change request header timeout while app is running", [timeout](detail::AppState& state) {
        detail::ensurePositiveDuration(timeout, "configured request header timeout must be greater than zero");
        state.options.requestHeaderTimeout = timeout;
    });
}

App& App::setRequestHeaderTimeout(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change request header timeout while app is running", [](detail::AppState& state) { state.options.requestHeaderTimeout.reset(); });
}

App& App::setRequestBodyTimeout(std::chrono::milliseconds timeout) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change request body timeout while app is running", [timeout](detail::AppState& state) {
        detail::ensurePositiveDuration(timeout, "configured request body timeout must be greater than zero");
        state.options.requestBodyTimeout = timeout;
    });
}

App& App::setRequestBodyTimeout(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change request body timeout while app is running", [](detail::AppState& state) { state.options.requestBodyTimeout.reset(); });
}

App& App::setWriteTimeout(std::chrono::milliseconds timeout) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change write timeout while app is running", [timeout](detail::AppState& state) {
        detail::ensurePositiveDuration(timeout, "configured write timeout must be greater than zero");
        state.options.writeTimeout = timeout;
    });
}

App& App::setWriteTimeout(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change write timeout while app is running", [](detail::AppState& state) { state.options.writeTimeout.reset(); });
}

App& App::setMaxConnectionsPerWorker(std::size_t maxConnections) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change connection limit while app is running", [maxConnections](detail::AppState& state) {
        detail::ensurePositiveSize(maxConnections, "configured connection limit must be greater than zero");
        state.options.maxConnections = maxConnections;
    });
}

App& App::setMaxConnectionsPerWorker(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change connection limit while app is running", [](detail::AppState& state) { state.options.maxConnections.reset(); });
}

App& App::setMaxRequestsPerConnection(std::size_t maxRequests) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change requests-per-connection limit while app is running", [maxRequests](detail::AppState& state) {
        detail::ensurePositiveSize(maxRequests, "configured requests-per-connection limit must be greater than zero");
        state.options.maxRequestsPerConnection = maxRequests;
    });
}

App& App::setMaxRequestsPerConnection(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change requests-per-connection limit while app is running", [](detail::AppState& state) { state.options.maxRequestsPerConnection.reset(); });
}

App& App::setDeadline(DeadlineConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change the deadline while app is running", [config](detail::AppState& state) {
        detail::ensurePositiveOptionalDuration(config.handler, "handler deadline must be greater than zero");
        state.options.deadline = config;
    });
}

App& App::setDeadline(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change the deadline while app is running", [](detail::AppState& state) { state.options.deadline.reset(); });
}

App& App::setBodyLimit(std::size_t bytes) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change buffered body limit while app is running", [bytes](detail::AppState& state) {
        detail::ensurePositiveSize(bytes, "buffered body limit must be greater than 0");
        state.options.maxBufferedBodyBytes = bytes;
    });
}

App& App::setStreamBodyLimit(std::size_t bytes) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change stream body limit while app is running", [bytes](detail::AppState& state) {
        detail::ensurePositiveSize(bytes, "configured stream body limit must be greater than zero");
        state.options.maxStreamBodyBytes = bytes;
    });
}

App& App::setStreamBodyLimit(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change stream body limit while app is running", [](detail::AppState& state) { state.options.maxStreamBodyBytes.reset(); });
}

App& App::setMaxWebSocketMessageBytes(std::size_t bytes) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change websocket message limit while app is running", [bytes](detail::AppState& state) {
        detail::ensurePositiveSize(bytes, "websocket message limit must be greater than 0");
        state.options.maxWebSocketMessageBytes = bytes;
    });
}

App& App::setMemoryPoolConfig(MemoryPoolConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change memory pool config while app is running", [config](detail::AppState& state) {
        detail::ensurePositiveSize(config.requestInitialBufferBytes, "memory pool config values must be greater than 0");

        state.options.memoryConfig = config;
    });
}

App& App::onStart(AppHook hook) {
    return detail::mutateStoppedApp(*this, *state_, "cannot register onStart hook while app is running", [&hook](detail::AppState& state) { state.onStartHooks.push_back(std::move(hook)); });
}

App& App::onStop(AppHook hook) {
    return detail::mutateStoppedApp(*this, *state_, "cannot register onStop hook while app is running", [&hook](detail::AppState& state) { state.onStopHooks.push_back(std::move(hook)); });
}

App& App::setRateLimit(RateLimitRule rule) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change the default rate limit per worker while app is running", [rule](detail::AppState& state) mutable {
        detail::validateRateLimitRule(rule);
        state.options.defaultRateLimitPerWorker = std::move(rule);
    });
}

App& App::setRateLimit(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change the default rate limit per worker while app is running", [](detail::AppState& state) { state.options.defaultRateLimitPerWorker.reset(); });
}

App& App::setTrustedProxies(std::span<const std::string_view> cidrs) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change trusted proxies while app is running", [cidrs](detail::AppState& state) {
        detail::TrustedProxySet parsed(detail::appResource());
        for (const auto cidr : cidrs) {
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

App& App::setRateLimitCapacityPerWorker(std::size_t capacityPerWorker) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change rate-limit capacity per worker while app is running", [capacityPerWorker](detail::AppState& state) {
        if (!std::has_single_bit(capacityPerWorker)) {
            throw std::invalid_argument("rate-limit capacity per worker must be a power of two");
        }
        state.options.rateLimitCapacityPerWorker = capacityPerWorker;
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
