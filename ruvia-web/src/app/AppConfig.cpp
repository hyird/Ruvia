#include "ruvia/web/detail/app/AppConfigMutation.h"

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

App& App::setHttpListenPort(std::uint16_t port) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change HTTP listen port while app is running",
        [port](detail::AppState& state) {
            detail::ensureNonZeroPort(port, "HTTP listen port must not be zero");
            state.httpListenPort = port;
        });
}

App& App::setHttpsListenPort(std::uint16_t port) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change HTTPS listen port while app is running",
        [port](detail::AppState& state) {
            detail::ensureNonZeroPort(port, "HTTPS listen port must not be zero");
            state.httpsListenPort = port;
        });
}

App& App::setAutoHttps(bool enabled) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change auto HTTPS while app is running",
        [enabled](detail::AppState& state) {
            state.autoHttps = enabled;
        });
}

App& App::setThreadNum(std::size_t threadNum) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change thread count while app is running",
        [threadNum](detail::AppState& state) {
            if (threadNum == 0) {
                throw std::invalid_argument("thread count must be greater than 0");
            }

            state.threadNum = threadNum;
        });
}

App& App::setKeepaliveTimeout(std::chrono::milliseconds timeout) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change keepalive timeout while app is running",
        [timeout](detail::AppState& state) {
            detail::ensureNonNegativeDuration(timeout, "keepalive timeout must not be negative");
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

App& App::setClientHeaderTimeout(std::chrono::milliseconds timeout) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change client header timeout while app is running",
        [timeout](detail::AppState& state) {
            detail::ensureNonNegativeDuration(timeout, "client header timeout must not be negative");
            state.options.clientHeaderTimeout = timeout;
        });
}

App& App::setClientBodyTimeout(std::chrono::milliseconds timeout) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change client body timeout while app is running",
        [timeout](detail::AppState& state) {
            detail::ensureNonNegativeDuration(timeout, "client body timeout must not be negative");
            state.options.clientBodyTimeout = timeout;
        });
}

App& App::setSendTimeout(std::chrono::milliseconds timeout) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change send timeout while app is running",
        [timeout](detail::AppState& state) {
            detail::ensureNonNegativeDuration(timeout, "send timeout must not be negative");
            state.options.sendTimeout = timeout;
        });
}

App& App::setMaxConnectionsPerWorker(std::size_t maxConnections) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change connection limit while app is running",
        [maxConnections](detail::AppState& state) {
            state.options.maxConnections = maxConnections;
        });
}

App& App::setKeepaliveRequests(std::size_t maxRequests) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change keepalive request limit while app is running",
        [maxRequests](detail::AppState& state) {
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

App& App::setMaxStreamBodyBytes(std::size_t bytes) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change stream body limit while app is running",
        [bytes](detail::AppState& state) {
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

            state.memoryConfig = config;
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

App& App::setGlobalRateLimit(RateLimitRule rule) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change the global rate limit while app is running",
        [rule](detail::AppState& state) mutable {
            state.options.rateLimit = detail::normalizeRateLimitRule(rule);
        });
}

App& App::onAccess(AccessLogCallback callback, void* user) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot register access-log hook while app is running",
        [callback, user](detail::AppState& state) {
            state.options.accessLog.callback = callback;
            state.options.accessLog.user = user;
        });
}

}  // namespace ruvia
