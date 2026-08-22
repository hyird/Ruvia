#include "ruvia/web/detail/app/AppConfigMutation.h"
#include "ruvia/web/detail/app/EnvState.h"
#include "ruvia/web/detail/app/AppListenerOptions.h"

#include <algorithm>
#include <bit>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ruvia {

App& App::loadDotenv(DotenvOptions options) {
    return detail::mutateStoppedApp(*this, *state_, "cannot load dotenv while app is running", [&](detail::AppState& state) { (void)detail::loadEnvFromExecutableDirectory(state.env, options); });
}

App& App::loadDotenv(const std::filesystem::path& path, DotenvOptions options) {
    return detail::mutateStoppedApp(*this, *state_, "cannot load dotenv while app is running", [&](detail::AppState& state) { (void)detail::loadEnvFromFile(state.env, path, options); });
}

App& App::setListeners(std::vector<ListenerConfig> listeners) {
    if (listeners.empty()) {
        throw std::invalid_argument("listener list must not be empty");
    }

    const auto portOf = [](const ListenerConfig& listener) {
        return std::visit([](const auto& config) { return config.port; }, listener.listener_);
    };
    for (std::size_t i = 0; i < listeners.size(); ++i) {
        const auto port = portOf(listeners[i]);
        for (std::size_t j = i + 1; j < listeners.size(); ++j) {
            if (listeners[i].id_ == listeners[j].id_) {
                throw std::invalid_argument("listener IDs must be unique");
            }
            if (port == portOf(listeners[j])) {
                throw std::invalid_argument("listener ports must be unique");
            }
        }
        if (const auto* redirect = std::get_if<ListenerConfig::RedirectHttpToHttps>(&listeners[i].listener_); redirect != nullptr) {
            bool targetExists = false;
            for (const auto& candidate : listeners) {
                if (std::holds_alternative<ListenerConfig::Https>(candidate.listener_) && candidate.id_ == redirect->target) {
                    targetExists = true;
                    break;
                }
            }
            if (!targetExists) {
                throw std::invalid_argument("HTTP redirect target must name an HTTPS listener");
            }
        }
    }

    return detail::mutateStoppedApp(*this, *state_, "cannot change listeners while app is running", [&listeners, &portOf](detail::AppState& state) {
        auto* const resource = detail::appResource();
        std::pmr::vector<detail::AppListenerConfig> replacement(resource);
        replacement.reserve(listeners.size());
        for (const auto& listener : listeners) {
            std::visit(
                [&replacement, &listeners, &listener, &portOf, resource]<typename Listener>(const Listener& config) {
                    if constexpr (std::is_same_v<Listener, ListenerConfig::Http>) {
                        replacement.emplace_back(listener.id_, resource, config.address, config.port, detail::HttpServerListenerDefinition::PlainHttp{});
                    } else if constexpr (std::is_same_v<Listener, ListenerConfig::Https>) {
                        replacement.emplace_back(listener.id_, resource, config.address, config.port, detail::makeTlsOptions(config.tls, resource));
                    } else {
                        const auto target = std::find_if(listeners.begin(), listeners.end(), [&config](const ListenerConfig& candidate) {
                            return candidate.id_ == config.target;
                        });
                        if (target == listeners.end()) std::terminate();
                        replacement.emplace_back(listener.id_, resource, config.address, config.port, detail::HttpServerListenerDefinition::RedirectHttpToHttps{portOf(*target)});
                    }
                },
                listener.listener_);
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

App& App::setIdleTimeout(std::optional<std::chrono::milliseconds> timeout) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change idle timeout while app is running", [timeout](detail::AppState& state) {
        detail::ensurePositiveOptionalDuration(timeout, "configured idle timeout must be greater than zero");
        state.options.idleTimeout = timeout;
    });
}

App& App::setConnectionScanInterval(std::chrono::milliseconds interval) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change connection scan interval while app is running", [interval](detail::AppState& state) {
        detail::ensurePositiveDuration(interval, "connection scan interval must be greater than 0");
        state.options.scanInterval = interval;
    });
}

App& App::setRequestHeaderTimeout(std::optional<std::chrono::milliseconds> timeout) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change request header timeout while app is running", [timeout](detail::AppState& state) {
        detail::ensurePositiveOptionalDuration(timeout, "configured request header timeout must be greater than zero");
        state.options.requestHeaderTimeout = timeout;
    });
}

App& App::setRequestBodyTimeout(std::optional<std::chrono::milliseconds> timeout) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change request body timeout while app is running", [timeout](detail::AppState& state) {
        detail::ensurePositiveOptionalDuration(timeout, "configured request body timeout must be greater than zero");
        state.options.requestBodyTimeout = timeout;
    });
}

App& App::setWriteTimeout(std::optional<std::chrono::milliseconds> timeout) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change write timeout while app is running", [timeout](detail::AppState& state) {
        detail::ensurePositiveOptionalDuration(timeout, "configured write timeout must be greater than zero");
        state.options.writeTimeout = timeout;
    });
}

App& App::setMaxConnectionsPerWorker(std::optional<std::size_t> maxConnections) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change connection limit while app is running", [maxConnections](detail::AppState& state) {
        detail::ensurePositiveOptionalSize(maxConnections, "configured connection limit must be greater than zero");
        state.options.maxConnections = maxConnections;
    });
}

App& App::setMaxRequestsPerConnection(std::optional<std::size_t> maxRequests) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change requests-per-connection limit while app is running", [maxRequests](detail::AppState& state) {
        detail::ensurePositiveOptionalSize(maxRequests, "configured requests-per-connection limit must be greater than zero");
        state.options.maxRequestsPerConnection = maxRequests;
    });
}

App& App::setDeadline(std::optional<DeadlineConfig> config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change the deadline while app is running", [config](detail::AppState& state) {
        if (config) {
            detail::ensurePositiveOptionalDuration(config->handler, "handler deadline must be greater than zero");
        }
        state.options.deadline = config;
    });
}

App& App::setBodyLimit(std::size_t bytes) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change buffered body limit while app is running", [bytes](detail::AppState& state) {
        detail::ensurePositiveSize(bytes, "buffered body limit must be greater than 0");
        state.options.maxBufferedBodyBytes = bytes;
    });
}

App& App::setStreamBodyLimit(std::optional<std::size_t> bytes) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change stream body limit while app is running", [bytes](detail::AppState& state) {
        detail::ensurePositiveOptionalSize(bytes, "configured stream body limit must be greater than zero");
        state.options.maxStreamBodyBytes = bytes;
    });
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

App& App::setRateLimit(std::optional<RateLimitRule> rule) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change the default rate limit per worker while app is running", [rule](detail::AppState& state) mutable { state.options.defaultRateLimitPerWorker = std::move(rule); });
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
