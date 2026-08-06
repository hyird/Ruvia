#include <atomic>
#include <array>
#include <chrono>
#include <exception>
#include <future>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/web/DataAccess.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/detail/db/DbHostResolution.h"
#endif

namespace {

static_assert(!std::is_copy_constructible_v<ruvia::DataAccessService>);
static_assert(!std::is_move_constructible_v<ruvia::DataAccessService>);
static_assert(!std::is_copy_constructible_v<ruvia::DataAccessContext>);
static_assert(!std::is_move_constructible_v<ruvia::DataAccessContext>);

template <typename T>
concept ExposesUntrackedContext = requires(T& runtime) { runtime.context(); };

template <typename T>
concept ExposesInertRequestMemoryTuning = requires(T& options) { options.memory; };

static_assert(!ExposesUntrackedContext<ruvia::DataAccessService>);
static_assert(!ExposesInertRequestMemoryTuning<ruvia::DataAccessOptions>);

#if defined(RUVIA_ENABLE_DATABASE) || defined(RUVIA_ENABLE_REDIS)
class StalledTcpServer final {
public:
    StalledTcpServer()
        : acceptor_(ioContext_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          acceptedFuture_(accepted_.get_future()),
          releaseFuture_(release_.get_future()),
          thread_([this] { run(); }) {}

    ~StalledTcpServer() {
        try {
            release_.set_value();
        } catch (...) {
        }
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    StalledTcpServer(const StalledTcpServer&) = delete;
    StalledTcpServer& operator=(const StalledTcpServer&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void waitUntilAccepted() {
        acceptedFuture_.get();
    }

private:
    void run() noexcept {
        try {
            asio::ip::tcp::socket socket(ioContext_);
            acceptor_.accept(socket);
            accepted_.set_value();
            releaseFuture_.wait();
            std::error_code ignored;
            socket.close(ignored);
        } catch (...) {
            try {
                accepted_.set_exception(std::current_exception());
            } catch (...) {
            }
        }
    }

    asio::io_context ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::promise<void> accepted_;
    std::future<void> acceptedFuture_;
    std::promise<void> release_;
    std::future<void> releaseFuture_;
    std::thread thread_;
};
#endif

#ifdef RUVIA_ENABLE_DATABASE
int testResolvedDatabaseHostLists() {
    std::pmr::monotonic_buffer_resource memory;
    const std::array endpoints{asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 3306), asio::ip::tcp::endpoint(asio::ip::make_address("::1"), 3306), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 3306)};
    const auto results = asio::ip::tcp::resolver::results_type::create(endpoints.begin(), endpoints.end(), "database.internal", "3306");
    const auto addresses = ruvia::detail::collectDbResolvedAddresses(results, &memory);
    if (addresses.size() != 2 || addresses[0] != "127.0.0.1" || addresses[1] != "::1") {
        return 1;
    }

    const auto mariaHosts = ruvia::detail::makeMariaDbResolvedHostList(addresses, &memory);
    if (mariaHosts != "127.0.0.1,[::1]") {
        return 2;
    }
    const auto singleMariaIpv6 = ruvia::detail::makeMariaDbResolvedHostList(std::span(addresses).subspan(1, 1), &memory);
    if (singleMariaIpv6 != "::1") {
        return 3;
    }

    const auto postgresHosts = ruvia::detail::makePostgreSqlResolvedHostList("database.internal", addresses, &memory);
    return postgresHosts.hosts == "database.internal,database.internal" && postgresHosts.addresses == "127.0.0.1,::1" ? 0 : 4;
}
#endif

int testWorkerAffinityAndAutomaticShutdown() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 4});
    const auto loop = loops.loop(0);

    std::promise<std::exception_ptr> completed;
    auto completedFuture = completed.get_future();
    auto options = ruvia::DataAccessOptions{};
    options.failureHandler = [&completed](std::exception_ptr failure) { completed.set_value(std::move(failure)); };
    ruvia::DataAccessService service(loop, std::move(options));

    try {
        (void)service.post([](ruvia::DataAccessContext&) -> ruvia::Task<void> { co_return; });
        return 1;
    } catch (const std::logic_error&) {
    }

    auto ready = service.connect();
    loops.start();
    ready.get();

    try {
        service.connect().get();
        return 2;
    } catch (const std::logic_error& error) {
        if (std::string_view(error.what()) != "data access service can only connect once") {
            throw;
        }
    }

    std::atomic_bool contextRanOnWorker{false};
    const auto posted = service.post([&completed, &contextRanOnWorker, &service](ruvia::DataAccessContext& context) -> ruvia::Task<void> {
        contextRanOnWorker.store(context.worker().isCurrent() && context.worker().id() == service.worker().id() && context.resource() != nullptr && !context.stopToken().stopRequested(), std::memory_order_release);

#ifdef RUVIA_ENABLE_DATABASE
        try {
            (void)context.db();
            throw std::runtime_error("an unconfigured worker database unexpectedly resolved");
        } catch (const std::logic_error&) {
        }
#endif
#ifdef RUVIA_ENABLE_REDIS
        try {
            (void)context.redis();
            throw std::runtime_error("an unconfigured worker redis pool unexpectedly resolved");
        } catch (const ruvia::RedisError& error) {
            if (error.code() != ruvia::RedisError::Code::kNotConfigured) {
                throw;
            }
        }
#endif

        completed.set_value(nullptr);
        co_return;
    });
    if (!posted.accepted()) {
        return 3;
    }

    const auto failure = completedFuture.get();
    loops.stop();
    loops.join();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    const auto stats = service.stats();
    return contextRanOnWorker.load(std::memory_order_acquire) && stats.accepted == 1 && stats.completed == 1 && stats.failed == 0 && stats.outstanding == 0 ? 0 : 4;
}

int testApplicationOwnedEventLoop() {
    asio::io_context ioContext;
    auto attachment = ruvia::attachEventLoop(ioContext, {.mailboxCapacity = 4});
    ruvia::DataAccessService service(attachment.loop());

    auto ready = service.connect();
    std::promise<bool> ranOnAttachedWorker;
    auto ranFuture = ranOnAttachedWorker.get_future();
    std::thread runner([&ioContext] { ioContext.run(); });

    std::exception_ptr failure;
    try {
        ready.get();
        if (service.post([&ranOnAttachedWorker](ruvia::DataAccessContext& context) -> ruvia::Task<void> {
                ranOnAttachedWorker.set_value(context.worker().isCurrent() && context.resource() != nullptr && !context.stopToken().stopRequested());
                co_return;
            }) != ruvia::PostStatus::kAccepted) {
            throw std::runtime_error("attached event loop rejected data access job");
        }
        if (!ranFuture.get()) {
            throw std::runtime_error("data access job did not run on attached event loop");
        }
    } catch (...) {
        failure = std::current_exception();
    }

    attachment.stop();
    runner.join();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    const auto stats = service.stats();
    return stats.accepted == 1 && stats.completed == 1 && stats.outstanding == 0 ? 0 : 1;
}

int testExplicitClose() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 4});
    const auto loop = loops.loop(0);

    std::promise<std::exception_ptr> completed;
    auto completedFuture = completed.get_future();
    auto options = ruvia::DataAccessOptions{};
    options.failureHandler = [&completed](std::exception_ptr failure) { completed.set_value(std::move(failure)); };
    ruvia::DataAccessService service(loop, std::move(options));
    auto ready = service.connect();
    loops.start();
    ready.get();

    std::atomic_bool closed{false};
    if (service.post([&service, &completed, &closed](ruvia::DataAccessContext& context) -> ruvia::Task<void> {
            service.close();
            closed.store(service.worker().isCurrent() && context.stopToken().stopRequested(), std::memory_order_release);
            completed.set_value(nullptr);
            co_return;
        }) != ruvia::PostStatus::kAccepted) {
        return 1;
    }

    const auto failure = completedFuture.get();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    if (service.post([](ruvia::DataAccessContext&) -> ruvia::Task<void> { co_return; }) != ruvia::PostStatus::kWorkerStopping) {
        return 2;
    }

    loops.stop();
    loops.join();
    const auto stats = service.stats();
    return closed.load(std::memory_order_acquire) && stats.accepted == 1 && stats.workerStopping == 1 && stats.completed == 1 && stats.outstanding == 0 ? 0 : 3;
}

int testFailureReporting() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 4});
    ruvia::DataAccessService service(loops.loop(0));

    auto ready = service.connect();
    loops.start();
    ready.get();
    if (service.post([](ruvia::DataAccessContext&) -> ruvia::Task<void> {
            throw std::runtime_error("data access job failed");
            co_return;
        }) != ruvia::PostStatus::kAccepted) {
        return 1;
    }

    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        const auto stats = service.stats();
        return std::string_view(error.what()) == "data access job failed" && stats.accepted == 1 && stats.completed == 1 && stats.failed == 1 && stats.outstanding == 0 ? 0 : 2;
    }
    return 3;
}

int testHandledFailureKeepsWorkerUsable() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 4});
    std::promise<std::exception_ptr> failed;
    auto failedFuture = failed.get_future();
    auto options = ruvia::DataAccessOptions{};
    options.failureHandler = [&failed](std::exception_ptr failure) { failed.set_value(std::move(failure)); };
    ruvia::DataAccessService service(loops.loop(0), std::move(options));

    auto ready = service.connect();
    loops.start();
    ready.get();
    if (service.post([](ruvia::DataAccessContext&) -> ruvia::Task<void> {
            throw std::runtime_error("handled data access failure");
            co_return;
        }) != ruvia::PostStatus::kAccepted) {
        return 1;
    }

    try {
        std::rethrow_exception(failedFuture.get());
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) != "handled data access failure") {
            return 2;
        }
    }

    std::promise<void> recovered;
    auto recoveredFuture = recovered.get_future();
    if (service.post([&recovered](ruvia::DataAccessContext&) -> ruvia::Task<void> {
            recovered.set_value();
            co_return;
        }) != ruvia::PostStatus::kAccepted) {
        return 3;
    }
    recoveredFuture.get();
    loops.stop();
    loops.join();

    const auto stats = service.stats();
    return stats.accepted == 2 && stats.completed == 2 && stats.failed == 1 && stats.outstanding == 0 ? 0 : 4;
}

int testStopBeforeConnectPublishesClosedGate() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 2});
    ruvia::DataAccessService service(loops.loop(0));

    loops.stop();
    try {
        service.connect().get();
        return 1;
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) != "worker stopped before data integrations connected") {
            throw;
        }
    }

    auto rejected = service.post([](ruvia::DataAccessContext&) -> ruvia::Task<void> { co_return; });
    if (rejected != ruvia::PostStatus::kWorkerStopping || rejected.rejected() == nullptr) {
        return 2;
    }

    loops.join();
    const auto stats = service.stats();
    return stats.accepted == 0 && stats.workerStopping == 1 && stats.outstanding == 0 ? 0 : 3;
}

#ifdef RUVIA_ENABLE_DATABASE
int testStopWhileDatabaseConnectWaits(ruvia::DbConfig config) {
    StalledTcpServer server;
    config.host = "127.0.0.1";
    config.port = server.port();

    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 2});
    auto options = ruvia::DataAccessOptions{};
    options.databases.push_back(ruvia::DataAccessDatabaseConfig{"default", std::move(config)});
    ruvia::DataAccessService service(loops.loop(0), std::move(options));

    auto ready = service.connect();
    loops.start();
    server.waitUntilAccepted();
    loops.stop();
    loops.join();

    if (ready.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return 1;
    }
    try {
        ready.get();
        return 2;
    } catch (const std::exception&) {
    }
    return service.stats().outstanding == 0 ? 0 : 3;
}
#endif

#ifdef RUVIA_ENABLE_REDIS
class SlowRedisReplyServer final {
public:
    SlowRedisReplyServer()
        : acceptor_(ioContext_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          thread_([this] { run(); }) {}

    ~SlowRedisReplyServer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    SlowRedisReplyServer(const SlowRedisReplyServer&) = delete;
    SlowRedisReplyServer& operator=(const SlowRedisReplyServer&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    void run() noexcept {
        try {
            asio::ip::tcp::socket socket(ioContext_);
            acceptor_.accept(socket);

            std::array<char, 256> command{};
            std::error_code error;
            (void)socket.read_some(asio::buffer(command), error);
            if (error) {
                return;
            }

            constexpr std::string_view response = "+PONG\r\n";
            for (const char byte : response) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                (void)socket.write_some(asio::buffer(&byte, 1), error);
                if (error) {
                    return;
                }
            }
        } catch (...) {
        }
    }

    asio::io_context ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

class DelayedRedisStartupServer final {
public:
    DelayedRedisStartupServer()
        : acceptor_(ioContext_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          thread_([this] { run(); }) {}

    ~DelayedRedisStartupServer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    DelayedRedisStartupServer(const DelayedRedisStartupServer&) = delete;
    DelayedRedisStartupServer& operator=(const DelayedRedisStartupServer&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    [[nodiscard]] static bool readCommand(asio::ip::tcp::socket& socket, std::size_t bytes) noexcept {
        std::array<char, 64> command{};
        std::error_code error;
        (void)asio::read(socket, asio::buffer(command.data(), bytes), error);
        return !error;
    }

    [[nodiscard]] static bool writeOk(asio::ip::tcp::socket& socket) noexcept {
        constexpr std::string_view response = "+OK\r\n";
        std::error_code error;
        (void)asio::write(socket, asio::buffer(response), error);
        return !error;
    }

    void run() noexcept {
        try {
            asio::ip::tcp::socket socket(ioContext_);
            acceptor_.accept(socket);
            constexpr std::string_view auth = "*2\r\n$4\r\nAUTH\r\n$6\r\nsecret\r\n";
            constexpr std::string_view select = "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n";

            if (!readCommand(socket, auth.size())) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            if (!writeOk(socket) || !readCommand(socket, select.size())) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            (void)writeOk(socket);
        } catch (...) {
        }
    }

    asio::io_context ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

class CancelledRedisBlockServer final {
public:
    CancelledRedisBlockServer()
        : acceptor_(ioContext_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          commandReadFuture_(commandRead_.get_future()),
          thread_([this] { run(); }) {}

    ~CancelledRedisBlockServer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void waitUntilBlocked() {
        commandReadFuture_.get();
    }

private:
    [[nodiscard]] static bool readExact(asio::ip::tcp::socket& socket, std::size_t size) {
        std::vector<char> input(size);
        std::error_code error;
        (void)asio::read(socket, asio::buffer(input), error);
        return !error;
    }

    void run() noexcept {
        try {
            asio::ip::tcp::socket blockedSocket(ioContext_);
            acceptor_.accept(blockedSocket);
            constexpr std::string_view xread = "*8\r\n$10\r\nXREADGROUP\r\n$5\r\nGROUP\r\n$1\r\ng\r\n$1\r\nc\r\n$5\r\nBLOCK\r\n$1\r\n0\r\n$7\r\nSTREAMS\r\n$6\r\nevents\r\n$1\r\n>\r\n";
            if (!readExact(blockedSocket, xread.size())) {
                throw std::runtime_error("failed to read blocking redis command");
            }
            commandRead_.set_value();

            std::array<char, 1> ignoredByte{};
            std::error_code closedError;
            (void)blockedSocket.read_some(asio::buffer(ignoredByte), closedError);
            if (!closedError) {
                throw std::runtime_error("cancelled redis socket remained open");
            }

            asio::ip::tcp::socket reconnected(ioContext_);
            acceptor_.accept(reconnected);
            constexpr std::string_view ping = "*1\r\n$4\r\nPING\r\n";
            if (!readExact(reconnected, ping.size())) {
                throw std::runtime_error("failed to read redis ping after reconnect");
            }
            constexpr std::string_view pong = "+PONG\r\n";
            std::error_code writeError;
            (void)asio::write(reconnected, asio::buffer(pong), writeError);
        } catch (...) {
            try {
                commandRead_.set_exception(std::current_exception());
            } catch (...) {
            }
        }
    }

    asio::io_context ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::promise<void> commandRead_;
    std::future<void> commandReadFuture_;
    std::thread thread_;
};

int testStopWhileRedisConnectWaits() {
    StalledTcpServer server;
    auto redis = ruvia::RedisConfig{};
    redis.host = "127.0.0.1";
    redis.port = server.port();
    // Force startup to wait for a server reply after TCP connect.
    redis.password = "secret";

    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 2});
    auto options = ruvia::DataAccessOptions{};
    options.redis.push_back(ruvia::DataAccessRedisConfig{"default", std::move(redis)});
    ruvia::DataAccessService service(loops.loop(0), std::move(options));

    auto ready = service.connect();
    loops.start();
    server.waitUntilAccepted();
    loops.stop();
    loops.join();

    if (ready.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return 1;
    }
    try {
        ready.get();
        return 2;
    } catch (const std::exception&) {
    }
    return service.stats().outstanding == 0 ? 0 : 3;
}

int testRedisCommandUsesOneAbsoluteTimeout() {
    SlowRedisReplyServer server;
    auto redis = ruvia::RedisConfig{};
    redis.host = "127.0.0.1";
    redis.port = server.port();
    redis.poolSizePerWorker = 1;
    redis.commandTimeout = std::chrono::milliseconds(250);

    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 2});
    auto options = ruvia::DataAccessOptions{};
    options.maintenanceInterval = std::chrono::milliseconds(25);
    options.redis.push_back(ruvia::DataAccessRedisConfig{"default", std::move(redis)});
    ruvia::DataAccessService service(loops.loop(0), std::move(options));

    auto ready = service.connect();
    loops.start();
    ready.get();

    std::promise<int> completed;
    auto completedFuture = completed.get_future();
    const auto posted = service.post([&completed](ruvia::DataAccessContext& context) -> ruvia::Task<void> {
        try {
            auto redisHandle = context.redis();
            co_await redisHandle.ping();
            completed.set_value(1);
        } catch (const ruvia::RedisError& error) {
            completed.set_value(error.code() == ruvia::RedisError::Code::kTimeout ? 0 : 2);
        }
    });
    if (!posted.accepted()) {
        loops.stop();
        loops.join();
        return 3;
    }

    const auto result = completedFuture.get();
    loops.stop();
    loops.join();
    return result;
}

int testRedisSingleCommandTimeoutWithoutPoolDefault() {
    SlowRedisReplyServer server;
    auto redis = ruvia::RedisConfig{};
    redis.host = "127.0.0.1";
    redis.port = server.port();
    redis.poolSizePerWorker = 1;

    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 2});
    auto options = ruvia::DataAccessOptions{};
    options.maintenanceInterval = std::chrono::milliseconds(25);
    options.redis.push_back(ruvia::DataAccessRedisConfig{"default", std::move(redis)});
    ruvia::DataAccessService service(loops.loop(0), std::move(options));

    auto ready = service.connect();
    loops.start();
    ready.get();

    std::promise<int> completed;
    auto completedFuture = completed.get_future();
    const auto posted = service.post([&completed](ruvia::DataAccessContext& context) -> ruvia::Task<void> {
        try {
            auto redisHandle = context.redis();
            co_await redisHandle.command(ruvia::RedisOperationOptions{.timeout = std::chrono::milliseconds(250)}, "PING");
            completed.set_value(1);
        } catch (const ruvia::RedisError& error) {
            completed.set_value(error.code() == ruvia::RedisError::Code::kTimeout ? 0 : 2);
        }
    });
    if (!posted.accepted()) {
        loops.stop();
        loops.join();
        return 3;
    }

    const auto result = completedFuture.get();
    loops.stop();
    loops.join();
    return result;
}

int testRedisCancellationDiscardsSocketAndReconnects() {
    CancelledRedisBlockServer server;
    auto redis = ruvia::RedisConfig{};
    redis.host = "127.0.0.1";
    redis.port = server.port();
    redis.poolSizePerWorker = 1;
    redis.usage = ruvia::RedisPoolUsage::kBlocking;

    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 4});
    auto options = ruvia::DataAccessOptions{};
    options.redis.push_back(ruvia::DataAccessRedisConfig{"default", std::move(redis)});
    ruvia::DataAccessService service(loops.loop(0), std::move(options));
    ruvia::detail::StopSource operationStop;

    auto ready = service.connect();
    loops.start();
    ready.get();

    std::promise<int> completed;
    auto completedFuture = completed.get_future();
    const auto posted = service.post([&completed, &operationStop](ruvia::DataAccessContext& context) -> ruvia::Task<void> {
        const std::array streams{ruvia::RedisStreamReadView{.stream = "events", .id = ">"}};
        bool cancelled = false;
        try {
            auto redisHandle = context.redis();
            ruvia::RedisXReadGroupOptions readOptions;
            readOptions.block = ruvia::RedisBlockWait::indefinitely();
            readOptions.operation.stopToken = operationStop.token();
            (void)co_await redisHandle.xreadGroup("g", "c", streams, std::move(readOptions));
        } catch (const ruvia::RedisError& error) {
            if (error.code() != ruvia::RedisError::Code::kCancelled) {
                completed.set_value(2);
                co_return;
            }
            cancelled = true;
        }
        if (!cancelled) {
            completed.set_value(1);
            co_return;
        }
        try {
            auto redisHandle = context.redis();
            co_await redisHandle.ping();
            completed.set_value(0);
        } catch (...) {
            completed.set_value(3);
        }
    });
    if (!posted.accepted()) {
        loops.stop();
        loops.join();
        return 4;
    }

    server.waitUntilBlocked();
    operationStop.requestStop();
    const auto result = completedFuture.get();
    loops.stop();
    loops.join();
    return result;
}

int testRedisConnectTimeoutIncludesStartupCommands() {
    DelayedRedisStartupServer server;
    auto redis = ruvia::RedisConfig{};
    redis.host = "127.0.0.1";
    redis.port = server.port();
    redis.password = "secret";
    redis.database = 1;
    redis.poolSizePerWorker = 1;
    redis.connectTimeout = std::chrono::milliseconds(250);
    redis.commandTimeout = std::chrono::seconds(1);

    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 2});
    auto options = ruvia::DataAccessOptions{};
    options.maintenanceInterval = std::chrono::milliseconds(25);
    options.redis.push_back(ruvia::DataAccessRedisConfig{"default", std::move(redis)});
    ruvia::DataAccessService service(loops.loop(0), std::move(options));

    auto ready = service.connect();
    loops.start();
    auto result = 0;
    try {
        ready.get();
        result = 1;
    } catch (const ruvia::RedisError& error) {
        if (error.code() != ruvia::RedisError::Code::kTimeout) {
            result = 2;
        }
    }
    loops.stop();
    loops.join();
    return result;
}
#endif

int testConfigurationValidation() {
    try {
        ruvia::DataAccessService invalid(ruvia::EventLoop{});
        return 1;
    } catch (const std::invalid_argument&) {
    }

    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    auto invalidInterval = ruvia::DataAccessOptions{};
    invalidInterval.maintenanceInterval = std::chrono::milliseconds(0);
    try {
        ruvia::DataAccessService invalid(loops.loop(0), std::move(invalidInterval));
        return 2;
    } catch (const std::invalid_argument&) {
    }

#ifdef RUVIA_ENABLE_DATABASE
    auto invalidDatabase = ruvia::DataAccessOptions{};
    auto database = ruvia::DbConfig::mariaDb();
    database.port = 0;
    invalidDatabase.databases.push_back(ruvia::DataAccessDatabaseConfig{"default", std::move(database)});
    try {
        ruvia::DataAccessService invalid(loops.loop(0), std::move(invalidDatabase));
        return 3;
    } catch (const std::invalid_argument&) {
    }
#endif

#ifdef RUVIA_ENABLE_REDIS
    auto invalidRedis = ruvia::DataAccessOptions{};
    auto redis = ruvia::RedisConfig{};
    redis.port = 0;
    invalidRedis.redis.push_back(ruvia::DataAccessRedisConfig{"default", std::move(redis)});
    try {
        ruvia::DataAccessService invalid(loops.loop(0), std::move(invalidRedis));
        return 4;
    } catch (const std::invalid_argument&) {
    }
#endif

    return 0;
}

}  // namespace

int main() {
    try {
#ifdef RUVIA_ENABLE_DATABASE
        if (const auto result = testResolvedDatabaseHostLists(); result != 0) {
            return 5 + result;
        }
#endif
        if (const auto result = testConfigurationValidation(); result != 0) {
            return 10 + result;
        }
        if (const auto result = testWorkerAffinityAndAutomaticShutdown(); result != 0) {
            return 20 + result;
        }
        if (const auto result = testApplicationOwnedEventLoop(); result != 0) {
            return 25 + result;
        }
        if (const auto result = testExplicitClose(); result != 0) {
            return 30 + result;
        }
        if (const auto result = testFailureReporting(); result != 0) {
            return 40 + result;
        }
        if (const auto result = testHandledFailureKeepsWorkerUsable(); result != 0) {
            return 50 + result;
        }
        if (const auto result = testStopBeforeConnectPublishesClosedGate(); result != 0) {
            return 60 + result;
        }
#ifdef RUVIA_ENABLE_MARIADB
        if (const auto result = testStopWhileDatabaseConnectWaits(ruvia::DbConfig::mariaDb()); result != 0) {
            return 70 + result;
        }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
        if (const auto result = testStopWhileDatabaseConnectWaits(ruvia::DbConfig::postgreSql()); result != 0) {
            return 80 + result;
        }
#endif
#ifdef RUVIA_ENABLE_REDIS
        if (const auto result = testStopWhileRedisConnectWaits(); result != 0) {
            return 90 + result;
        }
        if (const auto result = testRedisCommandUsesOneAbsoluteTimeout(); result != 0) {
            return 95 + result;
        }
        if (const auto result = testRedisSingleCommandTimeoutWithoutPoolDefault(); result != 0) {
            return 96 + result;
        }
        if (const auto result = testRedisCancellationDiscardsSocketAndReconnects(); result != 0) {
            return 97 + result;
        }
        if (const auto result = testRedisConnectTimeoutIncludesStartupCommands(); result != 0) {
            return 98 + result;
        }
#endif
        return 0;
    } catch (...) {
        return 100;
    }
}
