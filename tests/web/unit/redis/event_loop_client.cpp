#include <array>
#include <barrier>
#include <chrono>
#include <exception>
#include <future>
#include <istream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/redis/redis_client.h"

#include "redis/redis_client_runtime.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_client.h"
#include "ruvia/web/detail/db/db_result_access.h"

#include "db/db_query_cache.h"
#include "db/db_registry.h"
#endif

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_tls_identity.h"

namespace {

RUVIA_REDIS_ENTITY(client_user, "client_users",
    RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::string))

// Deterministic RESP peer, not an external Redis dependency. All sockets and
// cancellation run on its own thread, including cleanup after a failed test.
class redis_peer final {
public:
    enum class mode_type { normal,
        coalesced_pings,
        coalesced_arrays,
        huge_array,
        nested_arrays,
        deep_arrays,
        valid_nested_array,
        query_cache,
        reject_then_ping,
        batch_echo,
        transaction_success,
        transaction_aborted,
        transaction_exec_error,
        transaction_wrong_exec,
        transaction_queue_error,
        transaction_watch_error };

    explicit redis_peer(mode_type mode = mode_type::normal, asio::ssl::context* tls_context = nullptr,
        std::string cache_payload = {})
        : mode_(mode),
          cache_payload_(std::move(cache_payload)),
          tls_context_(tls_context),
          acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          socket_(io_),
          port_(acceptor_.local_endpoint().port()),
          work_(asio::make_work_guard(io_)),
          done_(asio::co_spawn(io_, serve(), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    ~redis_peer() {
        asio::post(io_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
            work_.reset();
        });
        thread_.join();
        try {
            done_.get();
        } catch (const std::system_error&) {
        }
    }

    ruvia::redis_config config() const {
        return {.host_ = "127.0.0.1", .port_ = port_, .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1, .connect_timeout_ = std::chrono::seconds(2), .command_timeout_ = std::chrono::seconds(2)};
    }

    void wait_for_blocked_command() {
        auto blocked = blocked_.get_future();
        if (blocked.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("Redis test peer did not receive the command");
        }
        blocked.get();
    }

    std::vector<std::string> expiration_command() {
        return expiration_.get_future().get();
    }

    std::vector<std::vector<std::string>> batch_commands() {
        return batch_commands_.get_future().get();
    }

private:
    template <typename stream_type>
    asio::awaitable<std::string> line(stream_type& stream) {
        co_await asio::async_read_until(stream, buffer_, "\r\n", asio::use_awaitable);
        std::istream input(&buffer_);
        std::string value;
        std::getline(input, value);
        value.pop_back();
        co_return value;
    }

    asio::awaitable<void> serve() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        try {
            co_await serve_transport();
        } catch (const std::system_error&) {
            if (mode_ != mode_type::reject_then_ping) {
                throw;
            }
        }
        if (mode_ == mode_type::reject_then_ping) {
            first_connection_ = false;
            socket_ = asio::ip::tcp::socket(io_);
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            co_await serve_transport();
        }
    }

    asio::awaitable<void> serve_transport() {
        if (tls_context_ != nullptr) {
            asio::ssl::stream<asio::ip::tcp::socket&> stream(socket_, *tls_context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            co_await serve_connection(stream);
        } else {
            co_await serve_connection(socket_);
        }
    }

    template <typename stream_type>
    asio::awaitable<void> serve_connection(stream_type& stream) {
        bool in_transaction = false;
        std::vector<std::vector<std::string>> transaction_commands;
        std::vector<std::string> queued_replies;
        for (;;) {
            const auto header_value = co_await line(stream);
            const auto count = std::stoi(header_value.substr(1));
            std::vector<std::string> args;
            for (int index = 0; index < count; ++index) {
                const auto bulk = co_await line(stream);
                const auto size = static_cast<std::size_t>(std::stoul(bulk.substr(1)));
                if (buffer_.size() < size + 2) {
                    co_await asio::async_read(stream, buffer_,
                        asio::transfer_exactly(size + 2 - buffer_.size()), asio::use_awaitable);
                }
                std::string arg(size, '\0');
                std::istream input(&buffer_);
                input.read(arg.data(), static_cast<std::streamsize>(size));
                input.ignore(2);
                args.push_back(std::move(arg));
            }
            if (args.front() == "STALL" || args.front() == "AUTH") {
                blocked_.set_value();
                // Read again without replying; client cancellation closes this socket.
                continue;
            }
            std::string reply;
            const bool transaction_mode = mode_ == mode_type::transaction_success ||
                                          mode_ == mode_type::transaction_aborted ||
                                          mode_ == mode_type::transaction_exec_error ||
                                          mode_ == mode_type::transaction_wrong_exec ||
                                          mode_ == mode_type::transaction_queue_error ||
                                          mode_ == mode_type::transaction_watch_error;
            if (transaction_mode &&
                (in_transaction || args.front() == "WATCH" || args.front() == "UNWATCH" || args.front() == "MULTI")) {
                transaction_commands.push_back(args);
                if (args.front() == "WATCH") {
                    reply = mode_ == mode_type::transaction_watch_error ? "-ERR watch failed\r\n" : "+OK\r\n";
                } else if (args.front() == "UNWATCH") {
                    reply = "+OK\r\n";
                } else if (args.front() == "MULTI") {
                    in_transaction = true;
                    reply = "+OK\r\n";
                } else if (args.front() == "EXEC") {
                    in_transaction = false;
                    batch_commands_.set_value(transaction_commands);
                    if (mode_ == mode_type::transaction_aborted) {
                        reply = "*-1\r\n";
                    } else if (mode_ == mode_type::transaction_exec_error) {
                        reply = "-EXECABORT discarded\r\n";
                    } else if (mode_ == mode_type::transaction_wrong_exec) {
                        reply = "+OK\r\n";
                    } else {
                        reply = "*" + std::to_string(queued_replies.size()) + "\r\n";
                        for (const auto& queued : queued_replies) {
                            reply += queued;
                        }
                    }
                } else {
                    queued_replies.push_back("$" + std::to_string(args.back().size()) + "\r\n" + args.back() + "\r\n");
                    reply = mode_ == mode_type::transaction_queue_error ? "-ERR queue failed\r\n" : "+QUEUED\r\n";
                }
            } else if (mode_ == mode_type::batch_echo && args.front() != "PING") {
                reply = "$" + std::to_string(args.back().size()) + "\r\n" + args.back() + "\r\n";
            } else if ((mode_ == mode_type::coalesced_pings || mode_ == mode_type::coalesced_arrays) &&
                       args.front() == "PING") {
                if (co_await line(stream) != "*1" || co_await line(stream) != "$4" ||
                    co_await line(stream) != "PING") {
                    throw std::runtime_error("expected second pipelined PING");
                }
                reply = mode_ == mode_type::coalesced_arrays
                            ? "*3\r\n+\r\n+\r\n+\r\n*3\r\n+\r\n+\r\n+\r\n"
                            : "+PONG\r\n+PONG\r\n";
            } else if ((mode_ == mode_type::huge_array ||
                           (mode_ == mode_type::reject_then_ping && first_connection_)) &&
                       args.front() == "PING") {
                reply = "*1048576\r\n";
            } else if (mode_ == mode_type::nested_arrays && args.front() == "PING") {
                for (int index = 0; index < 100; ++index) {
                    reply.append("*300\r\n");
                }
            } else if (mode_ == mode_type::deep_arrays && args.front() == "PING") {
                for (int index = 0; index < 5; ++index) {
                    reply.append("*1\r\n");
                }
            } else if (mode_ == mode_type::valid_nested_array && args.front() == "PING") {
                for (int index = 0; index < 4; ++index) {
                    reply.append("*1\r\n");
                }
                reply.append("+PONG\r\n");
            } else if (args.front() == "PING") {
                reply = args.size() == 1 ? "+PONG\r\n" : "$" + std::to_string(args[1].size()) + "\r\n" + args[1] + "\r\n";
            } else if (args.front() == "GET") {
                reply = mode_ == mode_type::query_cache
                            ? "$" + std::to_string(cache_payload_.size()) + "\r\n" + cache_payload_ + "\r\n"
                            : "$128\r\n" + std::string(128, 'v') + "\r\n";
            } else if (args.front() == "EXPIREAT" || args.front() == "PEXPIREAT") {
                expiration_.set_value(args);
                reply = ":1\r\n";
            } else if (args.front() == "HGETALL") {
                reply = "*6\r\n$14\r\n__ruvia_entity\r\n$1\r\n1\r\n$2\r\nid\r\n$1\r\n1\r\n$4\r\nname\r\n$128\r\n" + std::string(128, 'n') + "\r\n";
            } else {
                reply = "-ERR test error\r\n";
            }
            co_await asio::async_write(stream, asio::buffer(reply), asio::use_awaitable);
        }
    }

    mode_type mode_;
    std::string cache_payload_;
    asio::ssl::context* tls_context_;
    bool first_connection_{true};
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    std::uint16_t port_;
    asio::streambuf buffer_;
    std::promise<void> blocked_;
    std::promise<std::vector<std::string>> expiration_;
    std::promise<std::vector<std::vector<std::string>>> batch_commands_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::future<void> done_;
    std::thread thread_;
};

ruvia::task<void> check_commands(ruvia::redis_client& client, ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    bool duplicate_rejected = false;
    try {
        co_await client.connect();
    } catch (const std::logic_error&) {
        duplicate_rejected = true;
    }
    RUVIA_CHECK(duplicate_rejected);
    // A rejected second connect must not tear down the successful first one.
    co_await client.ping();
    auto retained = co_await client.get("retained");
    RUVIA_CHECK(retained.has_value());
    auto users = client.get_repository<client_user>();
    auto user_value = co_await users.find_one({.where_ = client_user::field<"id">() == "1"});
    RUVIA_CHECK(user_value.has_value());
    std::string mutable_ping(128, 'm');
    auto cold_ping = client.ping(mutable_ping);
    mutable_ping.assign(128, 'n');
    const auto owned_echo = co_await std::move(cold_ping);
    RUVIA_CHECK(owned_echo == std::string_view(std::string(128, 'm')));
    for (int index = 0; index < 32; ++index) {
        auto cold = client.get(std::string(128, 'k'));
        (void)cold;
        auto echo = co_await client.ping(std::string(128, 'x'));
        RUVIA_CHECK(echo == std::string_view(std::string(128, 'x')));
        RUVIA_CHECK(*retained == std::string_view(std::string(128, 'v')));
        RUVIA_CHECK(user_value->get<"name">().view() == std::string_view(std::string(128, 'n')));
    }
    bool failed = false;
    try {
        (void)co_await client.set("bad", "value");
    } catch (const ruvia::redis_error&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    co_await client.ping();
    auto handle = client.with_options({});
    auto cold = handle.get("not-started");
    co_await client.shutdown();
    co_await client.shutdown();
    bool expired = false;
    try {
        (void)handle.get("closed");
    } catch (const std::logic_error&) {
        expired = true;
    }
    RUVIA_CHECK(expired);
    RUVIA_CHECK(*retained == std::string_view(std::string(128, 'v')));
}

ruvia::task<void> check_coalesced_replies(ruvia::redis_client& client,
    ruvia::testing::test_context& ruvia_ctx, bool arrays = false) {
    co_await client.connect();
    auto pipeline = client.pipeline();
    pipeline.command("PING").command("PING");
    bool accepted = false;
    try {
        const auto replies = co_await std::move(pipeline).exec();
        accepted = replies.size() == 2;
        if (accepted && arrays) {
            accepted = replies[0].kind() == ruvia::redis_value::kind_type::array &&
                       replies[1].kind() == ruvia::redis_value::kind_type::array &&
                       replies[0].array().size() == 3 && replies[1].array().size() == 3;
        } else if (accepted) {
            accepted = replies[0].string() == "PONG" && replies[1].string() == "PONG";
        }
    } catch (const ruvia::redis_error&) {
    }
    RUVIA_CHECK(accepted);
    co_await client.shutdown();
}

ruvia::task<void> check_owned_pipeline(ruvia::redis_client& client,
    ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    const std::string binary_key("key\0binary", 10);
    const std::string binary_value("value\0binary", 12);
    auto cold = [&] {
        auto pipeline = client.pipeline();
        auto key = binary_key;
        auto value = binary_value;
        pipeline.get(key).set(key, value).incr_by(key, std::numeric_limits<std::int64_t>::min()).zadd(key, 1.5, "member");
        key.assign(128, 'x');
        value.assign(128, 'y');
        return std::move(pipeline).exec();
    }();
    const auto retained = co_await std::move(cold);
    RUVIA_CHECK_EQ(retained.size(), std::size_t{4});
    RUVIA_CHECK(retained[0].string() == std::string_view(binary_key));
    RUVIA_CHECK(retained[1].string() == std::string_view(binary_value));
    RUVIA_CHECK(retained[2].string() == "-9223372036854775808");
    RUVIA_CHECK(retained[3].string() == "member");
    auto next_value = client.pipeline();
    next_value.get(std::string(256, 'n'));
    const auto replies = co_await std::move(next_value).exec();
    RUVIA_CHECK_EQ(replies.size(), std::size_t{1});
    RUVIA_CHECK(retained[1].string() == std::string_view(binary_value));
    co_await client.shutdown();
    RUVIA_CHECK(retained[0].string() == std::string_view(binary_key));
}

ruvia::task<void> check_owned_transaction(ruvia::redis_client& client, redis_peer::mode_type mode,
    ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    const std::string binary_key("key\0binary", 10);
    const std::string binary_value("value\0binary", 12);
    auto cold = [&] {
        auto transaction = client.transaction();
        auto key = binary_key;
        auto value = binary_value;
        transaction.watch(key, "second").unwatch().watch(key).set(key, value).get(key);
        key.assign(128, 'x');
        value.assign(128, 'y');
        return std::move(transaction).exec();
    }();
    bool accepted = false;
    try {
        const auto retained = co_await std::move(cold);
        RUVIA_CHECK(mode == redis_peer::mode_type::transaction_success);
        RUVIA_CHECK_EQ(retained.size(), std::size_t{2});
        RUVIA_CHECK(retained[0].string() == std::string_view(binary_value));
        RUVIA_CHECK(retained[1].string() == std::string_view(binary_key));
        co_await client.ping();
        co_await client.shutdown();
        RUVIA_CHECK(retained[0].string() == std::string_view(binary_value));
        accepted = true;
    } catch (const ruvia::redis_error& error) {
        const auto expected_code = mode == redis_peer::mode_type::transaction_aborted
                                       ? ruvia::redis_error::code_type::transaction_aborted
                                       : ruvia::redis_error::code_type::command_error;
        RUVIA_CHECK(mode != redis_peer::mode_type::transaction_success);
        RUVIA_CHECK(error.code() == expected_code);
        if (mode == redis_peer::mode_type::transaction_queue_error) {
            RUVIA_CHECK((std::string_view(error.what()).find("reply 4") != std::string_view::npos));
            RUVIA_CHECK((std::string_view(error.what()).find("queue failed") != std::string_view::npos));
        } else if (mode == redis_peer::mode_type::transaction_watch_error) {
            RUVIA_CHECK((std::string_view(error.what()).find("reply 0") != std::string_view::npos));
            RUVIA_CHECK((std::string_view(error.what()).find("watch failed") != std::string_view::npos));
        } else if (mode == redis_peer::mode_type::transaction_exec_error) {
            RUVIA_CHECK((std::string_view(error.what()).find("EXECABORT") != std::string_view::npos));
        }
        accepted = true;
    }
    RUVIA_CHECK(accepted);
    co_await client.shutdown();
}

ruvia::task<void> check_oversized_reply(ruvia::redis_client& client,
    ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    bool rejected = false;
    try {
        (void)co_await client.get("large");
    } catch (const ruvia::redis_error& error) {
        rejected = error.code() == ruvia::redis_error::code_type::protocol_error;
    }
    RUVIA_CHECK(rejected);
    co_await client.shutdown();
}

ruvia::task<void> check_huge_declared_array(ruvia::redis_client& client,
    ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    bool rejected = false;
    try {
        co_await client.ping();
    } catch (const ruvia::redis_error& error) {
        rejected = error.code() == ruvia::redis_error::code_type::protocol_error;
    }
    RUVIA_CHECK(rejected);
    co_await client.shutdown();
}

ruvia::task<void> check_nested_array_at_depth_limit(ruvia::redis_client& client,
    ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    const std::string_view command[]{"PING"};
    const auto reply = co_await client.command(std::span<const std::string_view>(command));
    const ruvia::redis_value* value = &reply;
    bool valid = true;
    for (int index = 0; index < 4; ++index) {
        if (value->kind() != ruvia::redis_value::kind_type::array || value->array().size() != 1) {
            valid = false;
            break;
        }
        value = &value->array()[0];
    }
    RUVIA_CHECK(valid && value->kind() == ruvia::redis_value::kind_type::string &&
                value->string() == "PONG");
    co_await client.shutdown();
}

ruvia::task<void> check_reconnect_after_budget_rejection(ruvia::redis_client& client,
    ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    bool rejected = false;
    try {
        co_await client.ping();
    } catch (const ruvia::redis_error& error) {
        rejected = error.code() == ruvia::redis_error::code_type::protocol_error;
    }
    RUVIA_CHECK(rejected);
    co_await client.ping();
    co_await client.shutdown();
}

ruvia::task<void> blocked_command(ruvia::redis_client& client) {
    co_await client.connect();
    (void)co_await client.command("STALL");
}

ruvia::task<void> connect_until_stopped(ruvia::redis_client& client, bool& cancelled) {
    try {
        co_await client.connect();
    } catch (const ruvia::redis_error&) {
        cancelled = true;
    }
}

ruvia::task<void> check_expiration(ruvia::redis_client& client,
    std::chrono::system_clock::time_point expires_at, ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    const bool applied = co_await client.expire_at("session", expires_at);
    RUVIA_CHECK(applied);
    co_await client.shutdown();
}

ruvia::task<void> check_runtime_handle_timeout(ruvia::event_loop loop, ruvia::redis_config config,
    ruvia::testing::test_context& ruvia_ctx) {
    config.command_timeout_.reset();
    auto worker_value = loop.handle();
    ruvia::detail::redis_client_runtime runtime(loop.io_context(), worker_value,
        ruvia::detail::redis_config_storage(config, std::pmr::get_default_resource()),
        std::pmr::get_default_resource());
    ruvia::operation_scope scope;
    co_await runtime.connect();
    auto handle = runtime.handle(scope, {.timeout_ = std::chrono::seconds(5)});
    auto copied = handle;
    auto inherited = copied.with_options({});
    auto shortened = inherited.with_options({.timeout_ = std::chrono::milliseconds(100)});
    auto derived = shortened.with_options({.timeout_ = std::chrono::seconds(5)});
    auto cold_operation = derived.command("STALL");
    (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(250));
    const auto started = std::chrono::steady_clock::now();
    bool command_timed_out = false;
    try {
        (void)co_await std::move(cold_operation);
    } catch (const ruvia::redis_error& error) {
        command_timed_out = error.code() == ruvia::redis_error::code_type::timeout;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    RUVIA_CHECK(command_timed_out);
    // Cold time must not consume the deadline; a longer override cannot relax it.
    RUVIA_CHECK(elapsed >= std::chrono::milliseconds(50));
    RUVIA_CHECK(elapsed < std::chrono::seconds(2));
    runtime.close_now();
    co_await scope.close_and_join();
}

ruvia::task<void> check_runtime_handle_cancellation_bridge(ruvia::event_loop loop,
    ruvia::redis_config config, ruvia::stop_source& base_stop,
    ruvia::stop_source& override_stop, ruvia::testing::test_context& ruvia_ctx) {
    config.command_timeout_.reset();
    auto worker_value = loop.handle();
    ruvia::detail::redis_client_runtime runtime(loop.io_context(), worker_value,
        ruvia::detail::redis_config_storage(config, std::pmr::get_default_resource()),
        std::pmr::get_default_resource());
    ruvia::operation_scope scope;
    co_await runtime.connect();
    auto configured = runtime.handle(scope, {.stop_token_ = base_stop.token()});
    auto copied = configured;
    auto derived = copied.with_options({.stop_token_ = override_stop.token()});
    bool cancelled = false;
    try {
        (void)co_await derived.command("STALL");
    } catch (const ruvia::redis_error& error) {
        cancelled = error.code() == ruvia::redis_error::code_type::cancelled;
    }
    RUVIA_CHECK(cancelled);
    runtime.close_now();
    co_await scope.close_and_join();
}

ruvia::task<void> check_runtime_memory(ruvia::event_loop loop, ruvia::redis_config config,
    ruvia::testing::test_context& ruvia_ctx) {
    ruvia::test::counting_memory_resource memory;
    {
        auto worker_value = loop.handle();
        ruvia::detail::redis_client_runtime runtime(loop.io_context(), worker_value,
            ruvia::detail::redis_config_storage(config, &memory), &memory);
        ruvia::operation_scope scope;
        co_await runtime.connect();
        auto handle = runtime.handle(scope, {.timeout_ = std::chrono::seconds(2)});
        auto copied_handle = handle;
        // Warm connection buffers, then retain a result across later calls.
        {
            auto warm = co_await handle.ping(std::string(256, 'w'));
        }
        auto retained = co_await handle.get("retained");
        const auto baseline = memory.live_allocations();
        const auto allocations = memory.allocation_count();
        const auto deallocations = memory.deallocation_count();
        for (int index = 0; index < 32; ++index) {
            {
                auto cold = handle.get(std::string(128, 'k'));
            }
            RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
            {
                auto cold = [&] {
                    auto pipeline = handle.pipeline();
                    pipeline.set(std::string(128, 'k'), std::string(128, 'v'));
                    return std::move(pipeline).exec();
                }();
            }
            RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
            {
                auto cold = [&] {
                    auto transaction = handle.transaction();
                    transaction.watch(std::string(128, 'w'))
                        .set(std::string(128, 'k'), std::string(128, 'v'));
                    return std::move(transaction).exec();
                }();
            }
            RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
            {
                auto result_value = co_await handle.ping(std::string(128, 'x'));
            }
            RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
            bool failed = false;
            try {
                (void)co_await handle.set("bad", "value");
            } catch (const ruvia::redis_error&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
            ruvia::stop_source stop;
            stop.request_stop();
            bool cancelled = false;
            try {
                (void)co_await copied_handle.with_options({.stop_token_ = stop.token()})
                    .get(std::string(128, 'c'));
            } catch (const ruvia::redis_error&) {
                cancelled = true;
            }
            RUVIA_CHECK(cancelled);
            RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
            for (const bool transaction_batch : {false, true}) {
                bool batch_cancelled = false;
                try {
                    auto configured = copied_handle.with_options({.stop_token_ = stop.token()});
                    if (transaction_batch) {
                        auto transaction = configured.transaction();
                        transaction.watch(std::string(128, 'w'))
                            .set(std::string(128, 'k'), std::string(128, 'v'));
                        (void)co_await std::move(transaction).exec();
                    } else {
                        auto pipeline = configured.pipeline();
                        pipeline.set(std::string(128, 'k'), std::string(128, 'v'));
                        (void)co_await std::move(pipeline).exec();
                    }
                } catch (const ruvia::redis_error& error) {
                    batch_cancelled = error.code() == ruvia::redis_error::code_type::cancelled;
                }
                RUVIA_CHECK(batch_cancelled);
                RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
            }
            RUVIA_CHECK(*retained == std::string_view(std::string(128, 'v')));
        }
        RUVIA_CHECK(memory.allocation_count() > allocations);
        RUVIA_CHECK(memory.deallocation_count() > deallocations);
        retained.reset();
        RUVIA_CHECK(memory.live_allocations() < baseline);
        co_await scope.close_and_join();
        auto inactive_handle = runtime.handle(scope);
        bool inactive_rejected = false;
        try {
            (void)inactive_handle.ping();
        } catch (const std::logic_error&) {
            inactive_rejected = true;
        }
        RUVIA_CHECK(inactive_rejected);
        runtime.close_now();
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}

}  // namespace

RUVIA_TEST(redis_client_runtime_handle_options_survive_copy_and_derivation) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    pool.loop(0).start(check_runtime_handle_timeout(pool.loop(0), peer.config(), ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_client_runtime_handle_bridges_both_stop_tokens_after_copy_and_derivation) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    for (const bool stop_base : {true, false}) {
        redis_peer peer;
        ruvia::stop_source base_stop;
        ruvia::stop_source override_stop;
        auto operation = pool.loop(0).start(check_runtime_handle_cancellation_bridge(
            pool.loop(0), peer.config(), base_stop, override_stop, ruvia_ctx));
        peer.wait_for_blocked_command();
        (stop_base ? base_stop : override_stop).request_stop();
        operation.get();
    }
    pool.join();
}

RUVIA_TEST(redis_client_runtime_reclaims_operations_independently_of_retained_results) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    pool.loop(0).start(check_runtime_memory(pool.loop(0), peer.config(), ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_client_runs_on_its_event_loop_and_retains_results) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 2});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    auto wrong_loop = pool.loop(1).start(client.connect());
    bool rejected = false;
    try {
        wrong_loop.get();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    pool.loop(0).start(check_commands(client, ruvia_ctx)).get();
    pool.stop();
    pool.join();
}

RUVIA_TEST(redis_pipeline_owns_binary_input_and_retains_ordered_replies) {
    redis_peer peer(redis_peer::mode_type::batch_echo);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    pool.loop(0).start(check_owned_pipeline(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_transaction_owns_framing_and_handles_exec_outcomes) {
    for (const auto mode : {redis_peer::mode_type::transaction_success,
             redis_peer::mode_type::transaction_aborted, redis_peer::mode_type::transaction_exec_error,
             redis_peer::mode_type::transaction_wrong_exec, redis_peer::mode_type::transaction_queue_error,
             redis_peer::mode_type::transaction_watch_error}) {
        redis_peer peer(mode);
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::redis_client client(pool.loop(0), peer.config());
        pool.start();
        pool.loop(0).start(check_owned_transaction(client, mode, ruvia_ctx)).get();
        pool.join();
        const std::string key("key\0binary", 10);
        const std::string value("value\0binary", 12);
        const std::vector<std::vector<std::string>> expected{
            {"WATCH", key, "second"},
            {"UNWATCH"},
            {"WATCH", key},
            {"MULTI"},
            {"SET", key, value},
            {"GET", key},
            {"EXEC"},
        };
        RUVIA_CHECK(peer.batch_commands() == expected);
    }
}

RUVIA_TEST(redis_pipeline_reply_limit_counts_each_reply_not_the_tcp_batch) {
    redis_peer peer(redis_peer::mode_type::coalesced_pings);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 7;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_coalesced_replies(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_pipeline_array_element_budget_resets_for_each_reply) {
    redis_peer peer(redis_peer::mode_type::coalesced_arrays);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 13;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_coalesced_replies(client, ruvia_ctx, true)).get();
    pool.join();
}

RUVIA_TEST(redis_client_reply_limit_still_rejects_oversized_single_reply) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 7;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_oversized_reply(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_reply_budget_rejects_impossible_declared_array_before_its_elements_arrive) {
    redis_peer peer(redis_peer::mode_type::huge_array);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 1024;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_huge_declared_array(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_reply_budget_rejects_aggregate_nested_array_declarations) {
    redis_peer peer(redis_peer::mode_type::nested_arrays);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 1024;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_huge_declared_array(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_reply_depth_rejects_incomplete_nested_array) {
    redis_peer peer(redis_peer::mode_type::deep_arrays);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_array_depth_ = 4;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_huge_declared_array(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_reply_array_at_the_depth_limit_is_accepted) {
    redis_peer peer(redis_peer::mode_type::valid_nested_array);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_array_depth_ = 4;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_nested_array_at_depth_limit(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_reader_budget_rebinds_after_protocol_failure_and_reconnect) {
    redis_peer peer(redis_peer::mode_type::reject_then_ping);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 1024;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_reconnect_after_budget_rejection(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_client_expire_at_preserves_the_requested_deadline) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    const auto expires_at = std::chrono::system_clock::time_point{
                                std::chrono::seconds(2'000'000'000)} +
                            std::chrono::microseconds(1'001);
    pool.loop(0).start(check_expiration(client, expires_at, ruvia_ctx)).get();
    const auto command = peer.expiration_command();
    pool.join();
    RUVIA_CHECK_EQ(command, (std::vector<std::string>{"PEXPIREAT", "session", "2000000000002"}));
}

RUVIA_TEST(redis_client_loop_stop_cancels_and_joins_pending_commands) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    auto pending = pool.loop(0).start(blocked_command(client));
    peer.wait_for_blocked_command();
    pool.stop();
    bool cancelled = false;
    try {
        pending.get();
    } catch (const ruvia::redis_error&) {
        cancelled = true;
    }
    pool.join();
    RUVIA_CHECK(cancelled);
}

RUVIA_TEST(redis_client_close_from_another_thread_cancels_pending_commands) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    auto pending = pool.loop(0).start(blocked_command(client));
    peer.wait_for_blocked_command();
    client.close();
    bool cancelled = false;
    try {
        pending.get();
    } catch (const ruvia::redis_error&) {
        cancelled = true;
    }
    pool.loop(0).start(client.shutdown()).get();
    pool.stop();
    pool.join();
    RUVIA_CHECK(cancelled);
}

RUVIA_TEST(redis_client_event_loop_stop_awaits_retirement_of_pending_authentication) {
    for (const bool use_attachment_run : {false, true}) {
        redis_peer peer;
        auto config = peer.config();
        config.password_ = "stall-authentication";
        asio::io_context io;
        auto attachment = ruvia::attach_event_loop(io);
        ruvia::redis_client client(attachment.loop(), config);
        bool cancelled = false;
        auto root = attachment.loop().start(connect_until_stopped(client, cancelled));
        std::thread driver([&] {
            if (use_attachment_run) {
                attachment.run();
            } else {
                io.run();
            }
        });
        peer.wait_for_blocked_command();
        attachment.stop();
        driver.join();
        root.get();
        RUVIA_CHECK(cancelled);
        RUVIA_CHECK(!client.worker().accepting());
    }
}

RUVIA_TEST(redis_client_shutdown_joins_an_inflight_connect) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.password_ = "stall-authentication";
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    auto connecting = pool.loop(0).start(client.connect());
    peer.wait_for_blocked_command();
    bool duplicate_rejected = false;
    try {
        pool.loop(0).start(client.connect()).get();
    } catch (const std::logic_error&) {
        duplicate_rejected = true;
    }
    RUVIA_CHECK(duplicate_rejected);
    auto closing = pool.loop(0).start(client.shutdown());
    bool cancelled = false;
    try {
        connecting.get();
    } catch (const ruvia::redis_error&) {
        cancelled = true;
    }
    closing.get();
    pool.join();
    RUVIA_CHECK(cancelled);
}

RUVIA_TEST(redis_client_fresh_close_and_loop_stop_share_worker_completion) {
    for (int iteration = 0; iteration < 64; ++iteration) {
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::redis_client client(pool.loop(0), {.pool_size_per_worker_ = 1});
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

RUVIA_TEST(redis_client_cold_connect_can_be_discarded_without_starting_the_pool) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    {
        ruvia::redis_client client(pool.loop(0), {.host_ = "127.0.0.1"});
        auto discarded = client.connect();
        (void)discarded;
    }
    pool.join();
}

RUVIA_TEST(redis_client_failed_connect_can_be_shutdown) {
    asio::io_context io;
    // Bound but not listening: no other process can claim this port.
    asio::ip::tcp::acceptor unavailable(io);
    unavailable.open(asio::ip::tcp::v4());
    unavailable.bind({asio::ip::tcp::v4(), 0});
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), {.host_ = "127.0.0.1",
                                                 .port_ = unavailable.local_endpoint().port(),
                                                 .pool_size_per_worker_ = 1,
                                                 .connect_timeout_ = std::chrono::seconds(1)});
    pool.start();
    bool failed = false;
    try {
        pool.loop(0).start(client.connect()).get();
    } catch (const ruvia::redis_error&) {
        failed = true;
    }
    pool.loop(0).start(client.shutdown()).get();
    pool.join();
    RUVIA_CHECK(failed);
}

RUVIA_TEST(redis_tls_authenticates_identity_and_preserves_operation_memory) {
    ruvia::test::tls_identity identity("redis.test");
    redis_peer peer(redis_peer::mode_type::normal, &identity.context_);
    auto config = peer.config();
    config.tls_ = {.ca_file_ = identity.ca_file_.string(), .server_name_ = "redis.test"};
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    pool.loop(0).start(check_runtime_memory(pool.loop(0), config, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_tls_rejects_untrusted_and_wrong_name_before_authentication) {
    ruvia::test::tls_identity identity("redis.test");
    for (const bool trust_ca : {false, true}) {
        redis_peer peer(redis_peer::mode_type::normal, &identity.context_);
        auto config = peer.config();
        config.password_ = "must-not-send";
        config.tls_ = {.ca_file_ = trust_ca ? identity.ca_file_.string() : "", .server_name_ = trust_ca ? "wrong.test" : "redis.test"};
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::redis_client client(pool.loop(0), config);
        pool.start();
        bool rejected = false;
        try {
            pool.loop(0).start(client.connect()).get();
        } catch (const ruvia::redis_error& error) {
            rejected = error.code() == ruvia::redis_error::code_type::connect_failed;
        }
        RUVIA_CHECK(rejected);
        pool.loop(0).start(client.shutdown()).get();
        pool.join();
    }
}

RUVIA_TEST(redis_tls_shutdown_joins_pending_authenticated_transport_io) {
    ruvia::test::tls_identity identity("redis.test");
    redis_peer peer(redis_peer::mode_type::normal, &identity.context_);
    auto config = peer.config();
    config.tls_ = {.ca_file_ = identity.ca_file_.string(), .server_name_ = "redis.test"};
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), config);
    {
        auto cold = client.connect();
    }
    pool.start();
    auto pending = pool.loop(0).start(blocked_command(client));
    peer.wait_for_blocked_command();
    client.close();
    bool cancelled = false;
    try {
        pending.get();
    } catch (const ruvia::redis_error&) {
        cancelled = true;
    }
    pool.loop(0).start(client.shutdown()).get();
    pool.join();
    RUVIA_CHECK(cancelled);
}

#ifdef RUVIA_ENABLE_DATABASE
RUVIA_TEST(db_query_cache_reuses_connected_redis_and_leaves_its_pool_open) {
    using access = ruvia::detail::db_result_access;
    auto* resource = std::pmr::get_default_resource();
    auto rows = access::make_result(resource);
    auto row = access::owned_row(resource);
    access::owned_column_names(row).emplace_back("name");
    access::owned_fields(row).push_back(access::owned_field("cached", resource));
    access::rows(rows).push_back(std::move(row));
    auto bytes_value = ruvia::detail::encode_db_cache_rows(rows, resource);
    redis_peer peer(redis_peer::mode_type::query_cache, nullptr, std::string(bytes_value));
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto loop = pool.loop(0);
    ruvia::redis_client redis(loop, peer.config());
    auto run = [&]() -> ruvia::task<void> {
        co_await redis.connect();
        auto store_value = redis.with_options({});
#ifdef RUVIA_ENABLE_POSTGRESQL
        ruvia::db_config config{.driver_ = ruvia::db_driver::postgresql};
#else
        ruvia::db_config config{.driver_ = ruvia::db_driver::mariadb};
#endif
        ruvia::db_client client(loop, config, store_value, ruvia::db_cache_config{});
        ruvia::test::counting_memory_resource operation_memory;
        {
            ruvia::detail::db_registry databases(loop.io_context(), redis.worker(),
                &operation_memory, config, store_value, ruvia::db_cache_config{});
            ruvia::operation_scope scope;
            auto database_value = databases.get(scope);
            ruvia::db_query query;
            query.select(query.column("name")).from("items").cache(true);
            const auto baseline = operation_memory.live_allocations();
            for (int index = 0; index < 3; ++index) {
                {
                    const auto result_value = co_await database_value.query(query);
                    RUVIA_CHECK_EQ(result_value.size(), std::size_t{1});
                    RUVIA_CHECK_EQ(result_value[0]["name"].as<std::string_view>().value(), std::string_view("cached"));
                }
                RUVIA_CHECK_EQ(operation_memory.live_allocations(), baseline);
            }
            databases.close_now();
            co_await redis.ping();
        }
        RUVIA_CHECK_EQ(operation_memory.live_allocations(), std::size_t{0});
        co_await client.shutdown();
        co_await redis.ping();
        co_await redis.shutdown();
    };
    pool.start();
    loop.start(run()).get();
    pool.join();
}
#endif
