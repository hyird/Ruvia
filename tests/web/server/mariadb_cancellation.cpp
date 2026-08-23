// Deterministic interruption of active MariaDB I/O. A loopback peer accepts the
// connection but never sends its greeting, leaving mysql_real_connect suspended
// in a socket wait until either an operation StopToken or registry shutdown
// closes the failed lease.

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/db/DbRegistry.h"

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

struct SilentPeer final {
    asio::io_context ioContext{1};
    asio::ip::tcp::acceptor acceptor{
        ioContext,
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)};
    asio::ip::tcp::socket socket{ioContext};
    asio::steady_timer watchdog{ioContext, std::chrono::seconds(5)};
    std::array<char, 1> input{};
    bool accepted{false};
    bool disconnected{false};
    std::uint16_t port_{0};
    std::thread thread;

    SilentPeer() {
        port_ = acceptor.local_endpoint().port();
    }

    template <typename OnAccepted>
    void start(OnAccepted onAccepted) {
        acceptor.async_accept(socket, [this, onAccepted = std::move(onAccepted)](std::error_code error) mutable {
            if (error) {
                return;
            }
            accepted = true;
            onAccepted();
            readUntilClosed();
        });
        watchdog.expires_after(std::chrono::seconds(5));
        watchdog.async_wait([this](std::error_code error) {
            if (!error) {
                std::error_code ignored;
                acceptor.close(ignored);
                socket.close(ignored);
            }
        });
        thread = std::thread([this] { ioContext.run(); });
    }

    ~SilentPeer() {
        join();
    }

    void join() {
        if (thread.joinable()) {
            thread.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

private:
    void readUntilClosed() {
        socket.async_read_some(asio::buffer(input), [this](std::error_code error, std::size_t) {
            if (!error) {
                readUntilClosed();
                return;
            }
            disconnected = error == asio::error::eof ||
                error == asio::error::connection_reset;
            watchdog.cancel();
        });
    }
};

[[nodiscard]] ruvia::DbConfig silentPeerConfig(std::uint16_t port) {
    auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb};
    config.host = "127.0.0.1";
    config.port = port;
    config.username = "ruvia";
    config.password = "ruvia";
    config.database = "ruvia";
    config.connectTimeout = std::chrono::seconds(2);
    config.readTimeout = std::chrono::seconds(2);
    config.writeTimeout = std::chrono::seconds(2);
    config.queryTimeout = std::chrono::seconds(2);
    config.acquireTimeout = std::chrono::seconds(2);
    return config;
}

ruvia::Task<void> runCancelledQuery(
    ruvia::detail::DbRegistry& registry,
    ruvia::StopToken stopToken,
    std::optional<ruvia::DbError::Code>& code,
    std::string& message) {
    ruvia::detail::ScopedOperationScope operationScope;
    auto db = registry
                  .get(std::pmr::get_default_resource(), operationScope)
                  .withOptions(ruvia::OperationOptions{.stopToken = std::move(stopToken)});
    try {
        (void)co_await db.query("SELECT 1");
    } catch (const ruvia::DbError& error) {
        code = error.code();
        message = error.what();
    }
    registry.closeNow();
}

ruvia::Task<void> runClosingConnect(
    ruvia::detail::DbRegistry& registry,
    std::optional<ruvia::DbError::Code>& code,
    std::string& message) {
    try {
        co_await registry.connect();
    } catch (const ruvia::DbError& error) {
        code = error.code();
        message = error.what();
        co_return;
    }
    throw std::runtime_error("MariaDB handshake unexpectedly completed");
}

[[nodiscard]] bool exerciseStopCancellation() {
    ruvia::StopSource stopSource;
    SilentPeer peer;
    asio::io_context ioContext(1);
    auto attachment = ruvia::attachEventLoop(ioContext, {.mailboxCapacity = 16});
    const auto loop = attachment.loop();
    const auto worker = loop.handle();
    auto* resource = std::pmr::get_default_resource();
    const auto config = silentPeerConfig(peer.port());
    const std::array definitions{ruvia::detail::DbDefinition{
        std::pmr::string("default", resource),
        ruvia::detail::DbConfigStorage(config, resource)}};
    ruvia::detail::DbRegistry registry(ioContext, resource, definitions, &worker);
    std::optional<ruvia::DbError::Code> code;
    std::string message;
    std::exception_ptr failure;
    peer.start([&stopSource] { stopSource.requestStop(); });
    ruvia::detail::asyncStartTask(
        runCancelledQuery(registry, stopSource.token(), code, message),
        asio::bind_executor(ioContext.get_executor(), [&](ruvia::detail::TaskCompletionResult<void> result) {
            if (const auto* error = result.failure()) {
                failure = error->exception();
            }
            attachment.stop();
        }));
    ioContext.run();
    peer.join();

    if (failure != nullptr) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "MariaDB cancellation task failed: %s\n", error.what());
        }
        return false;
    }
    if (code != ruvia::DbError::Code::kCancelled) {
        std::fputs("active MariaDB I/O did not report kCancelled\n", stderr);
        return false;
    }
    if (!peer.accepted || !peer.disconnected) {
        std::fprintf(
            stderr,
            "cancelled MariaDB lease did not close its driver connection "
            "(accepted=%d, disconnected=%d)\n",
            peer.accepted ? 1 : 0,
            peer.disconnected ? 1 : 0);
        std::fprintf(stderr, "database result: %s\n", message.c_str());
        return false;
    }
    return true;
}

[[nodiscard]] bool exerciseRegistryClose() {
    SilentPeer peer;
    asio::io_context ioContext(1);
    auto attachment = ruvia::attachEventLoop(ioContext, {.mailboxCapacity = 16});
    const auto loop = attachment.loop();
    const auto worker = loop.handle();
    auto* resource = std::pmr::get_default_resource();
    const auto config = silentPeerConfig(peer.port());
    const std::array definitions{ruvia::detail::DbDefinition{
        std::pmr::string("default", resource),
        ruvia::detail::DbConfigStorage(config, resource)}};
    std::optional<ruvia::DbError::Code> code;
    std::string message;
    std::exception_ptr failure;
    unsigned closeCalls = 0;
    unsigned completions = 0;
    bool closeRanOnWorker = false;
    bool taskCompleted = false;

    {
        ruvia::detail::DbRegistry registry(ioContext, resource, definitions, &worker);
        asio::steady_timer closeTimer(ioContext);
        peer.start([&] {
            asio::post(ioContext, [&] {
                closeTimer.expires_after(std::chrono::milliseconds(25));
                closeTimer.async_wait([&](std::error_code error) {
                    if (error) {
                        return;
                    }
                    closeRanOnWorker = worker.isCurrent();
                    ++closeCalls;
                    registry.closeNow();
                });
            });
        });
        ruvia::detail::asyncStartTask(
            runClosingConnect(registry, code, message),
            asio::bind_executor(ioContext.get_executor(), [&](ruvia::detail::TaskCompletionResult<void> result) {
                ++completions;
                taskCompleted = true;
                if (const auto* error = result.failure()) {
                    failure = error->exception();
                }
                attachment.stop();
            }));
        ioContext.run();

        if (!taskCompleted) {
            std::fputs("MariaDB close task did not complete before registry destruction\n", stderr);
            return false;
        }
    }
    // Reaching this point proves destruction after the completed task did not
    // hit the active-wait termination guard.
    peer.join();

    if (failure != nullptr) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "MariaDB registry close task failed: %s\n", error.what());
        }
        return false;
    }
    if (closeCalls != 1 || completions != 1 || !closeRanOnWorker) {
        std::fprintf(
            stderr,
            "MariaDB registry close did not run exactly once on its worker "
            "(close=%u, completions=%u, on-worker=%d)\n",
            closeCalls,
            completions,
            closeRanOnWorker ? 1 : 0);
        return false;
    }
    if (code != ruvia::DbError::Code::kClosing) {
        std::fprintf(
            stderr,
            "active MariaDB handshake did not report kClosing: %s\n",
            message.c_str());
        return false;
    }
    if (!peer.accepted || !peer.disconnected) {
        std::fprintf(
            stderr,
            "closing MariaDB registry did not close its driver connection "
            "(accepted=%d, disconnected=%d)\n",
            peer.accepted ? 1 : 0,
            peer.disconnected ? 1 : 0);
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!exerciseStopCancellation()) {
        return 1;
    }
    if (!exerciseRegistryClose()) {
        return 1;
    }
    std::puts(
        "active MariaDB I/O interruption reported kCancelled/kClosing and "
        "closed both driver connections");
    return 0;
}
