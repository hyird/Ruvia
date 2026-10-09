#include <array>
#include <chrono>
#include <concepts>
#include <exception>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/app.h"
#include "ruvia/web/redis/redis_handle.h"

#include "memory_resource_fixture.h"
#include "redis/redis_handle_helpers.h"
#include "redis/redis_registry.h"
#include "redis/redis_types_access.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using ruvia::test::rejecting_memory_resource;
using ruvia::test::tracking_resource;

using redis_definitions_type = std::span<const ruvia::detail::redis_definition_type>;

class redis_test_worker final {
public:
    explicit redis_test_worker(asio::io_context& io_context)
        : io_context_(io_context),
          attachment_(ruvia::attach_event_loop(io_context)),
          handle_(attachment_.loop().handle()) {}

    redis_test_worker(const redis_test_worker&) = delete;
    redis_test_worker& operator=(const redis_test_worker&) = delete;

    [[nodiscard]] const ruvia::worker_handle& handle() const noexcept {
        return handle_;
    }

    void run() {
        attachment_.run();
    }

    void stop() noexcept {
        io_context_.stop();
        attachment_.stop();
    }

private:
    asio::io_context& io_context_;
    ruvia::event_loop_attachment attachment_;
    ruvia::worker_handle handle_;
};

[[nodiscard]] ruvia::detail::redis_definition_type redis_definition(std::string_view alias,
    const ruvia::redis_config& config = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    return {
        std::pmr::string(alias, resource),
        ruvia::detail::redis_config_storage(config, resource),
    };
}

class stalled_redis_command_server final {
public:
    stalled_redis_command_server()
        : io_context_(ruvia::test::new_test_io_context()),
          acceptor_(io_context_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          port_(acceptor_.local_endpoint().port()),
          command_read_future_(command_read_.get_future()),
          thread_([this] { run(); }) {}

    ~stalled_redis_command_server() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    stalled_redis_command_server(const stalled_redis_command_server&) = delete;
    stalled_redis_command_server& operator=(const stalled_redis_command_server&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

    void wait_until_command_read() {
        command_read_future_.get();
    }

private:
    void run() noexcept {
        try {
            asio::ip::tcp::socket socket(io_context_);
            acceptor_.accept(socket);

            constexpr std::string_view ping = "*1\r\n$4\r\nPING\r\n";
            std::array<char, ping.size()> command{};
            std::error_code error;
            (void)asio::read(socket, asio::buffer(command), error);
            if (error) {
                throw std::system_error(error);
            }
            command_read_.set_value();

            std::array<char, 1> ignored_byte{};
            (void)socket.read_some(asio::buffer(ignored_byte), error);
        } catch (...) {
            try {
                command_read_.set_exception(std::current_exception());
            } catch (...) {
            }
        }
    }

    asio::io_context& io_context_;
    asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::promise<void> command_read_;
    std::future<void> command_read_future_;
    std::thread thread_;
};

template <typename fn_type>
bool throws_invalid_argument(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(
    redis_blocking_commands_ignore_the_ordinary_pool_timeout_and_require_a_cancellation_bound) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::redis_config config;
    config.command_timeout_ = std::chrono::milliseconds(1);
    const std::array definitions{redis_definition("default", config)};
    ruvia::detail::redis_registry registry(
        io_context, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope general_scope;
    auto redis = registry.get(general_scope);
    const std::array<std::string_view, 1> keys{"queue"};
    const std::array streams{ruvia::redis_stream_read_view{.stream_ = "events", .id_ = ">"}};

    bool finite_pop_accepted = true;
    bool finite_stream_accepted = true;
    bool finite_raw_accepted = true;
    bool stateful_rejected = false;
    bool client_state_rejected = false;
    bool hello_rejected = false;
    bool asking_rejected = false;
    try {
        (void)redis.blpop(keys, ruvia::redis_block_wait::for_duration(std::chrono::seconds(1)));
    } catch (...) {
        finite_pop_accepted = false;
    }
    try {
        (void)redis.xread_group("workers", "consumer", streams,
            {.block_ = ruvia::redis_block_wait::for_duration(std::chrono::milliseconds(10))});
    } catch (...) {
        finite_stream_accepted = false;
    }
    try {
        (void)redis.with_options({.timeout_ = std::chrono::seconds(1)})
            .command("BLPOP", "queue", "1");
    } catch (...) {
        finite_raw_accepted = false;
    }
    try {
        (void)redis.command("SELECT", "1");
    } catch (const std::invalid_argument&) {
        stateful_rejected = true;
    }
    try {
        (void)redis.command("CLIENT", "REPLY", "OFF");
    } catch (const std::invalid_argument&) {
        client_state_rejected = true;
    }
    try {
        (void)redis.command("HELLO", "3");
    } catch (const std::invalid_argument&) {
        hello_rejected = true;
    }
    try {
        (void)redis.command("ASKING");
    } catch (const std::invalid_argument&) {
        asking_rejected = true;
    }

    bool infinite_stream_rejected = false;
    bool infinite_pop_rejected = false;
    bool unbounded_raw_rejected = false;
    try {
        (void)redis.xread_group(
            "workers", "consumer", streams, {.block_ = ruvia::redis_block_wait::indefinitely()});
    } catch (const std::invalid_argument&) {
        infinite_stream_rejected = true;
    }
    try {
        (void)redis.blpop(keys, ruvia::redis_block_wait::indefinitely());
    } catch (const std::invalid_argument&) {
        infinite_pop_rejected = true;
    }
    try {
        (void)redis.command("BLPOP", "queue", "0");
    } catch (const std::invalid_argument&) {
        unbounded_raw_rejected = true;
    }

    RUVIA_CHECK(finite_pop_accepted);
    RUVIA_CHECK(finite_stream_accepted);
    RUVIA_CHECK(finite_raw_accepted);
    RUVIA_CHECK(stateful_rejected);
    RUVIA_CHECK(client_state_rejected);
    RUVIA_CHECK(hello_rejected);
    RUVIA_CHECK(asking_rejected);
    RUVIA_CHECK(infinite_stream_rejected);
    RUVIA_CHECK(infinite_pop_rejected);
    RUVIA_CHECK(unbounded_raw_rejected);
}

RUVIA_TEST(redis_registry_derives_default_pool_from_owned_entry_index) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    const std::array<ruvia::detail::redis_definition_type, 2> definitions{{
        redis_definition("cache"),
        redis_definition("default"),
    }};
    ruvia::detail::redis_registry registry(
        io_context, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope operation_scope;

    bool default_resolved = true;
    bool alias_resolved = true;
    try {
        (void)registry.get(operation_scope);
    } catch (...) {
        default_resolved = false;
    }
    try {
        (void)registry.get("cache", operation_scope);
    } catch (...) {
        alias_resolved = false;
    }
    RUVIA_CHECK(default_resolved);
    RUVIA_CHECK(alias_resolved);
}

RUVIA_TEST(redis_registry_rejects_an_invalid_worker) {
    auto& io_context = ruvia::test::new_test_io_context();
    const redis_definitions_type definitions;
    const ruvia::worker_handle worker;

    RUVIA_CHECK(throws_invalid_argument([&] {
        ruvia::detail::redis_registry registry(
            io_context, std::pmr::get_default_resource(), definitions, worker);
    }));
}

RUVIA_TEST(redis_registry_owns_nested_pmr_configuration) {
    tracking_resource source_resource;
    std::pmr::unsynchronized_pool_resource target_resource;
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    std::optional<ruvia::detail::redis_definition_type> definition;
    ruvia::redis_config config{
        .host_ = std::string(80, 'h'),
        .port_ = 6379,
        .username_ = std::string(80, 'u'),
        .password_ = std::string(80, 'p'),
        .database_ = 0,
        .pool_size_per_worker_ = 1,
    };
    definition.emplace(redis_definition("default", config, &source_resource));

    std::optional<ruvia::detail::redis_registry> registry;
    registry.emplace(io_context, &target_resource,
        std::span<const ruvia::detail::redis_definition_type>(&*definition, 1), worker.handle());
    definition.reset();
    source_resource.release();
    registry.reset();

    RUVIA_CHECK(!source_resource.deallocated_after_release());
}

RUVIA_TEST(redis_request_capabilities_reject_after_parent_scope_closes) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    const std::array definitions{redis_definition("default")};
    ruvia::detail::redis_registry registry(
        io_context, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope operation_scope;
    auto handle = registry.get(operation_scope);
    auto copied_handle = handle;
    auto configured_handle = handle.with_options({.timeout_ = std::chrono::seconds(3)});
    auto copied_configured_handle = configured_handle;
    auto derived_configured_handle = copied_configured_handle.with_options(
        {.timeout_ = std::chrono::seconds(1)});
    auto pipeline = handle.pipeline();
    pipeline.get("key");
    auto moved_pipeline = std::move(pipeline);
    auto transaction = handle.transaction();
    transaction.get("key");
    auto moved_transaction = std::move(transaction);

    operation_scope.close();
    bool handle_rejected = false;
    bool copy_rejected = false;
    bool builder_rejected = false;
    bool transaction_rejected = false;
    const auto option_failure_order = [](const auto& capability) {
        bool validation_wrong_order = false;
        bool lifetime_rejected = false;
        try {
            (void)capability.with_options({.timeout_ = std::chrono::milliseconds::zero()});
        } catch (const std::invalid_argument&) {
            validation_wrong_order = true;
        } catch (const std::logic_error&) {
            lifetime_rejected = true;
        }
        return std::pair{validation_wrong_order, lifetime_rejected};
    };
    const auto handle_option_failure = option_failure_order(handle);
    const auto configured_option_failure = option_failure_order(configured_handle);
    const auto copied_configured_option_failure = option_failure_order(copied_configured_handle);
    const auto derived_configured_option_failure = option_failure_order(derived_configured_handle);
    const bool expired_options_rejected_before_validation =
        !handle_option_failure.first && handle_option_failure.second &&
        !configured_option_failure.first && configured_option_failure.second &&
        !copied_configured_option_failure.first && copied_configured_option_failure.second &&
        !derived_configured_option_failure.first && derived_configured_option_failure.second;
    try {
        (void)handle.ping();
    } catch (const std::logic_error&) {
        handle_rejected = true;
    }
    try {
        (void)copied_handle.ping();
    } catch (const std::logic_error&) {
        copy_rejected = true;
    }
    try {
        moved_pipeline.get("other");
    } catch (const std::logic_error&) {
        builder_rejected = true;
    }
    try {
        moved_transaction.get("other");
    } catch (const std::logic_error&) {
        transaction_rejected = true;
    }
    RUVIA_CHECK(expired_options_rejected_before_validation);
    RUVIA_CHECK(handle_rejected);
    RUVIA_CHECK(copy_rejected);
    RUVIA_CHECK(builder_rejected);
    RUVIA_CHECK(transaction_rejected);
}

RUVIA_TEST(redis_batch_builders_own_cold_payload_and_reject_reuse) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::counting_memory_resource memory;
    const std::array definitions{redis_definition("default")};
    ruvia::detail::redis_registry registry(io_context, &memory, definitions, worker.handle());
    ruvia::operation_scope scope;
    auto handle = registry.get(scope);
    const auto baseline = memory.live_allocations();
    const auto throws_logic_error = [](auto&& operation) {
        try {
            operation();
        } catch (const std::logic_error&) {
            return true;
        }
        return false;
    };
    {
        auto pipeline = handle.pipeline();
        pipeline.set(std::string(128, 'k'), std::string(128, 'v'));
        std::optional moved(std::move(pipeline));
        RUVIA_CHECK(throws_logic_error([&] { pipeline.get("moved"); }));
        auto cold = std::move(*moved).exec();
        RUVIA_CHECK(throws_logic_error([&] { moved->incr_by("used", 1); }));
        RUVIA_CHECK(throws_logic_error([&] { (void)std::move(*moved).exec(); }));
        moved.reset();
        RUVIA_CHECK(memory.live_allocations() > baseline);
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
    {
        auto transaction = handle.transaction();
        transaction.watch(std::string(128, 'w')).unwatch().set(std::string(128, 'k'), std::string(128, 'v'));
        std::optional moved(std::move(transaction));
        RUVIA_CHECK(throws_logic_error([&] { transaction.watch("moved"); }));
        auto cold = std::move(*moved).exec();
        RUVIA_CHECK(throws_logic_error([&] { moved->unwatch(); }));
        RUVIA_CHECK(throws_logic_error([&] { moved->zadd("used", 1, "member"); }));
        moved.reset();
        RUVIA_CHECK(memory.live_allocations() > baseline);
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
    {
        auto pipeline = handle.pipeline();
        auto transaction = handle.transaction();
        for (auto word : {"WATCH", "UNWATCH", "MULTI", "EXEC", "BLPOP"}) {
            RUVIA_CHECK(throws_invalid_argument([&] { pipeline.command(word, "key"); }));
            RUVIA_CHECK(throws_invalid_argument([&] { transaction.command(word, "key"); }));
        }
        for (auto score : {std::numeric_limits<double>::infinity(),
                 -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            RUVIA_CHECK(throws_invalid_argument([&] { pipeline.zadd("key", score, "member"); }));
            RUVIA_CHECK(throws_invalid_argument([&] { transaction.zadd("key", score, "member"); }));
        }
        pipeline.set(std::string(128, 'k'), std::string(128, 'v'));
        transaction.watch(std::string(128, 'w')).set(std::string(128, 'k'), std::string(128, 'v'));
        scope.close();
        RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
        RUVIA_CHECK(throws_logic_error([&] {
            pipeline.zadd("expired", std::numeric_limits<double>::quiet_NaN(), "member");
        }));
        RUVIA_CHECK(throws_logic_error([&] { (void)std::move(transaction).exec(); }));
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
}

RUVIA_TEST(redis_batch_typed_commands_preserve_order_arguments_and_resource) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::counting_memory_resource memory;
    const ruvia::detail::redis_config_storage config(ruvia::redis_config{}, &memory);
    ruvia::detail::redis_pool pool(io_context, config, config.command_timeout_, 1, worker.handle(), &memory);
    const auto baseline = memory.live_allocations();
    {
        ruvia::detail::redis_command_batch batch(pool, {}, &memory);
        const auto minimum = std::numeric_limits<std::int64_t>::min();
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        batch.get("k");
        batch.set("k", "v");
        batch.get_del("k");
        batch.append("k", "v");
        batch.strlen("k");
        batch.del("k");
        batch.unlink("k");
        batch.exists("k");
        batch.touch("k");
        batch.type("k");
        batch.rename("k", "n");
        batch.rename_nx("k", "n");
        batch.incr("k");
        batch.incr_by("k", minimum);
        batch.decr("k");
        batch.decr_by("k", maximum);
        batch.hget("k", "f");
        batch.hset("k", "f", "v");
        batch.hdel("k", "f");
        batch.hexists("k", "f");
        batch.hlen("k");
        batch.hget_all("k");
        batch.lpush("k", "v");
        batch.rpush("k", "v");
        batch.lpop("k");
        batch.rpop("k");
        batch.llen("k");
        batch.lrange("k", minimum, maximum);
        batch.sadd("k", "m");
        batch.srem("k", "m");
        batch.smembers("k");
        batch.scard("k");
        batch.zadd("k", 1.5, "m");
        batch.zrem("k", "m");
        batch.zrange("k", minimum, maximum);
        batch.zscore("k", "m");
        batch.zcard("k");
        const std::string binary_key("k\0ey", 4);
        const std::string binary_value("v\0alue", 6);
        auto source_key = binary_key;
        auto source_value = binary_value;
        batch.set(source_key, source_value);
        source_key.assign(128, 'x');
        source_value.assign(128, 'y');
        auto moved = std::move(batch);
        auto payload_value = moved.consume();
        RUVIA_CHECK_EQ(payload_value.commands_.size(), std::size_t{38});
        RUVIA_CHECK(payload_value.commands_.get_allocator().resource() == &memory);
        const std::initializer_list<std::initializer_list<std::string_view>> expected{
            {"GET", "k"},
            {"SET", "k", "v"},
            {"GETDEL", "k"},
            {"APPEND", "k", "v"},
            {"STRLEN", "k"},
            {"DEL", "k"},
            {"UNLINK", "k"},
            {"EXISTS", "k"},
            {"TOUCH", "k"},
            {"TYPE", "k"},
            {"RENAME", "k", "n"},
            {"RENAMENX", "k", "n"},
            {"INCR", "k"},
            {"INCRBY", "k", "-9223372036854775808"},
            {"DECR", "k"},
            {"DECRBY", "k", "9223372036854775807"},
            {"HGET", "k", "f"},
            {"HSET", "k", "f", "v"},
            {"HDEL", "k", "f"},
            {"HEXISTS", "k", "f"},
            {"HLEN", "k"},
            {"HGETALL", "k"},
            {"LPUSH", "k", "v"},
            {"RPUSH", "k", "v"},
            {"LPOP", "k"},
            {"RPOP", "k"},
            {"LLEN", "k"},
            {"LRANGE", "k", "-9223372036854775808", "9223372036854775807"},
            {"SADD", "k", "m"},
            {"SREM", "k", "m"},
            {"SMEMBERS", "k"},
            {"SCARD", "k"},
            {"ZADD", "k", "1.5", "m"},
            {"ZREM", "k", "m"},
            {"ZRANGE", "k", "-9223372036854775808", "9223372036854775807"},
            {"ZSCORE", "k", "m"},
            {"ZCARD", "k"},
        };
        std::size_t command_index = 0;
        for (auto arguments : expected) {
            const auto& command = payload_value.commands_[command_index++];
            RUVIA_CHECK_EQ(command.args_.size(), arguments.size());
            std::size_t argument_index = 0;
            for (auto argument : arguments) {
                const auto& actual = command.args_[argument_index++];
                RUVIA_CHECK(actual == argument);
                RUVIA_CHECK(actual.get_allocator().resource() == &memory);
            }
        }
        RUVIA_CHECK(payload_value.commands_.back().args_[1] == std::string_view(binary_key));
        RUVIA_CHECK(payload_value.commands_.back().args_[2] == std::string_view(binary_value));
        moved.expire();
        RUVIA_CHECK(payload_value.commands_.back().args_[2] == std::string_view(binary_value));
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
}

RUVIA_TEST(redis_set_expiration_cannot_represent_conflicting_modes) {
    const auto expiring = ruvia::redis_set_expiration::expires_after(std::chrono::milliseconds(1500));
    RUVIA_CHECK(expiring.duration() != nullptr);
    RUVIA_CHECK_EQ(expiring.duration()->count(), std::chrono::milliseconds::rep{1500});
    RUVIA_CHECK(!expiring.keeps_existing());

    const auto keep = ruvia::redis_set_expiration::keep_existing();
    RUVIA_CHECK(keep.duration() == nullptr);
    RUVIA_CHECK(keep.keeps_existing());

    bool zero_rejected = false;
    try {
        (void)ruvia::redis_set_expiration::expires_after(std::chrono::milliseconds(0));
    } catch (const std::invalid_argument&) {
        zero_rejected = true;
    }
    RUVIA_CHECK(zero_rejected);

    bool negative_rejected = false;
    try {
        (void)ruvia::redis_set_expiration::expires_after(std::chrono::milliseconds(-1));
    } catch (const std::invalid_argument&) {
        negative_rejected = true;
    }
    RUVIA_CHECK(negative_rejected);
}

RUVIA_TEST(redis_expire_rejects_non_positive_ttl_before_io) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    const std::array definitions{redis_definition("default")};
    ruvia::detail::redis_registry registry(
        io_context, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope operation_scope;
    auto redis = registry.get(operation_scope);

    bool zero_rejected = false;
    try {
        (void)redis.expire("key", std::chrono::seconds(0));
    } catch (const std::invalid_argument&) {
        zero_rejected = true;
    }
    RUVIA_CHECK(zero_rejected);

    bool negative_rejected = false;
    try {
        (void)redis.expire("key", std::chrono::seconds(-1));
    } catch (const std::invalid_argument&) {
        negative_rejected = true;
    }
    RUVIA_CHECK(negative_rejected);
}

RUVIA_TEST(redis_multi_key_commands_reject_empty_key_spans_before_io) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    const std::array definitions{redis_definition("default")};
    ruvia::detail::redis_registry registry(
        io_context, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope operation_scope;
    auto redis = registry.get(operation_scope);
    const std::span<const std::string_view> no_keys;

    RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.mget(no_keys); }));
    RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.sinter(no_keys); }));
    RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.sunion(no_keys); }));
    RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.sdiff(no_keys); }));
}

RUVIA_TEST(redis_transaction_errors_preserve_server_diagnostics) {
    const auto reply = ruvia::detail::redis_types_access::error_value(
        "EXECABORT Transaction discarded because of previous errors.",
        std::pmr::get_default_resource());
    bool preserved = false;
    try {
        ruvia::detail::throw_if_redis_transaction_reply_error(reply, 3);
    } catch (const ruvia::redis_error& error) {
        preserved = error.code() == ruvia::redis_error::code_type::command_error &&
                    (std::string_view(error.what()).find("reply 3") != std::string_view::npos) &&
                    (std::string_view(error.what()).find("EXECABORT") != std::string_view::npos);
    }
    RUVIA_CHECK(preserved);
}

RUVIA_TEST(redis_active_command_reports_pool_closing_instead_of_io_error) {
    stalled_redis_command_server server;
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    auto config = ruvia::redis_config{};
    config.host_ = "127.0.0.1";
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    config.port_ = server.port();
    config.command_timeout_ = std::nullopt;
    auto* const resource = std::pmr::get_default_resource();
    const auto stored_config = ruvia::detail::redis_config_storage(config, resource);
    ruvia::detail::redis_pool pool(
        io_context, stored_config, stored_config.command_timeout_, 1, worker.handle(), resource);

    auto exercise = [&]() -> ruvia::task<ruvia::redis_error::code_type> {
        std::pmr::vector<std::pmr::string> args(resource);
        args.emplace_back("PING");
        try {
            (void)co_await pool.execute_owned(std::move(args), resource);
        } catch (const ruvia::redis_error& error) {
            co_return error.code();
        }
        co_return ruvia::redis_error::code_type::protocol_error;
    };

    std::promise<ruvia::redis_error::code_type> completion;
    auto result_value = completion.get_future();
    asio::co_spawn(io_context, ruvia::as_awaitable(exercise()),
        [&worker, &completion](std::exception_ptr error, ruvia::redis_error::code_type code) {
            if (error) {
                completion.set_exception(std::move(error));
            } else {
                completion.set_value(code);
            }
            worker.stop();
        });
    std::jthread runner([&worker] { worker.run(); });
    server.wait_until_command_read();
    asio::post(io_context, [&pool] { pool.close_now(); });

    runner.join();
    const auto code = result_value.get();
    RUVIA_CHECK(code == ruvia::redis_error::code_type::closing);
}

RUVIA_TEST(redis_operation_arguments_are_reclaimed_after_cancellation_and_failure) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    const std::array definitions{redis_definition("default")};
    ruvia::test::counting_memory_resource operation_memory;
    ruvia::detail::redis_registry registry(
        io_context, &operation_memory, definitions, worker.handle());
    const auto registry_live_allocations = operation_memory.live_allocations();
    ruvia::operation_scope operation_scope;
    auto redis = registry.get(operation_scope);
    ruvia::stop_source cancellation;
    cancellation.request_stop();
    auto cancelled = redis.with_options({.stop_token_ = cancellation.token()});
    const std::string key(2048, 'k');

    auto exercise = [&]() -> ruvia::task<void> {
        for (int index = 0; index != 128; ++index) {
            bool rejected = false;
            try {
                (void)co_await cancelled.get(key);
            } catch (const ruvia::redis_error& error) {
                rejected = error.code() == ruvia::redis_error::code_type::cancelled;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(operation_memory.live_allocations(), registry_live_allocations);
        }
        registry.close_now();
        for (int index = 0; index != 128; ++index) {
            bool rejected = false;
            try {
                (void)co_await redis.get(key);
            } catch (const ruvia::redis_error& error) {
                rejected = error.code() == ruvia::redis_error::code_type::closing;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(operation_memory.live_allocations(), registry_live_allocations);
        }
    };
    std::promise<void> completion;
    auto result_value = completion.get_future();
    asio::co_spawn(io_context, ruvia::as_awaitable(exercise()),
        [&worker, &completion](std::exception_ptr error) {
            if (error) {
                completion.set_exception(std::move(error));
            } else {
                completion.set_value();
            }
            worker.stop();
        });
    worker.run();
    result_value.get();

    RUVIA_CHECK(operation_memory.allocation_count() > 0);
    RUVIA_CHECK_EQ(operation_memory.live_allocations(), registry_live_allocations);
}

RUVIA_TEST(redis_value_move_assignment_propagates_allocator_failure) {
    rejecting_memory_resource rejecting;
    const auto long_value =
        std::string_view("redis value large enough to exceed any small-string buffer");

    auto destination = ruvia::detail::redis_types_access::key_value({}, {}, &rejecting);
    auto source_value = ruvia::detail::redis_types_access::key_value(
        long_value, long_value, std::pmr::get_default_resource());
    rejecting.reject_allocations();
    bool allocation_failure = false;
    try {
        destination = std::move(source_value);
    } catch (const std::bad_alloc&) {
        allocation_failure = true;
    }
    RUVIA_CHECK(allocation_failure);

    rejecting.reject_allocations(false);
    auto destination_value = ruvia::detail::redis_types_access::null_value(&rejecting);
    auto source_string_value =
        ruvia::detail::redis_types_access::string_value(long_value, std::pmr::get_default_resource());
    rejecting.reject_allocations();
    allocation_failure = false;
    try {
        destination_value = std::move(source_string_value);
    } catch (const std::bad_alloc&) {
        allocation_failure = true;
    }
    RUVIA_CHECK(allocation_failure);
}

RUVIA_TEST(redis_value_array_move_assignment_uses_destination_resource) {
    ruvia::test::counting_memory_resource source_resource;
    ruvia::test::counting_memory_resource destination_resource;
    const std::string value(128, 'v');
    {
        auto destination = ruvia::detail::redis_types_access::null_value(&destination_resource);
        {
            std::pmr::vector<ruvia::redis_value> values(&source_resource);
            values.push_back(ruvia::detail::redis_types_access::string_value(value, &source_resource));
            auto source_value = ruvia::detail::redis_types_access::array_value(std::move(values), &source_resource);
            destination = std::move(source_value);
        }
        RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(destination.array().size(), std::size_t{1});
        if (destination.array().size() == 1) {
            RUVIA_CHECK_EQ(destination.array().front().string(), std::string_view(value));
        }
    }
    RUVIA_CHECK_EQ(destination_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
}

namespace {
template <typename factory_type, typename populate_type, typename check_type>
void check_redis_resource_transfer(ruvia::testing::test_context& ruvia_ctx,
    factory_type factory, populate_type populate, check_type check) {
    for (const bool copy : {false, true}) {
        ruvia::test::counting_memory_resource source_resource;
        ruvia::test::counting_memory_resource destination_resource;
        {
            auto destination = factory(&destination_resource);
            {
                auto source_value = factory(&source_resource);
                populate(source_value, &source_resource);
                if (copy) {
                    destination = source_value;
                } else {
                    destination = std::move(source_value);
                }
            }
            RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
            check(destination);
        }
        RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(destination_resource.live_allocations(), std::size_t{0});
    }
}
}  // namespace

RUVIA_TEST(redis_typed_result_transfers_use_destination_resource) {
    using access_type = ruvia::detail::redis_types_access;
    const std::string key(128, 'k');
    const std::string value(128, 'v');
    check_redis_resource_transfer(ruvia_ctx, [](auto* resource) { return access_type::hash_scan_result(resource); }, [&](auto& result_value, auto* resource) { access_type::entries(result_value).push_back(access_type::key_value(key, value, resource)); }, [&](const auto& result_value) {
            RUVIA_CHECK_EQ(result_value.entries().size(), std::size_t{1});
            if (result_value.entries().size() == 1) {
                RUVIA_CHECK_EQ(result_value.entries().front().key(), std::string_view(key));
                RUVIA_CHECK_EQ(result_value.entries().front().value(), std::string_view(value));
            } });
    check_redis_resource_transfer(ruvia_ctx, [](auto* resource) { return access_type::z_scan_result(resource); }, [&](auto& result_value, auto* resource) { access_type::entries(result_value).push_back(access_type::scored_value(value, 2.5, resource)); }, [&](const auto& result_value) {
            RUVIA_CHECK_EQ(result_value.entries().size(), std::size_t{1});
            if (result_value.entries().size() == 1) {
                RUVIA_CHECK_EQ(result_value.entries().front().value(), std::string_view(value));
                RUVIA_CHECK_EQ(result_value.entries().front().score(), 2.5);
            } });
    check_redis_resource_transfer(ruvia_ctx, [](auto* resource) { return access_type::xread_group_result(resource); }, [&](auto& result_value, auto* resource) {
            auto stream = access_type::stream_read_result(key, resource);
            auto entry_value = access_type::stream_entry(value, resource);
            access_type::fields(entry_value).push_back(access_type::key_value(key, value, resource));
            access_type::entries(stream).push_back(std::move(entry_value));
            access_type::streams(result_value).push_back(std::move(stream)); }, [&](const auto& result_value) {
            RUVIA_CHECK_EQ(result_value.streams().size(), std::size_t{1});
            if (result_value.streams().size() != 1) {
                return;
            }
            const auto& stream = result_value.streams().front();
            RUVIA_CHECK_EQ(stream.stream(), std::string_view(key));
            RUVIA_CHECK_EQ(stream.entries().size(), std::size_t{1});
            if (stream.entries().size() != 1) {
                return;
            }
            const auto& entry_value = stream.entries().front();
            RUVIA_CHECK_EQ(entry_value.id(), std::string_view(value));
            RUVIA_CHECK_EQ(entry_value.fields().size(), std::size_t{1});
            if (entry_value.fields().size() == 1) {
                RUVIA_CHECK_EQ(entry_value.fields().front().key(), std::string_view(key));
                RUVIA_CHECK_EQ(entry_value.fields().front().value(), std::string_view(value));
            } });
}
