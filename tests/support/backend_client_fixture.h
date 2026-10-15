#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <optional>
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

// Connection startup, scripted single-column PostgreSQL results and an
// optional transport failure are enough to exercise public backend operations
// without injecting a backend object or private storage.
class backend_startup_peer final {
public:
    enum class protocol { postgresql,
        mariadb,
        redis };
    // What a database peer does once the client is authenticated.
    enum class after_startup {
        // Reads and discards everything the client sends.
        idle,
        // Answers the first statement with the start of a one-column row set
        // and one row, then drops the connection before the result ends.
        drop_mid_result,
        // Like idle, but when the client closes the connection, accepts one
        // more and serves the remaining scripted replies on it.
        reaccept,
    };

    enum class postgres_copy_direction { none,
        input,
        output };

    struct postgres_reply final {
        std::string column_{"v"};
        std::uint32_t type_{25};
        std::vector<std::string> rows_;
        std::string command_{"SELECT 1"};
        char status_{'I'};
        bool deferred_{false};
        // Non-empty: the statement is answered with ErrorResponse carrying
        // this message and sql_state_ instead of a result.
        std::string error_;
        std::string sql_state_{"42601"};
        // COPY keeps the connection in transfer state; no ReadyForQuery follows.
        postgres_copy_direction copy_{postgres_copy_direction::none};
    };

    // One extended-protocol Parse message: the statement text and the
    // parameter type OIDs the client declared (0 leaves a type unknown).
    struct postgres_parse final {
        std::string sql_;
        std::vector<std::uint32_t> parameter_types_;
        std::vector<std::optional<std::string>> parameter_values_;
    };

    explicit backend_startup_peer(protocol selected = protocol::postgresql, bool defer_authentication = true,
        after_startup behavior = after_startup::idle, std::vector<postgres_reply> replies = {},
        std::size_t connections = 1)
        : gate_(io_, asio::steady_timer::time_point::max()),
          defer_authentication_(defer_authentication),
          after_startup_(behavior),
          postgres_replies_(std::move(replies)),
          startup_ready_(startup_.get_future()),
          acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          sockets_(make_sockets(io_, connections)),
          work_(asio::make_work_guard(io_)),
          done_(asio::co_spawn(io_, serve(selected), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    ~backend_startup_peer() noexcept(false) {
        asio::post(io_, [this] {
            closing_ = true;
            std::error_code ignored;
            acceptor_.close(ignored);
            for (auto& socket : sockets_) {
                socket.close(ignored);
            }
            gate_.cancel();
            work_.reset();
        });
        thread_.join();
        done_.get();
        for (auto& session : sessions_) {
            session.get();
        }
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

    void release_replies() {
        asio::post(io_, [this] {
            replies_released_ = true;
            gate_.cancel();
        });
    }

    // Every Parse message received so far, read on the peer's own thread.
    [[nodiscard]] std::vector<postgres_parse> postgres_parses() {
        return asio::post(io_, asio::use_future([this] { return postgres_parses_; })).get();
    }

    // Startups received so far, read on the peer's own thread.
    [[nodiscard]] std::size_t startups() {
        return asio::post(io_, asio::use_future([this] { return startup_count_; })).get();
    }

    void wait_for_disconnects() {
        if (done_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("backend test peer did not start its sessions");
        }
        for (const auto& session : sessions_) {
            if (session.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
                throw std::runtime_error("backend test connection was not closed");
            }
        }
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
    static std::vector<asio::ip::tcp::socket> make_sockets(asio::io_context& io, std::size_t count) {
        std::vector<asio::ip::tcp::socket> sockets;
        sockets.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            sockets.emplace_back(io);
        }
        return sockets;
    }

    asio::awaitable<void> wait_for_authentication() {
        if (++startup_count_ == 1) {
            startup_.set_value();
        }
        if (defer_authentication_) {
            std::error_code ignored;
            co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
    }

    static std::uint32_t integer(const unsigned char* bytes) {
        return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
               (std::uint32_t{bytes[2]} << 8) | std::uint32_t{bytes[3]};
    }

    static void append_integer(std::string& bytes, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<char>((value >> shift) & 255));
        }
    }

    static void append_short(std::string& bytes, std::uint16_t value) {
        bytes.push_back(static_cast<char>(value >> 8));
        bytes.push_back(static_cast<char>(value & 255));
    }

    static void message(std::string& bytes, char type, std::string_view payload) {
        bytes.push_back(type);
        append_integer(bytes, static_cast<std::uint32_t>(payload.size() + 4));
        bytes.append(payload);
    }

    static void maria_packet(std::string& bytes, std::string_view payload, unsigned char sequence) {
        bytes.push_back(static_cast<char>(payload.size() & 255));
        bytes.push_back(static_cast<char>((payload.size() >> 8) & 255));
        bytes.push_back(static_cast<char>((payload.size() >> 16) & 255));
        bytes.push_back(static_cast<char>(sequence));
        bytes.append(payload);
    }

    asio::awaitable<void> postgres_startup(asio::ip::tcp::socket& socket) {
        for (;;) {
            std::array<unsigned char, 4> length{};
            co_await asio::async_read(socket, asio::buffer(length), asio::use_awaitable);
            const auto size = integer(length.data());
            if (size < 8 || size > 65536) {
                throw std::runtime_error("invalid PostgreSQL startup length");
            }
            std::vector<unsigned char> startup(size - 4);
            co_await asio::async_read(socket, asio::buffer(startup), asio::use_awaitable);
            const auto code = integer(startup.data());
            if (code == 80877103 || code == 80877104) {
                const char no = 'N';
                co_await asio::async_write(socket, asio::buffer(&no, 1), asio::use_awaitable);
                continue;
            }
            break;
        }
        co_await wait_for_authentication();
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
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    }

    asio::awaitable<bool> postgres_statement(asio::ip::tcp::socket& socket) {
        std::optional<std::size_t> statement;
        for (;;) {
            std::array<unsigned char, 5> header{};
            co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
            const auto size = integer(header.data() + 1);
            if (size < 4 || size > 65536) {
                throw std::runtime_error("invalid PostgreSQL message length");
            }
            std::vector<unsigned char> body(size - 4);
            co_await asio::async_read(socket, asio::buffer(body), asio::use_awaitable);
            if (header[0] == 'P') {
                record_postgres_parse(body);
                statement = postgres_parses_.size() - 1;
            } else if (header[0] == 'B') {
                if (!statement) {
                    throw std::runtime_error("PostgreSQL Bind without Parse");
                }
                record_postgres_bind(*statement, body);
            }
            if (header[0] == 'X') {
                co_return false;
            }
            if (header[0] == 'S') {
                co_return true;
            }
        }
    }

    // Parse: statement name, query text, Int16 count, then one Int32 OID each.
    void record_postgres_parse(const std::vector<unsigned char>& body) {
        const auto text_end = [&body](std::size_t start) {
            for (auto index = start; index < body.size(); ++index) {
                if (body[index] == 0) {
                    return index;
                }
            }
            throw std::runtime_error("unterminated PostgreSQL Parse string");
        };
        const auto query_start = text_end(0) + 1;
        const auto query_end = text_end(query_start);
        postgres_parse parsed;
        parsed.sql_.assign(reinterpret_cast<const char*>(body.data()) + query_start, query_end - query_start);
        auto offset = query_end + 1;
        if (offset + 2 > body.size()) {
            throw std::runtime_error("truncated PostgreSQL Parse parameter count");
        }
        const auto count = (std::size_t{body[offset]} << 8) | std::size_t{body[offset + 1]};
        offset += 2;
        if (offset + count * 4 != body.size()) {
            throw std::runtime_error("invalid PostgreSQL Parse parameter types");
        }
        for (std::size_t index = 0; index < count; ++index, offset += 4) {
            parsed.parameter_types_.push_back(integer(body.data() + offset));
        }
        postgres_parses_.push_back(std::move(parsed));
    }

    // Bind follows Parse in the unnamed extended-query exchange used here.
    void record_postgres_bind(std::size_t statement, const std::vector<unsigned char>& body) {
        const std::string_view bytes(reinterpret_cast<const char*>(body.data()), body.size());
        std::size_t offset = 0;
        for (int i = 0; i < 2; ++i) {
            const auto end = bytes.find('\0', offset);
            if (end == std::string_view::npos) {
                throw std::runtime_error("unterminated PostgreSQL Bind name");
            }
            offset = end + 1;
        }
        const auto read_short = [&]() {
            if (body.size() - offset < 2) {
                throw std::runtime_error("truncated PostgreSQL Bind count");
            }
            const auto value = (std::size_t{body[offset]} << 8) | body[offset + 1];
            offset += 2;
            return value;
        };
        const auto formats = read_short();
        if (formats > (body.size() - offset) / 2) {
            throw std::runtime_error("truncated PostgreSQL Bind formats");
        }
        offset += formats * 2;
        const auto count = read_short();
        auto& values = postgres_parses_[statement].parameter_values_;
        for (std::size_t i = 0; i < count; ++i) {
            if (body.size() - offset < 4) {
                throw std::runtime_error("truncated PostgreSQL Bind length");
            }
            const auto length = integer(body.data() + offset);
            offset += 4;
            if (length == std::numeric_limits<std::uint32_t>::max()) {
                values.emplace_back(std::nullopt);
            } else {
                if (length > body.size() - offset) {
                    throw std::runtime_error("truncated PostgreSQL Bind value");
                }
                values.emplace_back(bytes.substr(offset, length));
                offset += length;
            }
        }
    }

    static void postgres_result(std::string& response, const postgres_reply& reply) {
        message(response, '1', {});
        message(response, '2', {});
        if (reply.column_.empty()) {
            message(response, 'n', {});
            return;
        }
        std::string description;
        append_short(description, 1);
        description.append(reply.column_);
        description.push_back('\0');
        append_integer(description, 0);
        append_short(description, 0);
        append_integer(description, reply.type_);
        append_short(description, 0xffff);
        append_integer(description, 0xffffffff);
        append_short(description, 0);
        message(response, 'T', description);
        for (const auto& value : reply.rows_) {
            std::string row;
            append_short(row, 1);
            append_integer(row, static_cast<std::uint32_t>(value.size()));
            row.append(value);
            message(response, 'D', row);
        }
    }

    // CommandComplete and ReadyForQuery never follow the partial row set.
    asio::awaitable<void> postgres_drop_mid_result(asio::ip::tcp::socket& socket) {
        if (!(co_await postgres_statement(socket))) {
            co_return;
        }
        std::string response;
        postgres_result(response, {.rows_ = {"x"}});
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    }

    asio::awaitable<void> postgres_script(asio::ip::tcp::socket& socket) {
        while (next_reply_ < postgres_replies_.size()) {
            if (!(co_await postgres_statement(socket))) {
                co_return;
            }
            if (next_reply_ == postgres_replies_.size()) {
                co_return;
            }
            const auto& reply = postgres_replies_[next_reply_++];
            if (reply.deferred_ && !replies_released_) {
                std::error_code ignored;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
                if (closing_) {
                    co_return;
                }
            }
            std::string response;
            if (!reply.error_.empty()) {
                std::string fields;
                for (const auto& [code, value] : std::array{std::pair{'S', std::string_view("ERROR")},
                         std::pair{'C', std::string_view(reply.sql_state_)},
                         std::pair{'M', std::string_view(reply.error_)}}) {
                    fields.push_back(code);
                    fields.append(value);
                    fields.push_back('\0');
                }
                fields.push_back('\0');
                message(response, 'E', fields);
                message(response, 'Z', std::string_view(&reply.status_, 1));
            } else if (reply.copy_ != postgres_copy_direction::none) {
                // Text format, one column in text format.
                std::string copy;
                copy.push_back('\0');
                append_short(copy, 1);
                append_short(copy, 0);
                message(response, '1', {});
                message(response, '2', {});
                message(response, reply.copy_ == postgres_copy_direction::input ? 'G' : 'H', copy);
            } else {
                postgres_result(response, reply);
                std::string command(reply.command_);
                command.push_back('\0');
                message(response, 'C', command);
                message(response, 'Z', std::string_view(&reply.status_, 1));
            }
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        }
    }

    asio::awaitable<void> maria_packet(asio::ip::tcp::socket& socket, std::string_view payload, unsigned char sequence) {
        std::string bytes;
        maria_packet(bytes, payload, sequence);
        co_await asio::async_write(socket, asio::buffer(bytes), asio::use_awaitable);
    }

    asio::awaitable<void> maria_startup(asio::ip::tcp::socket& socket) {
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
        co_await maria_packet(socket, greeting, 0);
        std::array<unsigned char, 4> header{};
        co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
        const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) |
                          (std::size_t{header[2]} << 16);
        if (size > 65536) {
            throw std::runtime_error("invalid MariaDB handshake length");
        }
        std::vector<unsigned char> handshake(size);
        co_await asio::async_read(socket, asio::buffer(handshake), asio::use_awaitable);
        co_await wait_for_authentication();
        if (closing_) {
            co_return;
        }
        co_await maria_packet(socket, std::string_view("\0\0\0\x02\0\0\0", 7),
            static_cast<unsigned char>(header[3] + 1));
    }

    // Column count, one VARCHAR column definition and its EOF, and one row
    // follow COM_QUERY; the closing EOF never does.
    asio::awaitable<void> maria_drop_mid_result(asio::ip::tcp::socket& socket) {
        std::array<unsigned char, 4> header{};
        co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
        const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) | (std::size_t{header[2]} << 16);
        if (size > 65536) {
            throw std::runtime_error("invalid MariaDB command length");
        }
        std::vector<unsigned char> command(size);
        co_await asio::async_read(socket, asio::buffer(command), asio::use_awaitable);
        auto sequence = static_cast<unsigned char>(header[3] + 1);
        std::string response;
        maria_packet(response, std::string_view("\x01", 1), sequence++);
        std::string column;
        for (const std::string_view part : {"def", "", "", "", "v", ""}) {
            column.push_back(static_cast<char>(part.size()));
            column.append(part);
        }
        // Fixed fields: charset utf8mb4, length 1024, VAR_STRING, no flags.
        column.append("\x0c\x2d\x00\x00\x04\x00\x00\xfd\x00\x00\x00\x00\x00", 13);
        maria_packet(response, column, sequence++);
        maria_packet(response, std::string_view("\xfe\0\0\x02\0", 5), sequence++);
        maria_packet(response, std::string_view("\x01x", 2), sequence++);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    }

    asio::awaitable<void> serve(protocol selected) {
        for (auto& socket : sockets_) {
            sessions_.push_back(asio::co_spawn(io_, serve_session(selected, socket), asio::use_future));
        }
        co_return;
    }

    asio::awaitable<void> serve_session(protocol selected, asio::ip::tcp::socket& socket) {
        for (;;) {
            try {
                co_await serve_connection(selected, socket);
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
            if (closing_ || after_startup_ != after_startup::reaccept || next_reply_ >= postgres_replies_.size()) {
                co_return;
            }
            socket = asio::ip::tcp::socket(io_);
        }
    }

    asio::awaitable<void> serve_connection(protocol selected, asio::ip::tcp::socket& socket) {
        co_await acceptor_.async_accept(socket, asio::use_awaitable);
        if (selected == protocol::postgresql) {
            co_await postgres_startup(socket);
        } else if (selected == protocol::mariadb) {
            co_await maria_startup(socket);
        }
        if (closing_) {
            co_return;
        }
        if (after_startup_ == after_startup::drop_mid_result) {
            if (selected == protocol::postgresql) {
                co_await postgres_drop_mid_result(socket);
            } else if (selected == protocol::mariadb) {
                co_await maria_drop_mid_result(socket);
            }
            socket.close();
            co_return;
        }
        if (selected == protocol::postgresql && !postgres_replies_.empty()) {
            co_await postgres_script(socket);
        }
        std::array<char, 64> buffer{};
        for (;;) {
            co_await socket.async_read_some(asio::buffer(buffer), asio::use_awaitable);
        }
    }

    asio::io_context io_;
    asio::steady_timer gate_;
    bool defer_authentication_;
    after_startup after_startup_;
    std::vector<postgres_reply> postgres_replies_;
    std::vector<postgres_parse> postgres_parses_;
    std::size_t next_reply_{0};
    std::size_t startup_count_{0};
    bool replies_released_{false};
    bool closing_{false};
    std::promise<void> startup_;
    std::future<void> startup_ready_;
    asio::ip::tcp::acceptor acceptor_;
    std::vector<asio::ip::tcp::socket> sockets_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::vector<std::future<void>> sessions_;
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
void with_connected_db_client(event_loop_attachment& attachment, db_config config,
    backend_startup_peer::after_startup behavior, callback_type&& callback) {
    backend_startup_peer peer(config.driver_ == db_driver::postgresql ? backend_startup_peer::protocol::postgresql
                                                                      : backend_startup_peer::protocol::mariadb,
        false, behavior, {}, config.pool_size_per_worker_);
    config.host_ = "127.0.0.1";
    config.port_ = peer.port();
    config.username_ = "test";
    config.password_.clear();
    config.database_.clear();
    config.tls_ = {.mode_ = client_tls_mode::disabled};
    db_client client(attachment.loop(), config);
    drive_backend_client(attachment, client, std::forward<callback_type>(callback));
}

template <typename callback_type>
void with_connected_db_client(event_loop_attachment& attachment, db_config config, callback_type&& callback) {
    with_connected_db_client(attachment, std::move(config), backend_startup_peer::after_startup::idle,
        std::forward<callback_type>(callback));
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
