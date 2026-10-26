#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include "ruvia/web/db/db.h"

#include "test_harness.h"
#include "test_tls_identity.h"

namespace database_tls_tests {

class handshake_peer final {
public:
    handshake_peer(asio::ssl::context& tls, bool maria)
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
          stream_(io_, tls),
          deadline_(io_, std::chrono::seconds(5)),
          maria_(maria),
          port_(acceptor_.local_endpoint().port()) {
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                close();
            }
        });
        done_ = asio::co_spawn(io_, serve(), asio::use_future);
        thread_ = std::thread([this] { io_.run(); });
    }
    ~handshake_peer() {
        asio::post(io_, [this] { close(); });
        thread_.join();
    }
    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }
    [[nodiscard]] bool received_application_data() {
        done_.get();
        return received_;
    }

private:
    void close() noexcept {
        std::error_code ignored;
        acceptor_.close(ignored);
        stream_.next_layer().close(ignored);
        deadline_.cancel();
    }
    asio::awaitable<void> serve() {
        struct cleanup final {
            handshake_peer& peer_;
            ~cleanup() {
                peer_.close();
            }
        } guard{*this};
        co_await acceptor_.async_accept(stream_.next_layer(), asio::use_awaitable);
        if (maria_) {
            std::string greeting(1, '\x0a');
            greeting.append("8.0.0-test", 11);
            greeting.append("\x01\0\0\0", 4);
            greeting.append("12345678\0", 9);
            greeting.append("\x05\xaa", 2);
            greeting.push_back('\x2d');
            greeting.append("\x02\0", 2);
            greeting.push_back('\x0a');
            greeting.push_back('\0');
            greeting.push_back('\x15');
            greeting.append(10, '\0');
            greeting.append("abcdefghijkl\0", 13);
            greeting.append("mysql_native_password", 22);
            std::string packet(1, static_cast<char>(greeting.size()));
            packet.append(3, '\0');
            packet.append(greeting);
            co_await asio::async_write(stream_.next_layer(), asio::buffer(packet), asio::use_awaitable);
            std::array<unsigned char, 4> header{};
            co_await asio::async_read(stream_.next_layer(), asio::buffer(header), asio::use_awaitable);
            const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) | (std::size_t{header[2]} << 16);
            if (size != 32) {
                throw std::runtime_error("expected MariaDB SSLRequest");
            }
            std::vector<unsigned char> request(size);
            co_await asio::async_read(stream_.next_layer(), asio::buffer(request), asio::use_awaitable);
        } else {
            std::array<unsigned char, 8> request{};
            co_await asio::async_read(stream_.next_layer(), asio::buffer(request), asio::use_awaitable);
            constexpr std::array<unsigned char, 8> expected{0, 0, 0, 8, 4, 210, 22, 47};
            if (request != expected) {
                throw std::runtime_error("expected PostgreSQL SSLRequest");
            }
            co_await asio::async_write(stream_.next_layer(), asio::buffer("S", 1), asio::use_awaitable);
        }
        std::error_code error;
        co_await stream_.async_handshake(asio::ssl::stream_base::server, asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        std::array<char, 4> length{};
        const auto size = co_await asio::async_read(stream_, asio::buffer(length), asio::redirect_error(asio::use_awaitable, error));
        received_ = !error && size == length.size();
    }
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    asio::steady_timer deadline_;
    bool maria_;
    std::uint16_t port_;
    bool received_{false};
    std::future<void> done_;
    std::thread thread_;
};

bool sends_startup(ruvia::db_driver driver, const char* reference, const char* common_name,
    const char* san, std::uint64_t record_limit = std::uint64_t{1} << 23) {
    ruvia::test::tls_identity identity(common_name, san);
    handshake_peer peer(identity.context_, driver == ruvia::db_driver::mariadb);
    ruvia::db_config config{.driver_ = driver, .host_ = "127.0.0.1", .port_ = peer.port(), .username_ = "test", .database_ = "test"};
    config.tls_.ca_file_ = identity.ca_file_.string();
    if (driver == ruvia::db_driver::postgresql) {
        config.tls_.server_name_ = reference;
    }
    config.tls_.max_outgoing_records_ = record_limit;
    config.connect_timeout_ = std::chrono::seconds(3);
    bool connect_failed = false;
    try {
        (void)ruvia::db_migrator::migrate(config, {});
    } catch (const ruvia::db_error& error) {
        connect_failed = error.code() == ruvia::db_error::code_type::connect_failed;
    }
    if (!connect_failed) {
        throw std::runtime_error("test peer must end the database startup");
    }
    return peer.received_application_data();
}

}  // namespace database_tls_tests

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_TEST(postgresql_tls_checks_san_identity_before_sending_startup) {
    using namespace database_tls_tests;
    const auto driver = ruvia::db_driver::postgresql;
    RUVIA_CHECK(!sends_startup(driver, "audit.test", "audit.test", ""));
    RUVIA_CHECK(!sends_startup(driver, "127.0.0.1", "127.0.0.1", ""));
    RUVIA_CHECK(!sends_startup(driver, "127.0.0.1", "other.test", "DNS:127.0.0.1"));
    RUVIA_CHECK(!sends_startup(driver, "audit.test", "audit.test", "DNS:other.test"));
    RUVIA_CHECK(sends_startup(driver, "audit.test", "other.test", "DNS:audit.test"));
    RUVIA_CHECK(sends_startup(driver, "AUDIT.test.", "other.test", "DNS:audit.test"));
    RUVIA_CHECK(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1"));
    RUVIA_CHECK(!sends_startup(driver, "audit.test", "other.test", "DNS:audit.test", 1));
}
#endif

#ifdef RUVIA_ENABLE_MARIADB
RUVIA_TEST(mariadb_tls_checks_ip_san_even_on_loopback_before_authentication) {
    using namespace database_tls_tests;
    const auto driver = ruvia::db_driver::mariadb;
    RUVIA_CHECK(!sends_startup(driver, "127.0.0.1", "127.0.0.1", ""));
    RUVIA_CHECK(!sends_startup(driver, "127.0.0.1", "other.test", "DNS:127.0.0.1"));
    RUVIA_CHECK(!sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.2"));
    RUVIA_CHECK(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1"));
    RUVIA_CHECK(!sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1", 1));
}
#endif
