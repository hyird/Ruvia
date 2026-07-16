#include "ruvia/web/detail/app/AppConfigMutation.h"

#include <bit>
#include <stdexcept>
#include <utility>

namespace ruvia {

App& App::loadDotenv(DotenvOptions options) {
    return detail::mutateStoppedApp(*this, *state_, "cannot load dotenv while app is running", [&](detail::AppState& state) {
        (void)detail::loadEnvFromExecutableDirectory(state.env, options);
    });
}

App& App::loadDotenv(const std::filesystem::path& path, DotenvOptions options) {
    return detail::mutateStoppedApp(*this, *state_, "cannot load dotenv while app is running", [&](detail::AppState& state) {
        (void)detail::loadEnvFromFile(state.env, path, options);
    });
}

App& App::setListenAddress(std::string_view address) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change listen address while app is running",
        [&](detail::AppState& state) {
            if (address.empty()) {
                throw std::invalid_argument("listen address must not be empty");
            }

            state.listenAddress.assign(address.data(), address.size());
        });
}

App& App::setServerTopology(ServerTopology topology) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change server topology while app is running",
        [&topology](detail::AppState& state) {
            state.topology = std::move(topology);
        });
}

App& App::setWorkersPerListener(std::size_t workersPerListener) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change workers per listener while app is running",
        [workersPerListener](detail::AppState& state) {
            if (workersPerListener == 0) {
                throw std::invalid_argument(
                    "workers per listener must be greater than 0");
            }

            state.workersPerListener = workersPerListener;
        });
}

App& App::setWorkerMailboxCapacity(std::size_t capacity) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change worker mailbox capacity while app is running",
        [capacity](detail::AppState& state) {
            if (capacity == 0) {
                throw std::invalid_argument(
                    "worker mailbox capacity must be greater than 0");
            }
            state.options.workerMailboxCapacity = capacity;
        });
}

App& App::setKeepaliveTimeout(std::optional<std::chrono::milliseconds> timeout) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change keepalive timeout while app is running",
        [timeout](detail::AppState& state) {
            detail::ensurePositiveOptionalDuration(
                timeout,
                "configured keepalive timeout must be greater than zero");
            state.options.keepaliveTimeout = timeout;
        });
}

App& App::setShutdownGracePeriod(std::chrono::milliseconds gracePeriod) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change shutdown grace period while app is running",
        [gracePeriod](detail::AppState& state) {
            detail::ensureNonNegativeDuration(gracePeriod, "shutdown grace period must not be negative");
            state.options.shutdownGracePeriod = gracePeriod;
        });
}

App& App::setConnectionScanInterval(std::chrono::milliseconds interval) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change connection scan interval while app is running",
        [interval](detail::AppState& state) {
            detail::ensurePositiveDuration(interval, "connection scan interval must be greater than 0");
            state.options.scanInterval = interval;
        });
}

App& App::setClientHeaderTimeout(std::optional<std::chrono::milliseconds> timeout) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change client header timeout while app is running",
        [timeout](detail::AppState& state) {
            detail::ensurePositiveOptionalDuration(
                timeout,
                "configured client header timeout must be greater than zero");
            state.options.clientHeaderTimeout = timeout;
        });
}

App& App::setClientBodyTimeout(std::optional<std::chrono::milliseconds> timeout) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change client body timeout while app is running",
        [timeout](detail::AppState& state) {
            detail::ensurePositiveOptionalDuration(
                timeout,
                "configured client body timeout must be greater than zero");
            state.options.clientBodyTimeout = timeout;
        });
}

App& App::setSendTimeout(std::optional<std::chrono::milliseconds> timeout) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change send timeout while app is running",
        [timeout](detail::AppState& state) {
            detail::ensurePositiveOptionalDuration(
                timeout,
                "configured send timeout must be greater than zero");
            state.options.sendTimeout = timeout;
        });
}

App& App::setMaxConnectionsPerWorker(std::optional<std::size_t> maxConnections) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change connection limit while app is running",
        [maxConnections](detail::AppState& state) {
            detail::ensurePositiveOptionalSize(
                maxConnections,
                "configured connection limit must be greater than zero");
            state.options.maxConnections = maxConnections;
        });
}

App& App::setKeepaliveRequests(std::optional<std::size_t> maxRequests) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change keepalive request limit while app is running",
        [maxRequests](detail::AppState& state) {
            detail::ensurePositiveOptionalSize(
                maxRequests,
                "configured keepalive request limit must be greater than zero");
            state.options.keepaliveRequests = maxRequests;
        });
}

App& App::setMaxBufferedBodyBytes(std::size_t bytes) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change buffered body limit while app is running",
        [bytes](detail::AppState& state) {
            detail::ensurePositiveSize(bytes, "buffered body limit must be greater than 0");
            state.options.maxBufferedBodyBytes = bytes;
        });
}

App& App::setMaxStreamBodyBytes(std::optional<std::size_t> bytes) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change stream body limit while app is running",
        [bytes](detail::AppState& state) {
            detail::ensurePositiveOptionalSize(
                bytes,
                "configured stream body limit must be greater than zero");
            state.options.maxStreamBodyBytes = bytes;
        });
}

App& App::setMaxWebSocketMessageBytes(std::size_t bytes) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change websocket message limit while app is running",
        [bytes](detail::AppState& state) {
            detail::ensurePositiveSize(bytes, "websocket message limit must be greater than 0");
            state.options.maxWebSocketMessageBytes = bytes;
        });
}

App& App::setMemoryPoolConfig(MemoryPoolConfig config) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change memory pool config while app is running",
        [config](detail::AppState& state) {
            detail::ensurePositiveSize(
                config.requestInitialBufferBytes,
                "memory pool config values must be greater than 0");

            state.options.memoryConfig = config;
        });
}

App& App::onStart(AppHook hook) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot register onStart hook while app is running",
        [&hook](detail::AppState& state) {
            state.onStartHooks.push_back(std::move(hook));
        });
}

App& App::onStop(AppHook hook) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot register onStop hook while app is running",
        [&hook](detail::AppState& state) {
            state.onStopHooks.push_back(std::move(hook));
        });
}

App& App::setDefaultRateLimitPerWorker(std::optional<RateLimitRule> rule) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change the default rate limit per worker while app is running",
        [rule](detail::AppState& state) mutable {
            state.options.defaultRateLimitPerWorker = std::move(rule);
        });
}

App& App::setRateLimitSlotsPerWorker(std::size_t slotsPerWorker) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change rate-limit slots per worker while app is running",
        [slotsPerWorker](detail::AppState& state) {
            if (!std::has_single_bit(slotsPerWorker)) {
                throw std::invalid_argument(
                    "rate-limit slots per worker must be a power of two");
            }
            state.options.rateLimitSlotsPerWorker = slotsPerWorker;
        });
}

App& App::onAccess(AccessLogCallback callback) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot register access-log hook while app is running",
        [callback](detail::AppState& state) {
            state.options.accessLog.callback = callback;
        });
}

}  // namespace ruvia
