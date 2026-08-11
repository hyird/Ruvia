// Regression for failed asynchronous handshakes. The MariaDB driver owns its
// native socket; the ASIO readiness wrapper must not release or close that
// handle while unwinding the original mysql_real_connect failure.

#include "ruvia/web/db/Db.h"
#include "ruvia/web/db/DbMigration.h"

#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <thread>

namespace {

[[nodiscard]] ruvia::DbConfig closedPeerConfig(std::uint16_t port) {
    auto config = ruvia::DbConfig::mariaDb();
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

}  // namespace

int main() {
    asio::io_context peerContext(1);
    asio::ip::tcp::acceptor acceptor(
        peerContext,
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = acceptor.local_endpoint().port();
    asio::ip::tcp::socket peerSocket(peerContext);
    asio::steady_timer watchdog(peerContext, std::chrono::seconds(5));
    bool accepted = false;

    acceptor.async_accept(peerSocket, [&](std::error_code error) {
        if (!error) {
            accepted = true;
            std::error_code ignored;
            peerSocket.close(ignored);
        }
        watchdog.cancel();
    });
    watchdog.async_wait([&](std::error_code error) {
        if (!error) {
            std::error_code ignored;
            acceptor.close(ignored);
        }
    });
    std::thread peer([&] { peerContext.run(); });

    static constexpr std::array migrations{
        ruvia::DbMigration{"001_never_runs", "CREATE TABLE ruvia_never_runs (id INT)"}};
    bool threw = false;
    bool typed = false;
    std::string message;
    try {
        (void)ruvia::DbMigrator::migrate(
            closedPeerConfig(port),
            std::span<const ruvia::DbMigration>(migrations));
    } catch (const ruvia::DbError& error) {
        threw = true;
        message = error.what();
        typed = error.code() == ruvia::DbError::Code::kConnectFailed &&
                error.driver() == ruvia::DbDriver::kMariaDb &&
                error.nativeCode().has_value();
    } catch (const std::exception& error) {
        threw = true;
        message = error.what();
    }
    peer.join();

    if (!accepted) {
        std::fputs("MariaDB client did not reach the loopback peer\n", stderr);
        return 1;
    }
    if (!threw) {
        std::fputs("MariaDB handshake unexpectedly succeeded against a closed peer\n", stderr);
        return 1;
    }
    if (message.find("mysql_real_connect") == std::string::npos) {
        std::fprintf(stderr, "MariaDB cleanup replaced the handshake error: %s\n", message.c_str());
        return 1;
    }
    if (!typed || message.find("[errno=") == std::string::npos) {
        std::fprintf(stderr, "MariaDB handshake failure lost typed diagnostics: %s\n", message.c_str());
        return 1;
    }

    std::printf("MariaDB handshake failure preserved: %s\n", message.c_str());
    return 0;
}
