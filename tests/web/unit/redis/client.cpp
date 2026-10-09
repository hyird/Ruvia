#include <array>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/redis/redis_client.h"
#include "ruvia/web/redis/redis_handle.h"

#include "backend_client_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

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

RUVIA_TEST(redis_blocking_commands_ignore_the_ordinary_pool_timeout_and_honor_operation_cancellation) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto worker = ruvia::attach_event_loop(io_context);
    ruvia::redis_config config;
    config.command_timeout_ = std::chrono::milliseconds(1);
    ruvia::test::with_connected_redis_client(worker, config, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        auto redis = client.with_options({});
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
                 {"CLIENT", "REPLY", "OFF"}, {"HELLO", "3"}, {"ASKING"}}) {
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
        auto handle = client.with_options({});
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
            transaction.watch(std::string(128, 'w')).unwatch().set(std::string(128, 'k'), std::string(128, 'v'));
            std::optional moved(std::move(transaction));
            RUVIA_CHECK(throws_logic_error([&] { transaction.watch("moved"); }));
            auto cold = std::move(*moved).exec();
            RUVIA_CHECK(throws_logic_error([&] { moved->unwatch(); }));
            RUVIA_CHECK(throws_logic_error([&] { moved->zadd("used", 1, "member"); }));
            moved.reset();
        }
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
        auto redis = client.with_options({});

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
        auto redis = client.with_options({});
        const std::span<const std::string_view> no_keys;

        RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.mget(no_keys); }));
        RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.sinter(no_keys); }));
        RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.sunion(no_keys); }));
        RUVIA_CHECK(throws_invalid_argument([&] { (void)redis.sdiff(no_keys); }));
        co_return;
    });
}
