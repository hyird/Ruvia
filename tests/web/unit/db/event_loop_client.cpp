#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/web/db/DbClient.h"

#include "test_harness.h"

namespace {

// Minimal PostgreSQL startup peer. Authentication is explicitly released by
// the test so concurrent connect/shutdown ordering needs no sleeps or server.
class PostgreSqlStartupPeer final {
public:
    PostgreSqlStartupPeer()
        : acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          socket_(io_),
          gate_(io_, std::chrono::steady_clock::time_point::max()),
          port_(acceptor_.local_endpoint().port()),
          work_(asio::make_work_guard(io_)),
          done_(asio::co_spawn(io_, serve(), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    ~PostgreSqlStartupPeer() {
        asio::post(io_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
            gate_.cancel();
            work_.reset();
        });
        thread_.join();
        try {
            done_.get();
        } catch (const std::system_error&) {
        }
    }

    ruvia::DbConfig config() const {
        return {.driver = ruvia::DbDriver::kPostgreSql, .host = "127.0.0.1", .port = port_, .username = "test", .database = "test", .connectTimeout = std::chrono::seconds(5)};
    }

    void waitForStartup() {
        auto started = started_.get_future();
        if (started.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("PostgreSQL test peer did not receive startup");
        }
        started.get();
    }

    void authenticate() {
        asio::post(io_, [this] { gate_.cancel(); });
    }

private:
    static std::uint32_t integer(const unsigned char* bytes) {
        return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
               (std::uint32_t{bytes[2]} << 8) | std::uint32_t{bytes[3]};
    }

    static void appendInteger(std::string& bytes, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<char>((value >> shift) & 255));
        }
    }

    static void message(std::string& bytes, char type, std::string_view payload) {
        bytes.push_back(type);
        appendInteger(bytes, static_cast<std::uint32_t>(payload.size() + 4));
        bytes.append(payload);
    }

    asio::awaitable<void> serve() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        for (;;) {
            std::array<unsigned char, 4> length{};
            co_await asio::async_read(socket_, asio::buffer(length), asio::use_awaitable);
            const auto size = integer(length.data());
            if (size < 8 || size > 65536) {
                throw std::runtime_error("invalid startup length");
            }
            std::vector<unsigned char> startup(size - 4);
            co_await asio::async_read(socket_, asio::buffer(startup), asio::use_awaitable);
            const auto code = integer(startup.data());
            if (code == 80877103 || code == 80877104) {
                const char no = 'N';
                co_await asio::async_write(socket_, asio::buffer(&no, 1), asio::use_awaitable);
                continue;
            }
            break;
        }
        started_.set_value();
        std::error_code ignored;
        co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        std::string response;
        message(response, 'R', std::string(4, '\0'));
        constexpr char encoding[] = "client_encoding\0UTF8\0";
        constexpr char standard[] = "standard_conforming_strings\0on\0";
        message(response, 'S', std::string_view(encoding, sizeof(encoding) - 1));
        message(response, 'S', std::string_view(standard, sizeof(standard) - 1));
        message(response, 'Z', "I");
        co_await asio::async_write(socket_, asio::buffer(response), asio::use_awaitable);
        std::array<char, 64> buffer{};
        while (co_await socket_.async_read_some(asio::buffer(buffer),
            asio::redirect_error(asio::use_awaitable, ignored))) {
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer gate_;
    std::uint16_t port_;
    std::promise<void> started_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::future<void> done_;
    std::thread thread_;
};

ruvia::Task<bool> canCreateOperation(ruvia::DbClient& client) {
    try {
        // A cold operation exercises the public connected-client contract
        // without requiring the startup peer to implement query execution.
        auto operation = client.query("SELECT 1");
        (void)operation;
        co_return true;
    } catch (const std::logic_error&) {
        co_return false;
    }
}

}  // namespace

RUVIA_TEST(db_client_rejects_duplicate_connect_without_interrupting_startup) {
    PostgreSqlStartupPeer peer;
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto loop = pool.loop(0);
    ruvia::DbClient client(loop, peer.config());
    pool.start();
    auto first = loop.start(client.connect());
    peer.waitForStartup();
    bool rejected = false;
    try {
        loop.start(client.connect()).get();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    peer.authenticate();
    bool connected = true;
    try {
        first.get();
    } catch (const std::exception&) {
        connected = false;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(connected);
    RUVIA_CHECK(loop.start(canCreateOperation(client)).get());
    loop.start(client.shutdown()).get();
    pool.join();
}

RUVIA_TEST(db_client_rejects_duplicate_connect_without_closing_connected_client) {
    PostgreSqlStartupPeer peer;
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto loop = pool.loop(0);
    ruvia::DbClient client(loop, peer.config());
    pool.start();
    auto first = loop.start(client.connect());
    peer.waitForStartup();
    peer.authenticate();
    first.get();
    bool rejected = false;
    try {
        loop.start(client.connect()).get();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(loop.start(canCreateOperation(client)).get());
    loop.start(client.shutdown()).get();
    pool.join();
}

RUVIA_TEST(db_client_fresh_close_and_loop_stop_share_worker_completion) {
    for (int iteration = 0; iteration < 64; ++iteration) {
        ruvia::EventLoopPool pool({.loopCount = 1});
        ruvia::DbClient client(pool.loop(0), {.driver = ruvia::DbDriver::kPostgreSql});
        pool.start();
        std::barrier rendezvous(2);
        std::thread closer([&] {
            rendezvous.arrive_and_wait();
            client.close();
        });
        rendezvous.arrive_and_wait();
        pool.stop();
        closer.join();
        pool.join();
        RUVIA_CHECK(!client.worker().accepting());
    }
}

RUVIA_TEST(db_client_cold_connect_can_be_discarded_before_pool_start) {
    ruvia::EventLoopPool pool({.loopCount = 1});
    {
        ruvia::DbClient client(pool.loop(0), {.driver = ruvia::DbDriver::kPostgreSql});
        auto cold = client.connect();
        (void)cold;
    }
    pool.join();
    RUVIA_CHECK(!pool.loop(0).accepting());
}

RUVIA_TEST(db_client_shutdown_joins_pending_authentication) {
    PostgreSqlStartupPeer peer;
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto loop = pool.loop(0);
    ruvia::DbClient client(loop, peer.config());
    pool.start();
    auto first = loop.start(client.connect());
    peer.waitForStartup();
    auto closing = loop.start(client.shutdown());
    bool cancelled = false;
    try {
        first.get();
    } catch (const ruvia::DbError&) {
        cancelled = true;
    }
    closing.get();
    RUVIA_CHECK(cancelled);
    pool.join();
}
