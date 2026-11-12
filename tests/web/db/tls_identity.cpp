#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/db/db.h"

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
            std::pmr::string greeting(1, '\x0a', ruvia::process_resource());
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
            std::pmr::string packet(1, static_cast<char>(greeting.size()), ruvia::process_resource());
            packet.append(3, '\0');
            packet.append(greeting);
            co_await asio::async_write(stream_.next_layer(), asio::buffer(packet), asio::use_awaitable);
            std::array<unsigned char, 4> header{};
            co_await asio::async_read(stream_.next_layer(), asio::buffer(header), asio::use_awaitable);
            const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) | (std::size_t{header[2]} << 16);
            if (size != 32) {
                throw std::runtime_error("expected MariaDB SSLRequest");
            }
            std::pmr::vector<unsigned char> request(size, ruvia::process_resource());
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
    const char* san, std::uint64_t record_limit = std::uint64_t{1} << 23, int version = 0,
    bool elliptic_curve = false, bool mutual_tls = false, const char* cipher = nullptr) {
    ruvia::test::tls_identity identity(common_name, san, elliptic_curve);
    auto* const context = identity.context_.native_handle();
    if (version != 0) {
        // Only the adversarial test peer relaxes OpenSSL's legacy-signature
        // policy. The database client must retain the production policy.
        SSL_CTX_set_security_level(context, 0);
        if (SSL_CTX_set_min_proto_version(context, version) != 1 ||
            SSL_CTX_set_max_proto_version(context, version) != 1) {
            throw std::runtime_error("cannot pin database test peer TLS version");
        }
    }
    if (cipher != nullptr && SSL_CTX_set_cipher_list(context, cipher) != 1) {
        throw std::runtime_error("cannot pin database test peer cipher");
    }
    if (mutual_tls) {
        if (SSL_CTX_load_verify_locations(context, identity.ca_file_.string().c_str(), nullptr) != 1) {
            throw std::runtime_error("cannot trust database test client identity");
        }
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    }
    handshake_peer peer(identity.context_, driver == ruvia::db_driver::mariadb);
    ruvia::db_config config{.driver_ = driver, .host_ = "127.0.0.1", .port_ = peer.port(), .username_ = "test", .database_ = "test"};
    config.tls_.ca_file_ = identity.ca_file_;
    if (mutual_tls) {
        // libpq rejects client private keys readable by group/other users;
        // the generic server identity fixture does not restrict its key file.
        std::filesystem::permissions(identity.private_key_file(),
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        config.tls_.identity_ = ruvia::tls_identity_config{
            .certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()};
    }
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

void checks_protocol_floor(ruvia::db_driver driver) {
    constexpr auto limit = std::uint64_t{1} << 23;
    for (const bool elliptic_curve : {false, true}) {
        SCOPED_TRACE(elliptic_curve ? "ECDSA" : "RSA");
        EXPECT_TRUE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1",
            limit, TLS1_1_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "127.0.0.1", "",
            limit, TLS1_1_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.2",
            limit, TLS1_1_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1",
            1, TLS1_1_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1",
            limit, TLS1_VERSION, elliptic_curve));
        EXPECT_TRUE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1",
            limit, TLS1_1_VERSION, elliptic_curve, true));
        const auto* const cbc = elliptic_curve ? "ECDHE-ECDSA-AES128-SHA" : "ECDHE-RSA-AES128-SHA";
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1",
            limit, TLS1_2_VERSION, elliptic_curve, false, cbc));
    }
}

}  // namespace database_tls_tests

#ifdef RUVIA_ENABLE_POSTGRESQL
TEST(Ruvia, postgresql_tls_checks_san_identity_before_sending_startup) {
    using namespace database_tls_tests;
    const auto driver = ruvia::db_driver::postgresql;
    EXPECT_TRUE(!sends_startup(driver, "audit.test", "audit.test", ""));
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "127.0.0.1", ""));
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "other.test", "DNS:127.0.0.1"));
    EXPECT_TRUE(!sends_startup(driver, "audit.test", "audit.test", "DNS:other.test"));
    EXPECT_TRUE(sends_startup(driver, "audit.test", "other.test", "DNS:audit.test"));
    EXPECT_TRUE(sends_startup(driver, "AUDIT.test.", "other.test", "DNS:audit.test"));
    EXPECT_TRUE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1"));
    EXPECT_TRUE(!sends_startup(driver, "audit.test", "other.test", "DNS:audit.test", 1));
}

TEST(Ruvia, postgresql_tls_accepts_1_1_and_rejects_1_0_without_relaxing_identity_or_record_limits) {
    database_tls_tests::checks_protocol_floor(ruvia::db_driver::postgresql);
}
#endif

#ifdef RUVIA_ENABLE_MARIADB
TEST(Ruvia, mariadb_tls_checks_ip_san_even_on_loopback_before_authentication) {
    using namespace database_tls_tests;
    const auto driver = ruvia::db_driver::mariadb;
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "127.0.0.1", ""));
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "other.test", "DNS:127.0.0.1"));
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.2"));
    EXPECT_TRUE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1"));
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1", 1));
}

TEST(Ruvia, mariadb_tls_accepts_1_1_and_rejects_1_0_without_relaxing_identity_or_record_limits) {
    database_tls_tests::checks_protocol_floor(ruvia::db_driver::mariadb);
}
#endif
