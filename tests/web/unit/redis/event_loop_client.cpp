#include <array>
#include <barrier>
#include <chrono>
#include <exception>
#include <future>
#include <istream>
#include <string>
#include <thread>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/web/detail/redis/RedisClientRuntime.h"
#include "ruvia/web/redis/RedisClient.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_REDIS_ENTITY(ClientUser, "client_users",
    RUVIA_REDIS_COLUMN(id, ruvia::String, ruvia::RedisColumnOptions{.primaryKey = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::String))

// Deterministic RESP peer, not an external Redis dependency. All sockets and
// cancellation run on its own thread, including cleanup after a failed test.
class RedisPeer final {
public:
    enum class Mode { kNormal,
        kCoalescedPings,
        kCoalescedArrays,
        kHugeArray,
        kNestedArrays,
        kDeepArrays,
        kValidNestedArray,
        kRejectThenPing };

    explicit RedisPeer(Mode mode = Mode::kNormal)
        : mode_(mode),
          acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          socket_(io_),
          port_(acceptor_.local_endpoint().port()),
          work_(asio::make_work_guard(io_)),
          done_(asio::co_spawn(io_, serve(), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    ~RedisPeer() {
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

    ruvia::RedisConfig config() const {
        return {.host = "127.0.0.1", .port = port_, .poolSizePerWorker = 1, .connectTimeout = std::chrono::seconds(2), .commandTimeout = std::chrono::seconds(2)};
    }

    void waitForBlockedCommand() {
        auto blocked = blocked_.get_future();
        if (blocked.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("Redis test peer did not receive the command");
        }
        blocked.get();
    }

    std::vector<std::string> expirationCommand() {
        return expiration_.get_future().get();
    }

private:
    asio::awaitable<std::string> line() {
        co_await asio::async_read_until(socket_, buffer_, "\r\n", asio::use_awaitable);
        std::istream input(&buffer_);
        std::string value;
        std::getline(input, value);
        value.pop_back();
        co_return value;
    }

    asio::awaitable<void> serve() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        try {
            co_await serveConnection();
        } catch (const std::system_error&) {
            if (mode_ != Mode::kRejectThenPing) {
                throw;
            }
        }
        if (mode_ == Mode::kRejectThenPing) {
            firstConnection_ = false;
            socket_ = asio::ip::tcp::socket(io_);
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            co_await serveConnection();
        }
    }

    asio::awaitable<void> serveConnection() {
        for (;;) {
            const auto header = co_await line();
            const auto count = std::stoi(header.substr(1));
            std::vector<std::string> args;
            for (int index = 0; index < count; ++index) {
                const auto bulk = co_await line();
                const auto size = static_cast<std::size_t>(std::stoul(bulk.substr(1)));
                if (buffer_.size() < size + 2) {
                    co_await asio::async_read(socket_, buffer_,
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
            if ((mode_ == Mode::kCoalescedPings || mode_ == Mode::kCoalescedArrays) &&
                args.front() == "PING") {
                if (co_await line() != "*1" || co_await line() != "$4" ||
                    co_await line() != "PING") {
                    throw std::runtime_error("expected second pipelined PING");
                }
                reply = mode_ == Mode::kCoalescedArrays
                            ? "*3\r\n+\r\n+\r\n+\r\n*3\r\n+\r\n+\r\n+\r\n"
                            : "+PONG\r\n+PONG\r\n";
            } else if ((mode_ == Mode::kHugeArray ||
                           (mode_ == Mode::kRejectThenPing && firstConnection_)) &&
                       args.front() == "PING") {
                reply = "*1048576\r\n";
            } else if (mode_ == Mode::kNestedArrays && args.front() == "PING") {
                for (int index = 0; index < 100; ++index) {
                    reply.append("*300\r\n");
                }
            } else if (mode_ == Mode::kDeepArrays && args.front() == "PING") {
                for (int index = 0; index < 5; ++index) {
                    reply.append("*1\r\n");
                }
            } else if (mode_ == Mode::kValidNestedArray && args.front() == "PING") {
                for (int index = 0; index < 4; ++index) {
                    reply.append("*1\r\n");
                }
                reply.append("+PONG\r\n");
            } else if (args.front() == "PING") {
                reply = args.size() == 1 ? "+PONG\r\n" : "$" + std::to_string(args[1].size()) + "\r\n" + args[1] + "\r\n";
            } else if (args.front() == "GET") {
                reply = "$128\r\n" + std::string(128, 'v') + "\r\n";
            } else if (args.front() == "EXPIREAT" || args.front() == "PEXPIREAT") {
                expiration_.set_value(args);
                reply = ":1\r\n";
            } else if (args.front() == "HGETALL") {
                reply = "*6\r\n$14\r\n__ruvia_entity\r\n$1\r\n1\r\n$2\r\nid\r\n$1\r\n1\r\n$4\r\nname\r\n$128\r\n" + std::string(128, 'n') + "\r\n";
            } else {
                reply = "-ERR test error\r\n";
            }
            co_await asio::async_write(socket_, asio::buffer(reply), asio::use_awaitable);
        }
    }

    Mode mode_;
    bool firstConnection_{true};
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    std::uint16_t port_;
    asio::streambuf buffer_;
    std::promise<void> blocked_;
    std::promise<std::vector<std::string>> expiration_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::future<void> done_;
    std::thread thread_;
};

ruvia::Task<void> checkCommands(ruvia::RedisClient& client, ruvia::testing::TestContext& ruvia_ctx) {
    co_await client.connect();
    bool duplicateRejected = false;
    try {
        co_await client.connect();
    } catch (const std::logic_error&) {
        duplicateRejected = true;
    }
    RUVIA_CHECK(duplicateRejected);
    // A rejected second connect must not tear down the successful first one.
    co_await client.ping();
    auto retained = co_await client.get("retained");
    RUVIA_CHECK(retained.has_value());
    auto users = client.getRepository<ClientUser>();
    auto user = co_await users.findOne({.where = ClientUser::column<"id">() == "1"});
    RUVIA_CHECK(user.has_value());
    for (int index = 0; index < 32; ++index) {
        auto cold = client.get(std::string(128, 'k'));
        (void)cold;
        auto echo = co_await client.ping(std::string(128, 'x'));
        RUVIA_CHECK(echo == std::string_view(std::string(128, 'x')));
        RUVIA_CHECK(*retained == std::string_view(std::string(128, 'v')));
        RUVIA_CHECK(user->get<"name">().view() == std::string_view(std::string(128, 'n')));
    }
    bool failed = false;
    try {
        (void)co_await client.set("bad", "value");
    } catch (const ruvia::RedisError&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    co_await client.ping();
    auto handle = client.withOptions({});
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

ruvia::Task<void> checkCoalescedReplies(ruvia::RedisClient& client,
    ruvia::testing::TestContext& ruvia_ctx, bool arrays = false) {
    co_await client.connect();
    auto pipeline = client.pipeline();
    pipeline.command("PING").command("PING");
    bool accepted = false;
    try {
        const auto replies = co_await std::move(pipeline).exec();
        accepted = replies.size() == 2;
        if (accepted && arrays) {
            accepted = replies[0].kind() == ruvia::RedisValue::Kind::kArray &&
                       replies[1].kind() == ruvia::RedisValue::Kind::kArray &&
                       replies[0].array().size() == 3 && replies[1].array().size() == 3;
        } else if (accepted) {
            accepted = replies[0].string() == "PONG" && replies[1].string() == "PONG";
        }
    } catch (const ruvia::RedisError&) {
    }
    RUVIA_CHECK(accepted);
    co_await client.shutdown();
}

ruvia::Task<void> checkOversizedReply(ruvia::RedisClient& client,
    ruvia::testing::TestContext& ruvia_ctx) {
    co_await client.connect();
    bool rejected = false;
    try {
        (void)co_await client.get("large");
    } catch (const ruvia::RedisError& error) {
        rejected = error.code() == ruvia::RedisError::Code::kProtocolError;
    }
    RUVIA_CHECK(rejected);
    co_await client.shutdown();
}

ruvia::Task<void> checkHugeDeclaredArray(ruvia::RedisClient& client,
    ruvia::testing::TestContext& ruvia_ctx) {
    co_await client.connect();
    bool rejected = false;
    try {
        co_await client.ping();
    } catch (const ruvia::RedisError& error) {
        rejected = error.code() == ruvia::RedisError::Code::kProtocolError;
    }
    RUVIA_CHECK(rejected);
    co_await client.shutdown();
}

ruvia::Task<void> checkNestedArrayAtDepthLimit(ruvia::RedisClient& client,
    ruvia::testing::TestContext& ruvia_ctx) {
    co_await client.connect();
    const std::string_view command[]{"PING"};
    const auto reply = co_await client.command(std::span<const std::string_view>(command));
    const ruvia::RedisValue* value = &reply;
    bool valid = true;
    for (int index = 0; index < 4; ++index) {
        if (value->kind() != ruvia::RedisValue::Kind::kArray || value->array().size() != 1) {
            valid = false;
            break;
        }
        value = &value->array()[0];
    }
    RUVIA_CHECK(valid && value->kind() == ruvia::RedisValue::Kind::kString &&
                value->string() == "PONG");
    co_await client.shutdown();
}

ruvia::Task<void> checkReconnectAfterBudgetRejection(ruvia::RedisClient& client,
    ruvia::testing::TestContext& ruvia_ctx) {
    co_await client.connect();
    bool rejected = false;
    try {
        co_await client.ping();
    } catch (const ruvia::RedisError& error) {
        rejected = error.code() == ruvia::RedisError::Code::kProtocolError;
    }
    RUVIA_CHECK(rejected);
    co_await client.ping();
    co_await client.shutdown();
}

ruvia::Task<void> blockedCommand(ruvia::RedisClient& client) {
    co_await client.connect();
    (void)co_await client.command("STALL");
}

ruvia::Task<void> checkExpiration(ruvia::RedisClient& client,
    std::chrono::system_clock::time_point expiresAt, ruvia::testing::TestContext& ruvia_ctx) {
    co_await client.connect();
    const bool applied = co_await client.expireAt("session", expiresAt);
    RUVIA_CHECK(applied);
    co_await client.shutdown();
}

ruvia::Task<void> checkRuntimeMemory(ruvia::EventLoop loop, ruvia::RedisConfig config,
    ruvia::testing::TestContext& ruvia_ctx) {
    ruvia::test::CountingMemoryResource memory;
    {
        auto worker = loop.handle();
        ruvia::detail::RedisClientRuntime runtime(loop.ioContext(), worker,
            ruvia::detail::RedisConfigStorage(config, &memory), &memory);
        ruvia::detail::ScopedOperationScope scope;
        co_await runtime.connect();
        auto handle = runtime.handle(scope);
        // Warm connection buffers, then retain a result across later calls.
        {
            auto warm = co_await handle.ping(std::string(256, 'w'));
        }
        auto retained = co_await handle.get("retained");
        const auto baseline = memory.liveAllocations();
        const auto allocations = memory.allocationCount();
        const auto deallocations = memory.deallocationCount();
        for (int index = 0; index < 32; ++index) {
            {
                auto cold = handle.get(std::string(128, 'k'));
            }
            RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
            {
                auto result = co_await handle.ping(std::string(128, 'x'));
            }
            RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
            bool failed = false;
            try {
                (void)co_await handle.set("bad", "value");
            } catch (const ruvia::RedisError&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
            ruvia::StopSource stop;
            stop.requestStop();
            bool cancelled = false;
            try {
                (void)co_await handle.withOptions({.stopToken = stop.token()}).get(std::string(128, 'c'));
            } catch (const ruvia::RedisError&) {
                cancelled = true;
            }
            RUVIA_CHECK(cancelled);
            RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
            RUVIA_CHECK(*retained == std::string_view(std::string(128, 'v')));
        }
        RUVIA_CHECK(memory.allocationCount() > allocations);
        RUVIA_CHECK(memory.deallocationCount() > deallocations);
        retained.reset();
        RUVIA_CHECK(memory.liveAllocations() < baseline);
        runtime.closeNow();
        co_await scope.closeAndJoin();
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
}

}  // namespace

RUVIA_TEST(redis_client_runtime_reclaims_operations_independently_of_retained_results) {
    RedisPeer peer;
    ruvia::EventLoopPool pool({.loopCount = 1});
    pool.start();
    pool.loop(0).start(checkRuntimeMemory(pool.loop(0), peer.config(), ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_client_runs_on_its_event_loop_and_retains_results) {
    RedisPeer peer;
    ruvia::EventLoopPool pool({.loopCount = 2});
    ruvia::RedisClient client(pool.loop(0), peer.config());
    pool.start();
    auto wrongLoop = pool.loop(1).start(client.connect());
    bool rejected = false;
    try {
        wrongLoop.get();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    pool.loop(0).start(checkCommands(client, ruvia_ctx)).get();
    pool.stop();
    pool.join();
}

RUVIA_TEST(redis_pipeline_reply_limit_counts_each_reply_not_the_tcp_batch) {
    RedisPeer peer(RedisPeer::Mode::kCoalescedPings);
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto config = peer.config();
    config.maxReplyBytes = 7;
    ruvia::RedisClient client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(checkCoalescedReplies(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_pipeline_array_element_budget_resets_for_each_reply) {
    RedisPeer peer(RedisPeer::Mode::kCoalescedArrays);
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto config = peer.config();
    config.maxReplyBytes = 13;
    ruvia::RedisClient client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(checkCoalescedReplies(client, ruvia_ctx, true)).get();
    pool.join();
}

RUVIA_TEST(redis_client_reply_limit_still_rejects_oversized_single_reply) {
    RedisPeer peer;
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto config = peer.config();
    config.maxReplyBytes = 7;
    ruvia::RedisClient client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(checkOversizedReply(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_reply_budget_rejects_impossible_declared_array_before_its_elements_arrive) {
    RedisPeer peer(RedisPeer::Mode::kHugeArray);
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto config = peer.config();
    config.maxReplyBytes = 1024;
    ruvia::RedisClient client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(checkHugeDeclaredArray(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_reply_budget_rejects_aggregate_nested_array_declarations) {
    RedisPeer peer(RedisPeer::Mode::kNestedArrays);
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto config = peer.config();
    config.maxReplyBytes = 1024;
    ruvia::RedisClient client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(checkHugeDeclaredArray(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_reply_depth_rejects_incomplete_nested_array) {
    RedisPeer peer(RedisPeer::Mode::kDeepArrays);
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto config = peer.config();
    config.maxArrayDepth = 4;
    ruvia::RedisClient client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(checkHugeDeclaredArray(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_reply_array_at_the_depth_limit_is_accepted) {
    RedisPeer peer(RedisPeer::Mode::kValidNestedArray);
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto config = peer.config();
    config.maxArrayDepth = 4;
    ruvia::RedisClient client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(checkNestedArrayAtDepthLimit(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_reader_budget_rebinds_after_protocol_failure_and_reconnect) {
    RedisPeer peer(RedisPeer::Mode::kRejectThenPing);
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto config = peer.config();
    config.maxReplyBytes = 1024;
    ruvia::RedisClient client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(checkReconnectAfterBudgetRejection(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_client_expire_at_preserves_the_requested_deadline) {
    RedisPeer peer;
    ruvia::EventLoopPool pool({.loopCount = 1});
    ruvia::RedisClient client(pool.loop(0), peer.config());
    pool.start();
    const auto expiresAt = std::chrono::system_clock::time_point{
                               std::chrono::seconds(2'000'000'000)} +
                           std::chrono::microseconds(1'001);
    pool.loop(0).start(checkExpiration(client, expiresAt, ruvia_ctx)).get();
    const auto command = peer.expirationCommand();
    pool.join();
    RUVIA_CHECK_EQ(command, (std::vector<std::string>{"PEXPIREAT", "session", "2000000000002"}));
}

RUVIA_TEST(redis_client_loop_stop_cancels_and_joins_pending_commands) {
    RedisPeer peer;
    ruvia::EventLoopPool pool({.loopCount = 1});
    ruvia::RedisClient client(pool.loop(0), peer.config());
    pool.start();
    auto pending = pool.loop(0).start(blockedCommand(client));
    peer.waitForBlockedCommand();
    pool.stop();
    bool cancelled = false;
    try {
        pending.get();
    } catch (const ruvia::RedisError&) {
        cancelled = true;
    }
    pool.join();
    RUVIA_CHECK(cancelled);
}

RUVIA_TEST(redis_client_close_from_another_thread_cancels_pending_commands) {
    RedisPeer peer;
    ruvia::EventLoopPool pool({.loopCount = 1});
    ruvia::RedisClient client(pool.loop(0), peer.config());
    pool.start();
    auto pending = pool.loop(0).start(blockedCommand(client));
    peer.waitForBlockedCommand();
    client.close();
    bool cancelled = false;
    try {
        pending.get();
    } catch (const ruvia::RedisError&) {
        cancelled = true;
    }
    pool.loop(0).start(client.shutdown()).get();
    pool.stop();
    pool.join();
    RUVIA_CHECK(cancelled);
}

RUVIA_TEST(redis_client_shutdown_joins_an_inflight_connect) {
    RedisPeer peer;
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto config = peer.config();
    config.password = "stall-authentication";
    ruvia::RedisClient client(pool.loop(0), config);
    pool.start();
    auto connecting = pool.loop(0).start(client.connect());
    peer.waitForBlockedCommand();
    bool duplicateRejected = false;
    try {
        pool.loop(0).start(client.connect()).get();
    } catch (const std::logic_error&) {
        duplicateRejected = true;
    }
    RUVIA_CHECK(duplicateRejected);
    auto closing = pool.loop(0).start(client.shutdown());
    bool cancelled = false;
    try {
        connecting.get();
    } catch (const ruvia::RedisError&) {
        cancelled = true;
    }
    closing.get();
    pool.join();
    RUVIA_CHECK(cancelled);
}

RUVIA_TEST(redis_client_fresh_close_and_loop_stop_share_worker_completion) {
    for (int iteration = 0; iteration < 64; ++iteration) {
        ruvia::EventLoopPool pool({.loopCount = 1});
        ruvia::RedisClient client(pool.loop(0), {.poolSizePerWorker = 1});
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
    ruvia::EventLoopPool pool({.loopCount = 1});
    {
        ruvia::RedisClient client(pool.loop(0), {.host = "127.0.0.1"});
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
    ruvia::EventLoopPool pool({.loopCount = 1});
    ruvia::RedisClient client(pool.loop(0), {.host = "127.0.0.1",
                                                .port = unavailable.local_endpoint().port(),
                                                .poolSizePerWorker = 1,
                                                .connectTimeout = std::chrono::seconds(1)});
    pool.start();
    bool failed = false;
    try {
        pool.loop(0).start(client.connect()).get();
    } catch (const ruvia::RedisError&) {
        failed = true;
    }
    pool.loop(0).start(client.shutdown()).get();
    pool.join();
    RUVIA_CHECK(failed);
}
