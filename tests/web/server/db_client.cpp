#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <future>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/EventLoopPool.h"
#include "ruvia/web/db/DbClient.h"

namespace {

class ClosingPeer final {
public:
    ClosingPeer()
        : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          socket_(io_) {
        acceptor_.async_accept(socket_, [this](std::error_code error) {
            if (error) {
                return;
            }
            std::error_code ignored;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
            socket_.close(ignored);
        });
        thread_ = std::thread([this] { io_.run(); });
    }

    ~ClosingPeer() {
        asio::post(io_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
        });
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    std::thread thread_;
};

class SilentPeer final {
public:
    SilentPeer()
        : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          socket_(io_) {
        acceptor_.async_accept(socket_, [](std::error_code) {});
        thread_ = std::thread([this] { io_.run(); });
    }

    ~SilentPeer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        socket_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    std::thread thread_;
};

[[nodiscard]] ruvia::DbConfig configure(ruvia::DbConfig config, std::uint16_t port) {
    config.host = "127.0.0.1";
    config.port = port;
    config.connectTimeout = std::chrono::seconds(1);
    return config;
}

bool attachedWorker(ruvia::DbConfig config) {
    ClosingPeer peer;
    asio::io_context io;
    auto attachment = ruvia::attachEventLoop(io);
    const auto loop = attachment.loop();
    ruvia::DbClient client(loop, configure(std::move(config), peer.port()));
    auto connected =
        asio::co_spawn(loop.executor(), ruvia::asAwaitable(client.connect()), asio::use_future);
    std::thread runner([&] { io.run(); });

    auto shutdownTask = loop.start(client.shutdown());
    shutdownTask.get();

    bool connectFailed = false;
    try {
        connected.get();
    } catch (const std::exception&) {
        connectFailed = true;
    }

    const auto correctWorker = client.worker().id() == loop.id();
    attachment.stop();
    runner.join();
    return connectFailed && correctWorker;
}

bool connectDeadlineExpires(ruvia::DbConfig config) {
    using namespace std::chrono_literals;

    SilentPeer peer;
    asio::io_context io;
    auto attachment = ruvia::attachEventLoop(io);
    const auto loop = attachment.loop();
    config = configure(std::move(config), peer.port());
    config.connectTimeout = 100ms;
    ruvia::DbClient client(loop, config);
    auto connected =
        asio::co_spawn(loop.executor(), ruvia::asAwaitable(client.connect()), asio::use_future);
    std::thread runner([&] { io.run(); });

    const bool completed = connected.wait_for(3s) == std::future_status::ready;
    bool timedOut = false;
    if (completed) {
        try {
            connected.get();
        } catch (const ruvia::DbError& error) {
            timedOut = error.code() == ruvia::DbError::Code::kTimeout;
        } catch (...) {
        }
    }

    client.close();
    attachment.stop();
    runner.join();
    return completed && timedOut;
}

bool closeBeforeDispatch(ruvia::DbConfig config) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    ruvia::DbClient client(loops.loop(0), std::move(config));
    auto connected = asio::co_spawn(
        loops.loop(0).executor(), ruvia::asAwaitable(client.connect()), asio::use_future);
    client.close();
    loops.start();

    bool connectCancelled = false;
    try {
        connected.get();
    } catch (const std::exception&) {
        connectCancelled = true;
    }

    loops.stop();
    loops.join();
    return connectCancelled;
}

bool shutdownBeforeConnect(ruvia::DbConfig config) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    auto loop = loops.loop(0);
    ruvia::DbClient client(loop, config);
    loops.start();
    auto shutdownTask = loop.start(client.shutdown());
    shutdownTask.get();
    loops.stop();
    loops.join();
    return true;
}

}  // namespace

int main() {
    try {
#ifdef RUVIA_ENABLE_MARIADB
        if (!attachedWorker(ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb})) {
            return 1;
        }
        if (!connectDeadlineExpires(ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb})) {
            return 2;
        }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
        if (!attachedWorker(ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql})) {
            return 3;
        }
        if (!connectDeadlineExpires(ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql})) {
            return 4;
        }
#endif
#ifdef RUVIA_ENABLE_MARIADB
        if (!closeBeforeDispatch(ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb})) {
#else
        if (!closeBeforeDispatch(ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql})) {
#endif
            return 5;
        }
#ifdef RUVIA_ENABLE_MARIADB
        if (!shutdownBeforeConnect(ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb})) {
#else
        if (!shutdownBeforeConnect(ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql})) {
#endif
            return 6;
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "standalone database client test failed: %s\n", error.what());
        return 100;
    }
}
