#include <array>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/redis/redis_handle.h"

#include "backend_client_fixture.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace redis_client_test {

template <typename fn_type>
bool throws_invalid_argument(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

RUVIA_TEST(redis_blocking_commands_ignore_the_ordinary_pool_timeout_and_honor_operation_cancellation) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    ruvia::redis_config config;
    config.command_timeout_ = std::chrono::milliseconds(1);
    ruvia::test::with_connected_redis_client(worker, config, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        auto redis = client.handle();
        const std::array<std::string_view, 1> keys{"queue"};
        const std::array streams{ruvia::redis_stream_read_view{.stream_ = "events", .id_ = ">"}};
        const auto timeout = std::chrono::milliseconds(20);
        auto bounded = redis.with_options({.timeout_ = timeout});

        // The startup peer deliberately sends no command replies. Awaiting each
        // operation must reach its own deadline, not the ordinary pool's 1 ms.
        const auto expect_timeout = [&](auto&& operation) -> ruvia::task<void> {
            bool timed_out = false;
            const auto started = std::chrono::steady_clock::now();
            try {
                (void)co_await std::move(operation);
            } catch (const ruvia::redis_error& error) {
                if (error.code() != ruvia::redis_error::code_type::timeout) {
                    throw;
                }
                timed_out = true;
            }
            RUVIA_CHECK(timed_out);
            RUVIA_CHECK(std::chrono::steady_clock::now() - started >= timeout);
        };
        co_await expect_timeout(bounded.blpop(keys, ruvia::redis_block_wait::for_duration(std::chrono::seconds(1))));
        co_await expect_timeout(bounded.xread_group("workers", "consumer", streams,
            {.block_ = ruvia::redis_block_wait::for_duration(std::chrono::milliseconds(10))}));
        co_await expect_timeout(bounded.command("BLPOP", "queue", "1"));

        for (auto args : {std::initializer_list<std::string_view>{"SELECT", "1"},
                 {"CLIENT", "REPLY", "OFF"}, {"HELLO", "3"}, {"ASKING"}, {"unsubscribe", "a", "b"},
                 {"PUNSUBSCRIBE", "a"}, {"SUNSUBSCRIBE", "a"}}) {
            bool rejected = false;
            try {
                (void)co_await redis.command(std::span<const std::string_view>(args.begin(), args.size()));
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
        }

        // Standalone clients always inherit an owner stop token, so indefinite
        // blocking requests are bounded by cancellation rather than rejected.
        ruvia::stop_source cancellation;
        cancellation.request_stop();
        auto cancelled = redis.with_options({.stop_token_ = cancellation.token()});
        const auto expect_cancelled = [&](auto&& operation) -> ruvia::task<void> {
            bool rejected = false;
            try {
                (void)co_await std::move(operation);
            } catch (const ruvia::redis_error& error) {
                if (error.code() != ruvia::redis_error::code_type::cancelled) {
                    throw;
                }
                rejected = true;
            }
            RUVIA_CHECK(rejected);
        };
        co_await expect_cancelled(cancelled.xread_group(
            "workers", "consumer", streams, {.block_ = ruvia::redis_block_wait::indefinitely()}));
        co_await expect_cancelled(cancelled.blpop(keys, ruvia::redis_block_wait::indefinitely()));
        co_await expect_cancelled(cancelled.command("BLPOP", "queue", "0"));
    });
}

RUVIA_TEST(redis_batch_builders_own_cold_payload_and_reject_reuse) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    ruvia::test::with_connected_redis_client(worker, {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        auto handle = client.handle();
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
        }
        {
            auto transaction = handle.transaction();
            transaction.set(std::string(128, 'k'), std::string(128, 'v'));
            std::optional moved(std::move(transaction));
            RUVIA_CHECK(throws_logic_error([&] { transaction.get("moved"); }));
            auto cold = std::move(*moved).exec();
            RUVIA_CHECK(throws_logic_error([&] { moved->incr_by("used", 1); }));
            RUVIA_CHECK(throws_logic_error([&] { moved->zadd("used", 1, "member"); }));
            moved.reset();
        }
        {
            auto pipeline = handle.pipeline();
            auto transaction = handle.transaction();
            for (auto word : {"WATCH", "UNWATCH", "MULTI", "EXEC", "SYNC", "PSYNC", "REPLCONF", "BLPOP",
                     "UNSUBSCRIBE", "PUNSUBSCRIBE", "SUNSUBSCRIBE"}) {
                RUVIA_CHECK(throws_invalid_argument([&] { pipeline.command(word, "key"); }));
                RUVIA_CHECK(throws_invalid_argument([&] { transaction.command(word, "key"); }));
            }
            // Infinite scores have a Redis spelling; NaN does not.
            pipeline.zadd("key", std::numeric_limits<double>::infinity(), "member");
            transaction.zadd("key", -std::numeric_limits<double>::infinity(), "member");
            RUVIA_CHECK(throws_invalid_argument(
                [&] { pipeline.zadd("key", std::numeric_limits<double>::quiet_NaN(), "member"); }));
            RUVIA_CHECK(throws_invalid_argument(
                [&] { transaction.zadd("key", std::numeric_limits<double>::quiet_NaN(), "member"); }));
        }
        co_return;
    });
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
    auto worker = ruvia::attach_event_loop(io_context);
    ruvia::test::with_connected_redis_client(worker, {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        auto redis = client.handle();

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
        co_return;
    });
}

RUVIA_TEST(redis_multi_key_commands_reject_empty_key_spans_before_io) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    ruvia::test::with_connected_redis_client(worker, {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        auto redis = client.handle();
        const std::span<const std::string_view> no_keys;

        RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.mget(no_keys); }));
        RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.sinter(no_keys); }));
        RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.sunion(no_keys); }));
        RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.sdiff(no_keys); }));
        co_return;
    });
}

// Minimal RESP2 peer: answers each complete command array with the next
// canned reply and records the received arguments.
class scripted_redis_peer final {
public:
    explicit scripted_redis_peer(std::vector<std::string> replies)
        : replies_(std::move(replies)),
          acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          thread_([this] { serve(); }) {}

    ~scripted_redis_peer() {
        join();
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    // Valid only after join().
    [[nodiscard]] const std::vector<std::vector<std::string>>& commands() const noexcept {
        return commands_;
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    // Parses one "*N\r\n($len\r\nbytes\r\n)*N" command from the front of input.
    static bool take_command(std::string& input, std::vector<std::string>& args) {
        std::size_t position = 0;
        const auto read_line = [&](char prefix, std::size_t& number) {
            if (position >= input.size() || input[position] != prefix) {
                return false;
            }
            const auto end = input.find("\r\n", position);
            if (end == std::string::npos) {
                return false;
            }
            number = std::stoul(input.substr(position + 1, end - position - 1));
            position = end + 2;
            return true;
        };
        std::size_t count = 0;
        if (!read_line('*', count)) {
            return false;
        }
        std::vector<std::string> parsed;
        for (std::size_t i = 0; i < count; ++i) {
            std::size_t length = 0;
            if (!read_line('$', length) || input.size() < position + length + 2) {
                return false;
            }
            parsed.emplace_back(input, position, length);
            position += length + 2;
        }
        input.erase(0, position);
        args = std::move(parsed);
        return true;
    }

    void serve() {
        asio::ip::tcp::socket socket(io_);
        acceptor_.accept(socket);
        std::string input;
        std::array<char, 1024> buffer{};
        std::size_t next_reply = 0;
        for (;;) {
            std::error_code error;
            const auto read = socket.read_some(asio::buffer(buffer), error);
            if (error) {
                return;
            }
            input.append(buffer.data(), read);
            std::vector<std::string> args;
            while (take_command(input, args)) {
                commands_.push_back(args);
                const std::string reply =
                    next_reply < replies_.size() ? replies_[next_reply++] : std::string("-ERR unscripted\r\n");
                asio::write(socket, asio::buffer(reply), error);
                if (error) {
                    return;
                }
            }
        }
    }

    std::vector<std::string> replies_;
    std::vector<std::vector<std::string>> commands_;
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

using ruvia::test::default_resource_guard;
using ruvia::test::equivalent_tracking_resource;

template <typename result_type>
[[nodiscard]] result_type copy_redis_result(const result_type& result, std::pmr::memory_resource& resource) {
    if constexpr (requires { typename result_type::allocator_type; }) {
        return result_type(result, typename result_type::allocator_type(&resource));
    } else {
        const default_resource_guard guard(resource);
        return result_type(result);
    }
}

template <typename result_type, typename check_type>
void check_redis_result_move_guarantees(ruvia::testing::test_context& ruvia_ctx, const result_type& original,
    const result_type& seed, bool allocator_move, check_type check);

template <typename result_type, typename check_type>
void check_redis_result_rebinding(ruvia::testing::test_context& ruvia_ctx, const result_type& original,
    const result_type& seed, bool allocator_move, check_type check) {
    ruvia::test::counting_memory_resource upstream;
    {
        equivalent_tracking_resource source(upstream);
        equivalent_tracking_resource target(upstream);
        RUVIA_CHECK(source.is_equal(target));
        std::optional<result_type> destination;
        std::optional<result_type> input(copy_redis_result(original, source));
        if constexpr (requires { typename result_type::allocator_type; }) {
            if (allocator_move) {
                destination.emplace(std::move(*input), typename result_type::allocator_type(&target));
            }
        }
        if (!destination) {
            destination.emplace(copy_redis_result(seed, target));
            *destination = std::move(*input);
        }
        input.reset();
        // Keep the source object alive to record stale allocator callbacks
        // safely, rather than making the red test invoke a destroyed resource.
        source.retire();
        check(*destination);
        destination.reset();
        RUVIA_CHECK_EQ(source.callbacks_after_retirement(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
    check_redis_result_move_guarantees(ruvia_ctx, original, seed, allocator_move, check);
}

template <typename result_type>
void check_redis_result_seed(ruvia::testing::test_context& ruvia_ctx, const result_type& actual, const result_type& seed) {
    if constexpr (std::same_as<result_type, ruvia::redis_key_value>) {
        RUVIA_CHECK_EQ(actual.key(), seed.key());
        RUVIA_CHECK_EQ(actual.value(), seed.value());
    } else if constexpr (std::same_as<result_type, ruvia::redis_scored_value>) {
        RUVIA_CHECK_EQ(actual.value(), seed.value());
        RUVIA_CHECK_EQ(actual.score(), seed.score());
    } else if constexpr (std::same_as<result_type, ruvia::redis_value>) {
        RUVIA_CHECK_EQ(actual.kind(), seed.kind());
        if (actual.kind() == ruvia::redis_value::kind_type::integer) {
            RUVIA_CHECK_EQ(actual.integer(), seed.integer());
        }
    } else if constexpr (requires { actual.next_cursor(); }) {
        RUVIA_CHECK_EQ(actual.next_cursor(), seed.next_cursor());
        RUVIA_CHECK_EQ(actual.done(), seed.done());
    }
    if constexpr (requires { actual.values(); }) {
        RUVIA_CHECK(actual.values().empty());
    }
    if constexpr (requires { actual.id(); }) {
        RUVIA_CHECK_EQ(actual.id(), seed.id());
        RUVIA_CHECK(actual.fields().empty());
    }
    if constexpr (requires { actual.entries(); }) {
        RUVIA_CHECK_EQ(actual.entries().size(), seed.entries().size());
    }
    if constexpr (requires { actual.stream(); }) {
        RUVIA_CHECK_EQ(actual.stream(), seed.stream());
        if (actual.entries().size() == 1) {
            RUVIA_CHECK_EQ(actual.entries()[0].id(), seed.entries()[0].id());
            RUVIA_CHECK(actual.entries()[0].fields().empty());
        }
    }
    if constexpr (requires { actual.streams(); }) {
        RUVIA_CHECK(actual.streams().empty());
    }
}

template <typename result_type, typename check_type>
void check_redis_result_move_guarantees(ruvia::testing::test_context& ruvia_ctx, const result_type& original,
    const result_type& seed, bool allocator_move, check_type check) {
    ruvia::test::counting_memory_resource upstream;
    const std::size_t max_failing_allowance =
        std::same_as<result_type, ruvia::redis_scored_value> ? std::size_t{0} : std::size_t{1};
    for (std::size_t allowance = 0; allowance <= max_failing_allowance; ++allowance) {
        {
            equivalent_tracking_resource source(upstream);
            equivalent_tracking_resource target(upstream);
            auto input = copy_redis_result(original, source);
            auto destination = copy_redis_result(seed, target);
            target.reject_allocations_after(allowance);
            bool rejected = false;
            try {
                if constexpr (requires { typename result_type::allocator_type; }) {
                    if (allocator_move) {
                        const result_type moved(std::move(input), typename result_type::allocator_type(&target));
                        check(moved);
                    } else {
                        destination = std::move(input);
                    }
                } else {
                    destination = std::move(input);
                }
            } catch (const std::bad_alloc&) {
                rejected = true;
            }
            target.allow_allocations();
            RUVIA_CHECK(rejected);
            if (rejected) {
                check(input);
                check_redis_result_seed(ruvia_ctx, destination, seed);
            }
        }
        RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    }
    {
        equivalent_tracking_resource resource(upstream);
        auto input = copy_redis_result(original, resource);
        auto destination = copy_redis_result(seed, resource);
        const auto attempts = resource.allocation_attempts();
        if constexpr (!ruvia::test::stl_moves_allocate_debug_metadata) {
            resource.reject_allocations_after(0);
        }
        bool rejected = false;
        try {
            if constexpr (requires { typename result_type::allocator_type; }) {
                if (allocator_move) {
                    const result_type moved(std::move(input), typename result_type::allocator_type(&resource));
                    check(moved);
                } else {
                    destination = std::move(input);
                    check(destination);
                }
            } else {
                destination = std::move(input);
                check(destination);
            }
        } catch (const std::bad_alloc&) {
            rejected = true;
        }
        resource.allow_allocations();
        RUVIA_CHECK(!rejected);
        if constexpr (!ruvia::test::stl_moves_allocate_debug_metadata) {
            RUVIA_CHECK_EQ(resource.allocation_attempts(), attempts);
        }
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

[[nodiscard]] std::string redis_bulk_reply(std::string_view value) {
    return "$" + std::to_string(value.size()) + "\r\n" + std::string(value) + "\r\n";
}

RUVIA_TEST(redis_typed_commands_preserve_arguments_across_direct_pipeline_and_transaction) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    const std::vector<std::string> command_replies{":1\r\n", ":2\r\n", "*1\r\n$5\r\nentry\r\n", ":1\r\n", "*1\r\n$6\r\nmember\r\n"};
    auto replies = command_replies;
    replies.insert(replies.end(), command_replies.begin(), command_replies.end());
    replies.emplace_back("+OK\r\n");
    replies.insert(replies.end(), 5, "+QUEUED\r\n");
    std::string executed = "*5\r\n";
    for (const auto& reply : command_replies) {
        executed.append(reply);
    }
    replies.push_back(std::move(executed));
    scripted_redis_peer peer(std::move(replies));
    ruvia::redis_config config{.host_ = "127.0.0.1", .port_ = peer.port(), .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1};
    ruvia::redis_client client(worker.loop(), config);
    const std::string key("key\0binary", 10);
    const std::string member("member\0value", 12);
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    ruvia::test::drive_backend_client(worker, client, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
        auto redis = connected.handle();
        RUVIA_CHECK_EQ(co_await redis.incr_by(key, minimum), std::int64_t{1});
        RUVIA_CHECK_EQ(co_await redis.decr_by(key, maximum), std::int64_t{2});
        const auto list = co_await redis.lrange(key, minimum, maximum);
        RUVIA_CHECK_EQ(list.size(), std::size_t{1});
        RUVIA_CHECK_EQ(co_await redis.zadd(key, -std::numeric_limits<double>::infinity(), member), std::int64_t{1});
        const auto sorted = co_await redis.zrange(key, maximum, minimum);
        RUVIA_CHECK_EQ(sorted.size(), std::size_t{1});

        const auto append = [&](auto& batch) {
            batch.incr_by(key, minimum).decr_by(key, maximum).lrange(key, minimum, maximum).zadd(key, -std::numeric_limits<double>::infinity(), member).zrange(key, maximum, minimum);
        };
        auto pipeline = redis.pipeline();
        append(pipeline);
        const auto pipelined = co_await std::move(pipeline).exec();
        RUVIA_CHECK_EQ(pipelined.size(), std::size_t{5});
        auto transaction = redis.transaction();
        append(transaction);
        const auto transacted = co_await std::move(transaction).exec();
        RUVIA_CHECK_EQ(transacted.size(), std::size_t{5});
        for (const auto* results : {&pipelined, &transacted}) {
            if (results->size() != 5) {
                continue;
            }
            RUVIA_CHECK_EQ((*results)[0].integer(), std::int64_t{1});
            RUVIA_CHECK_EQ((*results)[1].integer(), std::int64_t{2});
            const auto entries = (*results)[2].array();
            RUVIA_CHECK_EQ(entries.size(), std::size_t{1});
            if (!entries.empty()) {
                RUVIA_CHECK_EQ(entries[0].string(), std::string_view("entry"));
            }
            RUVIA_CHECK_EQ((*results)[3].integer(), std::int64_t{1});
            const auto members = (*results)[4].array();
            RUVIA_CHECK_EQ(members.size(), std::size_t{1});
            if (!members.empty()) {
                RUVIA_CHECK_EQ(members[0].string(), std::string_view("member"));
            }
        }
    });
    peer.join();
    const std::vector<std::vector<std::string>> expected{
        {"INCRBY", key, "-9223372036854775808"}, {"DECRBY", key, "9223372036854775807"},
        {"LRANGE", key, "-9223372036854775808", "9223372036854775807"}, {"ZADD", key, "-inf", member},
        {"ZRANGE", key, "9223372036854775807", "-9223372036854775808"}};
    auto all = expected;
    all.insert(all.end(), expected.begin(), expected.end());
    all.push_back({"MULTI"});
    all.insert(all.end(), expected.begin(), expected.end());
    all.push_back({"EXEC"});
    RUVIA_CHECK(peer.commands() == all);
}

RUVIA_TEST(redis_sorted_set_replies_and_arguments_round_trip_infinite_scores) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    scripted_redis_peer peer({
        ":1\r\n",
        "$3\r\ninf\r\n",
        "$4\r\n-inf\r\n",
        "*4\r\n$1\r\na\r\n$4\r\n-inf\r\n$1\r\nb\r\n$3\r\ninf\r\n",
        ":2\r\n",
        "$3\r\nnan\r\n",
        "$5\r\n1e999\r\n",
    });
    ruvia::redis_config config;
    config.host_ = "127.0.0.1";
    config.port_ = peer.port();
    config.pool_size_per_worker_ = 1;
    config.tls_ = {.mode_ = ruvia::client_tls_mode::disabled};
    ruvia::redis_client client(worker.loop(), config);
    ruvia::test::drive_backend_client(worker, client, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
        auto redis = connected.handle();
        constexpr auto infinity = std::numeric_limits<double>::infinity();
        RUVIA_CHECK_EQ(co_await redis.zadd("scores", infinity, "b"), std::int64_t{1});

        const auto positive = co_await redis.zscore("scores", "b");
        RUVIA_CHECK(positive.has_value() && *positive == infinity);
        const auto negative = co_await redis.zscore("scores", "a");
        RUVIA_CHECK(negative.has_value() && *negative == -infinity);

        const auto ranked = co_await redis.zrange_with_scores("scores", 0, -1);
        RUVIA_CHECK_EQ(ranked.size(), std::size_t{2});
        if (ranked.size() == 2) {
            RUVIA_CHECK(ranked[0].score() == -infinity);
            RUVIA_CHECK(ranked[1].score() == infinity);
        }

        RUVIA_CHECK_EQ(co_await redis.zcount("scores", -infinity, infinity), std::int64_t{2});

        const auto not_a_number = co_await redis.zscore("scores", "nan");
        RUVIA_CHECK(not_a_number.has_value() && std::isnan(*not_a_number));

        // Only the exact Redis spellings are non-finite; overflowing
        // decimals remain protocol errors.
        bool rejected = false;
        try {
            (void)co_await redis.zscore("scores", "overflow");
        } catch (const ruvia::redis_error& error) {
            rejected = error.code() == ruvia::redis_error::code_type::protocol_error;
        }
        RUVIA_CHECK(rejected);
    });
    peer.join();
    const auto& commands = peer.commands();
    RUVIA_CHECK_EQ(commands.size(), std::size_t{7});
    if (commands.size() == 7) {
        RUVIA_CHECK(commands[0] == (std::vector<std::string>{"ZADD", "scores", "+inf", "b"}));
        RUVIA_CHECK(commands[4] == (std::vector<std::string>{"ZCOUNT", "scores", "-inf", "+inf"}));
    }
}

RUVIA_TEST(redis_composite_results_rebind_equivalent_resource_owners) {
    const std::string stream_name(120, 's');
    const std::string field_name(128, 'f');
    const std::string field_value(256, 'v');
    const std::string key_name(128, 'k');
    const std::string member_name(128, 'm');
    const auto bulk_field = redis_bulk_reply(field_name);
    const auto bulk_value = redis_bulk_reply(field_value);
    const auto group_reply = "*2\r\n*2\r\n" + redis_bulk_reply(stream_name) +
                             "*2\r\n*2\r\n$3\r\n1-0\r\n*2\r\n" + bulk_field + bulk_value +
                             "*2\r\n$3\r\n2-0\r\n$-1\r\n*2\r\n$4\r\nseed\r\n*1\r\n*2\r\n$3\r\n3-0\r\n*0\r\n";
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    scripted_redis_peer peer({
        "*2\r\n*1\r\n" + bulk_value + ":42\r\n",
        ":7\r\n",
        group_reply,
        "*0\r\n",
        "*2\r\n$2\r\n12\r\n*2\r\n" + redis_bulk_reply(key_name) + "$6\r\nsecond\r\n",
        "*2\r\n$1\r\n0\r\n*0\r\n",
        "*2\r\n$1\r\n0\r\n*2\r\n" + bulk_field + bulk_value,
        "*2\r\n$1\r\n5\r\n*2\r\n" + redis_bulk_reply(std::string(120, 'x')) + redis_bulk_reply(std::string(120, 'y')),
        "*2\r\n$1\r\n9\r\n*2\r\n" + redis_bulk_reply(member_name) + "$3\r\n1.5\r\n",
        "*2\r\n$1\r\n0\r\n*2\r\n" + redis_bulk_reply(std::string(120, 'z')) + "$3\r\n2.5\r\n",
    });
    ruvia::redis_config config;
    config.host_ = "127.0.0.1";
    config.port_ = peer.port();
    config.pool_size_per_worker_ = 1;
    config.tls_ = {.mode_ = ruvia::client_tls_mode::disabled};
    ruvia::redis_client client(worker.loop(), config);
    ruvia::test::drive_backend_client(worker, client, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
        auto redis = connected.handle();
        const auto value = co_await redis.command("TEST", "nested");
        const auto value_seed = co_await redis.command("TEST", "integer");
        const std::array streams{ruvia::redis_stream_read_view{.stream_ = stream_name, .id_ = "0"}};
        auto grouped = co_await redis.xread_group("group", "consumer", streams);
        auto group_seed = co_await redis.xread_group("group", "consumer", streams);
        auto scanned = co_await redis.scan();
        auto scan_seed = co_await redis.scan();
        auto hashed = co_await redis.hscan("hash");
        auto hash_seed = co_await redis.hscan("hash");
        auto scored = co_await redis.zscan("scores");
        auto score_seed = co_await redis.zscan("scores");
        RUVIA_CHECK(group_seed.has_value());
        if (!grouped || !group_seed || grouped->streams().size() != 2 || grouped->streams()[0].entries().size() != 2) {
            co_return;
        }

        const auto check_fields = [&](std::span<const ruvia::redis_key_value> fields) {
            RUVIA_CHECK_EQ(fields.size(), std::size_t{1});
            if (fields.size() == 1) {
                RUVIA_CHECK_EQ(fields[0].key(), std::string_view(field_name));
                RUVIA_CHECK_EQ(fields[0].value(), std::string_view(field_value));
            }
        };
        const auto check_entry = [&](const ruvia::redis_stream_entry& entry) {
            RUVIA_CHECK_EQ(entry.id(), std::string_view("1-0"));
            check_fields(entry.fields());
        };
        const auto check_stream = [&](const ruvia::redis_stream_read_result& stream) {
            RUVIA_CHECK_EQ(stream.stream(), std::string_view(stream_name));
            RUVIA_CHECK_EQ(stream.entries().size(), std::size_t{2});
            if (stream.entries().size() == 2) {
                check_entry(stream.entries()[0]);
                RUVIA_CHECK_EQ(stream.entries()[1].id(), std::string_view("2-0"));
                RUVIA_CHECK(stream.entries()[1].fields().empty());
            }
        };
        for (const bool allocator_move : {true, false}) {
            check_redis_result_rebinding(ruvia_ctx, value, value_seed, allocator_move, [&](const ruvia::redis_value& result) {
                RUVIA_CHECK(result.kind() == ruvia::redis_value::kind_type::array);
                RUVIA_CHECK_EQ(result.array().size(), std::size_t{2});
                if (result.array().size() == 2) {
                    RUVIA_CHECK_EQ(result.array()[0].array().size(), std::size_t{1});
                    if (result.array()[0].array().size() == 1) {
                        RUVIA_CHECK_EQ(result.array()[0].array()[0].string(), std::string_view(field_value));
                    }
                    RUVIA_CHECK_EQ(result.array()[1].integer(), std::int64_t{42});
                }
            });
            const auto& stream = grouped->streams()[0];
            check_redis_result_rebinding(ruvia_ctx, stream.entries()[0], stream.entries()[1], allocator_move, check_entry);
            check_redis_result_rebinding(ruvia_ctx, stream, grouped->streams()[1], allocator_move, check_stream);
            if (hashed.entries().size() == 1 && hash_seed.entries().size() == 1) {
                check_redis_result_rebinding(ruvia_ctx, hashed.entries()[0], hash_seed.entries()[0], allocator_move,
                    [&](const ruvia::redis_key_value& kv) {
                        RUVIA_CHECK_EQ(kv.key(), std::string_view(field_name));
                        RUVIA_CHECK_EQ(kv.value(), std::string_view(field_value));
                    });
            }
            if (scored.entries().size() == 1 && score_seed.entries().size() == 1) {
                check_redis_result_rebinding(ruvia_ctx, scored.entries()[0], score_seed.entries()[0], allocator_move,
                    [&](const ruvia::redis_scored_value& sv) {
                        RUVIA_CHECK_EQ(sv.value(), std::string_view(member_name));
                        RUVIA_CHECK_EQ(sv.score(), 1.5);
                    });
            }
        }
        static_assert(!std::is_copy_constructible_v<ruvia::redis_x_read_group_result>);
        static_assert(!std::is_copy_assignable_v<ruvia::redis_x_read_group_result>);
        static_assert(!std::is_copy_constructible_v<ruvia::redis_scan_result>);
        static_assert(!std::is_copy_assignable_v<ruvia::redis_scan_result>);
        static_assert(!std::is_copy_constructible_v<ruvia::redis_hash_scan_result>);
        static_assert(!std::is_copy_assignable_v<ruvia::redis_hash_scan_result>);
        static_assert(!std::is_copy_constructible_v<ruvia::redis_z_scan_result>);
        static_assert(!std::is_copy_assignable_v<ruvia::redis_z_scan_result>);

        *group_seed = std::move(*grouped);
        RUVIA_CHECK_EQ(group_seed->streams().size(), std::size_t{2});
        if (group_seed->streams().size() == 2) {
            check_stream(group_seed->streams()[0]);
            RUVIA_CHECK_EQ(group_seed->streams()[1].stream(), std::string_view("seed"));
            RUVIA_CHECK_EQ(group_seed->streams()[1].entries().size(), std::size_t{1});
        }
        const auto expected_scan_cursor = scanned.next_cursor();
        scan_seed = std::move(scanned);
        RUVIA_CHECK(!scan_seed.done());
        RUVIA_CHECK_EQ(scan_seed.next_cursor(), expected_scan_cursor);
        RUVIA_CHECK_EQ(scan_seed.values().size(), std::size_t{2});
        if (scan_seed.values().size() == 2) {
            RUVIA_CHECK_EQ(std::string_view(scan_seed.values()[0]), std::string_view(key_name));
            RUVIA_CHECK_EQ(std::string_view(scan_seed.values()[1]), std::string_view("second"));
        }
        const auto expected_hash_cursor = hashed.next_cursor();
        hash_seed = std::move(hashed);
        RUVIA_CHECK(hash_seed.done());
        RUVIA_CHECK_EQ(hash_seed.next_cursor(), expected_hash_cursor);
        check_fields(hash_seed.entries());

        const auto expected_score_cursor = scored.next_cursor();
        score_seed = std::move(scored);
        RUVIA_CHECK(!score_seed.done());
        RUVIA_CHECK_EQ(score_seed.next_cursor(), expected_score_cursor);
        RUVIA_CHECK_EQ(score_seed.entries().size(), std::size_t{1});
        if (score_seed.entries().size() == 1) {
            RUVIA_CHECK_EQ(score_seed.entries()[0].value(), std::string_view(member_name));
            RUVIA_CHECK_EQ(score_seed.entries()[0].score(), 1.5);
        }
    });
    peer.join();
}

RUVIA_TEST(redis_typed_replies_retain_binary_payloads_and_recover_from_invalid_shapes) {
    std::string key(128, 'k');
    std::string payload(4097, 'v');
    key[17] = '\0';
    payload[2048] = '\0';
    const std::string_view expected_key(key);
    const std::string_view expected_payload(payload);
    const auto bulk_key = redis_bulk_reply(key);
    const auto bulk_payload = redis_bulk_reply(payload);
    const auto array = [](std::initializer_list<std::string> items) {
        std::string result = "*" + std::to_string(items.size()) + "\r\n";
        for (const auto& item : items) {
            result.append(item);
        }
        return result;
    };
    const auto fields = array({bulk_key, bulk_payload});
    const auto scored = array({bulk_key, redis_bulk_reply("1.5")});
    const auto scan = [&](const std::string& values) {
        return array({redis_bulk_reply("0"), values});
    };
    const auto stream = [&](const std::string& values) {
        return array({array({bulk_key, array({array({redis_bulk_reply("1-0"), values})})})});
    };
    scripted_redis_peer peer({bulk_payload, bulk_payload, bulk_payload,
        array({bulk_payload, "$-1\r\n", "$0\r\n\r\n"}), array({bulk_payload, bulk_key}),
        fields, scored, scan(array({bulk_key, bulk_payload})), scan(fields), scan(scored), stream(fields),
        array({bulk_payload, ":1\r\n"}), scan(array({bulk_key})), stream(array({bulk_key})),
        bulk_payload, ":1\r\n", "$-1\r\n"});
    auto& io = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io);
    ruvia::redis_client client(worker.loop(), {.host_ = "127.0.0.1", .port_ = peer.port(), .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1});
    ruvia::test::drive_backend_client(worker, client, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
        auto redis = connected.handle();
        const auto scalar = co_await redis.get(key);
        const auto status = co_await redis.ping(payload);
        const auto previous = co_await redis.set(key, "new", {.previous_value_ = ruvia::redis_set_previous_value_policy::return_value});
        const std::array<std::string_view, 3> keys{key, "missing", "empty"};
        const auto optional_values = co_await redis.mget(keys);
        const auto list = co_await redis.lrange(key, 0, -1);
        const auto hash = co_await redis.hget_all(key);
        const auto sorted = co_await redis.zrange_with_scores(key, 0, -1);
        const auto scanned = co_await redis.scan();
        const auto hash_scan = co_await redis.hscan(key);
        const auto sorted_scan = co_await redis.zscan(key);
        const std::array streams{ruvia::redis_stream_read_view{.stream_ = key, .id_ = ">"}};
        const auto grouped = co_await redis.xread_group("group", "consumer", streams);
        const auto rejects_protocol = [&](auto&& operation) -> ruvia::task<void> {
            bool rejected = false;
            try {
                (void)co_await std::move(operation);
            } catch (const ruvia::redis_error& error) {
                rejected = error.code() == ruvia::redis_error::code_type::protocol_error;
            }
            RUVIA_CHECK(rejected);
        };
        co_await rejects_protocol(redis.lrange(key, 0, -1));
        co_await rejects_protocol(redis.hscan(key));
        co_await rejects_protocol(redis.xread_group("group", "consumer", streams));
        const auto after_errors = co_await redis.get(key);
        co_await rejects_protocol(redis.get(key));
        RUVIA_CHECK(!(co_await redis.get("missing")));

        RUVIA_CHECK(scalar && *scalar == expected_payload);
        RUVIA_CHECK_EQ(status, expected_payload);
        RUVIA_CHECK(previous.applied() && previous.previous() && *previous.previous() == expected_payload);
        RUVIA_CHECK_EQ(optional_values.size(), std::size_t{3});
        if (optional_values.size() == 3) {
            RUVIA_CHECK(optional_values[0] && *optional_values[0] == expected_payload);
            RUVIA_CHECK(!optional_values[1]);
            RUVIA_CHECK(optional_values[2] && optional_values[2]->empty());
        }
        RUVIA_CHECK_EQ(list.size(), std::size_t{2});
        if (list.size() == 2) {
            RUVIA_CHECK(list[0] == expected_payload && list[1] == expected_key);
        }
        const auto check_fields = [&](std::span<const ruvia::redis_key_value> values) {
            RUVIA_CHECK_EQ(values.size(), std::size_t{1});
            if (values.size() == 1) {
                RUVIA_CHECK(values[0].key() == expected_key && values[0].value() == expected_payload);
            }
        };
        const auto check_scores = [&](std::span<const ruvia::redis_scored_value> values) {
            RUVIA_CHECK_EQ(values.size(), std::size_t{1});
            if (values.size() == 1) {
                RUVIA_CHECK(values[0].value() == expected_key && values[0].score() == 1.5);
            }
        };
        check_fields(hash);
        check_fields(hash_scan.entries());
        check_scores(sorted);
        check_scores(sorted_scan.entries());
        RUVIA_CHECK(scanned.done() && hash_scan.done() && sorted_scan.done());
        RUVIA_CHECK_EQ(scanned.values().size(), std::size_t{2});
        if (scanned.values().size() == 2) {
            RUVIA_CHECK(scanned.values()[0] == expected_key && scanned.values()[1] == expected_payload);
        }
        RUVIA_CHECK(grouped && grouped->streams().size() == 1);
        if (grouped && grouped->streams().size() == 1) {
            const auto& result = grouped->streams()[0];
            RUVIA_CHECK(result.stream() == expected_key && result.entries().size() == 1);
            if (result.entries().size() == 1) {
                RUVIA_CHECK_EQ(result.entries()[0].id(), std::string_view("1-0"));
                check_fields(result.entries()[0].fields());
            }
        }
        RUVIA_CHECK(after_errors && *after_errors == expected_payload);
    });
    peer.join();
}

}  // namespace redis_client_test
