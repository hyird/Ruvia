// A migration runs on its own event loop, away from the worker whose scanner
// expires pool deadlines. When nothing scanned them, every DbConfig timeout was
// armed and never fired: against a backend that completes the TCP handshake and
// then says nothing -- a load balancer in front of a dead server, a firewall
// swallowing the reply -- migrate() blocked its caller forever, which is
// startup code, before anything is listening or logging.
//
// The listener here never accepts: the kernel finishes the handshake into the
// backlog, so the client connects and then waits for a greeting that never
// comes. That is the shape that used to hang.

#include "ruvia/web/db/Db.h"
#include "ruvia/web/db/DbMigration.h"
#include "ruvia/web/db/DbTypes.h"

#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <span>
#include <string>

namespace {

constexpr auto kConnectTimeout = std::chrono::milliseconds(300);
// Wide enough that a loaded machine cannot fail it, narrow enough that the
// original unbounded wait cannot pass it.
constexpr auto kBudget = std::chrono::seconds(10);

[[nodiscard]] ruvia::DbConfig silentBackendConfig(std::uint16_t port) {
#ifdef RUVIA_ENABLE_MARIADB
    auto config = ruvia::DbConfig::mariaDb();
#else
    auto config = ruvia::DbConfig::postgreSql();
#endif
    config.host = "127.0.0.1";
    config.port = port;
    config.username = "ruvia";
    config.password = "ruvia";
    config.database = "ruvia";
    config.connectTimeout = kConnectTimeout;
    config.queryTimeout = kConnectTimeout;
    config.acquireTimeout = kConnectTimeout;
    return config;
}

}  // namespace

int main() {
    asio::io_context ioContext(1);
    asio::ip::tcp::acceptor acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();

    static constexpr std::array migrations{
        ruvia::DbMigration{"001_never_runs", "CREATE TABLE IF NOT EXISTS ruvia_timeout_probe (id INT PRIMARY KEY)"}};

    const auto start = std::chrono::steady_clock::now();
    bool threw = false;
    std::string message;
    try {
        (void)ruvia::DbMigrator::migrate(silentBackendConfig(port), std::span<const ruvia::DbMigration>(migrations));
    } catch (const std::exception& error) {
        threw = true;
        message = error.what();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    if (!threw) {
        std::fprintf(stderr, "migrate() reported success against a silent backend\n");
        return 1;
    }
    if (elapsed > kBudget) {
        std::fprintf(stderr, "migrate() honoured its connect timeout only after %lldms: %s\n", static_cast<long long>(elapsedMs), message.c_str());
        return 1;
    }
    std::printf("migration timed out after %lldms: %s\n", static_cast<long long>(elapsedMs), message.c_str());
    return 0;
}
