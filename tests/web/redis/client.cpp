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

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/redis/redis_client.h"
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
    if constexpr (std::same_as<result_type, ruvia::redis_value>) {
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
    for (const auto allowance : {std::size_t{0}, std::size_t{1}}) {
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
        resource.reject_allocations_after(0);
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
        RUVIA_CHECK_EQ(resource.allocation_attempts(), attempts);
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

[[nodiscard]] std::string redis_bulk_reply(std::string_view value) {
    return "$" + std::to_string(value.size()) + "\r\n" + std::string(value) + "\r\n";
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
        "*2\r\n$1\r\n5\r\n*0\r\n",
        "*2\r\n$1\r\n9\r\n*2\r\n" + redis_bulk_reply(member_name) + "$3\r\n1.5\r\n",
        "*2\r\n$1\r\n0\r\n*0\r\n",
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
        const auto grouped = co_await redis.xread_group("group", "consumer", streams);
        const auto group_seed = co_await redis.xread_group("group", "consumer", streams);
        const auto scanned = co_await redis.scan();
        const auto scan_seed = co_await redis.scan();
        const auto hashed = co_await redis.hscan("hash");
        const auto hash_seed = co_await redis.hscan("hash");
        const auto scored = co_await redis.zscan("scores");
        const auto score_seed = co_await redis.zscan("scores");
        RUVIA_CHECK(grouped.has_value());
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
        }
        check_redis_result_rebinding(ruvia_ctx, *grouped, *group_seed, false, [&](const ruvia::redis_x_read_group_result& result) {
            RUVIA_CHECK_EQ(result.streams().size(), std::size_t{2});
            if (result.streams().size() == 2) {
                check_stream(result.streams()[0]);
                RUVIA_CHECK_EQ(result.streams()[1].stream(), std::string_view("seed"));
                RUVIA_CHECK_EQ(result.streams()[1].entries().size(), std::size_t{1});
            }
        });
        check_redis_result_rebinding(ruvia_ctx, scanned, scan_seed, false, [&](const ruvia::redis_scan_result& result) {
            RUVIA_CHECK(!result.done());
            RUVIA_CHECK_EQ(result.next_cursor(), scanned.next_cursor());
            RUVIA_CHECK_EQ(result.values().size(), std::size_t{2});
            if (result.values().size() == 2) {
                RUVIA_CHECK_EQ(std::string_view(result.values()[0]), std::string_view(key_name));
                RUVIA_CHECK_EQ(std::string_view(result.values()[1]), std::string_view("second"));
            }
        });
        check_redis_result_rebinding(ruvia_ctx, hashed, hash_seed, false, [&](const ruvia::redis_hash_scan_result& result) {
            RUVIA_CHECK(result.done());
            RUVIA_CHECK_EQ(result.next_cursor(), hashed.next_cursor());
            check_fields(result.entries());
        });
        check_redis_result_rebinding(ruvia_ctx, scored, score_seed, false, [&](const ruvia::redis_z_scan_result& result) {
            RUVIA_CHECK(!result.done());
            RUVIA_CHECK_EQ(result.next_cursor(), scored.next_cursor());
            RUVIA_CHECK_EQ(result.entries().size(), std::size_t{1});
            if (result.entries().size() == 1) {
                RUVIA_CHECK_EQ(result.entries()[0].value(), std::string_view(member_name));
                RUVIA_CHECK_EQ(result.entries()[0].score(), 1.5);
            }
        });
    });
    peer.join();
}

}  // namespace redis_client_test
