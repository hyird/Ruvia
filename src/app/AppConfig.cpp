#include "ruvia/app/App.h"

#include <stdexcept>
#include <utility>

#include "AppConfigGuards.h"

namespace ruvia {

App& App::loadDotenv(DotenvOptions options) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot load dotenv while app is running");

    (void)env_.loadFromExecutableDirectory(options);
    return *this;
}

App& App::loadDotenv(const std::filesystem::path& path, DotenvOptions options) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot load dotenv while app is running");

    (void)env_.loadFromFile(path, options);
    return *this;
}

App& App::setListenAddress(std::string_view address) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change listen address while app is running");
    if (address.empty()) {
        throw std::invalid_argument("listen address must not be empty");
    }

    listenAddress_.assign(address.data(), address.size());
    return *this;
}

App& App::setListenAddress(std::string_view address, std::uint16_t port) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change listen address while app is running");
    if (address.empty()) {
        throw std::invalid_argument("listen address must not be empty");
    }

    listenAddress_.assign(address.data(), address.size());
    httpListenPort_ = port;
    return *this;
}

App& App::setHttpListenPort(std::uint16_t port) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change HTTP listen port while app is running");

    httpListenPort_ = port;
    return *this;
}

App& App::setHttpsListenPort(std::uint16_t port) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change HTTPS listen port while app is running");

    httpsListenPort_ = port;
    return *this;
}

App& App::setAutoHttps(bool enabled) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change auto HTTPS while app is running");

    autoHttps_ = enabled;
    return *this;
}

App& App::setThreadNum(std::size_t threadNum) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change thread count while app is running");
    if (threadNum == 0) {
        throw std::invalid_argument("thread count must be greater than 0");
    }

    threadNum_ = threadNum;
    return *this;
}

App& App::setIdleTimeout(std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change idle timeout while app is running");
    detail::ensureNonNegativeDuration(timeout, "idle timeout must not be negative");

    options_.idleTimeout = timeout;
    return *this;
}

App& App::setConnectionScanInterval(std::chrono::milliseconds interval) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change connection scan interval while app is running");
    detail::ensureNonNegativeDuration(interval, "connection scan interval must not be negative");

    options_.scanInterval = interval;
    return *this;
}

App& App::setHeaderTimeout(std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change header timeout while app is running");
    detail::ensureNonNegativeDuration(timeout, "header timeout must not be negative");

    options_.headerTimeout = timeout;
    return *this;
}

App& App::setBodyTimeout(std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change body timeout while app is running");
    detail::ensureNonNegativeDuration(timeout, "body timeout must not be negative");

    options_.bodyTimeout = timeout;
    return *this;
}

App& App::setWriteTimeout(std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change write timeout while app is running");
    detail::ensureNonNegativeDuration(timeout, "write timeout must not be negative");

    options_.writeTimeout = timeout;
    return *this;
}

App& App::setMaxConnectionsPerWorker(std::size_t maxConnections) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change connection limit while app is running");

    options_.maxConnections = maxConnections;
    return *this;
}

App& App::setMaxRequestsPerConnection(std::size_t maxRequests) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change keep-alive request limit while app is running");

    options_.maxRequestsPerConnection = maxRequests;
    return *this;
}

App& App::setMaxBufferedBodyBytes(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change buffered body limit while app is running");

    options_.maxBufferedBodyBytes = bytes;
    return *this;
}

App& App::setMaxStreamBodyBytes(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change stream body limit while app is running");

    options_.maxStreamBodyBytes = bytes;
    return *this;
}

App& App::setMaxWebSocketMessageBytes(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change websocket message limit while app is running");

    options_.maxWebSocketMessageBytes = bytes;
    return *this;
}

App& App::setMemoryPoolConfig(MemoryPoolConfig config) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change memory pool config while app is running");
    if (config.requestInitialBufferBytes == 0) {
        throw std::invalid_argument("memory pool config values must be greater than 0");
    }

    memoryConfig_ = config;
    return *this;
}

App& App::onStart(AppHook hook) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot register onStart hook while app is running");
    onStartHooks_.push_back(std::move(hook));
    return *this;
}

App& App::onStop(AppHook hook) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot register onStop hook while app is running");
    onStopHooks_.push_back(std::move(hook));
    return *this;
}

}  // namespace ruvia
