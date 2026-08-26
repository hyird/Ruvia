#include <chrono>
#include <cstdint>
#include <exception>
#include <cstdio>
#include <future>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/co_spawn.hpp>
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
          thread_([this] { acceptAndClose(); }) {}

    ~ClosingPeer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    void acceptAndClose() noexcept {
        try {
            asio::ip::tcp::socket socket(io_);
            acceptor_.accept(socket);
            std::error_code ignored;
            socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
            socket.close(ignored);
        } catch (...) {
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
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

    bool connectFailed = false;
    try {
        connected.get();
    } catch (const std::exception&) {
        connectFailed = true;
    }

    const auto correctWorker = client.worker().id() == loop.id();
    client.close();
    attachment.stop();
    runner.join();
    return connectFailed && correctWorker;
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

}  // namespace

int main() {
    try {
#ifdef RUVIA_ENABLE_MARIADB
        if (!attachedWorker(ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb})) {
            return 1;
        }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
        if (!attachedWorker(ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql})) {
            return 2;
        }
#endif
#ifdef RUVIA_ENABLE_MARIADB
        if (!closeBeforeDispatch(ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb})) {
#else
        if (!closeBeforeDispatch(ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql})) {
#endif
            return 3;
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "standalone database client test failed: %s\n", error.what());
        return 100;
    }
}
