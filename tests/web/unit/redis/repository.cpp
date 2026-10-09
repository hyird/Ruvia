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

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task.h"
#include "ruvia/web/detail/redis/redis_mapped_command.h"
#include "ruvia/web/detail/redis/redis_repository_commands.h"
#include "ruvia/web/detail/redis/redis_repository_mapping.h"
#include "ruvia/web/redis/redis_entity.h"
#include "ruvia/web/redis/redis_find_options.h"
#include "ruvia/web/redis/redis_handle.h"
#include "ruvia/web/redis/redis_repository.h"
#include "ruvia/web/redis/redis_repository_types.h"
#include "ruvia/web/redis/redis_write_result.h"

#include "memory_resource_fixture.h"
#include "redis/redis_registry.h"
#include "redis/redis_types_access.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using ruvia::redis_index_kind;

RUVIA_REDIS_ENTITY(test_redis_user, "users",
    RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, std::pmr::string),
    RUVIA_REDIS_COLUMN(active, bool),
    RUVIA_REDIS_COLUMN(role, std::pmr::string, ruvia::redis_column_options{.nullable_ = true}),
    RUVIA_REDIS_COLUMN(age, std::int32_t, ruvia::redis_column_options{.nullable_ = true}),
    RUVIA_REDIS_COLUMN(score, ruvia::double_value, ruvia::redis_column_options{.nullable_ = true}));

RUVIA_REDIS_ENTITY(test_redis_admin_user, "users:admin",
    RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}));

const ruvia::redis_repository_config test_redis_repository_config{
    .prefix_ = "users",
    .indexes_ = {{.field_ = "name", .kind_ = redis_index_kind::tag}},
};

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

class stalled_redis_command_server final {
public:
    stalled_redis_command_server()
        : io_context_(ruvia::test::new_test_io_context()),
          acceptor_(io_context_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          port_(acceptor_.local_endpoint().port()),
          command_read_future_(command_read_.get_future()),
          release_future_(release_.get_future()),
          thread_([this] { run(); }) {}

    ~stalled_redis_command_server() {
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

    stalled_redis_command_server(const stalled_redis_command_server&) = delete;
    stalled_redis_command_server& operator=(const stalled_redis_command_server&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    void wait_until_command_read() {
        command_read_future_.get();
    }

private:
    static std::string read_line(asio::ip::tcp::socket& socket) {
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

    static void read_command(asio::ip::tcp::socket& socket) {
        char marker = 0;
        asio::read(socket, asio::buffer(&marker, 1));
        if (marker != '*') {
            throw std::runtime_error("invalid RESP command array");
        }
        const auto count_value = std::stoull(read_line(socket));
        if (count_value > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("RESP command array is too large");
        }
        const auto count = static_cast<std::size_t>(count_value);
        for (std::size_t index = 0; index != count; ++index) {
            asio::read(socket, asio::buffer(&marker, 1));
            if (marker != '$') {
                throw std::runtime_error("invalid RESP bulk string");
            }
            const auto length_value = std::stoull(read_line(socket));
            if (length_value > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("RESP bulk string is too large");
            }
            const auto length = static_cast<std::size_t>(length_value);
            std::string value(length, '\0');
            asio::read(socket, asio::buffer(value));
            std::array<char, 2> line_ending{};
            asio::read(socket, asio::buffer(line_ending));
            if (line_ending != std::array<char, 2>{'\r', '\n'}) {
                throw std::runtime_error("invalid RESP bulk ending");
            }
        }
    }

    void run() noexcept {
        try {
            asio::ip::tcp::socket socket(io_context_);
            acceptor_.accept(socket);
            // Complete a warm-up command first so the pool's persistent
            // serialization buffer is established before the stalled command.
            read_command(socket);
            constexpr std::string_view reply = ":1\r\n";
            asio::write(socket, asio::buffer(reply));

            std::array<char, 1> command{};
            std::error_code error;
            (void)socket.read_some(asio::buffer(command), error);
            if (error) {
                throw std::system_error(error);
            }
            command_read_.set_value();
            // Keep the peer open after the first command byte. Reading another
            // byte here would consume the already-buffered command and let the
            // local socket close before the cancellation request reaches the
            // worker under test.
            release_future_.wait();
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
    std::promise<void> release_;
    std::future<void> release_future_;
    std::thread thread_;
};

class single_reply_redis_command_server final {
public:
    single_reply_redis_command_server()
        : io_context_(ruvia::test::new_test_io_context()),
          acceptor_(io_context_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          port_(acceptor_.local_endpoint().port()),
          command_read_future_(command_read_.get_future()),
          thread_([this] { run(); }) {}

    ~single_reply_redis_command_server() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    single_reply_redis_command_server(const single_reply_redis_command_server&) = delete;
    single_reply_redis_command_server& operator=(const single_reply_redis_command_server&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    void wait_until_command_read() {
        command_read_future_.get();
    }

    [[nodiscard]] const std::vector<std::string>& arguments() const noexcept {
        return arguments_;
    }

private:
    static std::string read_line(asio::ip::tcp::socket& socket) {
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

    static std::vector<std::string> read_command(asio::ip::tcp::socket& socket) {
        char marker = 0;
        asio::read(socket, asio::buffer(&marker, 1));
        if (marker != '*') {
            throw std::runtime_error("invalid RESP command array");
        }
        const auto count_value = std::stoull(read_line(socket));
        if (count_value > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("RESP command array is too large");
        }
        const auto count = static_cast<std::size_t>(count_value);
        std::vector<std::string> result;
        result.reserve(count);
        for (std::size_t index = 0; index != count; ++index) {
            asio::read(socket, asio::buffer(&marker, 1));
            if (marker != '$') {
                throw std::runtime_error("invalid RESP bulk string");
            }
            const auto length_value = std::stoull(read_line(socket));
            if (length_value > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("RESP bulk string is too large");
            }
            const auto length = static_cast<std::size_t>(length_value);
            std::string value(length, '\0');
            asio::read(socket, asio::buffer(value));
            std::array<char, 2> line_ending{};
            asio::read(socket, asio::buffer(line_ending));
            if (line_ending != std::array<char, 2>{'\r', '\n'}) {
                throw std::runtime_error("invalid RESP bulk ending");
            }
            result.push_back(std::move(value));
        }
        return result;
    }

    void run() noexcept {
        try {
            asio::ip::tcp::socket socket(io_context_);
            acceptor_.accept(socket);
            constexpr std::string_view reply = ":1\r\n";
            (void)read_command(socket);
            asio::write(socket, asio::buffer(reply));
            arguments_ = read_command(socket);
            command_read_.set_value();
            asio::write(socket, asio::buffer(reply));
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
    std::vector<std::string> arguments_;
    std::thread thread_;
};

[[nodiscard]] ruvia::detail::redis_definition_type redis_definition(std::string_view alias,
    const ruvia::redis_config& config = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    return {
        std::pmr::string(alias, resource),
        ruvia::detail::redis_config_storage(config, resource),
    };
}

[[nodiscard]] test_redis_user make_user(std::pmr::memory_resource* resource,
    std::string_view id = "u-1") {
    test_redis_user user(resource);
    user.set<"id">(id);
    user.set<"name">("Alice");
    user.set<"active">(true);
    user.set<"role">("admin");
    user.set<"age">(32);
    user.set<"score">(ruvia::double_value{4.5});
    return user;
}

[[nodiscard]] std::pmr::string test_redis_entity_key(
    std::string_view id, std::pmr::memory_resource* resource) {
    return ruvia::detail::redis_entity_key<test_redis_user>(
        id, resource, test_redis_repository_config.prefix_);
}

[[nodiscard]] ruvia::redis_value redis_hash_reply(std::pmr::memory_resource* resource,
    std::string_view id = "u-1") {
    std::pmr::vector<ruvia::redis_value> fields(resource);
    const auto add = [&fields, resource](std::string_view name, std::string_view value) {
        fields.emplace_back(ruvia::detail::redis_types_access::string_value(name, resource));
        fields.emplace_back(ruvia::detail::redis_types_access::string_value(value, resource));
    };
    add("__ruvia_entity", "1");
    add("id", id);
    add("name", "Alice");
    add("active", "1");
    add("role", "admin");
    add("age", "32");
    add("score", "4.5");
    add("__ruvia_tag_name", "x416c696365");
    return ruvia::detail::redis_types_access::array_value(std::move(fields), resource);
}

[[nodiscard]] ruvia::redis_value redis_search_reply(std::pmr::memory_resource* resource,
    std::string_view id = "u-1") {
    std::pmr::vector<ruvia::redis_value> values(resource);
    values.emplace_back(ruvia::detail::redis_types_access::integer_value(1, resource));
    const auto key = test_redis_entity_key(id, resource);
    values.emplace_back(ruvia::detail::redis_types_access::string_value(key, resource));
    values.emplace_back(redis_hash_reply(resource, id));
    return ruvia::detail::redis_types_access::array_value(std::move(values), resource);
}

[[nodiscard]] ruvia::task<ruvia::redis_value> immediate_redis_reply(ruvia::redis_value reply) {
    co_return reply;
}

template <typename fn_type>
[[nodiscard]] bool throws_protocol(fn_type&& function) {
    try {
        function();
    } catch (const ruvia::redis_error& error) {
        return error.code() == ruvia::redis_error::code_type::protocol_error;
    } catch (...) {
    }
    return false;
}

template <typename fn_type>
[[nodiscard]] bool throws_invalid_argument(fn_type&& function) {
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
    ruvia::test::counting_memory_resource wire_resource;
    ruvia::test::counting_memory_resource result_resource;

    auto one = ruvia::detail::redis_map_one<test_redis_user>{
        test_redis_entity_key("u-1", &result_resource),
        std::pmr::string(test_redis_repository_config.prefix_, &result_resource)}(redis_hash_reply(&wire_resource), &result_resource);
    RUVIA_CHECK(one.has_value());
    RUVIA_CHECK_EQ(one->get<"id">().view(), std::string_view("u-1"));
    RUVIA_CHECK_EQ(std::string_view(one->get<"name">()), std::string_view("Alice"));
    RUVIA_CHECK(one->get<"active">());
    RUVIA_CHECK_EQ(one->get<"age">(), 32);
    RUVIA_CHECK_EQ(static_cast<double>(one->get<"score">()), 4.5);

    {
        auto page = ruvia::detail::redis_map_search<test_redis_user>{
            std::pmr::string(test_redis_repository_config.prefix_, &result_resource)}(redis_search_reply(&wire_resource), &result_resource);
        RUVIA_CHECK_EQ(page.second, std::uint64_t{1});
        RUVIA_CHECK_EQ(page.first.size(), std::size_t{1});
        RUVIA_CHECK_EQ(page.first[0].get<"id">().view(), std::string_view("u-1"));
    }

    const auto allocations = result_resource.allocation_count();
    RUVIA_CHECK(allocations > 0);
    one.reset();
    RUVIA_CHECK_EQ(result_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK(wire_resource.deallocation_count() > 0);
}

RUVIA_TEST(redis_repository_mapping_rejects_untrusted_hash_replies) {
    ruvia::test::counting_memory_resource resource;

    auto no_marker = redis_hash_reply(&resource);
    // Replace the first marker with an ordinary field. The required fields
    // remain present, so this specifically exercises ownership validation.
    std::pmr::vector<ruvia::redis_value> fields_value(&resource);
    fields_value.emplace_back(ruvia::detail::redis_types_access::string_value("id", &resource));
    fields_value.emplace_back(ruvia::detail::redis_types_access::string_value("u-1", &resource));
    fields_value.emplace_back(ruvia::detail::redis_types_access::string_value("name", &resource));
    fields_value.emplace_back(ruvia::detail::redis_types_access::string_value("Alice", &resource));
    fields_value.emplace_back(ruvia::detail::redis_types_access::string_value("active", &resource));
    fields_value.emplace_back(ruvia::detail::redis_types_access::string_value("1", &resource));
    no_marker = ruvia::detail::redis_types_access::array_value(std::move(fields_value), &resource);
    RUVIA_CHECK(throws_protocol([&] {
        (void)ruvia::detail::redis_map_one<test_redis_user>{
            test_redis_entity_key("u-1", &resource),
            std::pmr::string(test_redis_repository_config.prefix_, &resource)}(
            std::move(no_marker), &resource);
    }));

    RUVIA_CHECK(throws_protocol([&] {
        std::pmr::vector<ruvia::redis_value> odd_fields(&resource);
        odd_fields.emplace_back(
            ruvia::detail::redis_types_access::string_value("__ruvia_entity", &resource));
        auto odd = ruvia::detail::redis_types_access::array_value(std::move(odd_fields), &resource);
        (void)ruvia::detail::redis_map_one<test_redis_user>{
            test_redis_entity_key("u-1", &resource),
            std::pmr::string(test_redis_repository_config.prefix_, &resource)}(
            std::move(odd), &resource);
    }));

    RUVIA_CHECK(throws_protocol([&] {
        auto wrong_key = redis_hash_reply(&resource, "other");
        (void)ruvia::detail::redis_map_one<test_redis_user>{
            test_redis_entity_key("u-1", &resource),
            std::pmr::string(test_redis_repository_config.prefix_, &resource)}(
            std::move(wrong_key), &resource);
    }));

    RUVIA_CHECK(throws_protocol([&] {
        auto malformed = redis_hash_reply(&resource);
        // The decoder rejects non-numeric values in numeric entity columns.
        std::pmr::vector<ruvia::redis_value> malformed_fields(&resource);
        const auto add = [&malformed_fields, &resource](std::string_view name,
                             std::string_view value) {
            malformed_fields.emplace_back(
                ruvia::detail::redis_types_access::string_value(name, &resource));
            malformed_fields.emplace_back(
                ruvia::detail::redis_types_access::string_value(value, &resource));
        };
        add("__ruvia_entity", "1");
        add("id", "u-1");
        add("name", "Alice");
        add("active", "1");
        add("age", "not-a-number");
        malformed = ruvia::detail::redis_types_access::array_value(
            std::move(malformed_fields), &resource);
        (void)ruvia::detail::redis_map_one<test_redis_user>{
            test_redis_entity_key("u-1", &resource),
            std::pmr::string(test_redis_repository_config.prefix_, &resource)}(
            std::move(malformed), &resource);
    }));
}

RUVIA_TEST(redis_repository_storage_keys_do_not_overlap_logical_prefixes) {
    ruvia::test::counting_memory_resource resource;
    const auto user_key = ruvia::detail::redis_entity_key<test_redis_user>("admin:42", &resource);
    const auto admin_key = ruvia::detail::redis_entity_key<test_redis_admin_user>("42", &resource);
    const auto user_prefix = ruvia::detail::redis_entity_storage_prefix<test_redis_user>();
    const auto admin_prefix = ruvia::detail::redis_entity_storage_prefix<test_redis_admin_user>();

    RUVIA_CHECK(user_prefix != admin_prefix);
    RUVIA_CHECK(user_key != admin_key);
    RUVIA_CHECK(!user_key.starts_with(admin_prefix));
    RUVIA_CHECK(!admin_key.starts_with(user_prefix));
}

RUVIA_TEST(redis_repository_command_mappers_validate_wire_results) {
    ruvia::test::counting_memory_resource resource;

    auto inserted = ruvia::detail::redis_orm_exec_result(
        ruvia::detail::redis_types_access::integer_value(1, &resource), &resource);
    RUVIA_CHECK_EQ(inserted.affected_entities(), std::uint64_t{1});

    auto updated = ruvia::detail::redis_orm_exec_result(
        ruvia::detail::redis_types_access::integer_value(2, &resource), &resource);
    RUVIA_CHECK_EQ(updated.affected_entities(), std::uint64_t{1});

    auto skipped = ruvia::detail::redis_orm_exec_result(
        ruvia::detail::redis_types_access::integer_value(0, &resource), &resource);
    RUVIA_CHECK_EQ(skipped.affected_entities(), std::uint64_t{0});

    auto deleted = ruvia::detail::redis_orm_delete_result(
        ruvia::detail::redis_types_access::integer_value(1, &resource), &resource);
    RUVIA_CHECK_EQ(deleted.affected_entities(), std::uint64_t{1});
    auto missing = ruvia::detail::redis_orm_delete_result(
        ruvia::detail::redis_types_access::integer_value(0, &resource), &resource);
    RUVIA_CHECK_EQ(missing.affected_entities(), std::uint64_t{0});

    RUVIA_CHECK(ruvia::detail::redis_orm_boolean_result(
        ruvia::detail::redis_types_access::integer_value(1, &resource), &resource));
    RUVIA_CHECK_EQ(ruvia::detail::redis_orm_count(
                       ruvia::detail::redis_types_access::integer_value(17, &resource)),
        std::uint64_t{17});
    RUVIA_CHECK(throws_protocol([&] {
        (void)ruvia::detail::redis_orm_exec_result(
            ruvia::detail::redis_types_access::integer_value(7, &resource), &resource);
    }));
    RUVIA_CHECK(throws_protocol([&] {
        (void)ruvia::detail::redis_orm_delete_result(
            ruvia::detail::redis_types_access::integer_value(2, &resource), &resource);
    }));
    RUVIA_CHECK(throws_protocol([&] {
        (void)ruvia::detail::redis_orm_count(
            ruvia::detail::redis_types_access::integer_value(-1, &resource));
    }));
    RUVIA_CHECK(throws_protocol([&] {
        ruvia::detail::redis_orm_status_result(
            ruvia::detail::redis_types_access::string_value("QUEUED", &resource), &resource);
    }));
}

RUVIA_TEST(redis_repository_async_mapping_reclaims_replies_and_retains_results) {
    auto& io_context = ruvia::test::new_test_io_context();
    ruvia::test::counting_memory_resource wire_resource;
    ruvia::test::counting_memory_resource result_resource;

    auto exercise = [&]() -> ruvia::task<void> {
        std::pmr::vector<test_redis_user> retained(&result_resource);
        retained.reserve(1);
        for (int index = 0; index != 32; ++index) {
            auto mapped = co_await ruvia::detail::map_redis_command<std::optional<test_redis_user>>(
                immediate_redis_reply(redis_hash_reply(&wire_resource)), &result_resource,
                ruvia::detail::redis_map_one<test_redis_user>{
                    test_redis_entity_key("u-1", &result_resource),
                    std::pmr::string(test_redis_repository_config.prefix_, &result_resource)});
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

    auto result_value = asio::co_spawn(
        io_context, ruvia::as_awaitable(exercise()), asio::use_future);
    io_context.run();
    result_value.get();
    RUVIA_CHECK_EQ(wire_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(result_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK(wire_resource.deallocation_count() > 0);
    RUVIA_CHECK(result_resource.deallocation_count() > 0);
}

RUVIA_TEST(redis_repository_async_mapping_reclaims_exception_frames) {
    auto& io_context = ruvia::test::new_test_io_context();
    ruvia::test::counting_memory_resource reply_resource;
    const auto baseline = reply_resource.live_allocations();

    auto exercise = [&]() -> ruvia::task<void> {
        for (int index = 0; index != 32; ++index) {
            bool rejected = false;
            try {
                const std::string invalid_status(128, 'X');
                auto operation = ruvia::detail::map_redis_command<void>(
                    immediate_redis_reply(ruvia::detail::redis_types_access::string_value(
                        invalid_status, &reply_resource)),
                    &reply_resource, ruvia::detail::redis_orm_status_result);
                (void)co_await std::move(operation);
            } catch (const ruvia::redis_error& error) {
                rejected = error.code() == ruvia::redis_error::code_type::protocol_error;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(reply_resource.live_allocations(), baseline);
        }
    };

    auto result_value = asio::co_spawn(
        io_context, ruvia::as_awaitable(exercise()), asio::use_future);
    io_context.run();
    result_value.get();
    RUVIA_CHECK(reply_resource.deallocation_count() > 0);
}

RUVIA_TEST(redis_repository_cold_operations_release_owned_arguments) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::counting_memory_resource operation_resource;
    const std::array definitions{redis_definition("default")};
    ruvia::detail::redis_registry registry(
        io_context, &operation_resource, definitions, worker.handle());
    ruvia::operation_scope scope;
    const auto handle = registry.get(scope);
    const auto repository = handle.get_repository<test_redis_user>(test_redis_repository_config);
    auto entity = make_user(&operation_resource);
    const auto baseline = operation_resource.live_allocations();

    for (int index = 0; index != 8; ++index) {
        {
            auto upsert = repository.upsert(entity);
            auto insert = repository.insert(entity);
            auto update = repository.update(test_redis_user::field<"id">() == "u-1", entity);
            auto many = repository.find();
            auto one = repository.find_one({.where_ = test_redis_user::field<"id">() == "u-1"});
            auto page = repository.find_and_count();
            auto count = repository.count({.where_ = test_redis_user::field<"name">() == "Alice"});
            auto exists = repository.exists({.where_ = test_redis_user::field<"name">() == "Alice"});
            auto removed = repository.delete_by(test_redis_user::field<"id">() == "u-1");
            auto removed_entity = repository.remove(entity);
            auto expire = repository.expire(
                test_redis_user::field<"id">() == "u-1", std::chrono::seconds(30));
            auto ttl = repository.ttl(test_redis_user::field<"id">() == "u-1");
            auto create_index = repository.create_index();
            auto drop_index = repository.drop_index();
            (void)upsert;
            (void)insert;
            (void)update;
            (void)many;
            (void)one;
            (void)page;
            (void)count;
            (void)exists;
            (void)removed;
            (void)removed_entity;
            (void)expire;
            (void)ttl;
            (void)create_index;
            (void)drop_index;
            RUVIA_CHECK(scope.has_pending_operations());
        }
        RUVIA_CHECK(!scope.has_pending_operations());
        RUVIA_CHECK_EQ(operation_resource.live_allocations(), baseline);
    }
    RUVIA_CHECK(operation_resource.allocation_count() > 0);
    RUVIA_CHECK(operation_resource.deallocation_count() > 0);
}

RUVIA_TEST(redis_repository_owns_input_before_entity_is_destroyed) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::counting_memory_resource operation_resource;
    ruvia::test::tracking_resource input_resource;
    const std::array definitions{redis_definition("default")};
    ruvia::detail::redis_registry registry(
        io_context, &operation_resource, definitions, worker.handle());
    ruvia::operation_scope scope;
    const auto repository = registry.get(scope).get_repository<test_redis_user>(test_redis_repository_config);

    std::optional<test_redis_user> input;
    input.emplace(&input_resource);
    input->set<"id">(std::string(160, 'i'));
    input->set<"name">(std::string(160, 'n'));
    input->set<"active">(true);
    auto pending = repository.insert(*input);
    input.reset();
    input_resource.release();
    RUVIA_CHECK(!input_resource.deallocated_after_release());
    // The pending command owns its copied key/field/value arguments and can
    // be discarded after the caller-owned entity and allocator are gone.
    RUVIA_CHECK(scope.has_pending_operations());
    (void)pending;
    scope.close();
    RUVIA_CHECK(!scope.has_pending_operations());
    RUVIA_CHECK(!input_resource.deallocated_after_release());
}

RUVIA_TEST(redis_repository_insert_owns_input_through_async_handoff) {
    single_reply_redis_command_server server;
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::counting_memory_resource operation_resource;
    ruvia::test::tracking_resource input_resource;
    ruvia::redis_config config;
    config.host_ = "127.0.0.1";
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    config.port_ = server.port();
    config.pool_size_per_worker_ = 1;
    const auto definition = redis_definition("default", config);
    {
        ruvia::detail::redis_registry registry(
            io_context, &operation_resource,
            std::span<const ruvia::detail::redis_definition_type>(&definition, 1), worker.handle());
        ruvia::operation_scope scope;
        const auto repository =
            registry.get(scope).get_repository<test_redis_user>(test_redis_repository_config);
        const std::string input_id(160, 'i');
        const std::string input_name(160, 'n');
        std::size_t baseline_after_warmup = 0;

        auto exercise = [&]() -> ruvia::task<std::uint64_t> {
            test_redis_user warmup(std::pmr::get_default_resource());
            warmup.set<"id">(input_id);
            warmup.set<"name">(input_name);
            warmup.set<"active">(true);
            warmup.set<"role">("admin");
            const auto warmup_result = co_await repository.insert(warmup);
            const bool warmup_succeeded = warmup_result.affected_entities() == 1;
            baseline_after_warmup = operation_resource.live_allocations();

            std::optional<test_redis_user> input;
            input.emplace(&input_resource);
            input->set<"id">(input_id);
            input->set<"name">(input_name);
            input->set<"active">(true);
            input->set<"role">("admin");
            auto pending = repository.insert(*input);
            input.reset();
            input_resource.release();
            // Destroying the entity before the operation starts must be safe.
            // The repository has synchronously copied every field into operation storage.
            try {
                const auto result_value = (co_await std::move(pending)).affected_entities();
                co_return warmup_succeeded&& result_value == 1 ? 1 : 0;
            } catch (...) {
                co_return 0;
            }
        };
        std::promise<std::uint64_t> completion;
        auto result_value = completion.get_future();
        asio::co_spawn(io_context, ruvia::as_awaitable(exercise()),
            [&worker, &completion](std::exception_ptr error, std::uint64_t value) {
                if (error) {
                    completion.set_exception(std::move(error));
                } else {
                    completion.set_value(value);
                }
                worker.stop();
            });
        std::jthread runner([&worker] { worker.run(); });
        server.wait_until_command_read();
        const auto& args = server.arguments();
        bool contains_id = false;
        bool contains_name = false;
        bool contains_name_shadow = false;
        bool contains_name_shadow_value = false;
        bool contains_name_presence = false;
        std::string expected_name_shadow("x");
        for (std::size_t i = 0; i < input_name.size(); ++i) {
            expected_name_shadow += "6e";
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& arg = args[i];
            contains_id = contains_id || arg == input_id;
            contains_name = contains_name || arg == input_name;
            contains_name_shadow = contains_name_shadow || arg == "__ruvia_tag_name";
            if (i + 1 < args.size() && arg == "__ruvia_tag_name") {
                contains_name_shadow_value = args[i + 1] == expected_name_shadow;
            }
            if (i + 1 < args.size() && arg == "__ruvia_present_name") {
                contains_name_presence = args[i + 1] == "1";
            }
        }
        RUVIA_CHECK(contains_id);
        RUVIA_CHECK(contains_name);
        RUVIA_CHECK(contains_name_shadow);
        RUVIA_CHECK(contains_name_shadow_value);
        RUVIA_CHECK(contains_name_presence);
        RUVIA_CHECK_EQ(result_value.get(), std::uint64_t{1});
        runner.join();
        RUVIA_CHECK(!input_resource.deallocated_after_release());
        // The warm-up establishes the connection's cached serialized buffer.
        // The handoff must not add any live operation-owned allocations.
        RUVIA_CHECK_EQ(operation_resource.live_allocations(), baseline_after_warmup);
    }
    RUVIA_CHECK_EQ(operation_resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(redis_repository_pre_cancelled_operations_release_each_operation) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::counting_memory_resource operation_resource;
    const std::array definitions{redis_definition("default")};
    ruvia::detail::redis_registry registry(
        io_context, &operation_resource, definitions, worker.handle());
    ruvia::operation_scope scope;
    const auto handle = registry.get(scope);
    ruvia::stop_source cancellation;
    cancellation.request_stop();
    const auto repository = handle.with_options({.stop_token_ = cancellation.token()})
                                .get_repository<test_redis_user>(test_redis_repository_config);
    const std::string id(2048, 'i');
    const auto baseline = operation_resource.live_allocations();

    auto exercise = [&]() -> ruvia::task<void> {
        for (int index = 0; index != 64; ++index) {
            bool cancelled = false;
            try {
                auto operation = repository.exists({.where_ = test_redis_user::field<"name">() == id});
                (void)co_await std::move(operation);
            } catch (const ruvia::redis_error& error) {
                cancelled = error.code() == ruvia::redis_error::code_type::cancelled;
            }
            RUVIA_CHECK(cancelled);
            RUVIA_CHECK_EQ(operation_resource.live_allocations(), baseline);
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
    RUVIA_CHECK(operation_resource.allocation_count() > 0);
    RUVIA_CHECK_EQ(operation_resource.live_allocations(), baseline);
}

RUVIA_TEST(redis_repository_input_may_die_before_a_cancelled_await) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::counting_memory_resource operation_resource;
    ruvia::test::tracking_resource input_resource;
    const std::array definitions{redis_definition("default")};
    ruvia::detail::redis_registry registry(
        io_context, &operation_resource, definitions, worker.handle());
    ruvia::operation_scope scope;
    const auto handle = registry.get(scope);
    ruvia::stop_source cancellation;
    cancellation.request_stop();
    const auto repository = handle.with_options({.stop_token_ = cancellation.token()})
                                .get_repository<test_redis_user>(test_redis_repository_config);
    const auto before_pending = operation_resource.live_allocations();

    std::optional<test_redis_user> input;
    input.emplace(&input_resource);
    input->set<"id">(std::string(160, 'i'));
    input->set<"name">(std::string(160, 'n'));
    input->set<"active">(true);
    auto pending = repository.insert(*input);
    input.reset();
    input_resource.release();

    auto exercise = [&]() -> ruvia::task<void> {
        bool cancelled = false;
        try {
            (void)co_await std::move(pending);
        } catch (const ruvia::redis_error& error) {
            cancelled = error.code() == ruvia::redis_error::code_type::cancelled;
        }
        RUVIA_CHECK(cancelled);
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
    RUVIA_CHECK(!input_resource.deallocated_after_release());
    RUVIA_CHECK_EQ(operation_resource.live_allocations(), before_pending);
}

RUVIA_TEST(redis_repository_inflight_cancellation_releases_operation_storage) {
    stalled_redis_command_server server;
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::counting_memory_resource operation_resource;
    ruvia::redis_config config;
    config.host_ = "127.0.0.1";
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    config.port_ = server.port();
    config.pool_size_per_worker_ = 1;
    config.command_timeout_ = std::nullopt;
    const auto definition = redis_definition("default", config);
    {
        ruvia::detail::redis_registry registry(
            io_context, &operation_resource,
            std::span<const ruvia::detail::redis_definition_type>(&definition, 1), worker.handle());
        ruvia::operation_scope scope;
        const auto handle = registry.get(scope);
        ruvia::stop_source cancellation;
        const auto repository = handle.with_options({.stop_token_ = cancellation.token()})
                                    .get_repository<test_redis_user>(test_redis_repository_config);
        const std::string warmup_id(2048, 'w');
        std::size_t baseline_after_warmup = 0;
        bool warmup_succeeded = false;

        auto exercise = [&]() -> ruvia::task<ruvia::redis_error::code_type> {
            {
                auto warmup = repository.exists(
                    {.where_ = test_redis_user::field<"id">() == warmup_id});
                try {
                    warmup_succeeded = co_await std::move(warmup);
                } catch (const ruvia::redis_error& error) {
                    co_return error.code();
                }
            }
            baseline_after_warmup = operation_resource.live_allocations();

            try {
                auto operation = repository.exists(
                    {.where_ = test_redis_user::field<"name">() == "Alice"});
                (void)co_await std::move(operation);
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
        cancellation.request_stop();
        RUVIA_CHECK_EQ(result_value.get(), ruvia::redis_error::code_type::cancelled);
        RUVIA_CHECK(warmup_succeeded);
        runner.join();
        // The warm-up establishes the connection's cached serialized buffer.
        // The cancelled operation's own command storage must be reclaimed.
        RUVIA_CHECK_EQ(operation_resource.live_allocations(), baseline_after_warmup);
    }
    RUVIA_CHECK_EQ(operation_resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(redis_repository_rejects_invalid_input_before_io) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    const std::array definitions{redis_definition("default")};
    ruvia::detail::redis_registry registry(
        io_context, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope scope;
    const auto repository = registry.get(scope).get_repository<test_redis_user>(test_redis_repository_config);

    test_redis_user missing_id;
    missing_id.set<"name">("Alice");
    missing_id.set<"active">(true);
    RUVIA_CHECK(throws_invalid_argument([&] { (void)repository.insert(missing_id); }));
    RUVIA_CHECK(throws_invalid_argument(
        [&] { (void)repository.insert(make_user(std::pmr::get_default_resource()),
                  {.ttl_ = std::chrono::milliseconds(0)}); }));
    RUVIA_CHECK(throws_invalid_argument(
        [&] { (void)repository.insert(make_user(std::pmr::get_default_resource()),
                  {.ttl_ = std::chrono::milliseconds(10), .persist_ = true}); }));
    RUVIA_CHECK(throws_invalid_argument([&] {
        (void)repository.find_one({.where_ = test_redis_user::field<"id">() == ""});
    }));
}

RUVIA_TEST(redis_repository_rejects_operations_after_scope_closes) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    const std::array definitions{redis_definition("default")};
    ruvia::detail::redis_registry registry(
        io_context, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope scope;
    const auto repository = registry.get(scope).get_repository<test_redis_user>(test_redis_repository_config);
    scope.close();

    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)repository.exists({.where_ = test_redis_user::field<"name">() == "Alice"});
    }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)repository.find(); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)repository.create_index(); }));
}

RUVIA_TEST(redis_repository_expired_escaped_repository_releases_owned_mapping) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::counting_memory_resource operation_resource;
    const std::array definitions{redis_definition("default")};
    ruvia::detail::redis_registry registry(
        io_context, &operation_resource, definitions, worker.handle());
    ruvia::operation_scope scope;
    ruvia::redis_repository_config config;
    config.prefix_.assign(256, 'p');
    config.indexes_.push_back({.field_ = "name", .kind_ = redis_index_kind::tag});

    const auto baseline = operation_resource.live_allocations();
    auto repository = registry.get(scope).get_repository<test_redis_user>(config);
    auto escaped = std::move(repository);
    config.prefix_.clear();
    config.indexes_.clear();
    RUVIA_CHECK(operation_resource.live_allocations() > baseline);
    scope.close();
    RUVIA_CHECK_EQ(operation_resource.live_allocations(), baseline);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)escaped.find(); }));
}
