#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
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
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/read.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/db/DbEntity.h"
#include "ruvia/web/db/DbExecResult.h"
#include "ruvia/web/db/DbFindOptions.h"
#include "ruvia/web/detail/redis/RedisMappedCommand.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisRepositoryCommands.h"
#include "ruvia/web/detail/redis/RedisRepositoryMapping.h"
#include "ruvia/web/detail/redis/RedisTypesAccess.h"
#include "ruvia/web/redis/RedisHandle.h"
#include "ruvia/web/redis/RedisRepository.h"
#include "ruvia/web/redis/RedisRepositoryTypes.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::RedisIndexKind;

RUVIA_DB_ENTITY(TestRedisUser, "users",
    RUVIA_DB_COLUMN(id, ruvia::String, ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_COLUMN(active, bool),
    RUVIA_DB_COLUMN(role, std::pmr::string, ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(age, std::int32_t, ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(score, ruvia::Double, ruvia::DbColumnOptions{.nullable = true}));

RUVIA_DB_ENTITY(TestRedisAdminUser, "users:admin",
    RUVIA_DB_COLUMN(id, ruvia::String, ruvia::DbColumnOptions{.primaryKey = true}));

const ruvia::RedisRepositoryConfig kTestRedisRepositoryConfig{
    .prefix = "users",
    .indexes = {{.column = "name", .kind = RedisIndexKind::kTag}},
};

class RedisTestWorker final {
public:
    explicit RedisTestWorker(asio::io_context& ioContext)
        : dispatcher_(std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 64)),
          handle_(ruvia::detail::WorkerHandleAccess::make(dispatcher_)) {}

    RedisTestWorker(const RedisTestWorker&) = delete;
    RedisTestWorker& operator=(const RedisTestWorker&) = delete;

    [[nodiscard]] const ruvia::WorkerHandle& handle() const noexcept {
        return handle_;
    }

    void run() {
        dispatcher_->runContext();
    }

private:
    std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher_;
    ruvia::WorkerHandle handle_;
};

class StalledRedisCommandServer final {
public:
    StalledRedisCommandServer()
        : acceptor_(ioContext_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          port_(acceptor_.local_endpoint().port()),
          commandReadFuture_(commandRead_.get_future()),
          releaseFuture_(release_.get_future()),
          thread_([this] { run(); }) {}

    ~StalledRedisCommandServer() {
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

    StalledRedisCommandServer(const StalledRedisCommandServer&) = delete;
    StalledRedisCommandServer& operator=(const StalledRedisCommandServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    void waitUntilCommandRead() {
        commandReadFuture_.get();
    }

private:
    static std::string readLine(asio::ip::tcp::socket& socket) {
        std::string result;
        char value = 0;
        for (;;) {
            asio::read(socket, asio::buffer(&value, 1));
            if (value == '\r') {
                asio::read(socket, asio::buffer(&value, 1));
                if (value != '\n') {
                    throw std::runtime_error("invalid RESP line ending");
                }
                return result;
            }
            result.push_back(value);
        }
    }

    static void readCommand(asio::ip::tcp::socket& socket) {
        char marker = 0;
        asio::read(socket, asio::buffer(&marker, 1));
        if (marker != '*') {
            throw std::runtime_error("invalid RESP command array");
        }
        const auto countValue = std::stoull(readLine(socket));
        if (countValue > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("RESP command array is too large");
        }
        const auto count = static_cast<std::size_t>(countValue);
        for (std::size_t index = 0; index != count; ++index) {
            asio::read(socket, asio::buffer(&marker, 1));
            if (marker != '$') {
                throw std::runtime_error("invalid RESP bulk string");
            }
            const auto lengthValue = std::stoull(readLine(socket));
            if (lengthValue > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("RESP bulk string is too large");
            }
            const auto length = static_cast<std::size_t>(lengthValue);
            std::string value(length, '\0');
            asio::read(socket, asio::buffer(value));
            std::array<char, 2> lineEnding{};
            asio::read(socket, asio::buffer(lineEnding));
            if (lineEnding != std::array<char, 2>{'\r', '\n'}) {
                throw std::runtime_error("invalid RESP bulk ending");
            }
        }
    }

    void run() noexcept {
        try {
            asio::ip::tcp::socket socket(ioContext_);
            acceptor_.accept(socket);
            // Complete a warm-up command first so the pool's persistent
            // serialization buffer is established before the stalled command.
            readCommand(socket);
            constexpr std::string_view reply = ":1\r\n";
            asio::write(socket, asio::buffer(reply));

            std::array<char, 1> command{};
            std::error_code error;
            (void)socket.read_some(asio::buffer(command), error);
            if (error) {
                throw std::system_error(error);
            }
            commandRead_.set_value();
            // Keep the peer open after the first command byte. Reading another
            // byte here would consume the already-buffered command and let the
            // local socket close before the cancellation request reaches the
            // worker under test.
            releaseFuture_.wait();
        } catch (...) {
            try {
                commandRead_.set_exception(std::current_exception());
            } catch (...) {
            }
        }
    }

    asio::io_context ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::promise<void> commandRead_;
    std::future<void> commandReadFuture_;
    std::promise<void> release_;
    std::future<void> releaseFuture_;
    std::thread thread_;
};

class SingleReplyRedisCommandServer final {
public:
    SingleReplyRedisCommandServer()
        : acceptor_(ioContext_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          port_(acceptor_.local_endpoint().port()),
          commandReadFuture_(commandRead_.get_future()),
          thread_([this] { run(); }) {}

    ~SingleReplyRedisCommandServer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    SingleReplyRedisCommandServer(const SingleReplyRedisCommandServer&) = delete;
    SingleReplyRedisCommandServer& operator=(const SingleReplyRedisCommandServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    void waitUntilCommandRead() {
        commandReadFuture_.get();
    }

    [[nodiscard]] const std::vector<std::string>& arguments() const noexcept {
        return arguments_;
    }

private:
    static std::string readLine(asio::ip::tcp::socket& socket) {
        std::string result;
        char value = 0;
        for (;;) {
            asio::read(socket, asio::buffer(&value, 1));
            if (value == '\r') {
                asio::read(socket, asio::buffer(&value, 1));
                if (value != '\n') {
                    throw std::runtime_error("invalid RESP line ending");
                }
                return result;
            }
            result.push_back(value);
        }
    }

    static std::vector<std::string> readCommand(asio::ip::tcp::socket& socket) {
        char marker = 0;
        asio::read(socket, asio::buffer(&marker, 1));
        if (marker != '*') {
            throw std::runtime_error("invalid RESP command array");
        }
        const auto countValue = std::stoull(readLine(socket));
        if (countValue > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("RESP command array is too large");
        }
        const auto count = static_cast<std::size_t>(countValue);
        std::vector<std::string> result;
        result.reserve(count);
        for (std::size_t index = 0; index != count; ++index) {
            asio::read(socket, asio::buffer(&marker, 1));
            if (marker != '$') {
                throw std::runtime_error("invalid RESP bulk string");
            }
            const auto lengthValue = std::stoull(readLine(socket));
            if (lengthValue > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("RESP bulk string is too large");
            }
            const auto length = static_cast<std::size_t>(lengthValue);
            std::string value(length, '\0');
            asio::read(socket, asio::buffer(value));
            std::array<char, 2> lineEnding{};
            asio::read(socket, asio::buffer(lineEnding));
            if (lineEnding != std::array<char, 2>{'\r', '\n'}) {
                throw std::runtime_error("invalid RESP bulk ending");
            }
            result.push_back(std::move(value));
        }
        return result;
    }

    void run() noexcept {
        try {
            asio::ip::tcp::socket socket(ioContext_);
            acceptor_.accept(socket);
            constexpr std::string_view reply = ":1\r\n";
            (void)readCommand(socket);
            asio::write(socket, asio::buffer(reply));
            arguments_ = readCommand(socket);
            commandRead_.set_value();
            asio::write(socket, asio::buffer(reply));
        } catch (...) {
            try {
                commandRead_.set_exception(std::current_exception());
            } catch (...) {
            }
        }
    }

    asio::io_context ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::promise<void> commandRead_;
    std::future<void> commandReadFuture_;
    std::vector<std::string> arguments_;
    std::thread thread_;
};

[[nodiscard]] ruvia::detail::RedisDefinition redisDefinition(std::string_view alias,
    const ruvia::RedisConfig& config = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    return {
        std::pmr::string(alias, resource),
        ruvia::detail::RedisConfigStorage(config, resource),
    };
}

[[nodiscard]] TestRedisUser makeUser(std::pmr::memory_resource* resource,
    std::string_view id = "u-1") {
    TestRedisUser user(resource);
    user.set<"id">(id);
    user.set<"name">("Alice");
    user.set<"active">(true);
    user.set<"role">("admin");
    user.set<"age">(32);
    user.set<"score">(ruvia::Double{4.5});
    return user;
}

[[nodiscard]] std::pmr::string testRedisEntityKey(
    std::string_view id, std::pmr::memory_resource* resource) {
    return ruvia::detail::redisEntityKey<TestRedisUser>(
        id, resource, kTestRedisRepositoryConfig.prefix);
}

[[nodiscard]] ruvia::RedisValue redisHashReply(std::pmr::memory_resource* resource,
    std::string_view id = "u-1") {
    std::pmr::vector<ruvia::RedisValue> fields(resource);
    const auto add = [&fields, resource](std::string_view name, std::string_view value) {
        fields.emplace_back(ruvia::detail::RedisTypesAccess::stringValue(name, resource));
        fields.emplace_back(ruvia::detail::RedisTypesAccess::stringValue(value, resource));
    };
    add("__ruvia_entity", "1");
    add("id", id);
    add("name", "Alice");
    add("active", "1");
    add("role", "admin");
    add("age", "32");
    add("score", "4.5");
    add("__ruvia_tag_name", "x416c696365");
    return ruvia::detail::RedisTypesAccess::arrayValue(std::move(fields), resource);
}

[[nodiscard]] ruvia::RedisValue redisSearchReply(std::pmr::memory_resource* resource,
    std::string_view id = "u-1") {
    std::pmr::vector<ruvia::RedisValue> values(resource);
    values.emplace_back(ruvia::detail::RedisTypesAccess::integerValue(1, resource));
    const auto key = testRedisEntityKey(id, resource);
    values.emplace_back(ruvia::detail::RedisTypesAccess::stringValue(key, resource));
    values.emplace_back(redisHashReply(resource, id));
    return ruvia::detail::RedisTypesAccess::arrayValue(std::move(values), resource);
}

[[nodiscard]] ruvia::Task<ruvia::RedisValue> immediateRedisReply(ruvia::RedisValue reply) {
    co_return reply;
}

template <typename Fn>
[[nodiscard]] bool throwsProtocol(Fn&& function) {
    try {
        function();
    } catch (const ruvia::RedisError& error) {
        return error.code() == ruvia::RedisError::Code::kProtocolError;
    } catch (...) {
    }
    return false;
}

template <typename Fn>
[[nodiscard]] bool throwsInvalidArgument(Fn&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
    }
    return false;
}

}  // namespace

RUVIA_TEST(redis_repository_mapping_owns_decoded_entities_and_search_results) {
    ruvia::test::CountingMemoryResource wireResource;
    ruvia::test::CountingMemoryResource resultResource;

    auto one = ruvia::detail::RedisMapOne<TestRedisUser>{
        testRedisEntityKey("u-1", &resultResource),
        std::pmr::string(kTestRedisRepositoryConfig.prefix, &resultResource)}(redisHashReply(&wireResource), &resultResource);
    RUVIA_CHECK(one.has_value());
    RUVIA_CHECK_EQ(one->get<"id">().view(), std::string_view("u-1"));
    RUVIA_CHECK_EQ(std::string_view(one->get<"name">()), std::string_view("Alice"));
    RUVIA_CHECK(one->get<"active">());
    RUVIA_CHECK_EQ(one->get<"age">(), 32);
    RUVIA_CHECK_EQ(static_cast<double>(one->get<"score">()), 4.5);

    {
        auto page = ruvia::detail::RedisMapSearch<TestRedisUser>{
            std::pmr::string(kTestRedisRepositoryConfig.prefix, &resultResource)}(redisSearchReply(&wireResource), &resultResource);
        RUVIA_CHECK_EQ(page.second, std::uint64_t{1});
        RUVIA_CHECK_EQ(page.first.size(), std::size_t{1});
        RUVIA_CHECK_EQ(page.first[0].get<"id">().view(), std::string_view("u-1"));
    }

    const auto allocations = resultResource.allocationCount();
    RUVIA_CHECK(allocations > 0);
    one.reset();
    RUVIA_CHECK_EQ(resultResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK(wireResource.deallocationCount() > 0);
}

RUVIA_TEST(redis_repository_mapping_rejects_untrusted_hash_replies) {
    ruvia::test::CountingMemoryResource resource;

    auto noMarker = redisHashReply(&resource);
    // Replace the first marker with an ordinary field. The required fields
    // remain present, so this specifically exercises ownership validation.
    std::pmr::vector<ruvia::RedisValue> fields(&resource);
    fields.emplace_back(ruvia::detail::RedisTypesAccess::stringValue("id", &resource));
    fields.emplace_back(ruvia::detail::RedisTypesAccess::stringValue("u-1", &resource));
    fields.emplace_back(ruvia::detail::RedisTypesAccess::stringValue("name", &resource));
    fields.emplace_back(ruvia::detail::RedisTypesAccess::stringValue("Alice", &resource));
    fields.emplace_back(ruvia::detail::RedisTypesAccess::stringValue("active", &resource));
    fields.emplace_back(ruvia::detail::RedisTypesAccess::stringValue("1", &resource));
    noMarker = ruvia::detail::RedisTypesAccess::arrayValue(std::move(fields), &resource);
    RUVIA_CHECK(throwsProtocol([&] {
        (void)ruvia::detail::RedisMapOne<TestRedisUser>{
            testRedisEntityKey("u-1", &resource),
            std::pmr::string(kTestRedisRepositoryConfig.prefix, &resource)}(
            std::move(noMarker), &resource);
    }));

    RUVIA_CHECK(throwsProtocol([&] {
        std::pmr::vector<ruvia::RedisValue> oddFields(&resource);
        oddFields.emplace_back(
            ruvia::detail::RedisTypesAccess::stringValue("__ruvia_entity", &resource));
        auto odd = ruvia::detail::RedisTypesAccess::arrayValue(std::move(oddFields), &resource);
        (void)ruvia::detail::RedisMapOne<TestRedisUser>{
            testRedisEntityKey("u-1", &resource),
            std::pmr::string(kTestRedisRepositoryConfig.prefix, &resource)}(
            std::move(odd), &resource);
    }));

    RUVIA_CHECK(throwsProtocol([&] {
        auto wrongKey = redisHashReply(&resource, "other");
        (void)ruvia::detail::RedisMapOne<TestRedisUser>{
            testRedisEntityKey("u-1", &resource),
            std::pmr::string(kTestRedisRepositoryConfig.prefix, &resource)}(
            std::move(wrongKey), &resource);
    }));

    RUVIA_CHECK(throwsProtocol([&] {
        auto malformed = redisHashReply(&resource);
        // The decoder rejects non-numeric values in numeric entity columns.
        std::pmr::vector<ruvia::RedisValue> malformedFields(&resource);
        const auto add = [&malformedFields, &resource](std::string_view name,
                             std::string_view value) {
            malformedFields.emplace_back(
                ruvia::detail::RedisTypesAccess::stringValue(name, &resource));
            malformedFields.emplace_back(
                ruvia::detail::RedisTypesAccess::stringValue(value, &resource));
        };
        add("__ruvia_entity", "1");
        add("id", "u-1");
        add("name", "Alice");
        add("active", "1");
        add("age", "not-a-number");
        malformed = ruvia::detail::RedisTypesAccess::arrayValue(
            std::move(malformedFields), &resource);
        (void)ruvia::detail::RedisMapOne<TestRedisUser>{
            testRedisEntityKey("u-1", &resource),
            std::pmr::string(kTestRedisRepositoryConfig.prefix, &resource)}(
            std::move(malformed), &resource);
    }));
}

RUVIA_TEST(redis_repository_storage_keys_do_not_overlap_logical_prefixes) {
    ruvia::test::CountingMemoryResource resource;
    const auto userKey = ruvia::detail::redisEntityKey<TestRedisUser>("admin:42", &resource);
    const auto adminKey = ruvia::detail::redisEntityKey<TestRedisAdminUser>("42", &resource);
    const auto userPrefix = ruvia::detail::redisEntityStoragePrefix<TestRedisUser>();
    const auto adminPrefix = ruvia::detail::redisEntityStoragePrefix<TestRedisAdminUser>();

    RUVIA_CHECK(userPrefix != adminPrefix);
    RUVIA_CHECK(userKey != adminKey);
    RUVIA_CHECK(!userKey.starts_with(adminPrefix));
    RUVIA_CHECK(!adminKey.starts_with(userPrefix));
}

RUVIA_TEST(redis_repository_command_mappers_validate_wire_results) {
    ruvia::test::CountingMemoryResource resource;

    auto inserted = ruvia::detail::redisOrmExecResult(
        ruvia::detail::RedisTypesAccess::integerValue(1, &resource), &resource);
    RUVIA_CHECK_EQ(inserted.affectedRows(), std::uint64_t{1});
    RUVIA_CHECK(!inserted.lastInsertId().has_value());

    auto updated = ruvia::detail::redisOrmExecResult(
        ruvia::detail::RedisTypesAccess::integerValue(2, &resource), &resource);
    RUVIA_CHECK_EQ(updated.affectedRows(), std::uint64_t{1});
    RUVIA_CHECK(!updated.lastInsertId().has_value());

    auto skipped = ruvia::detail::redisOrmExecResult(
        ruvia::detail::RedisTypesAccess::integerValue(0, &resource), &resource);
    RUVIA_CHECK_EQ(skipped.affectedRows(), std::uint64_t{0});
    RUVIA_CHECK(!skipped.lastInsertId().has_value());

    auto deleted = ruvia::detail::redisOrmDeleteResult(
        ruvia::detail::RedisTypesAccess::integerValue(1, &resource), &resource);
    RUVIA_CHECK_EQ(deleted.affectedRows(), std::uint64_t{1});
    auto missing = ruvia::detail::redisOrmDeleteResult(
        ruvia::detail::RedisTypesAccess::integerValue(0, &resource), &resource);
    RUVIA_CHECK_EQ(missing.affectedRows(), std::uint64_t{0});

    RUVIA_CHECK(ruvia::detail::redisOrmBooleanResult(
        ruvia::detail::RedisTypesAccess::integerValue(1, &resource), &resource));
    RUVIA_CHECK_EQ(ruvia::detail::redisOrmCount(
                       ruvia::detail::RedisTypesAccess::integerValue(17, &resource)),
        std::uint64_t{17});
    RUVIA_CHECK(throwsProtocol([&] {
        (void)ruvia::detail::redisOrmExecResult(
            ruvia::detail::RedisTypesAccess::integerValue(7, &resource), &resource);
    }));
    RUVIA_CHECK(throwsProtocol([&] {
        (void)ruvia::detail::redisOrmDeleteResult(
            ruvia::detail::RedisTypesAccess::integerValue(2, &resource), &resource);
    }));
    RUVIA_CHECK(throwsProtocol([&] {
        (void)ruvia::detail::redisOrmCount(
            ruvia::detail::RedisTypesAccess::integerValue(-1, &resource));
    }));
    RUVIA_CHECK(throwsProtocol([&] {
        ruvia::detail::redisOrmStatusResult(
            ruvia::detail::RedisTypesAccess::stringValue("QUEUED", &resource), &resource);
    }));
}

RUVIA_TEST(redis_repository_async_mapping_reclaims_replies_and_retains_results) {
    asio::io_context ioContext;
    ruvia::test::CountingMemoryResource wireResource;
    ruvia::test::CountingMemoryResource resultResource;

    auto exercise = [&]() -> ruvia::Task<void> {
        std::pmr::vector<TestRedisUser> retained(&resultResource);
        retained.reserve(1);
        for (int index = 0; index != 32; ++index) {
            auto mapped = co_await ruvia::detail::mapRedisCommand<std::optional<TestRedisUser>>(
                immediateRedisReply(redisHashReply(&wireResource)), &resultResource,
                ruvia::detail::RedisMapOne<TestRedisUser>{
                    testRedisEntityKey("u-1", &resultResource),
                    std::pmr::string(kTestRedisRepositoryConfig.prefix, &resultResource)});
            RUVIA_CHECK(mapped.has_value());
            if (index == 0) {
                retained.push_back(std::move(*mapped));
            } else {
                RUVIA_CHECK_EQ(std::string_view(retained.front().get<"name">()),
                    std::string_view("Alice"));
            }
        }
        RUVIA_CHECK(!retained.empty());
    };

    auto result = asio::co_spawn(
        ioContext, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    ioContext.run();
    result.get();
    RUVIA_CHECK_EQ(wireResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resultResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK(wireResource.deallocationCount() > 0);
    RUVIA_CHECK(resultResource.deallocationCount() > 0);
}

RUVIA_TEST(redis_repository_async_mapping_reclaims_exception_frames) {
    asio::io_context ioContext;
    ruvia::test::CountingMemoryResource replyResource;
    const auto baseline = replyResource.liveAllocations();

    auto exercise = [&]() -> ruvia::Task<void> {
        for (int index = 0; index != 32; ++index) {
            bool rejected = false;
            try {
                const std::string invalidStatus(128, 'X');
                auto operation = ruvia::detail::mapRedisCommand<void>(
                    immediateRedisReply(ruvia::detail::RedisTypesAccess::stringValue(
                        invalidStatus, &replyResource)),
                    &replyResource, ruvia::detail::redisOrmStatusResult);
                (void)co_await std::move(operation);
            } catch (const ruvia::RedisError& error) {
                rejected = error.code() == ruvia::RedisError::Code::kProtocolError;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(replyResource.liveAllocations(), baseline);
        }
    };

    auto result = asio::co_spawn(
        ioContext, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    ioContext.run();
    result.get();
    RUVIA_CHECK(replyResource.deallocationCount() > 0);
}

RUVIA_TEST(redis_repository_cold_operations_release_owned_arguments) {
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    ruvia::test::CountingMemoryResource operationResource;
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, &operationResource, definitions, worker.handle());
    ruvia::detail::ScopedOperationScope scope;
    const auto handle = registry.get(scope);
    const auto repository = handle.getRepository<TestRedisUser>(kTestRedisRepositoryConfig);
    auto entity = makeUser(&operationResource);
    const auto baseline = operationResource.liveAllocations();

    for (int index = 0; index != 8; ++index) {
        {
            auto upsert = repository.upsert(entity);
            auto insert = repository.insert(entity);
            auto update = repository.update(TestRedisUser::column<"id">() == "u-1", entity);
            auto many = repository.find();
            auto one = repository.findOne({.where = TestRedisUser::column<"id">() == "u-1"});
            auto page = repository.findAndCount();
            auto count = repository.count({.where = TestRedisUser::column<"name">() == "Alice"});
            auto exists = repository.exists({.where = TestRedisUser::column<"name">() == "Alice"});
            auto removed = repository.deleteBy(TestRedisUser::column<"id">() == "u-1");
            auto removedEntity = repository.remove(entity);
            auto expire = repository.expire(
                TestRedisUser::column<"id">() == "u-1", std::chrono::seconds(30));
            auto ttl = repository.ttl(TestRedisUser::column<"id">() == "u-1");
            auto createIndex = repository.createIndex();
            auto dropIndex = repository.dropIndex();
            (void)upsert;
            (void)insert;
            (void)update;
            (void)many;
            (void)one;
            (void)page;
            (void)count;
            (void)exists;
            (void)removed;
            (void)removedEntity;
            (void)expire;
            (void)ttl;
            (void)createIndex;
            (void)dropIndex;
            RUVIA_CHECK(scope.hasPendingOperations());
        }
        RUVIA_CHECK(!scope.hasPendingOperations());
        RUVIA_CHECK_EQ(operationResource.liveAllocations(), baseline);
    }
    RUVIA_CHECK(operationResource.allocationCount() > 0);
    RUVIA_CHECK(operationResource.deallocationCount() > 0);
}

RUVIA_TEST(redis_repository_owns_input_before_entity_is_destroyed) {
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    ruvia::test::CountingMemoryResource operationResource;
    ruvia::test::TrackingResource inputResource;
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, &operationResource, definitions, worker.handle());
    ruvia::detail::ScopedOperationScope scope;
    const auto repository = registry.get(scope).getRepository<TestRedisUser>(kTestRedisRepositoryConfig);

    std::optional<TestRedisUser> input;
    input.emplace(&inputResource);
    input->set<"id">(std::string(160, 'i'));
    input->set<"name">(std::string(160, 'n'));
    input->set<"active">(true);
    auto pending = repository.insert(*input);
    input.reset();
    inputResource.release();
    RUVIA_CHECK(!inputResource.deallocatedAfterRelease());
    // The pending command owns its copied key/field/value arguments and can
    // be discarded after the caller-owned entity and allocator are gone.
    RUVIA_CHECK(scope.hasPendingOperations());
    (void)pending;
    scope.close();
    RUVIA_CHECK(!scope.hasPendingOperations());
    RUVIA_CHECK(!inputResource.deallocatedAfterRelease());
}

RUVIA_TEST(redis_repository_insert_owns_input_through_async_handoff) {
    SingleReplyRedisCommandServer server;
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    ruvia::test::CountingMemoryResource operationResource;
    ruvia::test::TrackingResource inputResource;
    ruvia::RedisConfig config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.poolSizePerWorker = 1;
    const auto definition = redisDefinition("default", config);
    {
        ruvia::detail::RedisRegistry registry(
            ioContext, &operationResource,
            std::span<const ruvia::detail::RedisDefinition>(&definition, 1), worker.handle());
        ruvia::detail::ScopedOperationScope scope;
        const auto repository =
            registry.get(scope).getRepository<TestRedisUser>(kTestRedisRepositoryConfig);
        const std::string inputId(160, 'i');
        const std::string inputName(160, 'n');
        std::size_t baselineAfterWarmup = 0;

        auto exercise = [&]() -> ruvia::Task<std::uint64_t> {
            TestRedisUser warmup(std::pmr::get_default_resource());
            warmup.set<"id">(inputId);
            warmup.set<"name">(inputName);
            warmup.set<"active">(true);
            warmup.set<"role">("admin");
            const auto warmupResult = co_await repository.insert(warmup);
            const bool warmupSucceeded = warmupResult.affectedRows() == 1;
            baselineAfterWarmup = operationResource.liveAllocations();

            std::optional<TestRedisUser> input;
            input.emplace(&inputResource);
            input->set<"id">(inputId);
            input->set<"name">(inputName);
            input->set<"active">(true);
            input->set<"role">("admin");
            auto pending = repository.insert(*input);
            input.reset();
            inputResource.release();
            // Destroying the entity before the operation starts must be safe.
            // The repository has synchronously copied every field into operation storage.
            try {
                const auto result = (co_await std::move(pending)).affectedRows();
                co_return warmupSucceeded&& result == 1 ? 1 : 0;
            } catch (...) {
                co_return 0;
            }
        };
        auto result = asio::co_spawn(
            ioContext, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
        std::jthread runner([&worker] { worker.run(); });
        server.waitUntilCommandRead();
        const auto& args = server.arguments();
        bool containsId = false;
        bool containsName = false;
        bool containsNameShadow = false;
        bool containsNameShadowValue = false;
        bool containsNamePresence = false;
        std::string expectedNameShadow("x");
        for (std::size_t i = 0; i < inputName.size(); ++i) {
            expectedNameShadow += "6e";
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& arg = args[i];
            containsId = containsId || arg == inputId;
            containsName = containsName || arg == inputName;
            containsNameShadow = containsNameShadow || arg == "__ruvia_tag_name";
            if (i + 1 < args.size() && arg == "__ruvia_tag_name") {
                containsNameShadowValue = args[i + 1] == expectedNameShadow;
            }
            if (i + 1 < args.size() && arg == "__ruvia_present_name") {
                containsNamePresence = args[i + 1] == "1";
            }
        }
        RUVIA_CHECK(containsId);
        RUVIA_CHECK(containsName);
        RUVIA_CHECK(containsNameShadow);
        RUVIA_CHECK(containsNameShadowValue);
        RUVIA_CHECK(containsNamePresence);
        RUVIA_CHECK_EQ(result.get(), std::uint64_t{1});
        runner.join();
        RUVIA_CHECK(!inputResource.deallocatedAfterRelease());
        // The warm-up establishes the connection's cached serialized buffer.
        // The handoff must not add any live operation-owned allocations.
        RUVIA_CHECK_EQ(operationResource.liveAllocations(), baselineAfterWarmup);
    }
    RUVIA_CHECK_EQ(operationResource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(redis_repository_pre_cancelled_operations_release_each_operation) {
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    ruvia::test::CountingMemoryResource operationResource;
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, &operationResource, definitions, worker.handle());
    ruvia::detail::ScopedOperationScope scope;
    const auto handle = registry.get(scope);
    ruvia::StopSource cancellation;
    cancellation.requestStop();
    const auto repository = handle.withOptions({.stopToken = cancellation.token()})
                                .getRepository<TestRedisUser>(kTestRedisRepositoryConfig);
    const std::string id(2048, 'i');
    const auto baseline = operationResource.liveAllocations();

    auto exercise = [&]() -> ruvia::Task<void> {
        for (int index = 0; index != 64; ++index) {
            bool cancelled = false;
            try {
                auto operation = repository.exists({.where = TestRedisUser::column<"name">() == id});
                (void)co_await std::move(operation);
            } catch (const ruvia::RedisError& error) {
                cancelled = error.code() == ruvia::RedisError::Code::kCancelled;
            }
            RUVIA_CHECK(cancelled);
            RUVIA_CHECK_EQ(operationResource.liveAllocations(), baseline);
        }
    };

    auto result = asio::co_spawn(
        ioContext, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    worker.run();
    result.get();
    RUVIA_CHECK(operationResource.allocationCount() > 0);
    RUVIA_CHECK_EQ(operationResource.liveAllocations(), baseline);
}

RUVIA_TEST(redis_repository_input_may_die_before_a_cancelled_await) {
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    ruvia::test::CountingMemoryResource operationResource;
    ruvia::test::TrackingResource inputResource;
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, &operationResource, definitions, worker.handle());
    ruvia::detail::ScopedOperationScope scope;
    const auto handle = registry.get(scope);
    ruvia::StopSource cancellation;
    cancellation.requestStop();
    const auto repository = handle.withOptions({.stopToken = cancellation.token()})
                                .getRepository<TestRedisUser>(kTestRedisRepositoryConfig);
    const auto beforePending = operationResource.liveAllocations();

    std::optional<TestRedisUser> input;
    input.emplace(&inputResource);
    input->set<"id">(std::string(160, 'i'));
    input->set<"name">(std::string(160, 'n'));
    input->set<"active">(true);
    auto pending = repository.insert(*input);
    input.reset();
    inputResource.release();

    auto exercise = [&]() -> ruvia::Task<void> {
        bool cancelled = false;
        try {
            (void)co_await std::move(pending);
        } catch (const ruvia::RedisError& error) {
            cancelled = error.code() == ruvia::RedisError::Code::kCancelled;
        }
        RUVIA_CHECK(cancelled);
    };
    auto result = asio::co_spawn(
        ioContext, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    worker.run();
    result.get();
    RUVIA_CHECK(!inputResource.deallocatedAfterRelease());
    RUVIA_CHECK_EQ(operationResource.liveAllocations(), beforePending);
}

RUVIA_TEST(redis_repository_inflight_cancellation_releases_operation_storage) {
    StalledRedisCommandServer server;
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    ruvia::test::CountingMemoryResource operationResource;
    ruvia::RedisConfig config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.poolSizePerWorker = 1;
    config.commandTimeout = std::nullopt;
    const auto definition = redisDefinition("default", config);
    {
        ruvia::detail::RedisRegistry registry(
            ioContext, &operationResource,
            std::span<const ruvia::detail::RedisDefinition>(&definition, 1), worker.handle());
        ruvia::detail::ScopedOperationScope scope;
        const auto handle = registry.get(scope);
        ruvia::StopSource cancellation;
        const auto repository = handle.withOptions({.stopToken = cancellation.token()})
                                    .getRepository<TestRedisUser>(kTestRedisRepositoryConfig);
        const std::string warmupId(2048, 'w');
        std::size_t baselineAfterWarmup = 0;
        bool warmupSucceeded = false;

        auto exercise = [&]() -> ruvia::Task<ruvia::RedisError::Code> {
            {
                auto warmup = repository.exists(
                    {.where = TestRedisUser::column<"id">() == warmupId});
                try {
                    warmupSucceeded = co_await std::move(warmup);
                } catch (const ruvia::RedisError& error) {
                    co_return error.code();
                }
            }
            baselineAfterWarmup = operationResource.liveAllocations();

            try {
                auto operation = repository.exists(
                    {.where = TestRedisUser::column<"name">() == "Alice"});
                (void)co_await std::move(operation);
            } catch (const ruvia::RedisError& error) {
                co_return error.code();
            }
            co_return ruvia::RedisError::Code::kProtocolError;
        };
        auto result = asio::co_spawn(
            ioContext, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
        std::jthread runner([&worker] { worker.run(); });
        server.waitUntilCommandRead();
        cancellation.requestStop();
        RUVIA_CHECK_EQ(result.get(), ruvia::RedisError::Code::kCancelled);
        RUVIA_CHECK(warmupSucceeded);
        runner.join();
        // The warm-up establishes the connection's cached serialized buffer.
        // The cancelled operation's own command storage must be reclaimed.
        RUVIA_CHECK_EQ(operationResource.liveAllocations(), baselineAfterWarmup);
    }
    RUVIA_CHECK_EQ(operationResource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(redis_repository_rejects_invalid_input_before_io) {
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::detail::ScopedOperationScope scope;
    const auto repository = registry.get(scope).getRepository<TestRedisUser>(kTestRedisRepositoryConfig);

    TestRedisUser missingId;
    missingId.set<"name">("Alice");
    missingId.set<"active">(true);
    RUVIA_CHECK(throwsInvalidArgument([&] { (void)repository.insert(missingId); }));
    RUVIA_CHECK(throwsInvalidArgument(
        [&] { (void)repository.insert(makeUser(std::pmr::get_default_resource()),
                  {.ttl = std::chrono::milliseconds(0)}); }));
    RUVIA_CHECK(throwsInvalidArgument(
        [&] { (void)repository.insert(makeUser(std::pmr::get_default_resource()),
                  {.ttl = std::chrono::milliseconds(10), .persist = true}); }));
    RUVIA_CHECK(throwsInvalidArgument([&] {
        (void)repository.findOne({.where = TestRedisUser::column<"id">() == ""});
    }));
}

RUVIA_TEST(redis_repository_rejects_operations_after_scope_closes) {
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::detail::ScopedOperationScope scope;
    const auto repository = registry.get(scope).getRepository<TestRedisUser>(kTestRedisRepositoryConfig);
    scope.close();

    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        (void)repository.exists({.where = TestRedisUser::column<"name">() == "Alice"});
    }));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)repository.find(); }));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)repository.createIndex(); }));
}

RUVIA_TEST(redis_repository_expired_escaped_repository_releases_owned_mapping) {
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    ruvia::test::CountingMemoryResource operationResource;
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, &operationResource, definitions, worker.handle());
    ruvia::detail::ScopedOperationScope scope;
    ruvia::RedisRepositoryConfig config;
    config.prefix.assign(256, 'p');
    config.indexes.push_back({.column = "name", .kind = RedisIndexKind::kTag});

    const auto baseline = operationResource.liveAllocations();
    auto repository = registry.get(scope).getRepository<TestRedisUser>(config);
    auto escaped = std::move(repository);
    config.prefix.clear();
    config.indexes.clear();
    RUVIA_CHECK(operationResource.liveAllocations() > baseline);
    scope.close();
    RUVIA_CHECK_EQ(operationResource.liveAllocations(), baseline);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)escaped.find(); }));
}
