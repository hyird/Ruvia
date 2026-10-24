#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <span>
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

#include "ruvia/core/event_loop.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_handle.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_handle.h"
#endif

namespace ruvia::test {

// Connection startup, scripted PostgreSQL results and an
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
        // Non-empty: Bind is answered with ErrorResponse carrying this
        // message, sql_state_ and, when non-empty, routine_ instead of a result.
        std::string error_;
        std::string sql_state_{"42601"};
        std::string routine_;
        // COPY keeps the connection in transfer state; no ReadyForQuery follows.
        postgres_copy_direction copy_{postgres_copy_direction::none};
        // Flights read before anything is written: this reply answers the
        // first and the replies after it answer the others, in one write, so a
        // pipelining client must have sent them all before its first reply.
        std::size_t batch_{1};
        // After reading the batch, close the connection instead of answering.
        bool drop_{false};
        // The flight's Parse fails with error_ and keeps no statement, instead
        // of its Bind failing.
        bool parse_fails_{false};
        // Nonzero: held, like deferred_, until release_hold() names it.
        std::size_t hold_{0};
        // Non-empty: multi-column text metadata and row values replace column_/rows_.
        std::vector<std::string> columns_;
        std::vector<std::vector<std::string>> values_;
    };

    // One extended-protocol Parse message: the statement text, the parameter
    // type OIDs the client declared (0 leaves a type unknown), and the values
    // of every Bind that named this statement.
    struct postgres_parse final {
        std::string sql_;
        std::vector<std::uint32_t> parameter_types_;
        std::vector<std::optional<std::string>> parameter_values_;
    };

    // One column of a MariaDB result set: its name, type code, flags,
    // decimals and display length, as a column definition carries them.
    struct maria_column final {
        std::string name_{"v"};
        std::uint8_t type_{0xfd};
        std::uint16_t flags_{0};
        std::uint8_t decimals_{0};
        std::uint32_t length_{1024};
    };

    // How a MariaDB peer answers one COM_QUERY or COM_STMT_EXECUTE: a row set,
    // an OK packet when column_ is empty and no columns_ are given, or an ERR
    // packet when error_ is set. A COM_QUERY gets a one-column text row set of
    // rows_ (null_rows_ marks NULL values); a COM_STMT_EXECUTE gets a binary
    // row set of columns_ and binary_rows_ (encoded values, nullopt for NULL),
    // or of one VAR_STRING column of rows_ when columns_ is empty. status_ is
    // the server status the answer reports (2: autocommit; | 1 inside a
    // transaction). A COM_STMT_PREPARE is answered without a reply, from the
    // columns of the reply its execution will take, unless that reply sets
    // prepare_fails_: then it takes the reply and is refused with error_.
    struct maria_reply final {
        std::string column_{"v"};
        std::vector<std::string> rows_;
        std::vector<bool> null_rows_;
        std::uint64_t affected_rows_{0};
        std::uint64_t insert_id_{0};
        std::uint16_t status_{2};
        std::string error_;
        std::uint16_t code_{1064};
        std::string sql_state_{"42000"};
        // A procedure call: the row set ends flagged with more results, and a
        // final OK packet follows.
        bool procedure_{false};
        bool deferred_{false};
        // Commands read before anything is written: this reply answers the
        // first and the replies after it answer the others, in one write.
        std::size_t batch_{1};
        // After reading the batch, close the connection instead of answering.
        bool drop_{false};
        // Nonzero: held, like deferred_, until release_hold() names it.
        std::size_t hold_{0};
        std::vector<maria_column> columns_;
        std::vector<std::vector<std::optional<std::string>>> binary_rows_;
        bool prepare_fails_{false};
        // Nonzero: the commands this reply answers are not read until
        // release_hold() names it, so the client's writes back up.
        std::size_t read_hold_{0};
        // Non-empty: the query is a LOAD DATA LOCAL INFILE. The peer asks
        // for this file, reads its content and answers OK; a packet out of
        // sequence, as a command sent behind the LOAD would be, is logged as
        // "desync" and closes the connection, as the server does.
        std::string local_infile_;
        // The row set ends with the ERR packet of error_ instead of its EOF,
        // as a statement failing while it produces rows (a DOUBLE out of
        // range) does.
        bool error_after_rows_{false};
        // System variables its OK packet reports as changed, as session
        // tracking does.
        std::vector<std::pair<std::string, std::string>> tracked_;
        // Non-empty: the columns the COM_STMT_PREPARE answer describes in
        // place of columns_, as when a table was altered between the prepare
        // and the execution, or as a UNION of YEAR columns is described.
        std::vector<maria_column> prepare_columns_;
    };

    explicit backend_startup_peer(protocol selected = protocol::postgresql, bool defer_authentication = true,
        after_startup behavior = after_startup::idle, std::vector<postgres_reply> replies = {},
        std::size_t connections = 1)
        : backend_startup_peer(selected, defer_authentication, behavior, std::move(replies), {}, {}, connections) {}

    // A PostgreSQL peer whose connections, in accept order, each answer their
    // own scripted replies.
    explicit backend_startup_peer(std::vector<std::vector<postgres_reply>> connection_replies)
        : backend_startup_peer(
              protocol::postgresql, false, after_startup::idle, {}, std::move(connection_replies), {}, 0) {}

    // A MariaDB peer that answers its connections' COM_QUERY commands from
    // one script, in order. Unless `session_tracking` is false, it announces
    // session tracking and answers the client's setup of it on each new
    // connection itself, outside the script.
    backend_startup_peer(std::vector<maria_reply> replies, after_startup behavior, std::size_t connections = 1,
        bool session_tracking = true)
        : backend_startup_peer(
              protocol::mariadb, false, behavior, {}, {}, std::move(replies), connections, session_tracking) {}

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

    void release_hold(std::size_t hold) {
        asio::post(io_, [this, hold] {
            released_holds_.push_back(hold);
            gate_.cancel();
        });
    }

    // Every Parse message received so far, read on the peer's own thread.
    [[nodiscard]] std::vector<postgres_parse> postgres_parses() {
        return asio::post(io_, asio::use_future([this] { return postgres_parses_; })).get();
    }

    // Every COM_QUERY text received so far, read on the peer's own thread.
    [[nodiscard]] std::vector<std::string> maria_queries() {
        return asio::post(io_, asio::use_future([this] { return maria_queries_; })).get();
    }

    // Every MariaDB command received so far: "query:<sql>", "prepare:<sql>",
    // "execute:<id>:<params>", "reset:<id>", "close:<id>" and, for a LOAD
    // DATA LOCAL INFILE, "infile:<content>", where each parameter reads
    // "i:<n>", "u:<n>", "d:<n>", "t:<n>", "s:<text>" or "null".
    [[nodiscard]] std::vector<std::string> maria_commands() {
        return asio::post(io_, asio::use_future([this] { return maria_commands_; })).get();
    }

    // Startups received so far, read on the peer's own thread.
    [[nodiscard]] std::size_t startups() {
        return asio::post(io_, asio::use_future([this] { return startup_count_; })).get();
    }

    // MariaDB connections that turned on session tracking, read on the
    // peer's own thread.
    [[nodiscard]] std::size_t maria_session_setups() {
        return asio::post(io_, asio::use_future([this] { return maria_session_setups_; })).get();
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
    backend_startup_peer(protocol selected, bool defer_authentication, after_startup behavior,
        std::vector<postgres_reply> replies, std::vector<std::vector<postgres_reply>> connection_replies,
        std::vector<maria_reply> maria_replies, std::size_t connections, bool maria_session_tracking = true)
        : gate_(io_, asio::steady_timer::time_point::max()),
          defer_authentication_(defer_authentication),
          maria_session_tracking_(maria_session_tracking),
          after_startup_(behavior),
          postgres_replies_(std::move(replies)),
          connection_replies_(std::move(connection_replies)),
          connection_next_(connection_replies_.size(), 0),
          maria_replies_(std::move(maria_replies)),
          startup_ready_(startup_.get_future()),
          acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          sockets_(make_sockets(io_, std::max(connections, connection_replies_.size()))),
          work_(asio::make_work_guard(io_)),
          done_(asio::co_spawn(io_, serve(selected), asio::use_future)),
          thread_([this] { io_.run(); }) {}

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

    // Reads one flight of extended-protocol messages up to its Sync and
    // returns their types in order; nullopt when the client terminates.
    // Parse, Bind and Close keep the connection's named statements, as a
    // server does. A Parse that `reply` makes fail keeps no statement, and a
    // Bind of that name is recorded as 'b'; a Bind of any other name the
    // session does not hold is a client defect.
    asio::awaitable<std::optional<std::string>> postgres_flight(asio::ip::tcp::socket& socket,
        const postgres_reply* reply, std::map<std::string, std::size_t>& statements) {
        std::string messages;
        for (;;) {
            std::array<unsigned char, 5> header{};
            co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
            const auto size = integer(header.data() + 1);
            if (size < 4 || size > 65536) {
                throw std::runtime_error("invalid PostgreSQL message length");
            }
            std::vector<unsigned char> body(size - 4);
            co_await asio::async_read(socket, asio::buffer(body), asio::use_awaitable);
            auto type = static_cast<char>(header[0]);
            if (type == 'X') {
                co_return std::nullopt;
            }
            if (type == 'P') {
                record_postgres_parse(body, statements, reply != nullptr && reply->parse_fails_);
            } else if (type == 'B') {
                if (!record_postgres_bind(body, statements)) {
                    type = 'b';
                }
            } else if (type == 'C') {
                if (!body.empty() && body[0] == 'S') {
                    statements.erase(text_at(body, 1).first);
                }
            }
            messages.push_back(type);
            if (type == 'S') {
                co_return messages;
            }
        }
    }

    // The NUL-terminated string at `start` and the offset just past it.
    static std::pair<std::string, std::size_t> text_at(const std::vector<unsigned char>& body, std::size_t start) {
        for (auto index = start; index < body.size(); ++index) {
            if (body[index] == 0) {
                return {std::string(reinterpret_cast<const char*>(body.data()) + start, index - start), index + 1};
            }
        }
        throw std::runtime_error("unterminated PostgreSQL string");
    }

    // Parse: statement name, query text, Int16 count, then one Int32 OID each.
    void record_postgres_parse(
        const std::vector<unsigned char>& body, std::map<std::string, std::size_t>& statements, bool fails) {
        auto [name, query_start] = text_at(body, 0);
        auto [sql, offset] = text_at(body, query_start);
        postgres_parse parsed;
        parsed.sql_ = std::move(sql);
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
        if (!name.empty() && statements.contains(name) && statements.at(name) != failed_parse) {
            throw std::runtime_error("PostgreSQL Parse reuses a prepared statement name");
        }
        postgres_parses_.push_back(std::move(parsed));
        statements[name] = fails ? failed_parse : postgres_parses_.size() - 1;
    }

    // Marks a name whose Parse the script made fail: the server holds none.
    static constexpr std::size_t failed_parse = std::numeric_limits<std::size_t>::max();

    // Bind: portal name, statement name, formats, then the values. False for a
    // name whose Parse failed.
    [[nodiscard]] bool record_postgres_bind(
        const std::vector<unsigned char>& body, const std::map<std::string, std::size_t>& statements) {
        const auto portal_end = text_at(body, 0).second;
        auto [name, offset] = text_at(body, portal_end);
        const auto statement = statements.find(name);
        if (statement == statements.end()) {
            throw std::runtime_error("PostgreSQL Bind names no prepared statement");
        }
        if (statement->second == failed_parse) {
            return false;
        }
        const std::string_view bytes(reinterpret_cast<const char*>(body.data()), body.size());

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
        auto& values = postgres_parses_[statement->second].parameter_values_;
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
        return true;
    }

    static void postgres_error(std::string& response, const postgres_reply& reply) {
        std::string fields;
        for (const auto& [code, value] : std::array{std::pair{'S', std::string_view("ERROR")},
                 std::pair{'C', std::string_view(reply.sql_state_)}, std::pair{'M', std::string_view(reply.error_)},
                 std::pair{'R', std::string_view(reply.routine_)}}) {
            if (value.empty()) {
                continue;
            }
            fields.push_back(code);
            fields.append(value);
            fields.push_back('\0');
        }
        fields.push_back('\0');
        message(response, 'E', fields);
    }

    // Answers each message of a flight in order, with `reply` supplying the
    // statement's outcome. An incomplete answer stops after the first row,
    // before CommandComplete and ReadyForQuery.
    static void postgres_flight_response(
        std::string& response, std::string_view messages, const postgres_reply& reply, bool complete = true) {
        for (const char type : messages) {
            switch (type) {
                case 'C':
                    message(response, '3', {});
                    break;
                case 'P':
                    if (reply.parse_fails_) {
                        // The server skips the rest of the flight up to Sync.
                        postgres_error(response, reply);
                        message(response, 'Z', std::string_view(&reply.status_, 1));
                        return;
                    }
                    message(response, '1', {});
                    break;
                case 'b': {
                    const postgres_reply missing{.error_ = "prepared statement does not exist",
                        .sql_state_ = "26000",
                        .routine_ = "FetchPreparedStatement"};
                    postgres_error(response, missing);
                    message(response, 'Z', std::string_view(&reply.status_, 1));
                    return;
                }
                case 'B':
                    if (!reply.error_.empty() && !reply.parse_fails_) {
                        // The server skips the rest of the flight up to Sync.
                        postgres_error(response, reply);
                        message(response, 'Z', std::string_view(&reply.status_, 1));
                        return;
                    }
                    message(response, '2', {});
                    break;
                case 'D':
                    if (reply.copy_ != postgres_copy_direction::none) {
                        break;
                    }
                    if (reply.column_.empty() && reply.columns_.empty()) {
                        message(response, 'n', {});
                        break;
                    }
                    postgres_description(response, reply);
                    break;
                case 'E':
                    if (reply.copy_ != postgres_copy_direction::none) {
                        // Text format, one column in text format.
                        std::string copy;
                        copy.push_back('\0');
                        append_short(copy, 1);
                        append_short(copy, 0);
                        message(response, reply.copy_ == postgres_copy_direction::input ? 'G' : 'H', copy);
                        return;
                    }
                    for (const auto& value : reply.rows_) {
                        std::string row;
                        append_short(row, 1);
                        append_integer(row, static_cast<std::uint32_t>(value.size()));
                        row.append(value);
                        message(response, 'D', row);
                        if (!complete) {
                            return;
                        }
                    }
                    for (const auto& values : reply.values_) {
                        std::string row;
                        append_short(row, static_cast<std::uint16_t>(values.size()));
                        for (const auto& value : values) {
                            append_integer(row, static_cast<std::uint32_t>(value.size()));
                            row.append(value);
                        }
                        message(response, 'D', row);
                        if (!complete) {
                            return;
                        }
                    }
                    if (!complete) {
                        return;
                    }
                    {
                        std::string command(reply.command_);
                        command.push_back('\0');
                        message(response, 'C', command);
                    }
                    break;
                case 'S':
                    message(response, 'Z', std::string_view(&reply.status_, 1));
                    break;
                default:
                    break;
            }
        }
    }

    static void postgres_description(std::string& response, const postgres_reply& reply) {
        std::string description;
        const auto columns = reply.columns_.empty() ? std::span<const std::string>(&reply.column_, 1) : std::span<const std::string>(reply.columns_);
        append_short(description, static_cast<std::uint16_t>(columns.size()));
        for (const auto& column : columns) {
            description.append(column);
            description.push_back('\0');
            append_integer(description, 0);
            append_short(description, 0);
            append_integer(description, reply.type_);
            append_short(description, 0xffff);
            append_integer(description, 0xffffffff);
            append_short(description, 0);
        }
        message(response, 'T', description);
    }

    // CommandComplete and ReadyForQuery never follow the partial row set.
    asio::awaitable<void> postgres_drop_mid_result(asio::ip::tcp::socket& socket) {
        std::map<std::string, std::size_t> statements;
        const auto messages = co_await postgres_flight(socket, nullptr, statements);
        if (!messages) {
            co_return;
        }
        std::string response;
        postgres_flight_response(response, *messages, {.rows_ = {"x"}}, false);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    }

    template <typename reply_type>
    [[nodiscard]] bool held(const reply_type& reply) const {
        return (reply.deferred_ && !replies_released_) ||
               (reply.hold_ != 0 && std::ranges::find(released_holds_, reply.hold_) == released_holds_.end());
    }

    // Answers `replies` from `next` in order, each batch of flights in one
    // write. Returns true when a dropping reply closed the connection.
    asio::awaitable<bool> postgres_script(asio::ip::tcp::socket& socket, std::vector<postgres_reply>& replies,
        std::size_t& next, std::map<std::string, std::size_t>& statements) {
        while (next < replies.size()) {
            const auto& reply = replies[next];
            std::vector<std::string> flights;
            while (flights.size() < std::max<std::size_t>(reply.batch_, 1)) {
                const auto answer = next + flights.size();
                auto messages =
                    co_await postgres_flight(socket, answer < replies.size() ? &replies[answer] : nullptr, statements);
                if (!messages) {
                    co_return false;
                }
                flights.push_back(std::move(*messages));
            }
            while (held(reply) && !closing_) {
                std::error_code ignored;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
            }
            if (closing_) {
                co_return false;
            }
            if (reply.drop_) {
                ++next;
                socket.close();
                co_return true;
            }
            std::string response;
            for (const auto& messages : flights) {
                if (next == replies.size()) {
                    break;
                }
                postgres_flight_response(response, messages, replies[next++]);
            }
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        }
        co_return false;
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
        // Upper capabilities: plugin auth and multiple results, and session
        // tracking unless the test turned it off.
        greeting.push_back(maria_session_tracking_ ? '\x8a' : '\x0a');
        greeting.push_back('\0');
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
        if (!maria_session_tracking_) {
            co_return;
        }
        // The client turns on tracking of the variables its statements depend
        // on, by name, before anything else; the peer reports the change as
        // the server does.
        std::array<unsigned char, 4> setup_header{};
        co_await asio::async_read(socket, asio::buffer(setup_header), asio::use_awaitable);
        const auto setup_size =
            std::size_t{setup_header[0]} | (std::size_t{setup_header[1]} << 8) | (std::size_t{setup_header[2]} << 16);
        std::string setup(setup_size, '\0');
        co_await asio::async_read(socket, asio::buffer(setup), asio::use_awaitable);
        constexpr std::string_view setup_start = "\x03SET SESSION session_track_system_variables = '";
        constexpr std::string_view setup_end = "', session_track_schema = ON";
        if (!setup.starts_with(setup_start) || !setup.ends_with(setup_end) ||
            setup.find("sql_mode,") == std::string::npos || setup.find('*') != std::string::npos) {
            throw std::runtime_error("MariaDB client did not turn on session tracking first");
        }
        ++maria_session_setups_;
        const auto tracked =
            setup.substr(setup_start.size(), setup.size() - setup_start.size() - setup_end.size());
        co_await maria_packet(
            socket, maria_ok(0, 0, 2, {{"session_track_schema", "ON"}, {"session_track_system_variables", tracked}}), 1);
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

    static void maria_length(std::string& bytes, std::uint64_t value) {
        if (value < 251) {
            bytes.push_back(static_cast<char>(value));
            return;
        }
        const auto [marker, width] = value < 65536 ? std::pair{'\xfc', 2} : std::pair{'\xfe', 8};
        bytes.push_back(marker);
        for (int index = 0; index < width; ++index) {
            bytes.push_back(static_cast<char>((value >> (8 * index)) & 255));
        }
    }

    static void maria_status(std::string& bytes, std::uint16_t status) {
        bytes.push_back(static_cast<char>(status & 255));
        bytes.push_back(static_cast<char>(status >> 8));
    }

    // An OK packet; with `tracked` system variables it reports them as
    // changed (SERVER_SESSION_STATE_CHANGED), as session tracking does.
    static std::string maria_ok(std::uint64_t affected, std::uint64_t insert_id, std::uint16_t status,
        const std::vector<std::pair<std::string, std::string>>& tracked) {
        std::string packet(1, '\0');
        maria_length(packet, affected);
        maria_length(packet, insert_id);
        maria_status(packet, static_cast<std::uint16_t>(tracked.empty() ? status : status | 0x4000));
        packet.append(2, '\0');
        if (!tracked.empty()) {
            std::string changes;
            for (const auto& [name, value] : tracked) {
                std::string item;
                maria_length(item, name.size());
                item.append(name);
                maria_length(item, value.size());
                item.append(value);
                // SESSION_TRACK_SYSTEM_VARIABLES, then the item.
                changes.push_back('\0');
                maria_length(changes, item.size());
                changes.append(item);
            }
            // No info text, then the session state changes.
            maria_length(packet, 0);
            maria_length(packet, changes.size());
            packet.append(changes);
        }
        return packet;
    }

    // One command the client sent.
    struct maria_request final {
        char type_{0};
        std::string text_;
    };

    // A prepared statement the peer handed out: its parameter count and the
    // parameter types its last execution bound.
    struct maria_statement final {
        std::size_t params_{0};
        std::vector<std::uint16_t> types_;
    };

    // Reads one command and logs it; nullopt when the client quits.
    asio::awaitable<std::optional<maria_request>> maria_command(asio::ip::tcp::socket& socket) {
        std::array<unsigned char, 4> header{};
        co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
        const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) | (std::size_t{header[2]} << 16);
        if (size == 0 || header[3] != 0) {
            throw std::runtime_error("invalid MariaDB command packet");
        }
        std::string command(size, '\0');
        co_await asio::async_read(socket, asio::buffer(command), asio::use_awaitable);
        maria_request request{command[0], command.substr(1)};
        switch (request.type_) {
            case '\x01':
                co_return std::nullopt;
            case '\x03':
                maria_queries_.push_back(request.text_);
                maria_commands_.push_back("query:" + request.text_);
                break;
            case '\x16':
                maria_commands_.push_back("prepare:" + request.text_);
                break;
            case '\x17':
                maria_commands_.push_back(maria_execution(request.text_));
                break;
            case '\x19':
                maria_commands_.push_back("close:" + std::to_string(maria_integer(request.text_, 0, 4)));
                break;
            case '\x1a':
                maria_commands_.push_back("reset:" + std::to_string(maria_integer(request.text_, 0, 4)));
                break;
            default:
                throw std::runtime_error("unexpected MariaDB command");
        }
        co_return request;
    }

    static std::uint64_t maria_integer(std::string_view bytes, std::size_t offset, std::size_t width) {
        if (offset + width > bytes.size()) {
            throw std::runtime_error("truncated MariaDB integer");
        }
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < width; ++index) {
            value |= std::uint64_t{static_cast<unsigned char>(bytes[offset + index])} << (8 * index);
        }
        return value;
    }

    // COM_STMT_EXECUTE: statement id, flags, iteration count, then a NULL
    // bitmap, whether types follow, the types, and each non-NULL value.
    std::string maria_execution(std::string_view payload) {
        const auto id = static_cast<std::uint32_t>(maria_integer(payload, 0, 4));
        auto found = maria_statements_.find(id);
        if (found == maria_statements_.end()) {
            throw std::runtime_error("MariaDB execute names no prepared statement");
        }
        auto& statement = found->second;
        std::string logged = "execute:" + std::to_string(id) + ":";
        std::size_t offset = 9;
        if (statement.params_ == 0) {
            return logged;
        }
        const auto bitmap = offset;
        offset += (statement.params_ + 7) / 8;
        if (maria_integer(payload, offset++, 1) != 0) {
            statement.types_.clear();
            for (std::size_t index = 0; index < statement.params_; ++index, offset += 2) {
                statement.types_.push_back(static_cast<std::uint16_t>(maria_integer(payload, offset, 2)));
            }
        }
        for (std::size_t index = 0; index < statement.params_; ++index) {
            if (index != 0) {
                logged.push_back(',');
            }
            if ((maria_integer(payload, bitmap + index / 8, 1) & (std::uint64_t{1} << (index % 8))) != 0) {
                logged.append("null");
                continue;
            }
            const auto type = statement.types_.at(index);
            switch (type & 0xff) {
                case 0x08: {
                    const auto value = maria_integer(payload, offset, 8);
                    offset += 8;
                    logged.append((type & 0x8000) != 0 ? "u:" + std::to_string(value)
                                                       : "i:" + std::to_string(static_cast<std::int64_t>(value)));
                    break;
                }
                case 0x05: {
                    const auto bits = maria_integer(payload, offset, 8);
                    offset += 8;
                    double value = 0;
                    std::memcpy(&value, &bits, sizeof(value));
                    std::array<char, 32> text{};
                    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
                    logged.append("d:").append(text.data(), converted.ptr);
                    break;
                }
                case 0x01:
                    logged.append("t:" + std::to_string(maria_integer(payload, offset++, 1)));
                    break;
                default: {
                    auto length = maria_integer(payload, offset++, 1);
                    if (length == 0xfc) {
                        length = maria_integer(payload, offset, 2);
                        offset += 2;
                    }
                    logged.append("s:").append(payload.substr(offset, static_cast<std::size_t>(length)));
                    offset += static_cast<std::size_t>(length);
                    break;
                }
            }
        }
        return logged;
    }

    static void maria_column_definition(
        std::string& response, const maria_column& definition, unsigned char& sequence) {
        std::string column;
        for (const std::string_view part : {std::string_view("def"), std::string_view(), std::string_view(),
                 std::string_view(), std::string_view(definition.name_), std::string_view()}) {
            column.push_back(static_cast<char>(part.size()));
            column.append(part);
        }
        column.push_back('\x0c');
        // utf8mb4 for text, binary otherwise.
        column.append(definition.type_ >= 0xf5 ? std::string_view("\x2d\0", 2) : std::string_view("\x3f\0", 2));
        for (int shift = 0; shift < 32; shift += 8) {
            column.push_back(static_cast<char>((definition.length_ >> shift) & 255));
        }
        column.push_back(static_cast<char>(definition.type_));
        column.push_back(static_cast<char>(definition.flags_ & 255));
        column.push_back(static_cast<char>(definition.flags_ >> 8));
        column.push_back(static_cast<char>(definition.decimals_));
        column.append(2, '\0');
        maria_packet(response, column, sequence++);
    }

    [[nodiscard]] static std::vector<maria_column> maria_columns(const maria_reply& reply) {
        if (!reply.columns_.empty()) {
            return reply.columns_;
        }
        if (reply.column_.empty()) {
            return {};
        }
        return {maria_column{.name_ = reply.column_}};
    }

    // COM_STMT_PREPARE's answer: the statement id, its column and parameter
    // counts, and their definitions.
    void maria_prepared(std::string& response, std::string_view sql, const maria_reply* executed) {
        const auto id = ++maria_statement_ids_;
        const auto params = static_cast<std::size_t>(std::ranges::count(sql, '?'));
        maria_statements_[id] = maria_statement{params, {}};
        const auto columns = executed == nullptr                  ? std::vector<maria_column>{}
                             : executed->prepare_columns_.empty() ? maria_columns(*executed)
                                                                  : executed->prepare_columns_;
        unsigned char sequence = 1;
        std::string packet(1, '\0');
        for (int shift = 0; shift < 32; shift += 8) {
            packet.push_back(static_cast<char>((id >> shift) & 255));
        }
        packet.push_back(static_cast<char>(columns.size() & 255));
        packet.push_back(static_cast<char>(columns.size() >> 8));
        packet.push_back(static_cast<char>(params & 255));
        packet.push_back(static_cast<char>(params >> 8));
        packet.append(3, '\0');
        maria_packet(response, packet, sequence++);
        std::string eof("\xfe\0\0\x02\0", 5);
        if (params != 0) {
            for (std::size_t index = 0; index < params; ++index) {
                maria_column_definition(response, {.name_ = "?"}, sequence);
            }
            maria_packet(response, eof, sequence++);
        }
        if (!columns.empty()) {
            for (const auto& column : columns) {
                maria_column_definition(response, column, sequence);
            }
            maria_packet(response, eof, sequence++);
        }
    }

    // The packets answering one command, sequence numbers from 1: text rows
    // for a COM_QUERY, binary rows for a COM_STMT_EXECUTE.
    static void maria_response(std::string& response, const maria_reply& reply, bool binary) {
        unsigned char sequence = 1;
        const auto ok = [&](std::uint64_t affected, std::uint64_t insert_id, std::uint16_t status,
                            const std::vector<std::pair<std::string, std::string>>& tracked) {
            maria_packet(response, maria_ok(affected, insert_id, status, tracked), sequence++);
        };
        const auto error = [&] {
            std::string packet(1, '\xff');
            packet.push_back(static_cast<char>(reply.code_ & 255));
            packet.push_back(static_cast<char>(reply.code_ >> 8));
            packet.push_back('#');
            packet.append(reply.sql_state_);
            packet.append(reply.error_);
            maria_packet(response, packet, sequence++);
        };
        if (!reply.error_.empty() && !reply.error_after_rows_) {
            error();
            return;
        }
        const auto columns = binary ? maria_columns(reply) : maria_columns({.column_ = reply.column_});
        if (columns.empty()) {
            ok(reply.affected_rows_, reply.insert_id_, reply.status_, reply.tracked_);
            return;
        }
        std::string count;
        maria_length(count, columns.size());
        maria_packet(response, count, sequence++);
        for (const auto& column : columns) {
            maria_column_definition(response, column, sequence);
        }
        std::string eof("\xfe\0\0", 3);
        maria_status(eof, reply.status_);
        maria_packet(response, eof, sequence++);
        const auto put_text = [](std::string& row, std::string_view value) {
            maria_length(row, value.size());
            row.append(value);
        };
        if (binary && !reply.columns_.empty()) {
            for (const auto& values : reply.binary_rows_) {
                std::string row(1, '\0');
                const auto bitmap = row.size();
                row.append((columns.size() + 9) / 8, '\0');
                for (std::size_t index = 0; index < values.size(); ++index) {
                    if (!values[index].has_value()) {
                        row[bitmap + (index + 2) / 8] =
                            static_cast<char>(row[bitmap + (index + 2) / 8] | (1 << ((index + 2) % 8)));
                        continue;
                    }
                    row.append(*values[index]);
                }
                maria_packet(response, row, sequence++);
            }
        } else {
            for (std::size_t index = 0; index < reply.rows_.size(); ++index) {
                const bool null = index < reply.null_rows_.size() && reply.null_rows_[index];
                std::string row;
                if (binary) {
                    row.push_back('\0');
                    row.push_back(null ? '\x04' : '\0');
                    if (!null) {
                        put_text(row, reply.rows_[index]);
                    }
                } else if (null) {
                    row.push_back('\xfb');
                } else {
                    put_text(row, reply.rows_[index]);
                }
                maria_packet(response, row, sequence++);
            }
        }
        if (reply.error_after_rows_) {
            error();
            return;
        }
        // Server status bit 8: more results follow.
        eof = std::string("\xfe\0\0", 3);
        maria_status(eof, static_cast<std::uint16_t>(reply.procedure_ ? reply.status_ | 8 : reply.status_));
        maria_packet(response, eof, sequence++);
        if (reply.procedure_) {
            ok(0, 0, reply.status_, {});
        }
    }

    // Answers a LOAD DATA LOCAL INFILE: asks for reply.local_infile_, reads the
    // file's packets up to the empty one that ends it, and answers OK. Returns
    // true when a packet out of sequence closed the connection.
    asio::awaitable<bool> maria_local_infile(asio::ip::tcp::socket& socket, const maria_reply& reply) {
        unsigned char sequence = 1;
        co_await maria_packet(socket, "\xfb" + reply.local_infile_, sequence++);
        std::string content;
        for (;;) {
            std::array<unsigned char, 4> header{};
            co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
            const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) | (std::size_t{header[2]} << 16);
            std::string payload(size, '\0');
            if (size != 0) {
                co_await asio::async_read(socket, asio::buffer(payload), asio::use_awaitable);
            }
            if (header[3] != sequence) {
                maria_commands_.push_back("desync");
                socket.close();
                co_return true;
            }
            ++sequence;
            if (size == 0) {
                break;
            }
            content += payload;
        }
        maria_commands_.push_back("infile:" + content);
        std::string ok(1, '\0');
        maria_length(ok, reply.affected_rows_);
        maria_length(ok, 0);
        maria_status(ok, reply.status_);
        ok.append(2, '\0');
        co_await maria_packet(socket, ok, sequence);
        co_return false;
    }

    // Answers the script from next_maria_ in order, each batch of commands in
    // one write. A prepare is answered at once, and a close needs no answer.
    // Returns true when a dropping reply closed the connection.
    asio::awaitable<bool> maria_script(asio::ip::tcp::socket& socket) {
        while (next_maria_ < maria_replies_.size()) {
            const auto& reply = maria_replies_[next_maria_];
            while (reply.read_hold_ != 0 &&
                   std::ranges::find(released_holds_, reply.read_hold_) == released_holds_.end() && !closing_) {
                std::error_code ignored;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
            }
            if (closing_) {
                co_return false;
            }
            std::vector<char> answered;
            while (answered.size() < std::max<std::size_t>(reply.batch_, 1)) {
                const auto request = co_await maria_command(socket);
                if (!request) {
                    co_return false;
                }
                if (request->type_ == '\x19') {
                    continue;
                }
                if (request->type_ == '\x1a') {
                    // A statement reset is answered at once.
                    const auto ok = maria_ok(0, 0, 2, {});
                    std::string packet;
                    maria_packet(packet, ok, 1);
                    co_await asio::async_write(socket, asio::buffer(packet), asio::use_awaitable);
                    continue;
                }
                const auto taken = next_maria_ + answered.size();
                const auto* next = taken < maria_replies_.size() ? &maria_replies_[taken] : nullptr;
                if (request->type_ == '\x16' && (next == nullptr || !next->prepare_fails_)) {
                    std::string prepared;
                    maria_prepared(prepared, request->text_, next);
                    co_await asio::async_write(socket, asio::buffer(prepared), asio::use_awaitable);
                    continue;
                }
                answered.push_back(request->type_);
            }
            while (held(reply) && !closing_) {
                std::error_code ignored;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
            }
            if (closing_) {
                co_return false;
            }
            if (reply.drop_) {
                ++next_maria_;
                socket.close();
                co_return true;
            }
            if (!reply.local_infile_.empty()) {
                ++next_maria_;
                if (co_await maria_local_infile(socket, reply)) {
                    co_return true;
                }
                continue;
            }
            std::string response;
            for (const char type : answered) {
                if (next_maria_ == maria_replies_.size()) {
                    break;
                }
                maria_response(response, maria_replies_[next_maria_++], type == '\x17');
            }
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        }
        co_return false;
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
                // EOF/reset/connection abort/broken pipe are the client's public close path
                // (a reply written after the client closed). Only cancellation
                // caused by this peer's own teardown may suppress an aborted wait.
                if (error.code() != asio::error::eof &&
                    error.code() != asio::error::connection_reset &&
                    error.code() != asio::error::connection_aborted &&
                    error.code() != asio::error::broken_pipe &&
                    !(closing_ && (error.code() == asio::error::operation_aborted ||
                                      error.code() == asio::error::bad_descriptor))) {
                    throw;
                }
            }
            const bool script_done = selected == protocol::mariadb ? next_maria_ >= maria_replies_.size()
                                                                   : next_reply_ >= postgres_replies_.size();
            if (closing_ || after_startup_ != after_startup::reaccept || script_done) {
                co_return;
            }
            socket = asio::ip::tcp::socket(io_);
        }
    }

    asio::awaitable<void> serve_connection(protocol selected, asio::ip::tcp::socket& socket) {
        co_await acceptor_.async_accept(socket, asio::use_awaitable);
        const auto connection = accepted_++;
        // Named statements belong to one server session.
        std::map<std::string, std::size_t> statements;
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
        if (selected == protocol::postgresql) {
            // A connection with a script of its own serves that one.
            const bool own = connection < connection_replies_.size();
            auto& replies = own ? connection_replies_[connection] : postgres_replies_;
            auto& next = own ? connection_next_[connection] : next_reply_;
            if (!replies.empty() && co_await postgres_script(socket, replies, next, statements)) {
                co_return;
            }
        }
        if (selected == protocol::mariadb && !maria_replies_.empty() && co_await maria_script(socket)) {
            co_return;
        }
        std::array<char, 64> buffer{};
        for (;;) {
            co_await socket.async_read_some(asio::buffer(buffer), asio::use_awaitable);
        }
    }

    asio::io_context io_;
    asio::steady_timer gate_;
    bool defer_authentication_;
    // The MariaDB peer announces session tracking and answers its setup.
    bool maria_session_tracking_;
    after_startup after_startup_;
    std::vector<postgres_reply> postgres_replies_;
    // Scripts of the connections that have their own, in accept order.
    std::vector<std::vector<postgres_reply>> connection_replies_;
    std::vector<std::size_t> connection_next_;
    std::vector<postgres_parse> postgres_parses_;
    std::vector<maria_reply> maria_replies_;
    std::vector<std::string> maria_queries_;
    std::vector<std::string> maria_commands_;
    std::map<std::uint32_t, maria_statement> maria_statements_;
    std::uint32_t maria_statement_ids_{0};
    std::size_t next_maria_{0};
    std::vector<std::size_t> released_holds_;
    std::size_t accepted_{0};
    std::size_t next_reply_{0};
    std::size_t startup_count_{0};
    std::size_t maria_session_setups_{0};
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
