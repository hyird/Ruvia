#if defined(RUVIA_ENABLE_POSTGRESQL) && defined(RUVIA_ENABLE_REDIS)

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
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_repository.h"
#include "ruvia/web/redis/redis_handle.h"
#include "ruvia/web/redis/redis_types.h"

#include "backend_client_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

// Removing a query-result cache id must drop every result a repository builder
// cached under that id, so the next get_one/get_exists/get_count reads the
// database again.

namespace {

RUVIA_DB_ENTITY(record, "records", ruvia::db_column<"v", std::pmr::string>)

constexpr std::string_view cache_id = "item";
constexpr std::chrono::milliseconds cache_lifetime = std::chrono::minutes(1);

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
    void poison_reads(std::string value) {
        std::promise<void> poisoned;
        auto result = poisoned.get_future();
        asio::post(io_, [this, &poisoned, value = std::move(value)]() mutable {
            poison_ = std::move(value);
            poisoned.set_value();
        });
        result.get();
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
        std::string line = buffer.substr(0, size - 2);
        buffer.erase(0, size);
        co_return line;
    }

    static asio::awaitable<std::vector<std::string>> read_command(socket_type& socket, std::string& buffer) {
        const auto header = co_await read_line(socket, buffer);
        if (header.size() < 2 || header.front() != '*') {
            throw std::runtime_error("invalid RESP command array");
        }
        const auto count = std::stoull(header.substr(1));
        std::vector<std::string> arguments;
        arguments.reserve(count);
        for (std::size_t index = 0; index != count; ++index) {
            const auto length_line = co_await read_line(socket, buffer);
            if (length_line.size() < 2 || length_line.front() != '$') {
                throw std::runtime_error("invalid RESP bulk string");
            }
            const auto length = static_cast<std::size_t>(std::stoull(length_line.substr(1)));
            if (buffer.size() < length + 2) {
                co_await asio::async_read(socket, asio::dynamic_buffer(buffer),
                    asio::transfer_exactly(length + 2 - buffer.size()), asio::use_awaitable);
            }
            if (buffer.compare(length, 2, "\r\n") != 0) {
                throw std::runtime_error("invalid RESP bulk ending");
            }
            arguments.push_back(buffer.substr(0, length));
            buffer.erase(0, length + 2);
        }
        co_return arguments;
    }

    // Expiration options after a SET value are accepted and not enforced; the
    // scenario completes well within the cache lifetime.
    [[nodiscard]] std::string execute(std::vector<std::string>& arguments) {
        if (arguments.empty()) {
            return "-ERR empty command\r\n";
        }
        auto& name = arguments.front();
        std::ranges::transform(name, name.begin(), [](unsigned char value) {
            return static_cast<char>(std::toupper(value));
        });
        if (name == "GET" && arguments.size() == 2) {
            if (poison_) {
                return "$" + std::to_string(poison_->size()) + "\r\n" + *poison_ + "\r\n";
            }
            const auto found = store_.find(arguments[1]);
            if (found == store_.end()) {
                return "$-1\r\n";
            }
            return "$" + std::to_string(found->second.size()) + "\r\n" + found->second + "\r\n";
        }
        if (name == "SET" && arguments.size() >= 3) {
            store_.insert_or_assign(std::move(arguments[1]), std::move(arguments[2]));
            return "+OK\r\n";
        }
        if (name == "DEL" && arguments.size() >= 2) {
            std::size_t removed = 0;
            for (std::size_t index = 1; index != arguments.size(); ++index) {
                removed += store_.erase(arguments[index]);
            }
            return ":" + std::to_string(removed) + "\r\n";
        }
        if (name == "SCAN" && arguments.size() == 6 && arguments[1] == "0" &&
            arguments[2] == "MATCH" && arguments[3].ends_with('*') && arguments[4] == "COUNT") {
            const auto prefix = std::string_view(arguments[3]).substr(0, arguments[3].size() - 1);
            std::string values;
            std::size_t count = 0;
            for (const auto& [key, value] : store_) {
                (void)value;
                if (key.starts_with(prefix)) {
                    values.append("$").append(std::to_string(key.size())).append("\r\n").append(key).append("\r\n");
                    ++count;
                }
            }
            return "*2\r\n$1\r\n0\r\n*" + std::to_string(count) + "\r\n" + values;
        }
        return "-ERR unsupported command\r\n";
    }

    asio::awaitable<void> serve(std::shared_ptr<socket_type> socket) {
        std::string buffer;
        try {
            for (;;) {
                auto arguments = co_await read_command(*socket, buffer);
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
    std::vector<std::shared_ptr<socket_type>> sockets_;
    std::map<std::string, std::string, std::less<>> store_;
    std::optional<std::string> poison_;
    std::exception_ptr failure_;
    bool closing_{false};
    std::thread thread_;
};

// Every read names the same cache id; each result shape stores its own entry.
ruvia::task<std::string> read_one(const ruvia::db_handle& handle) {
    const auto row =
        co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_one();
    co_return row ? std::string(std::string_view(row->get<"v">())) : std::string("missing");
}

ruvia::task<bool> read_exists(const ruvia::db_handle& handle) {
    co_return co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_exists();
}

ruvia::task<std::uint64_t> read_count(const ruvia::db_handle& handle) {
    co_return co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_count();
}

ruvia::task<std::vector<std::string>> read_many(const ruvia::db_handle& handle, std::string_view id = cache_id) {
    const auto rows =
        co_await handle.repository<record>().query_builder().cache(id, cache_lifetime).get_many();
    std::vector<std::string> values;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        values.emplace_back(std::string_view(rows[index].get<"v">()));
    }
    co_return values;
}

}  // namespace

RUVIA_TEST(db_query_result_cache_remove_invalidates_repository_result_shapes) {
    redis_store_peer redis_peer;
    // The database answers the statements the cache lets through, in order. A
    // read served from the cache consumes no reply, so a stale entry shows up
    // as the previous value and an unexpected database read as the next one.
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle,
        {
            {.rows_ = {"one", "two", "three"}},
            {.rows_ = {"alpha"}},
            {.column_ = "exists", .rows_ = {"t"}},
            {.column_ = "count", .rows_ = {"3"}},
            {.rows_ = {"gamma"}},
            {.rows_ = {"beta"}},
            {.column_ = "exists", .rows_ = {"f"}},
            {.column_ = "count", .rows_ = {"5"}},
            {.rows_ = {"delta"}},
        });
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
            const auto store = redis.handle();
            ruvia::db_client database(attachment.loop(), database_peer.config(), store, ruvia::db_cache_config{});
            try {
                co_await database.connect();
                const auto handle = database.handle();
                // First reads come from the database and fill the cache.
                const std::vector<std::string> many{"one", "two", "three"};
                RUVIA_CHECK(co_await read_many(handle) == many);
                RUVIA_CHECK_EQ(co_await read_one(handle), std::string("alpha"));
                RUVIA_CHECK(co_await read_exists(handle));
                RUVIA_CHECK_EQ(co_await read_count(handle), std::uint64_t{3});
                RUVIA_CHECK(redis_peer.key_count() != 0);
                // Repeated reads are served from the cache; every decoded row
                // resolves its column through the result's shared names.
                RUVIA_CHECK(co_await read_many(handle) == many);
                RUVIA_CHECK_EQ(co_await read_one(handle), std::string("alpha"));
                RUVIA_CHECK(co_await read_exists(handle));
                RUVIA_CHECK_EQ(co_await read_count(handle), std::uint64_t{3});
                // A user id that spells another id plus a shape name is its own
                // entry: it reads the database instead of the "one" result.
                constexpr std::string_view suffixed_id = "item-one";
                const std::vector<std::string> gamma{"gamma"};
                RUVIA_CHECK(co_await read_many(handle, suffixed_id) == gamma);
                // Removing the caller's id drops every result shape cached under
                // it and leaves the independent id's entry alone.
                co_await handle.query_result_cache().remove(cache_id);
                RUVIA_CHECK_EQ(redis_peer.key_count(), std::size_t{1});
                RUVIA_CHECK_EQ(co_await read_one(handle), std::string("beta"));
                RUVIA_CHECK(!(co_await read_exists(handle)));
                RUVIA_CHECK_EQ(co_await read_count(handle), std::uint64_t{5});
                RUVIA_CHECK(co_await read_many(handle, suffixed_id) == gamma);
                // Clear uses the same cached namespace prefix without touching
                // an unrelated Redis key.
                (void)co_await store.set("outside-cache", "keep");
                co_await handle.query_result_cache().clear();
                RUVIA_CHECK_EQ(redis_peer.key_count(), std::size_t{1});
                const auto outside = co_await store.get("outside-cache");
                RUVIA_CHECK(outside.has_value() && *outside == "keep");
                RUVIA_CHECK(co_await read_many(handle, suffixed_id) == std::vector<std::string>{"delta"});
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
RUVIA_TEST(db_query_result_cache_rejects_corrupt_zero_column_entries) {
    redis_store_peer redis_peer;
    std::string corrupt("RUVIAQC2");
    corrupt.append(8, '\xff');
    corrupt.append(8, '\0');
    redis_peer.poison_reads(std::move(corrupt));
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle, {{.rows_ = {"fresh"}}});
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
            const auto store = redis.handle();
            ruvia::db_client database(
                attachment.loop(), database_peer.config(), store, ruvia::db_cache_config{.ignore_errors_ = true});
            try {
                co_await database.connect();
                const std::vector<std::string> fresh{"fresh"};
                RUVIA_CHECK(co_await read_many(database.handle()) == fresh);
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

#endif
