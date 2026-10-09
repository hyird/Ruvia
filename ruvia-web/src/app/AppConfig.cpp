#include <bit>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <variant>

#include "app/AppConfigMutation.h"
#include "app/AppListenerOptions.h"
#include "app/EnvState.h"
#include "server/HttpServerOptionsValidation.h"

namespace ruvia {

namespace detail {
void applyServerConfig(AppState& state, const server_config& config) {
    auto options = normalize_server_options(config, state.options);
    for (const auto& listener : state.listeners) {
        if (listener.http3) {
            validateHttp3ServerLimits(config.max_connections_per_worker,
                *listener.http3, config.max_requests_per_connection, config.worker_count);
        }
    }
    state.worker_count = config.worker_count;
    state.process_signal_handlers = config.process_signal_handlers;
    state.options = std::move(options);
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
            ruvia::ensureNonZeroOptionalPort(config.http, "HTTP listen port must not be zero");
            ruvia::ensureNonZeroOptionalPort(config.https, "HTTPS listen port must not be zero");
            if (config.http.has_value() && config.http == config.https) {
                throw std::invalid_argument("HTTP and HTTPS listen ports must be different");
            }
            if (config.autoHttpsRedirect &&
                (!config.http.has_value() || !config.https.has_value())) {
                throw std::invalid_argument(
                    "automatic HTTPS redirect requires both HTTP and HTTPS ports");
            }

            std::optional<Http3ListenConfig> effectiveHttp3;
            switch (config.http3.mode) {
                case Http3Mode::kAutomatic:
                    if (config.https.has_value()) {
                        effectiveHttp3 = config.http3;
                        effectiveHttp3->mode = Http3Mode::kEnabled;
                    }
                    break;
                case Http3Mode::kEnabled:
                    if (!config.https.has_value()) {
                        throw std::invalid_argument("HTTP/3 requires an HTTPS listen port");
                    }
                    effectiveHttp3 = config.http3;
                    break;
                case Http3Mode::kDisabled:
                    break;
                default:
                    throw std::invalid_argument("HTTP/3 mode is invalid");
            }

            if (!config.https.has_value() && detail::hasTlsConfiguration(config.tls)) {
                throw std::invalid_argument("TLS config requires an HTTPS listen port");
            }
            if (effectiveHttp3.has_value()) {
                detail::validate_http3_listen_config(*effectiveHttp3);
                detail::validateHttp3ServerLimits(state.options.maxConnections,
                    *effectiveHttp3, state.options.max_requests_per_connection,
                    state.worker_count);
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
                auto tls = detail::normalizeTlsOptions(config.tls, resource);
                tls.altSvc = detail::normalizeAltSvcAdvertisement(config.altSvc,
                    effectiveHttp3.has_value() ? config.https : std::nullopt, resource);
                replacement.emplace_back(asio::ip::tcp::endpoint(address, *config.https),
                    std::move(tls), effectiveHttp3);
            }
            state.listeners = std::move(replacement);
        });
}

App& App::server(server_config config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change server config while app is running",
        [config](detail::AppState& state) { detail::applyServerConfig(state, config); });
}

App& App::deadline(DeadlineConfig config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change the deadline while app is running", [config](detail::AppState& state) {
            ruvia::ensurePositiveDuration(
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
            parsed.trust_x_forwarded_proto(config.trust_x_forwarded_proto);
            for (const auto& cidr : config.cidrs) {
                const auto block = detail::parseTrustedProxyBlock(cidr);
                if ((block.index() != 0)) {
                    // A typo here would silently trust nothing and leave every
                    // client identified as the proxy, so it fails startup instead.
                    throw std::invalid_argument(
                        "trusted proxy must be an IP address or CIDR block");
                }
                parsed.add(std::get<0>(block));
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
