#if defined(RUVIA_ENABLE_POSTGRESQL) && defined(RUVIA_ENABLE_REDIS)

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/db/db_session.h"
#include "ruvia/web/redis/redis_session.h"

#include "backend_client_fixture.h"
#include "worker_task_fixture.h"
// Removing a query-result cache id must drop every result a repository builder
// cached under that id, so the next get_one/get_exists/get_count reads the
// database again.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/web/db/db.h"

#include "test_exceptions.h"

namespace {

RUVIA_DB_ENTITY(record, "records", ruvia::db_column<"v", std::string>)

constexpr std::string_view cache_id = "item";
constexpr std::chrono::milliseconds cache_lifetime = std::chrono::minutes(1);
// What every cache entry starts with, as the framework writes it.
constexpr std::string_view cache_entry_magic = "RUVIAQC";

// A RESP2 key-value store answering GET, SET, DEL and prefix SCAN on every accepted
// connection. Without credentials or a database index the client sends no
// handshake. All connections run on one thread, so the map needs no lock.
class redis_store_peer final {
public:
    redis_store_peer()
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
          work_(asio::make_work_guard(io_)),
          thread_([this] { io_.run(); }) {
        asio::co_spawn(io_, accept_connections(), [this](std::exception_ptr failure) { record_failure(failure); });
    }

    redis_store_peer(const redis_store_peer&) = delete;
    redis_store_peer& operator=(const redis_store_peer&) = delete;

    ~redis_store_peer() {
        asio::post(io_, [this] {
            closing_ = true;
            std::error_code ignored;
            acceptor_.close(ignored);
            for (const auto& socket : sockets_) {
                socket->close(ignored);
            }
            work_.reset();
        });
        thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    // Every later GET answers `value`, whatever the key, until the peer ends.
    void poison_reads(std::string_view value) {
        std::promise<void> poisoned;
        auto result = poisoned.get_future();
        asio::post(io_, [this, &poisoned, value = std::string(value)]() mutable {
            poison_ = std::move(value);
            poisoned.set_value();
        });
        result.get();
    }

    void stall_next_read(std::function<void()> on_read) {
        std::promise<void> configured;
        auto ready = configured.get_future();
        asio::post(io_, [this, &configured, on_read = std::move(on_read)]() mutable {
            on_read_ = std::move(on_read);
            configured.set_value();
        });
        ready.get();
    }

    // Rethrows the first connection failure instead of reporting a count.
    [[nodiscard]] std::size_t key_count() {
        std::promise<std::size_t> counted;
        auto result = counted.get_future();
        asio::post(io_, [this, &counted] {
            if (failure_) {
                counted.set_exception(failure_);
            } else {
                counted.set_value(store_.size());
            }
        });
        return result.get();
    }

private:
    using socket_type = asio::ip::tcp::socket;

    void record_failure(std::exception_ptr failure) noexcept {
        if (failure && !failure_) {
            failure_ = failure;
        }
    }

    [[nodiscard]] bool closed(const std::error_code& error) const noexcept {
        return error == asio::error::eof || error == asio::error::connection_reset ||
               error == asio::error::connection_aborted ||
               (closing_ && (error == asio::error::operation_aborted || error == asio::error::bad_descriptor));
    }

    asio::awaitable<void> accept_connections() {
        try {
            for (;;) {
                auto socket = std::make_shared<socket_type>(io_);
                co_await acceptor_.async_accept(*socket, asio::use_awaitable);
                sockets_.push_back(socket);
                asio::co_spawn(io_, serve(socket), [this](std::exception_ptr failure) { record_failure(failure); });
            }
        } catch (const std::system_error& error) {
            if (!closed(error.code())) {
                throw;
            }
        }
    }

    static asio::awaitable<std::string> read_line(socket_type& socket, std::string& buffer) {
        const auto size = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffer), "\r\n", asio::use_awaitable);
        std::string line(std::string_view(buffer).substr(0, size - 2));
        buffer.erase(0, size);
        co_return line;
    }

    static asio::awaitable<std::vector<std::string>> read_command(socket_type& socket, std::string& buffer) {
        const auto header = co_await read_line(socket, buffer);
        if (header.size() < 2 || header.front() != '*') {
            throw std::runtime_error("invalid RESP command array");
        }
        const auto count = std::stoull(std::string(std::string_view(header).substr(1)));
        std::vector<std::string> arguments{};
        arguments.reserve(count);
        for (std::size_t index = 0; index != count; ++index) {
            const auto length_line = co_await read_line(socket, buffer);
            if (length_line.size() < 2 || length_line.front() != '$') {
                throw std::runtime_error("invalid RESP bulk string");
            }
            const auto length = static_cast<std::size_t>(std::stoull(std::string(std::string_view(length_line).substr(1))));
            if (buffer.size() < length + 2) {
                co_await asio::async_read(socket, asio::dynamic_buffer(buffer),
                    asio::transfer_exactly(length + 2 - buffer.size()), asio::use_awaitable);
            }
            if (buffer.compare(length, 2, "\r\n") != 0) {
                throw std::runtime_error("invalid RESP bulk ending");
            }
            arguments.emplace_back(std::string_view(buffer).substr(0, length));
            buffer.erase(0, length + 2);
        }
        co_return arguments;
    }

    // Expiration options after a SET value are accepted and not enforced; the
    // scenario completes well within the cache lifetime.
    [[nodiscard]] std::string execute(std::vector<std::string>& arguments) {
        if (arguments.empty()) {
            return std::string("-ERR empty command\r\n");
        }
        auto& name = arguments.front();
        std::ranges::transform(name, name.begin(), [](unsigned char value) {
            return static_cast<char>(std::toupper(value));
        });
        if (name == "GET" && arguments.size() == 2) {
            if (poison_) {
                return "$" + std::string(std::to_string(poison_->size())) + "\r\n" + *poison_ + "\r\n";
            }
            const auto found = store_.find(arguments[1]);
            if (found == store_.end()) {
                return std::string("$-1\r\n");
            }
            return "$" + std::string(std::to_string(found->second.size())) + std::string("\r\n") + found->second + "\r\n";
        }
        if (name == "SET" && arguments.size() >= 3) {
            store_.insert_or_assign(std::move(arguments[1]), std::move(arguments[2]));
            return std::string("+OK\r\n");
        }
        if (name == "DEL" && arguments.size() >= 2) {
            std::size_t removed = 0;
            for (std::size_t index = 1; index != arguments.size(); ++index) {
                removed += store_.erase(arguments[index]);
            }
            return ":" + std::string(std::to_string(removed)) + "\r\n";
        }
        if (name == "SCAN" && arguments.size() == 6 && arguments[1] == "0" &&
            arguments[2] == "MATCH" && arguments[3].ends_with('*') && arguments[4] == "COUNT") {
            const auto prefix = std::string_view(arguments[3]).substr(0, arguments[3].size() - 1);
            std::string values{};
            std::size_t count = 0;
            for (const auto& [key, value] : store_) {
                (void)value;
                if (key.starts_with(prefix)) {
                    values.append("$").append(std::string(std::to_string(key.size()))).append("\r\n").append(key).append("\r\n");
                    ++count;
                }
            }
            return "*2\r\n$1\r\n0\r\n*" + std::string(std::to_string(count)) + std::string("\r\n") + values;
        }
        return std::string("-ERR unsupported command\r\n");
    }

    asio::awaitable<void> serve(std::shared_ptr<socket_type> socket) {
        std::string buffer{};
        try {
            for (;;) {
                auto arguments = co_await read_command(*socket, buffer);
                if (!arguments.empty() && arguments.front() == "GET" && on_read_) {
                    auto on_read = std::exchange(on_read_, {});
                    on_read();
                    continue;
                }
                const auto reply = execute(arguments);
                co_await asio::async_write(*socket, asio::buffer(reply), asio::use_awaitable);
            }
        } catch (const std::system_error& error) {
            if (!closed(error.code())) {
                throw;
            }
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::vector<std::shared_ptr<socket_type>> sockets_{};
    std::map<std::string, std::string, std::less<>> store_{};
    std::optional<std::string> poison_;
    std::function<void()> on_read_;
    std::exception_ptr failure_;
    bool closing_{false};
    std::thread thread_;
};

// Every read names the same cache id; each result shape stores its own entry.
ruvia::task<std::string> read_one(const ruvia::db_session& handle) {
    const auto row =
        co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_one();
    co_return row ? std::string(std::string_view(row->get<"v">())) : std::string("missing");
}

ruvia::task<bool> read_exists(const ruvia::db_session& handle) {
    co_return co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_exists();
}

ruvia::task<std::uint64_t> read_count(const ruvia::db_session& handle) {
    co_return co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_count();
}

ruvia::task<std::vector<std::string>> read_many(const ruvia::db_session& handle, std::string_view id = cache_id) {
    const auto rows =
        co_await handle.repository<record>().query_builder().cache(id, cache_lifetime).get_many();
    std::vector<std::string> values{};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        values.emplace_back(std::string_view(rows[index].get<"v">()));
    }
    co_return values;
}

ruvia::task<std::string> read_sql_cached(const ruvia::db_session& handle, std::string_view parameter) {
    auto query = handle.repository<record>().query_builder();
    query.where(record::column<"v">() == parameter).cache(cache_lifetime);
    const auto row = co_await query.get_one();
    co_return row ? std::string(std::string_view(row->get<"v">())) : std::string("missing");
}

}  // namespace

namespace query_cache_test {

// What one concurrent cached read observed. Failures are recorded rather than
// thrown so every reader in a scope finishes on its own.
struct read_outcome final {
    std::string value_{};
    std::optional<ruvia::db_error::code_type> error_;
    bool done_{false};
};

ruvia::task<void> read_sql_into(ruvia::db_session handle, std::string_view parameter, read_outcome& outcome) {
    try {
        outcome.value_ = co_await read_sql_cached(handle, parameter);
    } catch (const ruvia::db_error& error) {
        outcome.error_ = error.code();
    }
    outcome.done_ = true;
}

ruvia::task<void> read_many_into(ruvia::db_session handle, read_outcome& outcome) {
    try {
        const auto values = co_await read_many(handle);
        outcome.value_ = values.empty() ? std::string("missing") : values.front();
    } catch (const ruvia::db_error& error) {
        outcome.error_ = error.code();
    }
    outcome.done_ = true;
}

// Waits on the worker, a millisecond at a time, until `ready` holds.
template <typename predicate_type>
ruvia::task<void> wait_until(const ruvia::worker_ref& worker, predicate_type ready) {
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        if (std::chrono::steady_clock::now() > give_up) {
            throw std::runtime_error("query cache test condition was not reached");
        }
        (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(1));
    }
}

// Runs `scenario` with a connected Redis client and a database client that
// caches through it, on one event loop; both shut down before a failure is
// reported.
template <typename scenario_type>
void run_with_query_cache(redis_store_peer& redis_peer, const ruvia::db_config& database_config,
    scenario_type& scenario, std::optional<std::size_t> max_reply_bytes = ruvia::redis_config{}.max_reply_bytes_) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    redis_config.max_reply_bytes_ = max_reply_bytes;
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            const auto store = redis.redis();
            ruvia::db_client database(attachment.loop(), database_config, store, ruvia::db_cache_config{});
            try {
                co_await database.connect();
                co_await scenario(database, store);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

}  // namespace query_cache_test

TEST(Ruvia, db_query_result_cache_remove_invalidates_repository_result_shapes) {
    redis_store_peer redis_peer;
    // The database answers the statements the cache lets through, in order. A
    // read served from the cache consumes no reply, so a stale entry shows up
    // as the previous value and an unexpected database read as the next one.
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({
            {.rows_ = std::vector<std::string>({std::string("one"), std::string("two"), std::string("three")})},
            {.rows_ = std::vector<std::string>({std::string("alpha")})},
            {.column_ = std::string("exists"), .rows_ = std::vector<std::string>({std::string("t")})},
            {.column_ = std::string("count"), .rows_ = std::vector<std::string>({std::string("3")})},
            {.rows_ = std::vector<std::string>({std::string("gamma")})},
            {.rows_ = std::vector<std::string>({std::string("beta")})},
            {.column_ = std::string("exists"), .rows_ = std::vector<std::string>({std::string("f")})},
            {.column_ = std::string("count"), .rows_ = std::vector<std::string>({std::string("5")})},
            {.rows_ = std::vector<std::string>({std::string("delta")})},
            {.rows_ = std::vector<std::string>({std::string("epsilon")})},
        }));
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            const auto store = redis.redis();
            ruvia::db_client database(attachment.loop(), database_peer.config(), store, ruvia::db_cache_config{});
            try {
                co_await database.connect();
                const auto handle = database.db();
                // First reads come from the database and fill the cache.
                const std::vector<std::string> many({std::string("one"), std::string("two"), std::string("three")});
                EXPECT_TRUE(co_await read_many(handle) == many);
                EXPECT_EQ(co_await read_one(handle), std::string("alpha"));
                EXPECT_TRUE(co_await read_exists(handle));
                EXPECT_EQ(co_await read_count(handle), std::uint64_t{3});
                EXPECT_TRUE(redis_peer.key_count() != 0);
                // Repeated reads are served from the cache; every decoded row
                // resolves its column through the result's shared names.
                EXPECT_TRUE(co_await read_many(handle) == many);
                EXPECT_EQ(co_await read_one(handle), std::string("alpha"));
                EXPECT_TRUE(co_await read_exists(handle));
                EXPECT_EQ(co_await read_count(handle), std::uint64_t{3});
                auto raw_query = handle.repository<record>().query_builder();
                raw_query.cache(cache_id, cache_lifetime);
                auto cached_rows = co_await raw_query.get_raw_many();
                auto moved_rows = std::move(cached_rows);
                // Subsequent Redis reads must not invalidate an earlier result.
                EXPECT_TRUE(co_await read_many(handle) == many);
                EXPECT_EQ(moved_rows.size(), many.size());
                for (std::size_t index = 0; index < many.size(); ++index) {
                    EXPECT_TRUE(moved_rows[index]["v"].value() == many[index]);
                }
                // A user id that spells another id plus a shape name is its own
                // entry: it reads the database instead of the "one" result.
                constexpr std::string_view suffixed_id = "item-one";
                const std::vector<std::string> gamma({std::string("gamma")});
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == gamma);
                // Removing the caller's id drops every result shape cached under
                // it and leaves the independent id's entry alone.
                co_await handle.query_result_cache().remove(cache_id);
                EXPECT_EQ(redis_peer.key_count(), std::size_t{1});
                EXPECT_EQ(co_await read_one(handle), std::string("beta"));
                EXPECT_TRUE(!(co_await read_exists(handle)));
                EXPECT_EQ(co_await read_count(handle), std::uint64_t{5});
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == gamma);
                // A multi-id removal drops every shape for each id.
                co_await handle.query_result_cache().remove(cache_id, suffixed_id);
                EXPECT_EQ(redis_peer.key_count(), std::size_t{0});
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == std::vector<std::string>({std::string("delta")}));
                // Clear uses the same cached namespace prefix without touching
                // an unrelated Redis key.
                (void)co_await store.set("outside-cache", "keep");
                co_await handle.query_result_cache().clear();
                EXPECT_EQ(redis_peer.key_count(), std::size_t{1});
                const auto outside = co_await store.get("outside-cache");
                EXPECT_TRUE(outside.has_value() && *outside == "keep");
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == std::vector<std::string>({std::string("epsilon")}));
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

TEST(Ruvia, db_query_result_cache_distinguishes_sql_parameter_values) {
    redis_store_peer redis_peer;
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("first")})}, {.rows_ = std::vector<std::string>({std::string("second")})}}));
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            ruvia::db_client database(attachment.loop(), database_peer.config(), redis.redis(), ruvia::db_cache_config{});
            try {
                co_await database.connect();
                const auto handle = database.db();
                const std::string large_parameter(8192, 'x');
                std::string different_parameter(large_parameter);
                different_parameter.back() = 'y';
                EXPECT_EQ(co_await read_sql_cached(handle, large_parameter), std::string("first"));
                EXPECT_EQ(co_await read_sql_cached(handle, large_parameter), std::string("first"));
                EXPECT_EQ(co_await read_sql_cached(handle, different_parameter), std::string("second"));
                EXPECT_EQ(co_await read_sql_cached(handle, large_parameter), std::string("first"));
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

TEST(Ruvia, db_query_result_cache_preserves_empty_and_zero_column_results) {
    redis_store_peer redis_peer;
    // The entry magic, then little-endian 64-bit row and column counts.
    std::string empty(cache_entry_magic);
    const auto row_count_offset = cache_entry_magic.size();
    const auto column_count_offset = row_count_offset + 8;
    empty.append(16, '\0');
    redis_peer.poison_reads(empty);
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>());
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            ruvia::db_client database(attachment.loop(), database_peer.config(), redis.redis(), ruvia::db_cache_config{});
            try {
                co_await database.connect();
                auto query = database.db().repository<record>().query_builder();
                query.cache(cache_id, cache_lifetime);
                auto empty_rows = co_await query.get_raw_many();
                EXPECT_TRUE(empty_rows.empty());
                auto empty_with_schema = decltype(empty)(empty);
                empty_with_schema[column_count_offset] = 1;
                empty_with_schema.push_back('\1');
                empty_with_schema.append(7, '\0');
                empty_with_schema.push_back('v');
                redis_peer.poison_reads(std::move(empty_with_schema));
                auto named_empty_rows = co_await query.get_raw_many();
                EXPECT_TRUE(named_empty_rows.empty());
                auto zero_columns = decltype(empty)(empty);
                zero_columns[row_count_offset] = 2;
                zero_columns.append(2, '\2');
                redis_peer.poison_reads(std::move(zero_columns));
                auto rows = co_await query.get_raw_many();
                EXPECT_EQ(rows.size(), 2u);
                EXPECT_TRUE(rows[0].empty());
                EXPECT_TRUE(rows[1].empty());
                EXPECT_TRUE(empty_rows.empty());
                EXPECT_TRUE(named_empty_rows.empty());
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

// A corrupt entry claiming an enormous count of zero-column rows is an invalid
// cache entry: it is rejected before any row storage is reserved, so a cache
// that ignores errors reads the database instead.
TEST(Ruvia, db_query_result_cache_rejects_corrupt_zero_column_entries) {
    redis_store_peer redis_peer;
    std::string corrupt(cache_entry_magic);
    corrupt.append(8, '\xff');
    corrupt.append(8, '\0');
    redis_peer.poison_reads(std::move(corrupt));
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("fresh")})}}));
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            const auto store = redis.redis();
            ruvia::db_client database(
                attachment.loop(), database_peer.config(), store, ruvia::db_cache_config{.ignore_errors_ = true});
            try {
                co_await database.connect();
                const std::vector<std::string> fresh({std::string("fresh")});
                EXPECT_TRUE(co_await read_many(database.db()) == fresh);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

TEST(Ruvia, db_client_shutdown_cancels_inflight_cache_without_closing_borrowed_redis) {
    redis_store_peer redis_peer;
    ruvia::test::backend_startup_peer database_peer(
        ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            ruvia::db_client database(attachment.loop(), database_peer.config(), redis.redis(), ruvia::db_cache_config{});
            try {
                co_await database.connect();
                redis_peer.stall_next_read([&database] { database.close(); });
                bool cancelled = false;
                try {
                    (void)co_await read_sql_cached(database.db(), "cancel-cache");
                } catch (const ruvia::redis_error& error) {
                    cancelled = error.code() == ruvia::redis_error::code_type::cancelled ||
                                error.code() == ruvia::redis_error::code_type::closing;
                }
                EXPECT_TRUE(cancelled);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
            EXPECT_TRUE(!(co_await redis.redis().get("still-open")));
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

// An entry whose GET reply would exceed the Redis reply limit is never
// written: the result is returned uncached, and the next read of that key goes
// to the database instead of failing on an oversized reply. Smaller entries
// are still cached.
TEST(Ruvia, db_query_result_cache_skips_entries_beyond_the_redis_reply_limit) {
    using peer_type = ruvia::test::backend_startup_peer;
    redis_store_peer redis_peer;
    const std::string large(512, 'x');
    const std::string larger(513, 'y');
    peer_type database_peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string(large)})}, {.rows_ = std::vector<std::string>({std::string(larger)})}, {.rows_ = std::vector<std::string>({std::string("small")})}}));
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_session&) -> ruvia::task<void> {
        const auto handle = database.db().with_options({.timeout_ = std::chrono::seconds(5)});
        EXPECT_EQ(co_await read_sql_cached(handle, "large"), large);
        EXPECT_EQ(redis_peer.key_count(), std::size_t{0});
        EXPECT_EQ(co_await read_sql_cached(handle, "large"), larger);
        EXPECT_EQ(co_await read_sql_cached(handle, "small"), std::string("small"));
        EXPECT_EQ(co_await read_sql_cached(handle, "small"), std::string("small"));
        EXPECT_EQ(redis_peer.key_count(), std::size_t{1});
    };
    query_cache_test::run_with_query_cache(redis_peer, database_peer.config(), scenario, std::size_t{256});
}

// Concurrent misses of one key on one worker run the statement once: the
// first reader queries the database and the others take its result.
TEST(Ruvia, db_query_result_cache_coalesces_concurrent_misses_of_one_key) {
    using peer_type = ruvia::test::backend_startup_peer;
    redis_store_peer redis_peer;
    peer_type database_peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("first")}), .deferred_ = true}}));
    std::array<query_cache_test::read_outcome, 3> outcomes;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_session& store) -> ruvia::task<void> {
        const auto handle = database.db().with_options({.timeout_ = std::chrono::seconds(5)});
        ruvia::task_scope readers(database.worker());
        for (auto& outcome : outcomes) {
            readers.spawn(query_cache_test::read_sql_into(handle, "same", outcome));
        }
        co_await query_cache_test::wait_until(
            database.worker(), [&] { return database_peer.postgres_parses().size() == 1; });
        // Redis answers in order on its one connection: once this reply is in,
        // every reader has seen its miss.
        (void)co_await store.get("barrier");
        database_peer.release_replies();
        co_await readers.join();
    };
    query_cache_test::run_with_query_cache(redis_peer, database_peer.config(), scenario);
    for (const auto& outcome : outcomes) {
        EXPECT_TRUE(!outcome.error_.has_value());
        EXPECT_TRUE(outcome.value_ == "first");
    }
    EXPECT_EQ(database_peer.postgres_parses().size(), std::size_t{1});
    EXPECT_EQ(redis_peer.key_count(), std::size_t{1});
}

// A cancelled leader ends its fill without a result: the reader waiting for it
// reads the cache again and runs the statement itself. The leader's abandoned
// statement takes its connection with it, so the takeover connects anew.
TEST(Ruvia, db_query_result_cache_waiter_takes_over_from_a_cancelled_leader) {
    using peer_type = ruvia::test::backend_startup_peer;
    redis_store_peer redis_peer;
    peer_type database_peer(peer_type::protocol::postgresql, false, peer_type::after_startup::reaccept,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("abandoned")}), .deferred_ = true}, {.rows_ = std::vector<std::string>({std::string("taken over")})}}));
    query_cache_test::read_outcome leader;
    query_cache_test::read_outcome waiter;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_session& store) -> ruvia::task<void> {
        const auto handle = database.db().with_options({.timeout_ = std::chrono::seconds(5)});
        ruvia::stop_source stop;
        ruvia::task_scope readers(database.worker());
        readers.spawn(query_cache_test::read_sql_into(
            handle.with_options({.timeout_ = std::chrono::seconds(5), .stop_token_ = stop.token()}), "same", leader));
        readers.spawn(query_cache_test::read_sql_into(handle, "same", waiter));
        co_await query_cache_test::wait_until(
            database.worker(), [&] { return database_peer.postgres_parses().size() == 1; });
        (void)co_await store.get("barrier");
        stop.request_stop();
        co_await query_cache_test::wait_until(database.worker(), [&] { return leader.done_; });
        database_peer.release_replies();
        co_await readers.join();
    };
    query_cache_test::run_with_query_cache(redis_peer, database_peer.config(), scenario);
    EXPECT_TRUE(leader.error_ == ruvia::db_error::code_type::cancelled);
    EXPECT_TRUE(!waiter.error_.has_value());
    EXPECT_TRUE(waiter.value_ == "taken over");
    EXPECT_EQ(database_peer.postgres_parses().size(), std::size_t{2});
    EXPECT_EQ(database_peer.startups(), std::size_t{2});
}

// Waiting for another reader's fill is bounded by the waiter's own timeout,
// whatever the leader's.
TEST(Ruvia, db_query_result_cache_waiter_is_bounded_by_its_own_timeout) {
    using peer_type = ruvia::test::backend_startup_peer;
    redis_store_peer redis_peer;
    peer_type database_peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("first")}), .deferred_ = true}}));
    query_cache_test::read_outcome leader;
    query_cache_test::read_outcome waiter;
    bool waiter_finished_first = false;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_session&) -> ruvia::task<void> {
        const auto handle = database.db().with_options({.timeout_ = std::chrono::seconds(5)});
        ruvia::task_scope readers(database.worker());
        readers.spawn(query_cache_test::read_sql_into(handle, "same", leader));
        readers.spawn(query_cache_test::read_sql_into(
            handle.with_options({.timeout_ = std::chrono::milliseconds(100)}), "same", waiter));
        co_await query_cache_test::wait_until(database.worker(), [&] { return waiter.done_; });
        waiter_finished_first = !leader.done_;
        database_peer.release_replies();
        co_await readers.join();
    };
    query_cache_test::run_with_query_cache(redis_peer, database_peer.config(), scenario);
    EXPECT_TRUE(waiter_finished_first);
    EXPECT_TRUE(waiter.error_ == ruvia::db_error::code_type::timeout);
    EXPECT_TRUE(!leader.error_.has_value());
    EXPECT_TRUE(leader.value_ == "first");
    EXPECT_EQ(database_peer.postgres_parses().size(), std::size_t{1});
}

// remove() detaches the fills of its keys on this worker: a reader arriving
// after it runs its own statement instead of taking the result of a fill that
// started before the removal.
TEST(Ruvia, db_query_result_cache_remove_detaches_inflight_fills) {
    using peer_type = ruvia::test::backend_startup_peer;
    redis_store_peer redis_peer;
    peer_type database_peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("stale")}), .deferred_ = true}, {.rows_ = std::vector<std::string>({std::string("fresh")})}}));
    query_cache_test::read_outcome before;
    query_cache_test::read_outcome after;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_session& store) -> ruvia::task<void> {
        const auto handle = database.db().with_options({.timeout_ = std::chrono::seconds(5)});
        ruvia::task_scope readers(database.worker());
        readers.spawn(query_cache_test::read_many_into(handle, before));
        co_await query_cache_test::wait_until(
            database.worker(), [&] { return database_peer.postgres_parses().size() == 1; });
        co_await handle.query_result_cache().remove(cache_id);
        readers.spawn(query_cache_test::read_many_into(handle, after));
        (void)co_await store.get("barrier");
        database_peer.release_replies();
        co_await readers.join();
    };
    query_cache_test::run_with_query_cache(redis_peer, database_peer.config(), scenario);
    EXPECT_TRUE(!before.error_.has_value());
    EXPECT_TRUE(before.value_ == "stale");
    EXPECT_TRUE(!after.error_.has_value());
    EXPECT_TRUE(after.value_ == "fresh");
}

#endif

using ruvia::db_data_type;
using ruvia::db_driver;
using ruvia::db_generated_type;
using ruvia::db_procedure;
using ruvia::db_schema;
using ruvia::db_schema_column;
using ruvia::db_schema_constraint;
using ruvia::db_table_definition;
using ruvia::db_table_option;
using ruvia::test::throws_on;

RUVIA_DB_ENTITY(enum_entity_type, "enum_events",
    ruvia::db_column<"state", std::string,
        ruvia::db_column_options{.enum_name_ = ruvia::fixed_string{"app.event_state"}, .default_expression_ = ruvia::fixed_string{"'pending'::app.event_state"}}>,
    ruvia::db_column<"created_at", std::string,
        ruvia::db_column_options{.data_type_ = db_data_type::timestamp_tz, .default_expression_ = ruvia::fixed_string{"now()"}}>)

TEST(Ruvia, db_schema_entity_named_enum_and_default_expressions) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<enum_entity_type>();
    const auto migrations = schema.compile("enum_defaults");
    EXPECT_TRUE(migrations[0].sql().find("\"state\" \"app\".\"event_state\" NOT NULL DEFAULT ('pending'::app.event_state)") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("DEFAULT (now())") != std::string_view::npos);
    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] { maria.create_table<enum_entity_type>(); }));
    ruvia::db_query defaults;
    EXPECT_TRUE(throws_on([&] { schema.create_table<enum_entity_type>({.defaults_ = {{"state", defaults.value("ready")}}}); }));
}

RUVIA_DB_ENTITY(schema_target_type, "schema_targets", ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>)
RUVIA_DB_ENTITY(schema_owner_type, "schema_owners",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"parent_id", std::int64_t>, ruvia::db_column<"parent_tenant_id", std::int64_t>,
    ruvia::db_column<"peer_id", std::int64_t>,
    ruvia::db_many_to_one<"parent", schema_target_type, ruvia::db_join_column<"parent_id", "id">,
        ruvia::db_join_column<"parent_tenant_id", "tenant_id">>,
    ruvia::db_one_to_one<"peer", schema_target_type, ruvia::db_join_column<"peer_id", "id">>,
    ruvia::db_one_to_many<"children", schema_target_type, "parent">,
    ruvia::db_many_to_many<"labels", schema_target_type, ruvia::db_join_table<"owner_labels", ruvia::db_join_columns<ruvia::db_join_column<"owner_id", "id">>, ruvia::db_join_columns<ruvia::db_join_column<"label_id", "id">, ruvia::db_join_column<"label_tenant_id", "tenant_id">>>>)

TEST(Ruvia, db_schema_create_table_composite_foreign_key_and_nullable_default) {
    db_schema schema({.driver_ = db_driver::postgresql});
    db_table_definition table_value{.name_ = "tenant_users", .columns_ = {db_schema_column{.name_ = "tenant_id", .type_ = {.data_type_ = db_data_type::big_int}}, db_schema_column{.name_ = "user_id", .type_ = {.data_type_ = db_data_type::big_int}, .nullable_ = true}}, .constraints_ = {db_schema_constraint{.name_ = "tenant_users_pk", .kind_ = ruvia::db_constraint_kind::primary_key, .columns_ = {"tenant_id", "user_id"}}, db_schema_constraint{.name_ = "tenant_fk", .kind_ = ruvia::db_constraint_kind::foreign_key, .columns_ = {"tenant_id", "user_id"}, .referenced_table_ = "tenants", .referenced_columns_ = {"id", "id"}, .on_delete_ = ruvia::db_referential_action::cascade}}};
    schema.create_table(table_value);
    const auto migrations = schema.compile("001_create");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    EXPECT_TRUE(migrations[0].sql().find("PRIMARY KEY") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("FOREIGN KEY") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("ON DELETE CASCADE") != std::string_view::npos);
}

TEST(Ruvia, db_schema_partial_gin_index_quotes_identifiers_and_literals) {
    db_schema schema({.driver_ = db_driver::postgresql});
    ruvia::db_query query;
    const auto value = query.value(ruvia::db_value(std::string_view("x' OR 1=1")));
    const auto predicate = query.binary(query.column("state"), ruvia::db_binary_operator::equal, value);
    schema.create_index({.name_ = "events_idx", .table_ = "events", .keys_ = {{.column_ = "payload"}}, .method_ = ruvia::db_index_method::gin, .where_ = predicate});
    const auto migrations = schema.compile("002_index");
    EXPECT_TRUE(migrations[0].sql().find("USING gin") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("x'' OR 1=1") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("DROP TABLE") == std::string_view::npos);
}

TEST(Ruvia, db_schema_postgresql_batch_is_atomic_and_unwrapped_isolated) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_schema("one");
    schema.create_schema("two");
    auto migrations = schema.compile("003_batch");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    EXPECT_TRUE(migrations[0].sql().find("DO $ruvia$ BEGIN") == 0);

    db_schema invalid({.driver_ = db_driver::postgresql});
    invalid.create_schema("one");
    invalid.create_index({.name_ = "idx", .table_ = "t", .keys_ = {{.column_ = "x"}}, .concurrently_ = true});
    EXPECT_TRUE(throws_on([&] { (void)invalid.compile("004_invalid"); }));
}

TEST(Ruvia, db_schema_enum_and_postgresql_only_operations_reject_mariadb) {
    db_schema schema({.driver_ = db_driver::postgresql});
    const std::array<std::string_view, 2> values{"open", "closed"};
    schema.create_enum("status", values);
    auto migrations = schema.compile("005_enum");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    db_schema addition({.driver_ = db_driver::postgresql});
    addition.add_enum_value("status", "paused");
    const auto added = addition.compile("006_enum_value");
    EXPECT_TRUE(added[0].atomicity() == ruvia::db_migration_atomicity::unwrapped);

    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] { maria.create_enum("status", values); }));
}

RUVIA_DB_ENTITY(app_events_entity_type, "app.events",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true, .generated_ = true}>,
    ruvia::db_column<"payloads", std::vector<std::string>, ruvia::db_column_options{.data_type_ = db_data_type::jsonb}>)

TEST(Ruvia, db_schema_entity_metadata_preserves_explicit_array_element_types) {
    using entity_type = app_events_entity_type;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<entity_type>();
    const auto migrations = schema.compile("entity");
    EXPECT_EQ(migrations[0].sql(), "CREATE TABLE \"app\".\"events\" (\"id\" BIGINT GENERATED BY DEFAULT AS IDENTITY NOT NULL, \"payloads\" JSONB[] NOT NULL, CONSTRAINT \"events_pkey\" PRIMARY KEY (\"id\"))");
}

RUVIA_DB_ENTITY(char_records_entity_type, "char_records",
    ruvia::db_column<"code", std::string, ruvia::db_column_options{.data_type_ = db_data_type::char_value, .length_ = 64}>,
    ruvia::db_column<"codes", std::vector<std::string>, ruvia::db_column_options{.data_type_ = db_data_type::char_value, .length_ = 8}>)

TEST(Ruvia, db_schema_char_entity_and_column_operations_render_with_dialect_limits) {
    using entity_type = char_records_entity_type;
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_table<entity_type>();
    pg.alter_column_type("char_records", "code", {.data_type_ = db_data_type::char_value, .length_ = 32});
    const auto pg_migrations = pg.compile("char_pg");
    EXPECT_TRUE(pg_migrations[0].sql().find("\"code\" CHAR(64) NOT NULL") != std::string_view::npos);
    EXPECT_TRUE(pg_migrations[0].sql().find("\"codes\" CHAR(8)[] NOT NULL") != std::string_view::npos);
    EXPECT_TRUE(pg_migrations[0].sql().find("ALTER TABLE \"char_records\" ALTER COLUMN \"code\" TYPE CHAR(32)") != std::string_view::npos);

    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_table({.name_ = "char_records", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 255}}}});
    EXPECT_TRUE(maria.compile("char_maria")[0].sql().find("`code` CHAR(255) NOT NULL") != std::string_view::npos);

    db_schema pg_limit({.driver_ = db_driver::postgresql});
    pg_limit.create_table({.name_ = "char_limit", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 10485760}}}});
    EXPECT_TRUE(pg_limit.compile("char_pg_limit")[0].sql().find("\"code\" CHAR(10485760) NOT NULL") != std::string_view::npos);

    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 10485761}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::mariadb});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 256}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 8, .precision_ = 2}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::text, .length_ = 8}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::mariadb});
        invalid.create_table({.name_ = "invalid_decimal", .columns_ = {{.name_ = "amount", .type_ = {.data_type_ = db_data_type::numeric}}}});
    }));
    db_schema timestamps({.driver_ = db_driver::mariadb});
    timestamps.create_table({.name_ = "events", .columns_ = {{.name_ = "at", .type_ = {.data_type_ = db_data_type::timestamp}}}});
    EXPECT_TRUE(timestamps.compile("timestamps")[0].sql().find("`at` DATETIME(6)") != std::string_view::npos);
}

RUVIA_DB_ENTITY(computed_events_entity_type, "computed_events",
    ruvia::db_column<"first_name", ruvia::string>,
    ruvia::db_column<"last_name", ruvia::string>,
    ruvia::db_column<"display_name", ruvia::string,
        ruvia::db_column_options{.generated_type_ = db_generated_type::stored}>)
RUVIA_DB_ENTITY(maria_computed_entity_type, "maria_computed",
    ruvia::db_column<"value", std::int64_t>,
    ruvia::db_column<"double_value", std::int64_t,
        ruvia::db_column_options{.generated_type_ = db_generated_type::virtual_value, .nullable_ = true}>)

TEST(Ruvia, db_schema_entity_computed_columns_render_and_validate) {
    using entity_type = computed_events_entity_type;
    ruvia::db_query expression;
    auto generated = expression.binary(expression.column("first_name"), ruvia::db_binary_operator::concat,
        expression.column("last_name"));
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<entity_type>({.generated_columns_ = {{.column_ = "display_name", .expression_ = generated}}});
    const auto migrations = schema.compile("computed");
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("\"display_name\" TEXT GENERATED ALWAYS AS") != std::string_view::npos);
    EXPECT_TRUE(sql.find(" STORED") != std::string_view::npos);
    EXPECT_TRUE(throws_on([&] { schema.create_table<entity_type>(); }));

    using maria_entity_type = maria_computed_entity_type;
    ruvia::db_query maria_expression;
    auto doubled = maria_expression.binary(maria_expression.column("value"), ruvia::db_binary_operator::multiply,
        maria_expression.value(2));
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_table<maria_entity_type>({.generated_columns_ = {{.column_ = "double_value", .expression_ = doubled}}});
    EXPECT_TRUE(maria.compile("maria_computed")[0].sql().find(" VIRTUAL") != std::string_view::npos);
    EXPECT_TRUE(throws_on([&] {
        maria.add_column("maria_computed", {.name_ = "invalid", .type_ = {.data_type_ = db_data_type::big_int}, .generated_type_ = db_generated_type::stored, .as_expression_ = doubled});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema pg({.driver_ = db_driver::postgresql});
        pg.create_table<maria_entity_type>({.generated_columns_ = {{.column_ = "double_value", .expression_ = doubled}}});
    }));
}

RUVIA_DB_ENTITY(invalid_computed_type, "invalid_computed",
    ruvia::db_column<"value", std::int64_t>,
    ruvia::db_column<"total", std::int64_t,
        ruvia::db_column_options{.generated_type_ = db_generated_type::stored}>)

namespace db_schema_column_option_test {

template <typename value_type, auto options>
concept declarable_db_column = requires { typename ruvia::db_column<"value", value_type, options>; };

static_assert(declarable_db_column<std::int64_t, ruvia::db_column_options{.primary_key_ = true, .generated_ = true}>);
static_assert(declarable_db_column<std::string, ruvia::db_column_options{.read_only_ = true}>);
static_assert(declarable_db_column<std::string, ruvia::db_column_options{.read_only_ = true, .create_date_ = true}>);
static_assert(declarable_db_column<std::int32_t, ruvia::db_column_options{.version_ = true}>);
static_assert(declarable_db_column<std::string, ruvia::db_column_options{.delete_date_ = true}>);
// Identity and computed generation exclude each other and read-only columns.
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.generated_ = true, .generated_type_ = db_generated_type::stored}>);
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.generated_ = true, .read_only_ = true}>);
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.generated_type_ = db_generated_type::stored, .read_only_ = true}>);
// Identity needs a non-null integer.
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.generated_ = true}>);
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.generated_ = true, .nullable_ = true}>);
// A named enum type replaces the scalar type.
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.data_type_ = db_data_type::text, .enum_name_ = ruvia::fixed_string{"app.state"}}>);
// version_ is a non-null integer concurrency token.
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.version_ = true}>);
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.nullable_ = true, .version_ = true}>);
// Primary keys cannot be nullable or soft-delete markers.
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.primary_key_ = true, .nullable_ = true}>);
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.primary_key_ = true, .delete_date_ = true}>);
// One lifecycle role per column, and repository-stamped roles must be writable.
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.update_date_ = true, .version_ = true}>);
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.read_only_ = true, .delete_date_ = true}>);
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.generated_type_ = db_generated_type::stored, .update_date_ = true}>);

}  // namespace db_schema_column_option_test

TEST(Ruvia, db_schema_computed_columns_reject_invalid_metadata_and_conflicts) {
    using computed_type = invalid_computed_type;
    ruvia::db_query query;
    const auto expression = query.binary(query.column("value"), ruvia::db_binary_operator::add, query.value(1));
    EXPECT_TRUE(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{.column_ = "missing", .expression_ = expression}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{"total", expression}, {"total", expression}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{"value", expression}, {"total", expression}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{.column_ = "total", .expression_ = {}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.defaults_ = {{.column_ = "total", .value_ = expression}},
            .generated_columns_ = {{.column_ = "total", .expression_ = expression}}});
    }));
}

RUVIA_DB_ENTITY(computed_keys_entity_type, "computed_keys",
    ruvia::db_column<"source", std::int64_t>,
    ruvia::db_column<"computed", std::int64_t,
        ruvia::db_column_options{.primary_key_ = true, .generated_type_ = db_generated_type::stored}>)

TEST(Ruvia, db_schema_mariadb_computed_columns_reject_table_primary_keys) {
    using entity_type = computed_keys_entity_type;
    ruvia::db_query query;
    auto expression = query.column("source");
    db_schema schema({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] {
        schema.create_table<entity_type>({.generated_columns_ = {{"computed", expression}}});
    }));
    for (const bool composite : {false, true}) {
        db_table_definition table_value{.name_ = "computed_keys", .columns_ = {{.name_ = "source", .type_ = {.data_type_ = db_data_type::big_int}}, {.name_ = "computed", .type_ = {.data_type_ = db_data_type::big_int}, .nullable_ = true, .generated_type_ = db_generated_type::stored, .as_expression_ = expression}}, .constraints_ = {{.name_ = "computed_key", .kind_ = ruvia::db_constraint_kind::primary_key, .columns_ = {"computed"}}}};
        if (composite) {
            table_value.constraints_[0].columns_.push_back("source");
        }
        EXPECT_TRUE(throws_on([&] { schema.create_table(table_value); }));
    }
}

TEST(Ruvia, db_schema_entity_relations_emit_owning_foreign_keys_and_one_to_one_unique) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<schema_owner_type>();
    const auto migrations = schema.compile("relations");
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("FOREIGN KEY (\"parent_id\",\"parent_tenant_id\") REFERENCES \"schema_targets\" (\"id\",\"tenant_id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("FOREIGN KEY (\"peer_id\") REFERENCES \"schema_targets\" (\"id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CONSTRAINT \"schema_owners_peer_key\" UNIQUE (\"peer_id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("children") == std::string_view::npos);
}

TEST(Ruvia, db_schema_many_to_many_relation_table_has_composite_primary_key_and_two_foreign_keys) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_relation_tables<schema_owner_type>();
    const auto migrations = schema.compile("relation_tables");
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("CREATE TABLE \"owner_labels\"") != std::string_view::npos);
    EXPECT_TRUE(sql.find("PRIMARY KEY (\"owner_id\",\"label_id\",\"label_tenant_id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("FOREIGN KEY (\"owner_id\") REFERENCES \"schema_owners\" (\"id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("FOREIGN KEY (\"label_id\",\"label_tenant_id\") REFERENCES \"schema_targets\" (\"id\",\"tenant_id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"label_id\" BIGINT NOT NULL") != std::string_view::npos);
}

TEST(Ruvia, db_schema_qualified_types_and_replace_view_use_valid_ddl_grammar) {
    db_schema schema({.driver_ = db_driver::postgresql});
    const std::array<std::string_view, 2> values{"on", "off"};
    schema.create_enum("app.state", values);
    schema.create_table({.name_ = "app.devices", .columns_ = {{.name_ = "state", .type_ = {.custom_name_ = "app.state"}}}});
    ruvia::db_query query;
    query.select(query.column("state")).from("app.devices");
    schema.create_view("app.device_state", query, true);
    const auto migrations = schema.compile("types");
    EXPECT_TRUE(migrations[0].sql().find("CREATE TYPE \"app\".\"state\" AS ENUM") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("\"state\" \"app\".\"state\" NOT NULL") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("CREATE OR REPLACE VIEW \"app\".\"device_state\" AS SELECT") != std::string_view::npos);
}

TEST(Ruvia, db_schema_procedure_builds_channel_guard_and_escapes_body_delimiters) {
    ruvia::db_query query;
    ruvia::db_procedure body;
    body.declare_row("channel", "link");
    ruvia::db_query channel;
    channel.from("link").where(channel.binary(channel.column("id"), ruvia::db_binary_operator::equal, channel.column("link_id", "new"))).lock({.mode_ = ruvia::db_row_lock::share});
    const std::array<std::string_view, 1> targets{"channel"};
    body.select_into(channel, targets, true);
    body.begin_if(query.binary(query.column("protocol", "channel"), ruvia::db_binary_operator::not_equal, query.column("protocol", "new")));
    body.raise_exception("channel mismatch $ruvia$ 'quoted'", "23514");
    body.end_if();
    body.assign("new.revision", query.binary(query.column("revision", "old"), ruvia::db_binary_operator::add, query.value(1)));
    body.return_value(query.column("new"));
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_trigger_function("app.bind_channel", body);
    schema.create_trigger({.name_ = "bind_channel", .table_ = "app.device", .function_ = "app.bind_channel", .events_ = {ruvia::db_trigger_event::insert, ruvia::db_trigger_event::update}});
    const auto migrations = schema.compile("guard");
    EXPECT_TRUE(migrations[0].sql().find("\"channel\" \"link\"%ROWTYPE") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("FOR SHARE INTO STRICT \"channel\"") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("AS $ruvia1$") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("DO $ruvia2$") == 0);
    EXPECT_TRUE(migrations[0].sql().find("BEFORE INSERT OR UPDATE ON \"app\".\"device\" FOR EACH ROW EXECUTE FUNCTION \"app\".\"bind_channel\"()") != std::string_view::npos);
}

TEST(Ruvia, db_schema_timescale_policies_are_typed_function_calls_and_table_options) {
    ruvia::db_query query;
    auto interval = query.cast(query.value("7 days"), db_data_type::interval);
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_hypertable({.table_ = "telemetry.events", .time_column_ = "ts", .chunk_interval_ = interval, .if_not_exists_ = true});
    schema.set_compression("telemetry.events", {.segment_by_ = {"device_id"}, .order_by_ = {{"ts", ruvia::db_order_direction::desc}}});
    schema.add_compression_policy("telemetry.events", interval, true);
    schema.add_retention_policy("telemetry.events", query.cast(query.value("90 days"), db_data_type::interval));
    const auto migrations = schema.compile("storage");
    EXPECT_TRUE(migrations[0].sql().find("\"create_hypertable\"(E'\"telemetry\".\"events\"', \"by_range\"(E'ts', CAST(E'7 days' AS INTERVAL))") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("\"timescaledb\".\"compress\" = true") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("\"compress_after\" => CAST(E'7 days' AS INTERVAL)") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("\"drop_after\" => CAST(E'90 days' AS INTERVAL)") != std::string_view::npos);
}

TEST(Ruvia, db_schema_single_perform_and_control_flow_have_complete_blocks) {
    ruvia::db_query query;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.perform(query.call("pg_notify", {query.value("channel"), query.value("body $ruvia$")}));
    const auto migrations = schema.compile("notify");
    EXPECT_TRUE(migrations[0].sql().find("DO $ruvia1$ BEGIN PERFORM") == 0);
    ruvia::db_procedure body;
    body.begin_if(query.value(true));
    EXPECT_TRUE(throws_on([&] { (void)body.body(); }));
    body.end_if();
    EXPECT_TRUE(throws_on([&] { body.otherwise(); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] { maria.validate_constraint("t", "fk"); }));
    ruvia::db_query removed;
    removed.delete_from("t").returning({removed.column("id")});
    EXPECT_TRUE(throws_on([&] { schema.create_view("v", removed); }));
    ruvia::db_query read;
    read.with("removed", removed).from("removed");
    EXPECT_TRUE(throws_on([&] { schema.create_view("v", read); }));
}

TEST(Ruvia, db_schema_ddl_operations_and_table_options_render_for_both_drivers) {
    ruvia::db_query pg_expressions;
    const auto pg_default = pg_expressions.value("pending");
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_schema("cs_pg", true);
    pg.create_table({.name_ = "cs_pg.records",
        .columns_ = {{.name_ = "id",
                         .type_ = {.data_type_ = db_data_type::big_int},
                         .identity_ = ruvia::db_identity::always},
            {.name_ = "name",
                .type_ = {.data_type_ = db_data_type::varchar, .length_ = 32},
                .nullable_ = true,
                .unique_ = true},
            {.name_ = "state", .type_ = {.data_type_ = db_data_type::text}, .default_value_ = pg_default}},
        .if_not_exists_ = true,
        .unlogged_ = true});
    pg.rename_table("cs_pg.records", "records_renamed");
    pg.add_column("cs_pg.records_renamed",
        {.name_ = "created_at", .type_ = {.data_type_ = db_data_type::timestamp_tz}, .nullable_ = true}, true);
    pg.rename_column("cs_pg.records_renamed", "created_at", "created_on");
    pg.alter_column_type("cs_pg.records_renamed", "state", {.data_type_ = db_data_type::varchar, .length_ = 64},
        pg_expressions.column("state"));
    pg.set_column_nullable("cs_pg.records_renamed", "state", true);
    pg.set_column_nullable("cs_pg.records_renamed", "state", false);
    pg.set_column_default("cs_pg.records_renamed", "state", pg_expressions.value("ready"));
    pg.drop_column_default("cs_pg.records_renamed", "state");
    pg.drop_column("cs_pg.records_renamed", "created_on", ruvia::db_drop_behavior::cascade, true);
    pg.drop_table("cs_pg.records_renamed", ruvia::db_drop_behavior::cascade, true);
    pg.drop_schema("cs_pg", ruvia::db_drop_behavior::cascade, true);
    const auto pg_migrations = pg.compile("cs_pg_ops");
    EXPECT_EQ(pg_migrations.size(), std::size_t{1});
    const auto pg_sql = pg_migrations[0].sql();
    EXPECT_TRUE(pg_sql.find("CREATE SCHEMA IF NOT EXISTS \"cs_pg\"") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("CREATE UNLOGGED TABLE IF NOT EXISTS \"cs_pg\".\"records\"") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("GENERATED ALWAYS AS IDENTITY") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("VARCHAR(32) UNIQUE") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("ALTER COLUMN \"state\" TYPE VARCHAR(64) USING \"state\"") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("DROP COLUMN IF EXISTS \"created_on\" CASCADE") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("DROP SCHEMA IF EXISTS \"cs_pg\" CASCADE") != std::string_view::npos);

    ruvia::db_query maria_expressions;
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_schema("cs_maria", true);
    maria.create_table({.name_ = "cs_maria.records",
        .columns_ = {{.name_ = "id",
                         .type_ = {.data_type_ = db_data_type::big_int},
                         .identity_ = ruvia::db_identity::by_default},
            {.name_ = "state",
                .type_ = {.data_type_ = db_data_type::varchar, .length_ = 32},
                .nullable_ = true,
                .default_value_ = maria_expressions.value("ready")}},
        .if_not_exists_ = true});
    maria.add_column("cs_maria.records", {.name_ = "note", .type_ = {.data_type_ = db_data_type::text}, .nullable_ = true}, true);
    maria.rename_column("cs_maria.records", "note", "memo");
    maria.set_column_default("cs_maria.records", "memo", maria_expressions.value("n/a"));
    maria.drop_column_default("cs_maria.records", "memo");
    EXPECT_TRUE(throws_on([&] { maria.drop_column("cs_maria.records", "memo", ruvia::db_drop_behavior::cascade, true); }));
    maria.drop_column("cs_maria.records", "memo", ruvia::db_drop_behavior::restrict, true);
    maria.add_constraint("cs_maria.records", {.name_ = "records_state_unique", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"state"}});
    EXPECT_TRUE(throws_on([&] { maria.drop_constraint("cs_maria.records", "records_state_unique", ruvia::db_drop_behavior::cascade, true); }));
    maria.drop_constraint("cs_maria.records", "records_state_unique", ruvia::db_drop_behavior::restrict, true);
    maria.rename_table("cs_maria.records", "records_renamed");
    EXPECT_TRUE(throws_on([&] { maria.drop_table("cs_maria.records_renamed", ruvia::db_drop_behavior::cascade, true); }));
    maria.drop_table("cs_maria.records_renamed", ruvia::db_drop_behavior::restrict, true);
    EXPECT_TRUE(throws_on([&] { maria.drop_schema("cs_maria", ruvia::db_drop_behavior::cascade, true); }));
    maria.drop_schema("cs_maria", ruvia::db_drop_behavior::restrict, true);
    const auto maria_migrations = maria.compile("cs_maria_ops");
    EXPECT_EQ(maria_migrations.size(), std::size_t{12});
    EXPECT_EQ(maria_migrations[0].id(), "cs_maria_ops_1");
    EXPECT_TRUE(maria_migrations[1].sql().find("CREATE TABLE IF NOT EXISTS `cs_maria`.`records`") != std::string_view::npos);
    EXPECT_TRUE(maria_migrations[1].sql().find("AUTO_INCREMENT") != std::string_view::npos);
    EXPECT_TRUE(maria_migrations[2].sql().find("ADD COLUMN IF NOT EXISTS `note` TEXT") != std::string_view::npos);
    EXPECT_TRUE(maria_migrations[9].sql().find("ALTER TABLE `cs_maria`.`records` RENAME TO `cs_maria`.`records_renamed`") != std::string_view::npos);
    EXPECT_TRUE(maria_migrations[10].sql().find("DROP TABLE IF EXISTS `cs_maria`.`records_renamed`") != std::string_view::npos);
    for (const auto& migration : maria_migrations) {
        EXPECT_TRUE(migration.sql().find("CASCADE") == std::string_view::npos);
    }
    EXPECT_TRUE(throws_on([&] {
        maria.alter_column_type("cs_maria.records", "state", {.data_type_ = db_data_type::text});
    }));
    EXPECT_TRUE(throws_on([&] { maria.set_column_nullable("cs_maria.records", "state", true); }));
}

TEST(Ruvia, db_schema_constraints_cover_actions_deferred_not_valid_and_rejections) {
    ruvia::db_query expressions;
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.add_constraint("cs_child",
        {.name_ = "child_fk",
            .kind_ = ruvia::db_constraint_kind::foreign_key,
            .columns_ = {"parent_id"},
            .referenced_table_ = "cs_parent",
            .referenced_columns_ = {"id"},
            .on_delete_ = ruvia::db_referential_action::set_null,
            .on_update_ = ruvia::db_referential_action::set_default,
            .deferrable_ = true,
            .initially_deferred_ = true,
            .not_valid_ = true});
    pg.add_constraint("cs_child",
        {.name_ = "child_unique", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"parent_id"}, .deferrable_ = true});
    pg.add_constraint("cs_child",
        {.name_ = "child_check",
            .kind_ = ruvia::db_constraint_kind::check,
            .not_valid_ = true,
            .check_ = expressions.binary(expressions.column("amount"), ruvia::db_binary_operator::greater_equal,
                expressions.value(0))});
    pg.validate_constraint("cs_child", "child_fk");
    const auto migrations = pg.compile("cs_constraints");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("ON DELETE SET NULL ON UPDATE SET DEFAULT DEFERRABLE INITIALLY DEFERRED NOT VALID") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CONSTRAINT \"child_unique\" UNIQUE (\"parent_id\") DEFERRABLE") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CONSTRAINT \"child_check\" CHECK ((\"amount\" >= 0)) NOT VALID") != std::string_view::npos);
    EXPECT_TRUE(sql.find("VALIDATE CONSTRAINT \"child_fk\"") != std::string_view::npos);

    db_table_definition invalid_table{.name_ = "cs_invalid",
        .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::big_int}}},
        .constraints_ = {{.name_ = "bad", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"id"}, .not_valid_ = true}}};
    EXPECT_TRUE(throws_on([&] { pg.create_table(invalid_table); }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"id"}, .initially_deferred_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::check, .deferrable_ = true, .check_ = expressions.value(true)});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"id"}, .not_valid_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::foreign_key, .columns_ = {"id"}, .referenced_table_ = "cs_parent", .referenced_columns_ = {}});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::check});
    }));

    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] {
        maria.add_constraint("cs_child", {.name_ = "fk", .kind_ = ruvia::db_constraint_kind::foreign_key, .columns_ = {"id"}, .referenced_table_ = "cs_parent", .referenced_columns_ = {"id"}, .deferrable_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        maria.add_constraint("cs_child", {.name_ = "check", .kind_ = ruvia::db_constraint_kind::check, .not_valid_ = true, .check_ = expressions.value(true)});
    }));
}

TEST(Ruvia, db_schema_constructor_table_flags_and_compile_boundaries_are_checked) {
    EXPECT_TRUE(throws_on([&] { db_schema invalid({}); }));
    db_schema empty({.driver_ = db_driver::postgresql});
    EXPECT_TRUE(empty.compile("cs_empty").empty());
    EXPECT_TRUE(throws_on([&] { (void)empty.compile(""); }));

    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_table({.name_ = "cs_temp_pg", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .temporary_ = true});
    EXPECT_TRUE(pg.compile("cs_temp_pg")[0].sql().find("CREATE TEMPORARY TABLE") != std::string_view::npos);
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_table({.name_ = "cs_temp_maria", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .temporary_ = true});
    EXPECT_TRUE(maria.compile("cs_temp_maria")[0].sql().find("CREATE TEMPORARY TABLE") != std::string_view::npos);
    EXPECT_TRUE(throws_on([&] {
        maria.create_table({.name_ = "cs_invalid_flags", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .temporary_ = true, .unlogged_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        maria.create_table({.name_ = "cs_unlogged", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .unlogged_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        maria.create_table({.name_ = "cs_always", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}, .identity_ = ruvia::db_identity::always}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.create_table({.name_ = "cs_duplicate", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}, {.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}});
    }));
}

TEST(Ruvia, db_schema_index_variants_and_dialect_rejections) {
    ruvia::db_query expressions;
    const auto predicate = expressions.binary(expressions.column("active"), ruvia::db_binary_operator::equal, expressions.value(true));
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_index({.name_ = "cs_idx", .table_ = "cs_records", .keys_ = {{.column_ = "name", .order_ = ruvia::db_order_direction::desc, .nulls_ = ruvia::db_nulls_order::last, .operator_class_ = "text_ops"}, {.expression_ = expressions.column("lower_name")}}, .unique_ = true, .if_not_exists_ = true, .method_ = ruvia::db_index_method::btree, .include_ = {"id"}, .where_ = predicate});
    const auto pg_migrations = pg.compile("cs_index");
    EXPECT_EQ(pg_migrations.size(), std::size_t{1});
    EXPECT_TRUE(pg_migrations[0].sql().find("CREATE UNIQUE INDEX IF NOT EXISTS \"cs_idx\" ON \"cs_records\" USING btree") != std::string_view::npos);
    EXPECT_TRUE(pg_migrations[0].sql().find("\"name\" \"text_ops\" DESC NULLS LAST, (\"lower_name\")") != std::string_view::npos);
    EXPECT_TRUE(pg_migrations[0].sql().find("INCLUDE (\"id\") WHERE (\"active\" = TRUE)") != std::string_view::npos);

    for (const auto method : {ruvia::db_index_method::hash, ruvia::db_index_method::gin, ruvia::db_index_method::gist,
             ruvia::db_index_method::sp_gist, ruvia::db_index_method::brin}) {
        db_schema method_schema({.driver_ = db_driver::postgresql});
        method_schema.create_index({.name_ = "cs_method", .table_ = "cs_records", .keys_ = {{.column_ = "name"}}, .method_ = method});
        EXPECT_TRUE(method_schema.compile("cs_method")[0].sql().find("USING ") != std::string_view::npos);
    }
    db_schema concurrent({.driver_ = db_driver::postgresql});
    concurrent.create_index({.name_ = "cs_concurrent", .table_ = "cs_records", .keys_ = {{.column_ = "id"}}, .concurrently_ = true});
    const auto concurrent_migrations = concurrent.compile("cs_concurrent");
    EXPECT_EQ(concurrent_migrations[0].atomicity(), ruvia::db_migration_atomicity::unwrapped);
    db_schema drop({.driver_ = db_driver::postgresql});
    drop.drop_index("cs_records.cs_idx", true, true);
    EXPECT_TRUE(drop.compile("cs_drop_index")[0].sql().find("DROP INDEX CONCURRENTLY IF EXISTS \"cs_records\".\"cs_idx\"") != std::string_view::npos);

    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id", .expression_ = expressions.column("id")}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}, {.column_ = ""}}});
    }));
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_index({.name_ = "cs_maria_idx", .table_ = "cs_records", .keys_ = {{.column_ = "name", .order_ = ruvia::db_order_direction::desc}}, .unique_ = true, .if_not_exists_ = true});
    EXPECT_TRUE(maria.compile("cs_maria_idx")[0].sql().find("CREATE UNIQUE INDEX IF NOT EXISTS `cs_maria_idx` USING btree ON `cs_records`") != std::string_view::npos);
    EXPECT_TRUE(throws_on([&] {
        maria.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}}, .concurrently_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        maria.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}}, .method_ = ruvia::db_index_method::hash});
    }));
    EXPECT_TRUE(throws_on([&] {
        maria.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}}, .include_ = {"extra"}});
    }));
    EXPECT_TRUE(throws_on([&] { maria.drop_index("idx"); }));
}

TEST(Ruvia, db_schema_views_enums_extensions_and_commands_cover_dialect_boundaries) {
    ruvia::db_query query;
    query.select(query.column("id")).from("cs_records");
    db_schema pg({.driver_ = db_driver::postgresql});
    const std::array<std::string_view, 2> values{"new", "done"};
    pg.create_enum("cs_status", values);
    pg.drop_enum("cs_status", ruvia::db_drop_behavior::cascade, true);
    pg.create_extension("hstore", true);
    pg.create_view("cs_view", query, true);
    pg.drop_view("cs_view", false, ruvia::db_drop_behavior::cascade, true);
    pg.create_view("cs_mat", query, false, true);
    pg.drop_view("cs_mat", true, ruvia::db_drop_behavior::restrict, true);
    ruvia::db_query command;
    command.update("cs_records").set("state", command.value("done"));
    pg.execute(command);
    const auto migrations = pg.compile("cs_objects");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("CREATE TYPE \"cs_status\" AS ENUM (E'new',E'done')") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DROP TYPE IF EXISTS \"cs_status\" CASCADE") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CREATE EXTENSION IF NOT EXISTS \"hstore\"") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CREATE OR REPLACE VIEW \"cs_view\" AS SELECT") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DROP MATERIALIZED VIEW IF EXISTS \"cs_mat\"") != std::string_view::npos);
    EXPECT_TRUE(sql.find("UPDATE \"cs_records\" SET \"state\" = E'done'") != std::string_view::npos);
    db_schema enum_addition({.driver_ = db_driver::postgresql});
    enum_addition.add_enum_value("cs_status", "archived", true);
    const auto enum_migrations = enum_addition.compile("cs_enum_addition");
    EXPECT_EQ(enum_migrations.size(), std::size_t{1});
    EXPECT_TRUE(enum_migrations[0].sql().find("ALTER TYPE \"cs_status\" ADD VALUE IF NOT EXISTS E'archived'") != std::string_view::npos);

    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_view("cs_view", query);
    EXPECT_TRUE(throws_on([&] { maria.drop_view("cs_view", false, ruvia::db_drop_behavior::cascade, true); }));
    maria.drop_view("cs_view", false, ruvia::db_drop_behavior::restrict, true);
    maria.execute(command);
    EXPECT_TRUE(maria.compile("cs_objects").size() == std::size_t{3});
    EXPECT_TRUE(throws_on([&] { maria.create_enum("status", values); }));
    EXPECT_TRUE(throws_on([&] { maria.add_enum_value("status", "x"); }));
    EXPECT_TRUE(throws_on([&] { maria.drop_enum("status"); }));
    EXPECT_TRUE(throws_on([&] { maria.create_extension("hstore"); }));
    EXPECT_TRUE(throws_on([&] { maria.create_view("mat", query, false, true); }));
    EXPECT_TRUE(throws_on([&] { maria.drop_view("mat", true); }));
    EXPECT_TRUE(throws_on([&] { pg.create_view("bad", command); }));
    EXPECT_TRUE(throws_on([&] { pg.create_view("bad", query, true, true); }));
    EXPECT_TRUE(throws_on([&] { pg.create_enum("empty", {}); }));
    ruvia::db_query returned;
    returned.delete_from("cs_records").returning({returned.column("id")});
    EXPECT_TRUE(throws_on([&] { pg.execute(returned); }));
}

TEST(Ruvia, db_schema_trigger_variants_cover_events_timing_when_arguments_and_rejections) {
    ruvia::db_query expressions;
    const auto when = expressions.binary(expressions.column("enabled"), ruvia::db_binary_operator::equal, expressions.value(true));
    db_procedure procedure;
    procedure.return_value();
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_trigger_function("cs_trigger_fn", procedure, true);
    pg.create_trigger({.name_ = "cs_before_insert", .table_ = "cs_records", .function_ = "cs_trigger_fn", .events_ = {ruvia::db_trigger_event::insert}});
    pg.create_trigger({.name_ = "cs_after_update", .table_ = "cs_records", .function_ = "cs_trigger_fn", .timing_ = ruvia::db_trigger_timing::after, .events_ = {ruvia::db_trigger_event::update}, .update_columns_ = {"state", "enabled"}, .when_ = when, .arguments_ = {"arg 'one'", "two"}});
    pg.create_trigger({.name_ = "cs_statement", .table_ = "cs_records", .function_ = "cs_trigger_fn", .events_ = {ruvia::db_trigger_event::delete_value, ruvia::db_trigger_event::truncate}, .for_each_row_ = false});
    pg.create_trigger({.name_ = "cs_instead", .table_ = "cs_records_view", .function_ = "cs_trigger_fn", .timing_ = ruvia::db_trigger_timing::instead_of, .events_ = {ruvia::db_trigger_event::insert}});
    pg.drop_trigger("cs_records", "cs_after_update", true);
    pg.drop_trigger_function("cs_trigger_fn", ruvia::db_drop_behavior::cascade, true);
    const auto migrations = pg.compile("cs_triggers");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("CREATE OR REPLACE FUNCTION \"cs_trigger_fn\"() RETURNS trigger LANGUAGE plpgsql") != std::string_view::npos);
    EXPECT_TRUE(sql.find("AFTER UPDATE OF \"state\", \"enabled\" ON \"cs_records\" FOR EACH ROW WHEN ((\"enabled\" = TRUE)) EXECUTE FUNCTION \"cs_trigger_fn\"(E'arg ''one''', E'two')") != std::string_view::npos);
    EXPECT_TRUE(sql.find("AFTER UPDATE OF") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DELETE OR TRUNCATE ON \"cs_records\" FOR EACH STATEMENT") != std::string_view::npos);
    EXPECT_TRUE(sql.find("INSTEAD OF INSERT ON \"cs_records_view\" FOR EACH ROW") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DROP TRIGGER IF EXISTS \"cs_after_update\" ON \"cs_records\"") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DROP FUNCTION IF EXISTS \"cs_trigger_fn\"() CASCADE") != std::string_view::npos);

    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f"}); }));
    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::insert, ruvia::db_trigger_event::insert}}); }));
    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::insert}, .update_columns_ = {"x"}}); }));
    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::truncate}}); }));
    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .timing_ = ruvia::db_trigger_timing::instead_of, .events_ = {ruvia::db_trigger_event::insert}, .for_each_row_ = false}); }));
    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .timing_ = ruvia::db_trigger_timing::instead_of, .events_ = {ruvia::db_trigger_event::insert}, .when_ = when}); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] { maria.create_trigger_function("f", procedure); }));
    EXPECT_TRUE(throws_on([&] { maria.create_trigger({.name_ = "f", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::insert}}); }));
    EXPECT_TRUE(throws_on([&] { maria.drop_trigger("t", "f"); }));
    EXPECT_TRUE(throws_on([&] { maria.perform(expressions.value(true)); }));
    EXPECT_TRUE(throws_on([&] { maria.run(procedure); }));
}

TEST(Ruvia, db_schema_timescale_options_cover_direct_settings_and_rejections) {
    ruvia::db_query expressions;
    const auto interval = expressions.cast(expressions.value("1 day"), db_data_type::interval);
    db_schema pg({.driver_ = db_driver::postgresql});
    const std::array table_options{db_table_option{"autovacuum_enabled", true}, db_table_option{"fillfactor", std::int64_t{80}}, db_table_option{"toast_tuple_target", "128"}};
    pg.set_table_options("cs_events", table_options);
    pg.set_database_option("cs_db", "timescaledb.max_background_workers", "8");
    pg.set_chunk_time_interval("cs_events", interval);
    pg.set_compression("cs_events", {.enabled_ = false});
    pg.remove_compression_policy("cs_events", true);
    pg.remove_retention_policy("cs_events", true);
    const auto migrations = pg.compile("cs_timescale_options");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("ALTER TABLE \"cs_events\" SET (\"autovacuum_enabled\" = true, \"fillfactor\" = 80, \"toast_tuple_target\" = E'128')") != std::string_view::npos);
    EXPECT_TRUE(sql.find("ALTER DATABASE \"cs_db\" SET \"timescaledb\".\"max_background_workers\" TO E'8'") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"set_chunk_time_interval\"(E'\"cs_events\"', CAST(E'1 day' AS INTERVAL))") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"timescaledb\".\"compress\" = false") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"remove_compression_policy\"(E'\"cs_events\"', \"if_exists\" => TRUE)") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"remove_retention_policy\"(E'\"cs_events\"', \"if_exists\" => TRUE)") != std::string_view::npos);
    EXPECT_TRUE(throws_on([&] { pg.set_table_options("cs_events", std::span<const db_table_option>()); }));
    const std::array duplicate_options{db_table_option{"fillfactor", std::int64_t{80}}, db_table_option{"fillfactor", std::int64_t{90}}};
    EXPECT_TRUE(throws_on([&] {
        pg.set_table_options("cs_events", duplicate_options);
    }));
    EXPECT_TRUE(throws_on([&] { pg.set_compression("cs_events", {.enabled_ = false, .segment_by_ = {"device_id"}}); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    const std::array maria_options{db_table_option{"x", true}};
    EXPECT_TRUE(throws_on([&] { maria.set_table_options("t", maria_options); }));
    EXPECT_TRUE(throws_on([&] { maria.set_database_option("d", "x", "y"); }));
    EXPECT_TRUE(throws_on([&] { maria.create_hypertable({.table_ = "t", .time_column_ = "ts"}); }));
    EXPECT_TRUE(throws_on([&] { maria.set_chunk_time_interval("t", interval); }));
    EXPECT_TRUE(throws_on([&] { maria.set_compression("t", {}); }));
    EXPECT_TRUE(throws_on([&] { maria.add_compression_policy("t", interval); }));
    EXPECT_TRUE(throws_on([&] { maria.remove_compression_policy("t"); }));
    EXPECT_TRUE(throws_on([&] { maria.add_retention_policy("t", interval); }));
    EXPECT_TRUE(throws_on([&] { maria.remove_retention_policy("t"); }));
}

TEST(Ruvia, db_procedure_public_helpers_render_and_validate) {
    ruvia::db_query expressions;
    ruvia::db_procedure procedure;
    procedure.declare_variable({.name_ = "counter", .type_ = {.data_type_ = db_data_type::integer}, .default_value_ = expressions.value(0)});
    procedure.declare_row("row_value", "cs_records");
    procedure.declare_record("record_value");
    procedure.assign("counter", expressions.binary(expressions.column("counter"), ruvia::db_binary_operator::add, expressions.value(1)));
    ruvia::db_query select;
    select.select(select.column("id")).from("cs_records");
    const std::array<std::string_view, 1> targets{"row_value"};
    procedure.select_into(select, targets, false);
    procedure.select_into(select, targets, true);
    ruvia::db_query command;
    command.update("cs_records").set("state", command.value("done"));
    procedure.execute(command);
    procedure.perform(expressions.call("pg_notify", {expressions.value("cs"), expressions.value("body")}));
    procedure.begin_if(expressions.column("enabled"));
    procedure.else_if(expressions.column("fallback"));
    procedure.otherwise();
    procedure.end_if();
    procedure.begin_block();
    procedure.catch_sql_state("23505");
    procedure.end_block();
    procedure.return_value();
    procedure.raise_exception("failed", "P0001");
    const auto body = procedure.body();
    EXPECT_TRUE(body.find("DECLARE\n") == 0);
    EXPECT_TRUE(body.find("\"counter\" INTEGER := 0;") != std::string_view::npos);
    EXPECT_TRUE(body.find("\"row_value\" \"cs_records\"%ROWTYPE;") != std::string_view::npos);
    EXPECT_TRUE(body.find("SELECT \"id\" FROM \"cs_records\" INTO \"row_value\";") != std::string_view::npos);
    EXPECT_TRUE(body.find("INTO STRICT \"row_value\";") != std::string_view::npos);
    EXPECT_TRUE(body.find("ELSIF \"fallback\" THEN") != std::string_view::npos);
    EXPECT_TRUE(body.find("EXCEPTION\nWHEN SQLSTATE E'23505'") != std::string_view::npos);
    EXPECT_TRUE(body.find("RETURN;\nRAISE EXCEPTION USING MESSAGE = E'failed'") != std::string_view::npos);

    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_schema("cs_nested");
    ruvia::db_procedure applied;
    applied.apply(pg);
    EXPECT_TRUE(applied.body().find("CREATE SCHEMA \"cs_nested\";") != std::string_view::npos);
    db_schema non_transactional({.driver_ = db_driver::postgresql});
    non_transactional.create_index({.name_ = "cs_idx", .table_ = "cs_records", .keys_ = {{.column_ = "id"}}, .concurrently_ = true});
    EXPECT_TRUE(throws_on([&] { applied.apply(non_transactional); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] { applied.apply(maria); }));

    EXPECT_TRUE(throws_on([&] { procedure.declare_record("counter"); }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.execute(select);
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.select_into(command, targets);
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.select_into(select, {});
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.else_if(expressions.value(true));
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.otherwise();
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.end_if();
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.catch_sql_state("23505");
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.begin_block();
        invalid.catch_sql_state("bad");
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.end_block();
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.raise_exception("bad", "00000");
    }));
}

RUVIA_DB_ENTITY(devices_read_only_entity, "devices",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"protocol_address", std::string, ruvia::db_column_options{.read_only_ = true}>)

TEST(Ruvia, db_schema_trigger_transition_tables_and_read_only_columns_render_and_validate) {
    using read_only_entity = devices_read_only_entity;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<read_only_entity>();
    schema.create_trigger({
        .name_ = "devices_live_update",
        .table_ = "devices",
        .function_ = "publish_query_change",
        .timing_ = ruvia::db_trigger_timing::after,
        .events_ = {ruvia::db_trigger_event::update},
        .for_each_row_ = false,
        .old_table_ = "old_rows",
        .new_table_ = "new_rows",
        .arguments_ = {"device"},
    });
    const auto migrations = schema.compile("transition_trigger");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    EXPECT_TRUE(migrations[0].sql().find("\"protocol_address\" TEXT NOT NULL") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find(
                    "CREATE TRIGGER \"devices_live_update\" AFTER UPDATE ON \"devices\" "
                    "REFERENCING OLD TABLE AS \"old_rows\" NEW TABLE AS \"new_rows\" "
                    "FOR EACH STATEMENT EXECUTE FUNCTION \"publish_query_change\"(E'device')") != std::string_view::npos);

    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_trigger({
            .name_ = "bad_before",
            .table_ = "devices",
            .function_ = "fn",
            .timing_ = ruvia::db_trigger_timing::before,
            .events_ = {ruvia::db_trigger_event::insert},
            .for_each_row_ = false,
            .new_table_ = "new_rows",
        });
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_trigger({
            .name_ = "bad_multi_event",
            .table_ = "devices",
            .function_ = "fn",
            .timing_ = ruvia::db_trigger_timing::after,
            .events_ = {ruvia::db_trigger_event::insert, ruvia::db_trigger_event::update},
            .for_each_row_ = false,
            .new_table_ = "new_rows",
        });
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_trigger({
            .name_ = "bad_old_insert",
            .table_ = "devices",
            .function_ = "fn",
            .timing_ = ruvia::db_trigger_timing::after,
            .events_ = {ruvia::db_trigger_event::insert},
            .for_each_row_ = false,
            .old_table_ = "old_rows",
        });
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_trigger({
            .name_ = "bad_same_name",
            .table_ = "devices",
            .function_ = "fn",
            .timing_ = ruvia::db_trigger_timing::after,
            .events_ = {ruvia::db_trigger_event::update},
            .for_each_row_ = false,
            .old_table_ = "rows",
            .new_table_ = "rows",
        });
    }));
}

// Enum values outside the declared enumerators (reachable only through a cast)
// are rejected instead of rendering a default clause.
TEST(Ruvia, db_schema_rejects_unknown_enum_values_instead_of_defaulting) {
    db_schema pg({.driver_ = db_driver::postgresql});
    EXPECT_TRUE(throws_on([&] { pg.drop_table("cs_records", static_cast<ruvia::db_drop_behavior>(7)); }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "child_fk", .kind_ = ruvia::db_constraint_kind::foreign_key, .columns_ = {"parent_id"}, .referenced_table_ = "cs_parent", .referenced_columns_ = {"id"}, .on_delete_ = static_cast<ruvia::db_referential_action>(9)});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.create_trigger({.name_ = "cs_trigger", .table_ = "cs_records", .function_ = "fn", .timing_ = static_cast<ruvia::db_trigger_timing>(9), .events_ = {ruvia::db_trigger_event::insert}});
    }));
}

RUVIA_DB_ENTITY(versioned_articles_entity, "versioned_articles",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"revision", std::int64_t, ruvia::db_column_options{.version_ = true}>,
    ruvia::db_column<"created_at", std::string, ruvia::db_column_options{.data_type_ = db_data_type::timestamp_tz, .create_date_ = true}>,
    ruvia::db_column<"updated_at", std::string, ruvia::db_column_options{.data_type_ = db_data_type::timestamp_tz, .update_date_ = true}>,
    ruvia::db_column<"deleted_at", std::string, ruvia::db_column_options{.data_type_ = db_data_type::timestamp_tz, .delete_date_ = true}>)

TEST(Ruvia, db_schema_and_procedure_functions_loops_dynamic_sql_and_table_locks) {
    using versioned_entity = versioned_articles_entity;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<versioned_entity>();
    schema.lock_table("versioned_articles", ruvia::db_table_lock_mode::access_exclusive, true);

    ruvia::db_query sql_fn_body;
    sql_fn_body.select(sql_fn_body.value(true));
    schema.create_function(
        {
            .name_ = "is_article_ready",
            .parameters_ = {{.name_ = "article_id", .type_ = {.data_type_ = db_data_type::big_int}}},
            .return_type_ = {.data_type_ = db_data_type::boolean},
            .volatility_ = ruvia::db_function_volatility::stable,
            .strict_ = true,
            .replace_ = true,
        },
        sql_fn_body);

    ruvia::db_query loop_query;
    loop_query.select(loop_query.column("id")).from("versioned_articles");
    ruvia::db_query expressions;
    db_procedure proc;
    proc.declare_record("rec");
    proc.declare_variable({.name_ = "op", .type_ = {.data_type_ = db_data_type::text}});
    proc.lock_table("versioned_articles", ruvia::db_table_lock_mode::share);
    proc.begin_for_query("rec", loop_query);
    proc.begin_foreach("op", expressions.array({expressions.value("INSERT"), expressions.value("UPDATE")}));
    proc.execute_dynamic(
        expressions.call("format", {expressions.value("SELECT %L, %L"), expressions.column("id", "rec"), expressions.column("op")}));
    proc.end_loop();
    proc.end_loop();
    schema.run(proc);
    const std::array<ruvia::db_type_definition, 1> drop_params{{{.data_type_ = db_data_type::big_int}}};
    schema.drop_function("is_article_ready", drop_params, ruvia::db_drop_behavior::cascade, true);

    const auto migrations = schema.compile("functions_and_loops");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("\"revision\" BIGINT NOT NULL DEFAULT 1") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"created_at\" TIMESTAMPTZ NOT NULL DEFAULT \"now\"()") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"deleted_at\" TIMESTAMPTZ") != std::string_view::npos);
    EXPECT_TRUE(sql.find("LOCK TABLE \"versioned_articles\" IN ACCESS EXCLUSIVE MODE NOWAIT") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CREATE OR REPLACE FUNCTION \"is_article_ready\"(\"article_id\" BIGINT) RETURNS BOOLEAN LANGUAGE sql STABLE STRICT AS") != std::string_view::npos);
    EXPECT_TRUE(sql.find("FOR \"rec\" IN SELECT \"id\" FROM \"versioned_articles\" LOOP") != std::string_view::npos);
    EXPECT_TRUE(sql.find("FOREACH \"op\" IN ARRAY ARRAY[E'INSERT', E'UPDATE'] LOOP") != std::string_view::npos);
    EXPECT_TRUE(sql.find("EXECUTE \"format\"(E'SELECT %L, %L', \"rec\".\"id\", \"op\");") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DROP FUNCTION IF EXISTS \"is_article_ready\"(BIGINT) CASCADE") != std::string_view::npos);
}
