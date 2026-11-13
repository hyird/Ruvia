#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <limits>
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
#include "ruvia/web/redis/redis_session.h"

#include "backend_client_fixture.h"
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

TEST(Ruvia, redis_block_wait_saturates_before_narrowing_and_rounds_up) {
    using namespace std::chrono_literals;
    const auto huge = std::chrono::duration<std::uint64_t, std::milli>{
        (std::numeric_limits<std::uint64_t>::max)()};
    const auto saturated = ruvia::redis_block_wait::for_duration(huge);
    EXPECT_TRUE(!saturated.infinite());
    EXPECT_TRUE(saturated.duration() == std::chrono::milliseconds::max());
    EXPECT_TRUE(ruvia::redis_block_wait::for_duration(
                    std::chrono::duration<std::uint64_t>{(std::numeric_limits<std::uint64_t>::max)()})
                    .duration() == std::chrono::milliseconds::max());
    EXPECT_TRUE(ruvia::redis_block_wait::for_duration(1ns).duration() == 1ms);
    EXPECT_TRUE(ruvia::redis_block_wait::for_duration(1001us).duration() == 2ms);
    EXPECT_TRUE(ruvia::redis_block_wait::for_duration(2s).duration() == 2000ms);
    EXPECT_TRUE(throws_invalid_argument([] { (void)ruvia::redis_block_wait::for_duration(0ms); }));
    EXPECT_TRUE(throws_invalid_argument([] { (void)ruvia::redis_block_wait::for_duration(-1ns); }));
    EXPECT_TRUE(throws_invalid_argument([] { (void)ruvia::redis_block_wait::for_duration(-1ms); }));
    const auto infinite = ruvia::redis_block_wait::indefinitely();
    EXPECT_TRUE(infinite.infinite());
    EXPECT_TRUE(!infinite.duration().has_value());
}

TEST(Ruvia, redis_timeout_config_rejects_non_positive_inputs) {
    using namespace std::chrono_literals;
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    for (const auto field : {&ruvia::redis_config::connect_timeout_, &ruvia::redis_config::command_timeout_,
             &ruvia::redis_config::acquire_timeout_, &ruvia::redis_config::subscription_ping_interval_}) {
        ruvia::redis_config config{.tls_ = {.mode_ = ruvia::client_tls_mode::disabled}};
        for (const auto invalid : {0ms, -1ms}) {
            config.*field = invalid;
            EXPECT_TRUE(throws_invalid_argument([&] {
                ruvia::redis_client client(worker.loop(), config);
            }));
        }
    }
}

TEST(Ruvia, redis_blocking_commands_ignore_the_ordinary_pool_timeout_and_honor_operation_cancellation) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    ruvia::redis_config config;
    config.command_timeout_ = std::chrono::milliseconds(1);
    ruvia::test::with_connected_redis_client(worker, config, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        auto redis = client.redis();
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
            EXPECT_TRUE(timed_out);
            EXPECT_TRUE(std::chrono::steady_clock::now() - started >= timeout);
        };
        co_await expect_timeout(bounded.blpop(keys, ruvia::redis_block_wait::for_duration(std::chrono::seconds(1))));
        co_await expect_timeout(bounded.xread_group("workers", "consumer", streams,
            {.block_ = ruvia::redis_block_wait::for_duration(std::chrono::milliseconds(10))}));
        const auto huge_wait = ruvia::redis_block_wait::for_duration(
            std::chrono::duration<std::uint64_t, std::milli>{(std::numeric_limits<std::uint64_t>::max)()});
        co_await expect_timeout(bounded.blpop(keys, huge_wait));
        co_await expect_timeout(bounded.xread_group("workers", "consumer", streams, {.block_ = huge_wait}));
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
            EXPECT_TRUE(rejected);
        }
        for (auto word : {"WAIT", "wait", "WaItAoF"}) {
            EXPECT_TRUE(throws_invalid_argument([&] { (void)bounded.command(word, "1", "1000"); }));
            auto transaction = bounded.transaction();
            EXPECT_TRUE(throws_invalid_argument([&] { transaction.command(word, "1", "1000"); }));
        }
        for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}, std::int64_t{0}}) {
            EXPECT_TRUE(throws_invalid_argument([&] { (void)redis.scan({.count_ = invalid}); }));
            EXPECT_TRUE(throws_invalid_argument([&] { (void)redis.xread_group("workers", "consumer", streams, {.count_ = invalid}); }));
        }
        for (const auto invalid : {static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1,
                 (std::numeric_limits<std::uint64_t>::max)()}) {
            EXPECT_TRUE(throws_invalid_argument([&] { (void)redis.scan({.count_ = invalid}); }));
            EXPECT_TRUE(throws_invalid_argument([&] { (void)redis.xread_group("workers", "consumer", streams, {.count_ = invalid}); }));
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
            EXPECT_TRUE(rejected);
        };
        co_await expect_cancelled(cancelled.xread_group(
            "workers", "consumer", streams, {.block_ = ruvia::redis_block_wait::indefinitely()}));
        co_await expect_cancelled(cancelled.blpop(keys, ruvia::redis_block_wait::indefinitely()));
        co_await expect_cancelled(cancelled.blpop(keys, huge_wait));
        co_await expect_cancelled(cancelled.xread_group("workers", "consumer", streams, {.block_ = huge_wait}));
        co_await expect_cancelled(cancelled.command("BLPOP", "queue", "0"));
    });
}

TEST(Ruvia, redis_stream_read_options_route_claim_as_blocking_and_reject_unknown_options) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    ruvia::test::with_connected_redis_client(worker, {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        auto redis = client.redis();
        auto pipeline = redis.pipeline();
        // CLAIM (Redis 8.4) takes a value; a BLOCK after it still blocks, so the
        // command cannot join a pipeline.
        EXPECT_TRUE(throws_invalid_argument([&] {
            pipeline.command("XREADGROUP", "GROUP", "g", "c", "CLAIM", "1000", "BLOCK", "0", "STREAMS", "s", ">");
        }));
        EXPECT_TRUE(throws_invalid_argument([&] {
            pipeline.command("xreadgroup", "GROUP", "g", "c", "COUNT", "1", "claim", "5", "NOACK", "block", "10",
                "STREAMS", "s", ">");
        }));
        pipeline.command("XREADGROUP", "GROUP", "g", "c", "CLAIM", "1000", "NOACK", "STREAMS", "s", ">");
        pipeline.command("XREAD", "COUNT", "1", "STREAMS", "s", "0");
        // An option outside the documented grammar is rejected rather than
        // guessed non-blocking; CLAIM and NOACK belong to XREADGROUP only.
        for (auto args : {std::initializer_list<std::string_view>{"XREADGROUP", "GROUP", "g", "c", "LATER", "STREAMS", "s", ">"},
                 {"XREAD", "CLAIM", "1000", "BLOCK", "0", "STREAMS", "s", "0"},
                 {"XREAD", "NOACK", "STREAMS", "s", "0"}}) {
            const std::span<const std::string_view> command(args.begin(), args.size());
            EXPECT_TRUE(throws_invalid_argument([&] { (void)redis.command(command); }));
            EXPECT_TRUE(throws_invalid_argument([&] { pipeline.command(command); }));
        }
        co_return;
    });
}

TEST(Ruvia, redis_batch_builders_own_cold_payload_and_reject_reuse) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    ruvia::test::with_connected_redis_client(worker, {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        auto handle = client.redis();
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
            EXPECT_TRUE(throws_logic_error([&] { pipeline.get("moved"); }));
            auto cold = std::move(*moved).exec();
            EXPECT_TRUE(throws_logic_error([&] { moved->incr_by("used", 1); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)std::move(*moved).exec(); }));
            moved.reset();
        }
        {
            auto transaction = handle.transaction();
            transaction.set(std::string(128, 'k'), std::string(128, 'v'));
            std::optional moved(std::move(transaction));
            EXPECT_TRUE(throws_logic_error([&] { transaction.get("moved"); }));
            auto cold = std::move(*moved).exec();
            EXPECT_TRUE(throws_logic_error([&] { moved->incr_by("used", 1); }));
            EXPECT_TRUE(throws_logic_error([&] { moved->zadd("used", 1, "member"); }));
            moved.reset();
        }
        {
            auto pipeline = handle.pipeline();
            auto transaction = handle.transaction();
            for (auto word : {"WATCH", "UNWATCH", "MULTI", "EXEC", "SYNC", "PSYNC", "REPLCONF", "BLPOP",
                     "UNSUBSCRIBE", "PUNSUBSCRIBE", "SUNSUBSCRIBE"}) {
                EXPECT_TRUE(throws_invalid_argument([&] { pipeline.command(word, "key"); }));
                EXPECT_TRUE(throws_invalid_argument([&] { transaction.command(word, "key"); }));
            }
            // Infinite scores have a Redis spelling; NaN does not.
            pipeline.zadd("key", std::numeric_limits<double>::infinity(), "member");
            transaction.zadd("key", -std::numeric_limits<double>::infinity(), "member");
            EXPECT_TRUE(throws_invalid_argument(
                [&] { pipeline.zadd("key", std::numeric_limits<double>::quiet_NaN(), "member"); }));
            EXPECT_TRUE(throws_invalid_argument(
                [&] { transaction.zadd("key", std::numeric_limits<double>::quiet_NaN(), "member"); }));
        }
        co_return;
    });
}

TEST(Ruvia, redis_set_expiration_cannot_represent_conflicting_modes) {
    const auto expiring = ruvia::redis_set_expiration::expires_after(std::chrono::milliseconds(1500));
    EXPECT_TRUE(expiring.duration() != nullptr);
    EXPECT_EQ(expiring.duration()->count(), std::chrono::milliseconds::rep{1500});
    EXPECT_TRUE(!expiring.keeps_existing());

    const auto keep = ruvia::redis_set_expiration::keep_existing();
    EXPECT_TRUE(keep.duration() == nullptr);
    EXPECT_TRUE(keep.keeps_existing());

    bool zero_rejected = false;
    try {
        (void)ruvia::redis_set_expiration::expires_after(std::chrono::milliseconds(0));
    } catch (const std::invalid_argument&) {
        zero_rejected = true;
    }
    EXPECT_TRUE(zero_rejected);

    bool negative_rejected = false;
    try {
        (void)ruvia::redis_set_expiration::expires_after(std::chrono::milliseconds(-1));
    } catch (const std::invalid_argument&) {
        negative_rejected = true;
    }
    EXPECT_TRUE(negative_rejected);
}

TEST(Ruvia, redis_expire_rejects_non_positive_ttl_before_io) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    ruvia::test::with_connected_redis_client(worker, {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        auto redis = client.redis();

        bool zero_rejected = false;
        try {
            (void)redis.expire("key", std::chrono::seconds(0));
        } catch (const std::invalid_argument&) {
            zero_rejected = true;
        }
        EXPECT_TRUE(zero_rejected);

        bool negative_rejected = false;
        try {
            (void)redis.expire("key", std::chrono::seconds(-1));
        } catch (const std::invalid_argument&) {
            negative_rejected = true;
        }
        EXPECT_TRUE(negative_rejected);
        co_return;
    });
}

TEST(Ruvia, redis_multi_key_commands_reject_empty_key_spans_before_io) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    ruvia::test::with_connected_redis_client(worker, {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        auto redis = client.redis();
        const std::span<const std::string_view> no_keys;

        EXPECT_TRUE(throws_invalid_argument([&] { (void)redis.mget(no_keys); }));
        EXPECT_TRUE(throws_invalid_argument([&] { (void)redis.sinter(no_keys); }));
        EXPECT_TRUE(throws_invalid_argument([&] { (void)redis.sunion(no_keys); }));
        EXPECT_TRUE(throws_invalid_argument([&] { (void)redis.sdiff(no_keys); }));
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
            number = std::stoul(std::string(std::string_view(input).substr(position + 1, end - position - 1)));
            position = end + 2;
            return true;
        };
        std::size_t count = 0;
        if (!read_line('*', count)) {
            return false;
        }
        std::vector<std::string> parsed{};
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
        std::string input{};
        std::array<char, 1024> buffer{};
        std::size_t next_reply = 0;
        for (;;) {
            std::error_code error;
            const auto read = socket.read_some(asio::buffer(buffer), error);
            if (error) {
                return;
            }
            input.append(buffer.data(), read);
            std::vector<std::string> args{};
            while (take_command(input, args)) {
                commands_.push_back(args);
                const std::string reply(next_reply < replies_.size() ? replies_[next_reply++] : std::string("-ERR unscripted\r\n"));
                asio::write(socket, asio::buffer(reply), error);
                if (error) {
                    return;
                }
            }
        }
    }

    std::vector<std::string> replies_{};
    std::vector<std::vector<std::string>> commands_{};
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

template <typename result_type, typename check_type>
void check_redis_result_rebinding(const result_type& original,
    const result_type& seed, bool, check_type check) {
    if constexpr (std::is_copy_constructible_v<result_type>) {
        auto input = original;
        auto destination = seed;
        destination = std::move(input);
        check(destination);
        const result_type moved(std::move(destination));
        check(moved);
    }
}

[[nodiscard]] std::string redis_bulk_reply(std::string_view value) {
    return "$" + std::string(std::to_string(value.size())) + "\r\n" + std::string(value) + "\r\n";
}

TEST(Ruvia, redis_typed_commands_preserve_arguments_across_direct_pipeline_and_transaction) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    const std::vector<std::string> command_replies({std::string(":1\r\n"), std::string(":2\r\n"), std::string("*1\r\n$5\r\nentry\r\n"), std::string(":1\r\n"), std::string("*1\r\n$6\r\nmember\r\n")});
    auto replies = decltype(command_replies)(command_replies);
    replies.insert(replies.end(), command_replies.begin(), command_replies.end());
    replies.emplace_back("+OK\r\n");
    replies.insert(replies.end(), 5, std::string("+QUEUED\r\n"));
    std::string executed("*5\r\n");
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
        auto redis = connected.redis();
        EXPECT_EQ(co_await redis.incr_by(key, minimum), std::int64_t{1});
        EXPECT_EQ(co_await redis.decr_by(key, maximum), std::int64_t{2});
        const auto list = co_await redis.lrange(key, minimum, maximum);
        EXPECT_EQ(list.size(), std::size_t{1});
        EXPECT_EQ(co_await redis.zadd(key, -std::numeric_limits<double>::infinity(), member), std::int64_t{1});
        const auto sorted = co_await redis.zrange(key, maximum, minimum);
        EXPECT_EQ(sorted.size(), std::size_t{1});

        const auto append = [&](auto& batch) {
            batch.incr_by(key, minimum).decr_by(key, maximum).lrange(key, minimum, maximum).zadd(key, -std::numeric_limits<double>::infinity(), member).zrange(key, maximum, minimum);
        };
        auto pipeline = redis.pipeline();
        append(pipeline);
        const auto pipelined = co_await std::move(pipeline).exec();
        EXPECT_EQ(pipelined.size(), std::size_t{5});
        auto transaction = redis.transaction();
        append(transaction);
        const auto transacted = co_await std::move(transaction).exec();
        EXPECT_EQ(transacted.size(), std::size_t{5});
        for (const auto* results : {&pipelined, &transacted}) {
            if (results->size() != 5) {
                continue;
            }
            EXPECT_EQ((*results)[0].integer(), std::int64_t{1});
            EXPECT_EQ((*results)[1].integer(), std::int64_t{2});
            const auto entries = (*results)[2].array();
            EXPECT_EQ(entries.size(), std::size_t{1});
            if (!entries.empty()) {
                EXPECT_EQ(entries[0].string(), std::string_view("entry"));
            }
            EXPECT_EQ((*results)[3].integer(), std::int64_t{1});
            const auto members = (*results)[4].array();
            EXPECT_EQ(members.size(), std::size_t{1});
            if (!members.empty()) {
                EXPECT_EQ(members[0].string(), std::string_view("member"));
            }
        }
    });
    peer.join();
    const std::vector<std::vector<std::string>> expected({std::vector<std::string>({std::string("INCRBY"), std::string(key), std::string("-9223372036854775808")}), std::vector<std::string>({std::string("DECRBY"), std::string(key), std::string("9223372036854775807")}),
        std::vector<std::string>({std::string("LRANGE"), std::string(key), std::string("-9223372036854775808"), std::string("9223372036854775807")}), std::vector<std::string>({std::string("ZADD"), std::string(key), std::string("-inf"), std::string(member)}),
        std::vector<std::string>({std::string("ZRANGE"), std::string(key), std::string("9223372036854775807"), std::string("-9223372036854775808")})});
    auto all = decltype(expected)(expected);
    all.insert(all.end(), expected.begin(), expected.end());
    all.push_back(std::vector<std::string>({std::string("MULTI")}));
    all.insert(all.end(), expected.begin(), expected.end());
    all.push_back(std::vector<std::string>({std::string("EXEC")}));
    EXPECT_TRUE(peer.commands() == all);
}

TEST(Ruvia, redis_sorted_set_replies_and_arguments_round_trip_infinite_scores) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    scripted_redis_peer peer(std::vector<std::string>({
        std::string(":1\r\n"),
        std::string("$3\r\ninf\r\n"),
        std::string("$4\r\n-inf\r\n"),
        std::string("*4\r\n$1\r\na\r\n$4\r\n-inf\r\n$1\r\nb\r\n$3\r\ninf\r\n"),
        std::string(":2\r\n"),
        std::string("$3\r\nnan\r\n"),
        std::string("$5\r\n1e999\r\n"),
    }));
    ruvia::redis_config config;
    config.host_ = "127.0.0.1";
    config.port_ = peer.port();
    config.pool_size_per_worker_ = 1;
    config.tls_ = {.mode_ = ruvia::client_tls_mode::disabled};
    ruvia::redis_client client(worker.loop(), config);
    ruvia::test::drive_backend_client(worker, client, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
        auto redis = connected.redis();
        constexpr auto infinity = std::numeric_limits<double>::infinity();
        EXPECT_EQ(co_await redis.zadd("scores", infinity, "b"), std::int64_t{1});

        const auto positive = co_await redis.zscore("scores", "b");
        EXPECT_TRUE(positive.has_value() && *positive == infinity);
        const auto negative = co_await redis.zscore("scores", "a");
        EXPECT_TRUE(negative.has_value() && *negative == -infinity);

        const auto ranked = co_await redis.zrange_with_scores("scores", 0, -1);
        EXPECT_EQ(ranked.size(), std::size_t{2});
        if (ranked.size() == 2) {
            EXPECT_TRUE(ranked[0].score() == -infinity);
            EXPECT_TRUE(ranked[1].score() == infinity);
        }

        EXPECT_EQ(co_await redis.zcount("scores", -infinity, infinity), std::int64_t{2});

        const auto not_a_number = co_await redis.zscore("scores", "nan");
        EXPECT_TRUE(not_a_number.has_value() && std::isnan(*not_a_number));

        // Only the exact Redis spellings are non-finite; overflowing
        // decimals remain protocol errors.
        bool rejected = false;
        try {
            (void)co_await redis.zscore("scores", "overflow");
        } catch (const ruvia::redis_error& error) {
            rejected = error.code() == ruvia::redis_error::code_type::protocol_error;
        }
        EXPECT_TRUE(rejected);
    });
    peer.join();
    const auto& commands = peer.commands();
    EXPECT_EQ(commands.size(), std::size_t{7});
    if (commands.size() == 7) {
        EXPECT_TRUE(commands[0] == (std::vector<std::string>({std::string("ZADD"), std::string("scores"), std::string("+inf"), std::string("b")})));
        EXPECT_TRUE(commands[4] == (std::vector<std::string>({std::string("ZCOUNT"), std::string("scores"), std::string("-inf"), std::string("+inf")})));
    }
}

TEST(Ruvia, redis_composite_results_rebind_equivalent_resource_owners) {
    const std::string stream_name(120, 's');
    const std::string field_name(128, 'f');
    const std::string field_value(256, 'v');
    const std::string key_name(128, 'k');
    const std::string member_name(128, 'm');
    const auto bulk_field = redis_bulk_reply(field_name);
    const auto bulk_value = redis_bulk_reply(field_value);
    const auto group_reply = "*2\r\n*2\r\n" + redis_bulk_reply(stream_name) +
                             std::string("*2\r\n*2\r\n$3\r\n1-0\r\n*2\r\n") + bulk_field + bulk_value +
                             "*2\r\n$3\r\n2-0\r\n$-1\r\n*2\r\n$4\r\nseed\r\n*1\r\n*2\r\n$3\r\n3-0\r\n*0\r\n";
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    scripted_redis_peer peer(std::vector<std::string>({
        std::string("*2\r\n*1\r\n") + bulk_value + ":42\r\n",
        std::string(":7\r\n"),
        std::string(group_reply),
        std::string("*0\r\n"),
        std::string("*2\r\n$2\r\n12\r\n*2\r\n" + redis_bulk_reply(key_name) + "$6\r\nsecond\r\n"),
        std::string("*2\r\n$1\r\n0\r\n*0\r\n"),
        std::string("*2\r\n$1\r\n0\r\n*2\r\n") + bulk_field + bulk_value,
        std::string("*2\r\n$1\r\n5\r\n*2\r\n" + redis_bulk_reply(std::string(120, 'x')) + redis_bulk_reply(std::string(120, 'y'))),
        std::string("*2\r\n$1\r\n9\r\n*2\r\n" + redis_bulk_reply(member_name) + "$3\r\n1.5\r\n"),
        std::string("*2\r\n$1\r\n0\r\n*2\r\n" + redis_bulk_reply(std::string(120, 'z')) + "$3\r\n2.5\r\n"),
    }));
    ruvia::redis_config config;
    config.host_ = "127.0.0.1";
    config.port_ = peer.port();
    config.pool_size_per_worker_ = 1;
    config.tls_ = {.mode_ = ruvia::client_tls_mode::disabled};
    ruvia::redis_client client(worker.loop(), config);
    ruvia::test::drive_backend_client(worker, client, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
        auto redis = connected.redis();
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
        EXPECT_TRUE(group_seed.has_value());
        if (!grouped || !group_seed || grouped->streams().size() != 2 || grouped->streams()[0].entries().size() != 2) {
            co_return;
        }

        const auto check_fields = [&](std::span<const ruvia::redis_key_value> fields) {
            EXPECT_EQ(fields.size(), std::size_t{1});
            if (fields.size() == 1) {
                EXPECT_EQ(fields[0].key(), std::string_view(field_name));
                EXPECT_EQ(fields[0].value(), std::string_view(field_value));
            }
        };
        const auto check_entry = [&](const ruvia::redis_stream_entry& entry) {
            EXPECT_EQ(entry.id(), std::string_view("1-0"));
            check_fields(entry.fields());
        };
        const auto check_stream = [&](const ruvia::redis_stream_read_result& stream) {
            EXPECT_EQ(stream.stream(), std::string_view(stream_name));
            EXPECT_EQ(stream.entries().size(), std::size_t{2});
            if (stream.entries().size() == 2) {
                check_entry(stream.entries()[0]);
                EXPECT_EQ(stream.entries()[1].id(), std::string_view("2-0"));
                EXPECT_TRUE(stream.entries()[1].fields().empty());
            }
        };
        for (const bool allocator_move : {true, false}) {
            check_redis_result_rebinding(value, value_seed, allocator_move, [&](const ruvia::redis_value& result) {
                EXPECT_TRUE(result.kind() == ruvia::redis_value::kind_type::array);
                EXPECT_EQ(result.array().size(), std::size_t{2});
                if (result.array().size() == 2) {
                    EXPECT_EQ(result.array()[0].array().size(), std::size_t{1});
                    if (result.array()[0].array().size() == 1) {
                        EXPECT_EQ(result.array()[0].array()[0].string(), std::string_view(field_value));
                    }
                    EXPECT_EQ(result.array()[1].integer(), std::int64_t{42});
                }
            });
            const auto& stream = grouped->streams()[0];
            check_redis_result_rebinding(stream.entries()[0], stream.entries()[1], allocator_move, check_entry);
            check_redis_result_rebinding(stream, grouped->streams()[1], allocator_move, check_stream);
            if (hashed.entries().size() == 1 && hash_seed.entries().size() == 1) {
                check_redis_result_rebinding(hashed.entries()[0], hash_seed.entries()[0], allocator_move,
                    [&](const ruvia::redis_key_value& kv) {
                        EXPECT_EQ(kv.key(), std::string_view(field_name));
                        EXPECT_EQ(kv.value(), std::string_view(field_value));
                    });
            }
            if (scored.entries().size() == 1 && score_seed.entries().size() == 1) {
                check_redis_result_rebinding(scored.entries()[0], score_seed.entries()[0], allocator_move,
                    [&](const ruvia::redis_scored_value& sv) {
                        EXPECT_EQ(sv.value(), std::string_view(member_name));
                        EXPECT_EQ(sv.score(), 1.5);
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
        EXPECT_EQ(group_seed->streams().size(), std::size_t{2});
        if (group_seed->streams().size() == 2) {
            check_stream(group_seed->streams()[0]);
            EXPECT_EQ(group_seed->streams()[1].stream(), std::string_view("seed"));
            EXPECT_EQ(group_seed->streams()[1].entries().size(), std::size_t{1});
        }
        const auto expected_scan_cursor = scanned.next_cursor();
        scan_seed = std::move(scanned);
        EXPECT_TRUE(!scan_seed.done());
        EXPECT_EQ(scan_seed.next_cursor(), expected_scan_cursor);
        EXPECT_EQ(scan_seed.values().size(), std::size_t{2});
        if (scan_seed.values().size() == 2) {
            EXPECT_EQ(std::string_view(scan_seed.values()[0]), std::string_view(key_name));
            EXPECT_EQ(std::string_view(scan_seed.values()[1]), std::string_view("second"));
        }
        const auto expected_hash_cursor = hashed.next_cursor();
        hash_seed = std::move(hashed);
        EXPECT_TRUE(hash_seed.done());
        EXPECT_EQ(hash_seed.next_cursor(), expected_hash_cursor);
        check_fields(hash_seed.entries());

        const auto expected_score_cursor = scored.next_cursor();
        score_seed = std::move(scored);
        EXPECT_TRUE(!score_seed.done());
        EXPECT_EQ(score_seed.next_cursor(), expected_score_cursor);
        EXPECT_EQ(score_seed.entries().size(), std::size_t{1});
        if (score_seed.entries().size() == 1) {
            EXPECT_EQ(score_seed.entries()[0].value(), std::string_view(member_name));
            EXPECT_EQ(score_seed.entries()[0].score(), 1.5);
        }
    });
    peer.join();
}

TEST(Ruvia, redis_string_array_replies_reject_nil_elements) {
    // LRANGE positions and set members are never nil; dropping one would
    // silently shorten or shift the result.
    scripted_redis_peer peer(std::vector<std::string>({std::string("*3\r\n$1\r\na\r\n$-1\r\n$1\r\nc\r\n"), std::string("*1\r\n$-1\r\n"),
        std::string("*2\r\n$1\r\n0\r\n*1\r\n$-1\r\n"), std::string("$1\r\nv\r\n")}));
    auto& io = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io);
    ruvia::redis_client client(worker.loop(), {.host_ = "127.0.0.1", .port_ = peer.port(), .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1});
    ruvia::test::drive_backend_client(worker, client, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
        auto redis = connected.redis();
        const auto rejects_protocol = [&](auto&& operation) -> ruvia::task<void> {
            bool rejected = false;
            try {
                (void)co_await std::move(operation);
            } catch (const ruvia::redis_error& error) {
                rejected = error.code() == ruvia::redis_error::code_type::protocol_error;
            }
            EXPECT_TRUE(rejected);
        };
        co_await rejects_protocol(redis.lrange("list", 0, -1));
        co_await rejects_protocol(redis.smembers("set"));
        co_await rejects_protocol(redis.scan());
        // The complete reply was consumed, so the connection stays in step.
        const auto value = co_await redis.get("key");
        EXPECT_TRUE(value && *value == "v");
    });
    peer.join();
}

TEST(Ruvia, redis_set_result_copy_and_assignment_preserve_previous_value) {
    std::string text(4096, 'v');
    text[17] = '\0';
    scripted_redis_peer peer(std::vector<std::string>{redis_bulk_reply(text), "$0\r\n\r\n", "$-1\r\n"});
    auto& io = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io);
    ruvia::redis_client client(worker.loop(), {.host_ = "127.0.0.1", .port_ = peer.port(), .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1});
    ruvia::test::drive_backend_client(worker, client, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
        auto redis = connected.redis();
        const ruvia::redis_set_options return_previous{
            .condition_ = ruvia::redis_set_condition::if_absent,
            .previous_value_ = ruvia::redis_set_previous_value_policy::return_value};
        const auto with_text = co_await redis.set("key", "replacement", return_previous);
        const auto empty_text = co_await redis.set("key", "replacement", return_previous);
        const auto absent = co_await redis.set("key", "replacement", return_previous);

        const ruvia::redis_set_result copied(with_text);
        EXPECT_FALSE(copied.applied());
        EXPECT_TRUE(copied.previous().has_value());
        if (copied.previous().has_value()) {
            EXPECT_EQ(*copied.previous(), std::string_view(text));
        }

        auto target = absent;
        EXPECT_TRUE(target.applied());
        EXPECT_FALSE(target.previous().has_value());
        target = with_text;
        EXPECT_TRUE(target.previous().has_value());
        if (target.previous().has_value()) {
            EXPECT_EQ(*target.previous(), std::string_view(text));
        }
        target = empty_text;
        EXPECT_TRUE(target.previous().has_value());
        if (target.previous().has_value()) {
            EXPECT_TRUE(target.previous()->empty());
        }
        auto movable = with_text;
        target = std::move(movable);
        EXPECT_TRUE(target.previous().has_value());
        if (target.previous().has_value()) {
            EXPECT_EQ(*target.previous(), std::string_view(text));
        }
    });
    peer.join();
}

TEST(Ruvia, redis_typed_replies_retain_binary_payloads_and_recover_from_invalid_shapes) {
    std::string key(128, 'k');
    std::string payload(64 * 1024 + 1, 'v');
    key[17] = '\0';
    payload[2048] = '\0';
    const std::string_view expected_key(key);
    const std::string_view expected_payload(payload);
    const auto bulk_key = redis_bulk_reply(key);
    const auto bulk_payload = redis_bulk_reply(payload);
    const auto array = [](std::initializer_list<std::string_view> items) {
        std::string result("*" + std::string(std::to_string(items.size())) + "\r\n");
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
    scripted_redis_peer peer(std::vector<std::string>({std::string(bulk_payload), std::string(bulk_payload), std::string(bulk_payload),
        std::string(array({bulk_payload, "$-1\r\n", "$0\r\n\r\n"})), std::string(array({bulk_payload, bulk_key})),
        std::string(fields), std::string(scored), std::string(scan(array({bulk_key, bulk_payload}))), std::string(scan(fields)), std::string(scan(scored)), std::string(stream(fields)),
        std::string(array({bulk_payload, ":1\r\n"})), std::string(scan(array({bulk_key}))), std::string(stream(array({bulk_key}))),
        std::string(bulk_payload), std::string(":1\r\n"), std::string("$-1\r\n")}));
    auto& io = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io);
    ruvia::redis_client client(worker.loop(), {.host_ = "127.0.0.1", .port_ = peer.port(), .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1});
    ruvia::test::drive_backend_client(worker, client, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
        auto redis = connected.redis();
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
            EXPECT_TRUE(rejected);
        };
        co_await rejects_protocol(redis.lrange(key, 0, -1));
        co_await rejects_protocol(redis.hscan(key));
        co_await rejects_protocol(redis.xread_group("group", "consumer", streams));
        const auto after_errors = co_await redis.get(key);
        co_await rejects_protocol(redis.get(key));
        EXPECT_TRUE(!(co_await redis.get("missing")));

        EXPECT_TRUE(scalar && *scalar == expected_payload);
        EXPECT_EQ(status, expected_payload);
        EXPECT_TRUE(previous.applied() && previous.previous() && *previous.previous() == expected_payload);
        EXPECT_EQ(optional_values.size(), std::size_t{3});
        if (optional_values.size() == 3) {
            EXPECT_TRUE(optional_values[0] && *optional_values[0] == expected_payload);
            EXPECT_TRUE(!optional_values[1]);
            EXPECT_TRUE(optional_values[2] && optional_values[2]->empty());
        }
        EXPECT_EQ(list.size(), std::size_t{2});
        if (list.size() == 2) {
            EXPECT_TRUE(list[0] == expected_payload && list[1] == expected_key);
        }
        const auto check_fields = [&](std::span<const ruvia::redis_key_value> values) {
            EXPECT_EQ(values.size(), std::size_t{1});
            if (values.size() == 1) {
                EXPECT_TRUE(values[0].key() == expected_key && values[0].value() == expected_payload);
            }
        };
        const auto check_scores = [&](std::span<const ruvia::redis_scored_value> values) {
            EXPECT_EQ(values.size(), std::size_t{1});
            if (values.size() == 1) {
                EXPECT_TRUE(values[0].value() == expected_key && values[0].score() == 1.5);
            }
        };
        check_fields(hash);
        check_fields(hash_scan.entries());
        check_scores(sorted);
        check_scores(sorted_scan.entries());
        EXPECT_TRUE(scanned.done() && hash_scan.done() && sorted_scan.done());
        EXPECT_EQ(scanned.values().size(), std::size_t{2});
        if (scanned.values().size() == 2) {
            EXPECT_TRUE(scanned.values()[0] == expected_key && scanned.values()[1] == expected_payload);
        }
        EXPECT_TRUE(grouped && grouped->streams().size() == 1);
        if (grouped && grouped->streams().size() == 1) {
            const auto& result = grouped->streams()[0];
            EXPECT_TRUE(result.stream() == expected_key && result.entries().size() == 1);
            if (result.entries().size() == 1) {
                EXPECT_EQ(result.entries()[0].id(), std::string_view("1-0"));
                check_fields(result.entries()[0].fields());
            }
        }
        EXPECT_TRUE(after_errors && *after_errors == expected_payload);
    });
    peer.join();
}

}  // namespace redis_client_test
