#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop_attachment.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_client.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_client.h"
#endif

namespace ruvia::test {

// Only connection startup is implemented. Tests exercise cold public operations
// after authentication, never an injected backend object or private storage.
class backend_startup_peer final {
public:
    enum class protocol { postgresql,
        mariadb,
        redis };
    explicit backend_startup_peer(protocol selected = protocol::postgresql, bool defer_authentication = true)
        : gate_(io_),
          defer_authentication_(defer_authentication),
          startup_ready_(startup_.get_future()),
          acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          socket_(io_),
          work_(asio::make_work_guard(io_)),
          done_(asio::co_spawn(io_, serve(selected), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    ~backend_startup_peer() noexcept(false) {
        asio::post(io_, [this] {
            closing_ = true;
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
            gate_.cancel();
            work_.reset();
        });
        thread_.join();
        done_.get();
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void wait_for_startup() {
        if (startup_ready_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("PostgreSQL test peer did not receive startup");
        }
        startup_ready_.get();
    }

    void authenticate() {
        asio::post(io_, [this] { gate_.cancel(); });
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_config config() const {
        return {.driver_ = db_driver::postgresql,
            .host_ = "127.0.0.1",
            .port_ = port(),
            .tls_ = {.mode_ = client_tls_mode::disabled}};
    }
#endif

private:
    static std::uint32_t integer(const unsigned char* bytes) {
        return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
               (std::uint32_t{bytes[2]} << 8) | std::uint32_t{bytes[3]};
    }

    static void append_integer(std::string& bytes, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<char>((value >> shift) & 255));
        }
    }

    static void message(std::string& bytes, char type, std::string_view payload) {
        bytes.push_back(type);
        append_integer(bytes, static_cast<std::uint32_t>(payload.size() + 4));
        bytes.append(payload);
    }

    asio::awaitable<void> postgres_startup() {
        for (;;) {
            std::array<unsigned char, 4> length{};
            co_await asio::async_read(socket_, asio::buffer(length), asio::use_awaitable);
            const auto size = integer(length.data());
            if (size < 8 || size > 65536) {
                throw std::runtime_error("invalid PostgreSQL startup length");
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
        gate_.expires_at(asio::steady_timer::time_point::max());
        startup_.set_value();
        if (defer_authentication_) {
            std::error_code ignored;
            co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
        if (closing_) {
            co_return;
        }
        std::string response;
        message(response, 'R', std::string(4, '\0'));
        for (const auto& [name, value] : std::array{
                 std::pair{"server_version", "16.6"},
                 std::pair{"server_encoding", "UTF8"},
                 std::pair{"client_encoding", "UTF8"},
                 std::pair{"DateStyle", "ISO, MDY"},
                 std::pair{"integer_datetimes", "on"},
                 std::pair{"standard_conforming_strings", "on"},
                 std::pair{"TimeZone", "UTC"}}) {
            std::string parameter(name);
            parameter.push_back('\0');
            parameter.append(value);
            parameter.push_back('\0');
            message(response, 'S', parameter);
        }
        std::string key;
        append_integer(key, 1);
        append_integer(key, 2);
        message(response, 'K', key);
        message(response, 'Z', "I");
        co_await asio::async_write(socket_, asio::buffer(response), asio::use_awaitable);
    }

    asio::awaitable<void> maria_packet(std::string_view payload, unsigned char sequence) {
        std::array<unsigned char, 4> header{
            static_cast<unsigned char>(payload.size()),
            static_cast<unsigned char>(payload.size() >> 8),
            static_cast<unsigned char>(payload.size() >> 16), sequence};
        co_await asio::async_write(socket_, asio::buffer(header), asio::use_awaitable);
        co_await asio::async_write(socket_, asio::buffer(payload), asio::use_awaitable);
    }

    asio::awaitable<void> maria_startup() {
        // Protocol 10 greeting, protocol-41 secure connection and native auth.
        std::string greeting(1, '\x0a');
        greeting.append("8.0.0-test", 11);
        greeting.append("\x01\0\0\0", 4);
        greeting.append("12345678\0", 9);
        greeting.append("\x05\xa2", 2);
        greeting.push_back('\x2d');
        greeting.append("\x02\0", 2);
        greeting.append("\x08\0", 2);
        greeting.push_back('\x15');
        greeting.append(10, '\0');
        greeting.append("abcdefghijkl\0", 13);
        greeting.append("mysql_native_password", 22);
        co_await maria_packet(greeting, 0);
        std::array<unsigned char, 4> header{};
        co_await asio::async_read(socket_, asio::buffer(header), asio::use_awaitable);
        const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) |
                          (std::size_t{header[2]} << 16);
        if (size > 65536) {
            throw std::runtime_error("invalid MariaDB handshake length");
        }
        std::vector<unsigned char> handshake(size);
        co_await asio::async_read(socket_, asio::buffer(handshake), asio::use_awaitable);
        co_await maria_packet(std::string_view("\0\0\0\x02\0\0\0", 7),
            static_cast<unsigned char>(header[3] + 1));
    }

    asio::awaitable<void> serve(protocol selected) {
        try {
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            if (selected == protocol::postgresql) {
                co_await postgres_startup();
            } else if (selected == protocol::mariadb) {
                co_await maria_startup();
            }
            if (closing_) {
                co_return;
            }
            std::array<char, 64> buffer{};
            for (;;) {
                co_await socket_.async_read_some(asio::buffer(buffer), asio::use_awaitable);
            }
        } catch (const std::system_error& error) {
            // EOF/reset are the client's public close path. Only cancellation
            // caused by this peer's own teardown may suppress an aborted wait.
            if (error.code() != asio::error::eof &&
                error.code() != asio::error::connection_reset &&
                !(closing_ && (error.code() == asio::error::operation_aborted ||
                                  error.code() == asio::error::bad_descriptor))) {
                throw;
            }
        }
    }

    asio::io_context io_;
    asio::steady_timer gate_;
    bool defer_authentication_;
    bool closing_{false};
    std::promise<void> startup_;
    std::future<void> startup_ready_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::future<void> done_;
    std::thread thread_;
};

inline task<void> observe_backend_completion(task<void> operation, bool& completed) {
    try {
        co_await std::move(operation);
    } catch (...) {
        completed = true;
        throw;
    }
    completed = true;
}

inline void drive_backend_task(event_loop_attachment& attachment, task<void> operation) {
    bool done = false;
    auto loop = attachment.loop();
    auto completed = loop.start(observe_backend_completion(std::move(operation), done));
    auto& io = loop.io_context();
    io.restart();
    // The observer runs before the root completion handler is retired. Keep the
    // owner running until root.wait() publishes completion, not merely until
    // the observer sets its flag.
    std::jthread runner([&attachment] { attachment.run(); });
    completed.wait();
    io.stop();
    runner.join();
    completed.get();
}

inline void retire_backend_attachment(event_loop_attachment& attachment) {
    auto loop = attachment.loop();
    if (!loop.valid()) {
        return;
    }
    auto& io = loop.io_context();
    attachment.stop();
    io.restart();
    attachment.run();
}

// Startup and public client operations share the attached owner. Shutdown is
// awaited before propagating a callback failure.
template <typename client_type, typename callback_type>
task<void> exercise_backend_client(client_type& client, callback_type& callback) {
    std::exception_ptr failure;
    try {
        co_await client.connect();
        co_await callback(client);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await client.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

template <typename client_type, typename callback_type>
void drive_backend_client(
    event_loop_attachment& attachment, client_type& client, callback_type&& callback) {
    try {
        drive_backend_task(attachment, exercise_backend_client(client, callback));
    } catch (...) {
        retire_backend_attachment(attachment);
        throw;
    }
    retire_backend_attachment(attachment);
}

#ifdef RUVIA_ENABLE_DATABASE
template <typename callback_type>
void with_connected_db_client(
    event_loop_attachment& attachment, db_config config, callback_type&& callback) {
    backend_startup_peer peer(config.driver_ == db_driver::postgresql
                                  ? backend_startup_peer::protocol::postgresql
                                  : backend_startup_peer::protocol::mariadb,
        false);
    config.host_ = "127.0.0.1";
    config.port_ = peer.port();
    config.username_ = "test";
    config.password_.clear();
    config.database_.clear();
    config.tls_ = {.mode_ = client_tls_mode::disabled};
    db_client client(attachment.loop(), config);
    drive_backend_client(attachment, client, std::forward<callback_type>(callback));
}
#endif

#ifdef RUVIA_ENABLE_REDIS
template <typename callback_type>
void with_connected_redis_client(
    event_loop_attachment& attachment, redis_config config, callback_type&& callback) {
    backend_startup_peer peer(backend_startup_peer::protocol::redis, false);
    config.host_ = "127.0.0.1";
    config.port_ = peer.port();
    config.pool_size_per_worker_ = 1;
    config.tls_ = {.mode_ = client_tls_mode::disabled};
    redis_client client(attachment.loop(), config);
    drive_backend_client(attachment, client, std::forward<callback_type>(callback));
}
#endif

}  // namespace ruvia::test
