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
#include <memory_resource>
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
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_repository.h"
#include "ruvia/web/redis/redis_handle.h"
#include "ruvia/web/redis/redis_types.h"

#include "backend_client_fixture.h"
#include "test_io_context.h"

// Removing a query-result cache id must drop every result a repository builder
// cached under that id, so the next get_one/get_exists/get_count reads the
// database again.

namespace {

RUVIA_DB_ENTITY(record, "records", ruvia::db_column<"v", std::pmr::string>)

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
        asio::post(io_, [this, &poisoned, value = std::pmr::string(value, ruvia::process_resource())]() mutable {
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

    static asio::awaitable<std::pmr::string> read_line(socket_type& socket, std::pmr::string& buffer) {
        const auto size = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffer), "\r\n", asio::use_awaitable);
        std::pmr::string line(std::string_view(buffer).substr(0, size - 2), ruvia::process_resource());
        buffer.erase(0, size);
        co_return line;
    }

    static asio::awaitable<std::pmr::vector<std::pmr::string>> read_command(socket_type& socket, std::pmr::string& buffer) {
        const auto header = co_await read_line(socket, buffer);
        if (header.size() < 2 || header.front() != '*') {
            throw std::runtime_error("invalid RESP command array");
        }
        const auto count = std::stoull(std::string(std::string_view(header).substr(1)));
        std::pmr::vector<std::pmr::string> arguments{ruvia::process_resource()};
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
    [[nodiscard]] std::pmr::string execute(std::pmr::vector<std::pmr::string>& arguments) {
        if (arguments.empty()) {
            return std::pmr::string("-ERR empty command\r\n", ruvia::process_resource());
        }
        auto& name = arguments.front();
        std::ranges::transform(name, name.begin(), [](unsigned char value) {
            return static_cast<char>(std::toupper(value));
        });
        if (name == "GET" && arguments.size() == 2) {
            if (poison_) {
                return "$" + std::pmr::string(std::to_string(poison_->size()), ruvia::process_resource()) + "\r\n" + *poison_ + "\r\n";
            }
            const auto found = store_.find(arguments[1]);
            if (found == store_.end()) {
                return std::pmr::string("$-1\r\n", ruvia::process_resource());
            }
            return "$" + std::pmr::string(std::to_string(found->second.size()), ruvia::process_resource()) + std::pmr::string("\r\n", ruvia::process_resource()) + found->second + "\r\n";
        }
        if (name == "SET" && arguments.size() >= 3) {
            store_.insert_or_assign(std::move(arguments[1]), std::move(arguments[2]));
            return std::pmr::string("+OK\r\n", ruvia::process_resource());
        }
        if (name == "DEL" && arguments.size() >= 2) {
            std::size_t removed = 0;
            for (std::size_t index = 1; index != arguments.size(); ++index) {
                removed += store_.erase(arguments[index]);
            }
            return ":" + std::pmr::string(std::to_string(removed), ruvia::process_resource()) + "\r\n";
        }
        if (name == "SCAN" && arguments.size() == 6 && arguments[1] == "0" &&
            arguments[2] == "MATCH" && arguments[3].ends_with('*') && arguments[4] == "COUNT") {
            const auto prefix = std::string_view(arguments[3]).substr(0, arguments[3].size() - 1);
            std::pmr::string values{ruvia::process_resource()};
            std::size_t count = 0;
            for (const auto& [key, value] : store_) {
                (void)value;
                if (key.starts_with(prefix)) {
                    values.append("$").append(std::pmr::string(std::to_string(key.size()), ruvia::process_resource())).append("\r\n").append(key).append("\r\n");
                    ++count;
                }
            }
            return "*2\r\n$1\r\n0\r\n*" + std::pmr::string(std::to_string(count), ruvia::process_resource()) + std::pmr::string("\r\n", ruvia::process_resource()) + values;
        }
        return std::pmr::string("-ERR unsupported command\r\n", ruvia::process_resource());
    }

    asio::awaitable<void> serve(std::shared_ptr<socket_type> socket) {
        std::pmr::string buffer{ruvia::process_resource()};
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
    std::pmr::vector<std::shared_ptr<socket_type>> sockets_{ruvia::process_resource()};
    std::pmr::map<std::pmr::string, std::pmr::string, std::less<>> store_{ruvia::process_resource()};
    std::optional<std::pmr::string> poison_;
    std::function<void()> on_read_;
    std::exception_ptr failure_;
    bool closing_{false};
    std::thread thread_;
};

// Every read names the same cache id; each result shape stores its own entry.
ruvia::task<std::pmr::string> read_one(const ruvia::db_handle& handle) {
    const auto row =
        co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_one();
    co_return row ? std::pmr::string(std::string_view(row->get<"v">()), ruvia::process_resource()) : std::pmr::string("missing", ruvia::process_resource());
}

ruvia::task<bool> read_exists(const ruvia::db_handle& handle) {
    co_return co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_exists();
}

ruvia::task<std::uint64_t> read_count(const ruvia::db_handle& handle) {
    co_return co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_count();
}

ruvia::task<std::pmr::vector<std::pmr::string>> read_many(const ruvia::db_handle& handle, std::string_view id = cache_id) {
    const auto rows =
        co_await handle.repository<record>().query_builder().cache(id, cache_lifetime).get_many();
    std::pmr::vector<std::pmr::string> values{ruvia::process_resource()};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        values.emplace_back(std::string_view(rows[index].get<"v">()));
    }
    co_return values;
}

ruvia::task<std::pmr::string> read_sql_cached(const ruvia::db_handle& handle, std::string_view parameter) {
    auto query = handle.repository<record>().query_builder();
    query.where(record::column<"v">() == parameter).cache(cache_lifetime);
    const auto row = co_await query.get_one();
    co_return row ? std::pmr::string(std::string_view(row->get<"v">()), ruvia::process_resource()) : std::pmr::string("missing", ruvia::process_resource());
}

}  // namespace

namespace query_cache_test {

// What one concurrent cached read observed. Failures are recorded rather than
// thrown so every reader in a scope finishes on its own.
struct read_outcome final {
    std::pmr::string value_{ruvia::process_resource()};
    std::optional<ruvia::db_error::code_type> error_;
    bool done_{false};
};

ruvia::task<void> read_sql_into(ruvia::db_handle handle, std::string_view parameter, read_outcome& outcome) {
    try {
        outcome.value_ = co_await read_sql_cached(handle, parameter);
    } catch (const ruvia::db_error& error) {
        outcome.error_ = error.code();
    }
    outcome.done_ = true;
}

ruvia::task<void> read_many_into(ruvia::db_handle handle, read_outcome& outcome) {
    try {
        const auto values = co_await read_many(handle);
        outcome.value_ = values.empty() ? std::pmr::string("missing", ruvia::process_resource()) : values.front();
    } catch (const ruvia::db_error& error) {
        outcome.error_ = error.code();
    }
    outcome.done_ = true;
}

// Waits on the worker, a millisecond at a time, until `ready` holds.
template <typename predicate_type>
ruvia::task<void> wait_until(const ruvia::worker_handle& worker, predicate_type ready) {
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
        std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({
                                                                                {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("one", ruvia::process_resource()), std::pmr::string("two", ruvia::process_resource()), std::pmr::string("three", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("alpha", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.column_ = std::pmr::string("exists", ruvia::process_resource()), .rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("t", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.column_ = std::pmr::string("count", ruvia::process_resource()), .rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("3", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("gamma", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("beta", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.column_ = std::pmr::string("exists", ruvia::process_resource()), .rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("f", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.column_ = std::pmr::string("count", ruvia::process_resource()), .rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("5", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("delta", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("epsilon", ruvia::process_resource())}, ruvia::process_resource())},
                                                                            },
            ruvia::process_resource()));
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
                const std::pmr::vector<std::pmr::string> many({std::pmr::string("one", ruvia::process_resource()), std::pmr::string("two", ruvia::process_resource()), std::pmr::string("three", ruvia::process_resource())}, ruvia::process_resource());
                EXPECT_TRUE(co_await read_many(handle) == many);
                EXPECT_EQ(co_await read_one(handle), std::pmr::string("alpha", ruvia::process_resource()));
                EXPECT_TRUE(co_await read_exists(handle));
                EXPECT_EQ(co_await read_count(handle), std::uint64_t{3});
                EXPECT_TRUE(redis_peer.key_count() != 0);
                // Repeated reads are served from the cache; every decoded row
                // resolves its column through the result's shared names.
                EXPECT_TRUE(co_await read_many(handle) == many);
                EXPECT_EQ(co_await read_one(handle), std::pmr::string("alpha", ruvia::process_resource()));
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
                const std::pmr::vector<std::pmr::string> gamma({std::pmr::string("gamma", ruvia::process_resource())}, ruvia::process_resource());
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == gamma);
                // Removing the caller's id drops every result shape cached under
                // it and leaves the independent id's entry alone.
                co_await handle.query_result_cache().remove(cache_id);
                EXPECT_EQ(redis_peer.key_count(), std::size_t{1});
                EXPECT_EQ(co_await read_one(handle), std::pmr::string("beta", ruvia::process_resource()));
                EXPECT_TRUE(!(co_await read_exists(handle)));
                EXPECT_EQ(co_await read_count(handle), std::uint64_t{5});
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == gamma);
                // A multi-id removal drops every shape for each id.
                co_await handle.query_result_cache().remove(cache_id, suffixed_id);
                EXPECT_EQ(redis_peer.key_count(), std::size_t{0});
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == std::pmr::vector<std::pmr::string>({std::pmr::string("delta", ruvia::process_resource())}, ruvia::process_resource()));
                // Clear uses the same cached namespace prefix without touching
                // an unrelated Redis key.
                (void)co_await store.set("outside-cache", "keep");
                co_await handle.query_result_cache().clear();
                EXPECT_EQ(redis_peer.key_count(), std::size_t{1});
                const auto outside = co_await store.get("outside-cache");
                EXPECT_TRUE(outside.has_value() && *outside == "keep");
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == std::pmr::vector<std::pmr::string>({std::pmr::string("epsilon", ruvia::process_resource())}, ruvia::process_resource()));
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
        ruvia::test::backend_startup_peer::after_startup::idle, std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("first", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("second", ruvia::process_resource())}, ruvia::process_resource())}}, ruvia::process_resource()));
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
                const std::pmr::string large_parameter(8192, 'x', ruvia::process_resource());
                std::pmr::string different_parameter(large_parameter, ruvia::process_resource());
                different_parameter.back() = 'y';
                EXPECT_EQ(co_await read_sql_cached(handle, large_parameter), std::pmr::string("first", ruvia::process_resource()));
                EXPECT_EQ(co_await read_sql_cached(handle, large_parameter), std::pmr::string("first", ruvia::process_resource()));
                EXPECT_EQ(co_await read_sql_cached(handle, different_parameter), std::pmr::string("second", ruvia::process_resource()));
                EXPECT_EQ(co_await read_sql_cached(handle, large_parameter), std::pmr::string("first", ruvia::process_resource()));
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
    std::pmr::string empty(cache_entry_magic, ruvia::process_resource());
    const auto row_count_offset = cache_entry_magic.size();
    const auto column_count_offset = row_count_offset + 8;
    empty.append(16, '\0');
    redis_peer.poison_reads(empty);
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle, std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>(ruvia::process_resource()));
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
                auto empty_with_schema = decltype(empty)(empty, ruvia::process_resource());
                empty_with_schema[column_count_offset] = 1;
                empty_with_schema.push_back('\1');
                empty_with_schema.append(7, '\0');
                empty_with_schema.push_back('v');
                redis_peer.poison_reads(std::move(empty_with_schema));
                auto named_empty_rows = co_await query.get_raw_many();
                EXPECT_TRUE(named_empty_rows.empty());
                auto zero_columns = decltype(empty)(empty, ruvia::process_resource());
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
    std::pmr::string corrupt(cache_entry_magic, ruvia::process_resource());
    corrupt.append(8, '\xff');
    corrupt.append(8, '\0');
    redis_peer.poison_reads(std::move(corrupt));
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle, std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("fresh", ruvia::process_resource())}, ruvia::process_resource())}}, ruvia::process_resource()));
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
                const std::pmr::vector<std::pmr::string> fresh({std::pmr::string("fresh", ruvia::process_resource())}, ruvia::process_resource());
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
    const std::pmr::string large(512, 'x', ruvia::process_resource());
    const std::pmr::string larger(513, 'y', ruvia::process_resource());
    peer_type database_peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string(large, ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string(larger, ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("small", ruvia::process_resource())}, ruvia::process_resource())}}, ruvia::process_resource()));
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_handle&) -> ruvia::task<void> {
        const auto handle = database.db().with_options({.timeout_ = std::chrono::seconds(5)});
        EXPECT_EQ(co_await read_sql_cached(handle, "large"), large);
        EXPECT_EQ(redis_peer.key_count(), std::size_t{0});
        EXPECT_EQ(co_await read_sql_cached(handle, "large"), larger);
        EXPECT_EQ(co_await read_sql_cached(handle, "small"), std::pmr::string("small", ruvia::process_resource()));
        EXPECT_EQ(co_await read_sql_cached(handle, "small"), std::pmr::string("small", ruvia::process_resource()));
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
        std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("first", ruvia::process_resource())}, ruvia::process_resource()), .deferred_ = true}}, ruvia::process_resource()));
    std::array<query_cache_test::read_outcome, 3> outcomes;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_handle& store) -> ruvia::task<void> {
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
        std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("abandoned", ruvia::process_resource())}, ruvia::process_resource()), .deferred_ = true}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("taken over", ruvia::process_resource())}, ruvia::process_resource())}}, ruvia::process_resource()));
    query_cache_test::read_outcome leader;
    query_cache_test::read_outcome waiter;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_handle& store) -> ruvia::task<void> {
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
        std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("first", ruvia::process_resource())}, ruvia::process_resource()), .deferred_ = true}}, ruvia::process_resource()));
    query_cache_test::read_outcome leader;
    query_cache_test::read_outcome waiter;
    bool waiter_finished_first = false;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_handle&) -> ruvia::task<void> {
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
        std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("stale", ruvia::process_resource())}, ruvia::process_resource()), .deferred_ = true}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("fresh", ruvia::process_resource())}, ruvia::process_resource())}}, ruvia::process_resource()));
    query_cache_test::read_outcome before;
    query_cache_test::read_outcome after;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_handle& store) -> ruvia::task<void> {
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
