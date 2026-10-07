#include <array>
#include <barrier>
#include <chrono>
#include <exception>
#include <future>
#include <istream>
#include <limits>
#include <string>
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

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/Timer.h"
#include "ruvia/web/detail/redis/RedisClientRuntime.h"
#include "ruvia/web/redis/RedisClient.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/DbClient.h"
#include "ruvia/web/detail/db/DbQueryCache.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#endif

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_tls_identity.h"

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
        kQueryCache,
        kRejectThenPing,
        batch_echo,
        transaction_success,
        transaction_aborted,
        transaction_exec_error,
        transaction_wrong_exec,
        transaction_queue_error,
        transaction_watch_error };

    explicit RedisPeer(Mode mode = Mode::kNormal, asio::ssl::context* tls_context = nullptr,
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
        return {.host = "127.0.0.1", .port = port_, .tls = {.mode = ruvia::client_tls_mode::disabled}, .poolSizePerWorker = 1, .connectTimeout = std::chrono::seconds(2), .commandTimeout = std::chrono::seconds(2)};
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

    std::vector<std::vector<std::string>> batch_commands() {
        return batch_commands_.get_future().get();
    }

private:
    template <typename Stream>
    asio::awaitable<std::string> line(Stream& stream) {
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
            if (mode_ != Mode::kRejectThenPing) {
                throw;
            }
        }
        if (mode_ == Mode::kRejectThenPing) {
            firstConnection_ = false;
            socket_ = asio::ip::tcp::socket(io_);
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            co_await serve_transport();
        }
    }

    asio::awaitable<void> serve_transport() {
        if (tls_context_ != nullptr) {
            asio::ssl::stream<asio::ip::tcp::socket&> stream(socket_, *tls_context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            co_await serveConnection(stream);
        } else {
            co_await serveConnection(socket_);
        }
    }

    template <typename Stream>
    asio::awaitable<void> serveConnection(Stream& stream) {
        bool in_transaction = false;
        std::vector<std::vector<std::string>> transaction_commands;
        std::vector<std::string> queued_replies;
        for (;;) {
            const auto header = co_await line(stream);
            const auto count = std::stoi(header.substr(1));
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
            const bool transaction_mode = mode_ == Mode::transaction_success ||
                                          mode_ == Mode::transaction_aborted ||
                                          mode_ == Mode::transaction_exec_error ||
                                          mode_ == Mode::transaction_wrong_exec ||
                                          mode_ == Mode::transaction_queue_error ||
                                          mode_ == Mode::transaction_watch_error;
            if (transaction_mode &&
                (in_transaction || args.front() == "WATCH" || args.front() == "UNWATCH" || args.front() == "MULTI")) {
                transaction_commands.push_back(args);
                if (args.front() == "WATCH") {
                    reply = mode_ == Mode::transaction_watch_error ? "-ERR watch failed\r\n" : "+OK\r\n";
                } else if (args.front() == "UNWATCH") {
                    reply = "+OK\r\n";
                } else if (args.front() == "MULTI") {
                    in_transaction = true;
                    reply = "+OK\r\n";
                } else if (args.front() == "EXEC") {
                    in_transaction = false;
                    batch_commands_.set_value(transaction_commands);
                    if (mode_ == Mode::transaction_aborted) {
                        reply = "*-1\r\n";
                    } else if (mode_ == Mode::transaction_exec_error) {
                        reply = "-EXECABORT discarded\r\n";
                    } else if (mode_ == Mode::transaction_wrong_exec) {
                        reply = "+OK\r\n";
                    } else {
                        reply = "*" + std::to_string(queued_replies.size()) + "\r\n";
                        for (const auto& queued : queued_replies) {
                            reply += queued;
                        }
                    }
                } else {
                    queued_replies.push_back("$" + std::to_string(args.back().size()) + "\r\n" + args.back() + "\r\n");
                    reply = mode_ == Mode::transaction_queue_error ? "-ERR queue failed\r\n" : "+QUEUED\r\n";
                }
            } else if (mode_ == Mode::batch_echo && args.front() != "PING") {
                reply = "$" + std::to_string(args.back().size()) + "\r\n" + args.back() + "\r\n";
            } else if ((mode_ == Mode::kCoalescedPings || mode_ == Mode::kCoalescedArrays) &&
                       args.front() == "PING") {
                if (co_await line(stream) != "*1" || co_await line(stream) != "$4" ||
                    co_await line(stream) != "PING") {
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
                reply = mode_ == Mode::kQueryCache
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

    Mode mode_;
    std::string cache_payload_;
    asio::ssl::context* tls_context_;
    bool firstConnection_{true};
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
    auto user = co_await users.findOne({.where = ClientUser::field<"id">() == "1"});
    RUVIA_CHECK(user.has_value());
    std::string mutablePing(128, 'm');
    auto coldPing = client.ping(mutablePing);
    mutablePing.assign(128, 'n');
    const auto ownedEcho = co_await std::move(coldPing);
    RUVIA_CHECK(ownedEcho == std::string_view(std::string(128, 'm')));
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

ruvia::Task<void> check_owned_pipeline(ruvia::RedisClient& client,
    ruvia::testing::TestContext& ruvia_ctx) {
    co_await client.connect();
    const std::string binary_key("key\0binary", 10);
    const std::string binary_value("value\0binary", 12);
    auto cold = [&] {
        auto pipeline = client.pipeline();
        auto key = binary_key;
        auto value = binary_value;
        pipeline.get(key).set(key, value).incrBy(key, std::numeric_limits<std::int64_t>::min()).zadd(key, 1.5, "member");
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
    auto next = client.pipeline();
    next.get(std::string(256, 'n'));
    const auto replies = co_await std::move(next).exec();
    RUVIA_CHECK_EQ(replies.size(), std::size_t{1});
    RUVIA_CHECK(retained[1].string() == std::string_view(binary_value));
    co_await client.shutdown();
    RUVIA_CHECK(retained[0].string() == std::string_view(binary_key));
}

ruvia::Task<void> check_owned_transaction(ruvia::RedisClient& client, RedisPeer::Mode mode,
    ruvia::testing::TestContext& ruvia_ctx) {
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
        RUVIA_CHECK(mode == RedisPeer::Mode::transaction_success);
        RUVIA_CHECK_EQ(retained.size(), std::size_t{2});
        RUVIA_CHECK(retained[0].string() == std::string_view(binary_value));
        RUVIA_CHECK(retained[1].string() == std::string_view(binary_key));
        co_await client.ping();
        co_await client.shutdown();
        RUVIA_CHECK(retained[0].string() == std::string_view(binary_value));
        accepted = true;
    } catch (const ruvia::RedisError& error) {
        const auto expected_code = mode == RedisPeer::Mode::transaction_aborted
                                       ? ruvia::RedisError::Code::kTransactionAborted
                                       : ruvia::RedisError::Code::kCommandError;
        RUVIA_CHECK(mode != RedisPeer::Mode::transaction_success);
        RUVIA_CHECK(error.code() == expected_code);
        if (mode == RedisPeer::Mode::transaction_queue_error) {
            RUVIA_CHECK(std::string_view(error.what()).contains("reply 4"));
            RUVIA_CHECK(std::string_view(error.what()).contains("queue failed"));
        } else if (mode == RedisPeer::Mode::transaction_watch_error) {
            RUVIA_CHECK(std::string_view(error.what()).contains("reply 0"));
            RUVIA_CHECK(std::string_view(error.what()).contains("watch failed"));
        } else if (mode == RedisPeer::Mode::transaction_exec_error) {
            RUVIA_CHECK(std::string_view(error.what()).contains("EXECABORT"));
        }
        accepted = true;
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

ruvia::Task<void> connectUntilStopped(ruvia::RedisClient& client, bool& cancelled) {
    try {
        co_await client.connect();
    } catch (const ruvia::RedisError&) {
        cancelled = true;
    }
}

ruvia::Task<void> checkExpiration(ruvia::RedisClient& client,
    std::chrono::system_clock::time_point expiresAt, ruvia::testing::TestContext& ruvia_ctx) {
    co_await client.connect();
    const bool applied = co_await client.expireAt("session", expiresAt);
    RUVIA_CHECK(applied);
    co_await client.shutdown();
}

ruvia::Task<void> checkRuntimeHandleTimeout(ruvia::EventLoop loop, ruvia::RedisConfig config,
    ruvia::testing::TestContext& ruvia_ctx) {
    config.commandTimeout.reset();
    auto worker = loop.handle();
    ruvia::detail::RedisClientRuntime runtime(loop.ioContext(), worker,
        ruvia::detail::RedisConfigStorage(config, std::pmr::get_default_resource()),
        std::pmr::get_default_resource());
    ruvia::operation_scope scope;
    co_await runtime.connect();
    auto handle = runtime.handle(scope, {.timeout = std::chrono::seconds(5)});
    auto copied = handle;
    auto inherited = copied.withOptions({});
    auto shortened = inherited.withOptions({.timeout = std::chrono::milliseconds(100)});
    auto derived = shortened.withOptions({.timeout = std::chrono::seconds(5)});
    auto coldOperation = derived.command("STALL");
    (void)co_await ruvia::sleepFor(worker, std::chrono::milliseconds(250));
    const auto started = std::chrono::steady_clock::now();
    bool commandTimedOut = false;
    try {
        (void)co_await std::move(coldOperation);
    } catch (const ruvia::RedisError& error) {
        commandTimedOut = error.code() == ruvia::RedisError::Code::kTimeout;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    RUVIA_CHECK(commandTimedOut);
    // Cold time must not consume the deadline; a longer override cannot relax it.
    RUVIA_CHECK(elapsed >= std::chrono::milliseconds(50));
    RUVIA_CHECK(elapsed < std::chrono::seconds(2));
    runtime.closeNow();
    co_await scope.close_and_join();
}

ruvia::Task<void> checkRuntimeHandleCancellationBridge(ruvia::EventLoop loop,
    ruvia::RedisConfig config, ruvia::StopSource& baseStop,
    ruvia::StopSource& overrideStop, ruvia::testing::TestContext& ruvia_ctx) {
    config.commandTimeout.reset();
    auto worker = loop.handle();
    ruvia::detail::RedisClientRuntime runtime(loop.ioContext(), worker,
        ruvia::detail::RedisConfigStorage(config, std::pmr::get_default_resource()),
        std::pmr::get_default_resource());
    ruvia::operation_scope scope;
    co_await runtime.connect();
    auto configured = runtime.handle(scope, {.stopToken = baseStop.token()});
    auto copied = configured;
    auto derived = copied.withOptions({.stopToken = overrideStop.token()});
    bool cancelled = false;
    try {
        (void)co_await derived.command("STALL");
    } catch (const ruvia::RedisError& error) {
        cancelled = error.code() == ruvia::RedisError::Code::kCancelled;
    }
    RUVIA_CHECK(cancelled);
    runtime.closeNow();
    co_await scope.close_and_join();
}

ruvia::Task<void> checkRuntimeMemory(ruvia::EventLoop loop, ruvia::RedisConfig config,
    ruvia::testing::TestContext& ruvia_ctx) {
    ruvia::test::CountingMemoryResource memory;
    {
        auto worker = loop.handle();
        ruvia::detail::RedisClientRuntime runtime(loop.ioContext(), worker,
            ruvia::detail::RedisConfigStorage(config, &memory), &memory);
        ruvia::operation_scope scope;
        co_await runtime.connect();
        auto handle = runtime.handle(scope, {.timeout = std::chrono::seconds(2)});
        auto copiedHandle = handle;
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
                auto cold = [&] {
                    auto pipeline = handle.pipeline();
                    pipeline.set(std::string(128, 'k'), std::string(128, 'v'));
                    return std::move(pipeline).exec();
                }();
            }
            RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
            {
                auto cold = [&] {
                    auto transaction = handle.transaction();
                    transaction.watch(std::string(128, 'w'))
                        .set(std::string(128, 'k'), std::string(128, 'v'));
                    return std::move(transaction).exec();
                }();
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
                (void)co_await copiedHandle.withOptions({.stopToken = stop.token()})
                    .get(std::string(128, 'c'));
            } catch (const ruvia::RedisError&) {
                cancelled = true;
            }
            RUVIA_CHECK(cancelled);
            RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
            for (const bool transaction_batch : {false, true}) {
                bool batch_cancelled = false;
                try {
                    auto configured = copiedHandle.withOptions({.stopToken = stop.token()});
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
                } catch (const ruvia::RedisError& error) {
                    batch_cancelled = error.code() == ruvia::RedisError::Code::kCancelled;
                }
                RUVIA_CHECK(batch_cancelled);
                RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
            }
            RUVIA_CHECK(*retained == std::string_view(std::string(128, 'v')));
        }
        RUVIA_CHECK(memory.allocationCount() > allocations);
        RUVIA_CHECK(memory.deallocationCount() > deallocations);
        retained.reset();
        RUVIA_CHECK(memory.liveAllocations() < baseline);
        co_await scope.close_and_join();
        auto inactiveHandle = runtime.handle(scope);
        bool inactiveRejected = false;
        try {
            (void)inactiveHandle.ping();
        } catch (const std::logic_error&) {
            inactiveRejected = true;
        }
        RUVIA_CHECK(inactiveRejected);
        runtime.closeNow();
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
}

}  // namespace

RUVIA_TEST(redis_client_runtime_handle_options_survive_copy_and_derivation) {
    RedisPeer peer;
    ruvia::EventLoopPool pool({.loopCount = 1});
    pool.start();
    pool.loop(0).start(checkRuntimeHandleTimeout(pool.loop(0), peer.config(), ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_client_runtime_handle_bridges_both_stop_tokens_after_copy_and_derivation) {
    ruvia::EventLoopPool pool({.loopCount = 1});
    pool.start();
    for (const bool stopBase : {true, false}) {
        RedisPeer peer;
        ruvia::StopSource baseStop;
        ruvia::StopSource overrideStop;
        auto operation = pool.loop(0).start(checkRuntimeHandleCancellationBridge(
            pool.loop(0), peer.config(), baseStop, overrideStop, ruvia_ctx));
        peer.waitForBlockedCommand();
        (stopBase ? baseStop : overrideStop).requestStop();
        operation.get();
    }
    pool.join();
}

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

RUVIA_TEST(redis_pipeline_owns_binary_input_and_retains_ordered_replies) {
    RedisPeer peer(RedisPeer::Mode::batch_echo);
    ruvia::EventLoopPool pool({.loopCount = 1});
    ruvia::RedisClient client(pool.loop(0), peer.config());
    pool.start();
    pool.loop(0).start(check_owned_pipeline(client, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_transaction_owns_framing_and_handles_exec_outcomes) {
    for (const auto mode : {RedisPeer::Mode::transaction_success,
             RedisPeer::Mode::transaction_aborted, RedisPeer::Mode::transaction_exec_error,
             RedisPeer::Mode::transaction_wrong_exec, RedisPeer::Mode::transaction_queue_error,
             RedisPeer::Mode::transaction_watch_error}) {
        RedisPeer peer(mode);
        ruvia::EventLoopPool pool({.loopCount = 1});
        ruvia::RedisClient client(pool.loop(0), peer.config());
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

RUVIA_TEST(redis_client_event_loop_stop_awaits_retirement_of_pending_authentication) {
    for (const bool useAttachmentRun : {false, true}) {
        RedisPeer peer;
        auto config = peer.config();
        config.password = "stall-authentication";
        asio::io_context io;
        auto attachment = ruvia::attachEventLoop(io);
        ruvia::RedisClient client(attachment.loop(), config);
        bool cancelled = false;
        auto root = attachment.loop().start(connectUntilStopped(client, cancelled));
        std::thread driver([&] {
            if (useAttachmentRun) {
                attachment.run();
            } else {
                io.run();
            }
        });
        peer.waitForBlockedCommand();
        attachment.stop();
        driver.join();
        root.get();
        RUVIA_CHECK(cancelled);
        RUVIA_CHECK(!client.worker().accepting());
    }
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

RUVIA_TEST(redis_tls_authenticates_identity_and_preserves_operation_memory) {
    ruvia::test::tls_identity identity("redis.test");
    RedisPeer peer(RedisPeer::Mode::kNormal, &identity.context);
    auto config = peer.config();
    config.tls = {.ca_file = identity.ca_file.string(), .server_name = "redis.test"};
    ruvia::EventLoopPool pool({.loopCount = 1});
    pool.start();
    pool.loop(0).start(checkRuntimeMemory(pool.loop(0), config, ruvia_ctx)).get();
    pool.join();
}

RUVIA_TEST(redis_tls_rejects_untrusted_and_wrong_name_before_authentication) {
    ruvia::test::tls_identity identity("redis.test");
    for (const bool trust_ca : {false, true}) {
        RedisPeer peer(RedisPeer::Mode::kNormal, &identity.context);
        auto config = peer.config();
        config.password = "must-not-send";
        config.tls = {.ca_file = trust_ca ? identity.ca_file.string() : "", .server_name = trust_ca ? "wrong.test" : "redis.test"};
        ruvia::EventLoopPool pool({.loopCount = 1});
        ruvia::RedisClient client(pool.loop(0), config);
        pool.start();
        bool rejected = false;
        try {
            pool.loop(0).start(client.connect()).get();
        } catch (const ruvia::RedisError& error) {
            rejected = error.code() == ruvia::RedisError::Code::kConnectFailed;
        }
        RUVIA_CHECK(rejected);
        pool.loop(0).start(client.shutdown()).get();
        pool.join();
    }
}

RUVIA_TEST(redis_tls_shutdown_joins_pending_authenticated_transport_io) {
    ruvia::test::tls_identity identity("redis.test");
    RedisPeer peer(RedisPeer::Mode::kNormal, &identity.context);
    auto config = peer.config();
    config.tls = {.ca_file = identity.ca_file.string(), .server_name = "redis.test"};
    ruvia::EventLoopPool pool({.loopCount = 1});
    ruvia::RedisClient client(pool.loop(0), config);
    {
        auto cold = client.connect();
    }
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
    pool.join();
    RUVIA_CHECK(cancelled);
}

#ifdef RUVIA_ENABLE_DATABASE
RUVIA_TEST(db_query_cache_reuses_connected_redis_and_leaves_its_pool_open) {
    using access = ruvia::detail::DbResultAccess;
    auto* resource = std::pmr::get_default_resource();
    auto rows = access::makeResult(resource);
    auto row = access::ownedRow(resource);
    access::ownedColumnNames(row).emplace_back("name");
    access::ownedFields(row).push_back(access::ownedField("cached", resource));
    access::rows(rows).push_back(std::move(row));
    auto bytes = ruvia::detail::encodeDbCacheRows(rows, resource);
    RedisPeer peer(RedisPeer::Mode::kQueryCache, nullptr, std::string(bytes));
    ruvia::EventLoopPool pool({.loopCount = 1});
    auto loop = pool.loop(0);
    ruvia::RedisClient redis(loop, peer.config());
    auto run = [&]() -> ruvia::Task<void> {
        co_await redis.connect();
        auto store = redis.withOptions({});
#ifdef RUVIA_ENABLE_POSTGRESQL
        ruvia::DbConfig config{.driver = ruvia::DbDriver::kPostgreSql};
#else
        ruvia::DbConfig config{.driver = ruvia::DbDriver::kMariaDb};
#endif
        ruvia::DbClient client(loop, config, store, ruvia::DbCacheConfig{});
        ruvia::test::CountingMemoryResource operation_memory;
        {
            ruvia::detail::DbRegistry databases(loop.ioContext(), redis.worker(),
                &operation_memory, config, store, ruvia::DbCacheConfig{});
            ruvia::operation_scope scope;
            auto database = databases.get(scope);
            ruvia::DbQuery query;
            query.select(query.column("name")).from("items").cache(true);
            const auto baseline = operation_memory.liveAllocations();
            for (int index = 0; index < 3; ++index) {
                {
                    const auto result = co_await database.query(query);
                    RUVIA_CHECK_EQ(result.size(), std::size_t{1});
                    RUVIA_CHECK_EQ(result[0]["name"].as<std::string_view>().value(), std::string_view("cached"));
                }
                RUVIA_CHECK_EQ(operation_memory.liveAllocations(), baseline);
            }
            databases.closeNow();
            co_await redis.ping();
        }
        RUVIA_CHECK_EQ(operation_memory.liveAllocations(), std::size_t{0});
        co_await client.shutdown();
        co_await redis.ping();
        co_await redis.shutdown();
    };
    pool.start();
    loop.start(run()).get();
    pool.join();
}
#endif
