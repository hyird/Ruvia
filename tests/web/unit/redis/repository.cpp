#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
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

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task.h"
#include "ruvia/web/redis/redis_client.h"
#include "ruvia/web/redis/redis_entity.h"
#include "ruvia/web/redis/redis_find_options.h"
#include "ruvia/web/redis/redis_handle.h"
#include "ruvia/web/redis/redis_repository.h"
#include "ruvia/web/redis/redis_repository_types.h"
#include "ruvia/web/redis/redis_write_result.h"

#include "backend_client_fixture.h"
#include "memory_resource_fixture.h"
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

const ruvia::redis_repository_config test_redis_repository_config{
    .prefix_ = "users",
    .indexes_ = {{.field_ = "name", .kind_ = redis_index_kind::tag}},
};

class redis_test_worker final {
public:
    explicit redis_test_worker(asio::io_context& io_context)
        : io_context_(io_context),
          attachment_(ruvia::attach_event_loop(io_context)) {}

    redis_test_worker(const redis_test_worker&) = delete;
    redis_test_worker& operator=(const redis_test_worker&) = delete;

    ~redis_test_worker() {
        ruvia::test::retire_backend_attachment(attachment_);
    }

    [[nodiscard]] ruvia::event_loop loop() const noexcept {
        return attachment_.loop();
    }

    [[nodiscard]] ruvia::event_loop_attachment& attachment() noexcept {
        return attachment_;
    }

    void run() {
        io_context_.restart();
        attachment_.run();
    }

    void stop() noexcept {
        io_context_.stop();
    }

private:
    asio::io_context& io_context_;
    ruvia::event_loop_attachment attachment_;
};

class redis_command_server final {
public:
    enum class response { integer,
        stalled };

    explicit redis_command_server(response selected)
        : gate_(io_context_),
          acceptor_(io_context_, {asio::ip::tcp::v4(), 0}),
          socket_(io_context_),
          work_(asio::make_work_guard(io_context_)),
          command_read_future_(command_read_.get_future()),
          done_(asio::co_spawn(io_context_, serve(selected), asio::use_future)),
          thread_([this] { io_context_.run(); }) {}

    ~redis_command_server() noexcept(false) {
        finish();
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void wait_until_command_read() {
        if (command_read_future_.wait_for(std::chrono::seconds(5)) !=
            std::future_status::ready) {
            throw std::runtime_error("Redis peer did not receive a command");
        }
        command_read_future_.get();
    }

    [[nodiscard]] const std::vector<std::string>& arguments() const noexcept {
        return arguments_;
    }

    void finish() {
        if (finished_) {
            return;
        }
        finished_ = true;
        asio::post(io_context_, [this] {
            closing_ = true;
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
            gate_.cancel();
            work_.reset();
        });
        thread_.join();
        done_.get();
    }

private:
    asio::awaitable<std::string> read_line() {
        std::string result;
        char value = 0;
        for (;;) {
            co_await asio::async_read(socket_, asio::buffer(&value, 1), asio::use_awaitable);
            if (value == '\r') {
                co_await asio::async_read(socket_, asio::buffer(&value, 1), asio::use_awaitable);
                if (value != '\n') {
                    throw std::runtime_error("invalid RESP line ending");
                }
                co_return result;
            }
            result.push_back(value);
        }
    }

    asio::awaitable<void> read_command() {
        char marker = 0;
        co_await asio::async_read(socket_, asio::buffer(&marker, 1), asio::use_awaitable);
        if (marker != '*') {
            throw std::runtime_error("invalid RESP command array");
        }
        const auto count = std::stoull(co_await read_line());
        for (std::size_t index = 0; index != count; ++index) {
            co_await asio::async_read(socket_, asio::buffer(&marker, 1), asio::use_awaitable);
            if (marker != '$') {
                throw std::runtime_error("invalid RESP bulk string");
            }
            const auto length = std::stoull(co_await read_line());
            std::string value(length, '\0');
            co_await asio::async_read(socket_, asio::buffer(value), asio::use_awaitable);
            std::array<char, 2> line_ending{};
            co_await asio::async_read(socket_, asio::buffer(line_ending), asio::use_awaitable);
            if (line_ending != std::array<char, 2>{'\r', '\n'}) {
                throw std::runtime_error("invalid RESP bulk ending");
            }
            arguments_.push_back(std::move(value));
        }
    }

    asio::awaitable<void> serve(response selected) {
        bool command_ready = false;
        try {
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            if (selected == response::stalled) {
                // Leave the command unanswered until public cancellation closes
                // the connection; no pool-buffer warm-up is required.
                char marker = 0;
                co_await asio::async_read(socket_, asio::buffer(&marker, 1), asio::use_awaitable);
                gate_.expires_at(asio::steady_timer::time_point::max());
                command_read_.set_value();
                command_ready = true;
                std::error_code error;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, error));
            } else {
                co_await read_command();
                command_read_.set_value();
                command_ready = true;
                constexpr std::string_view reply = ":1\r\n";
                co_await asio::async_write(socket_, asio::buffer(reply), asio::use_awaitable);
                std::array<char, 64> buffer{};
                for (;;) {
                    co_await socket_.async_read_some(asio::buffer(buffer), asio::use_awaitable);
                }
            }
        } catch (const std::system_error& error) {
            if (!command_ready && !closing_) {
                command_read_.set_exception(std::current_exception());
            }
            if (error.code() != asio::error::eof &&
                error.code() != asio::error::connection_reset &&
                !(closing_ && (error.code() == asio::error::operation_aborted ||
                                  error.code() == asio::error::bad_descriptor))) {
                throw;
            }
        } catch (...) {
            if (!command_ready) {
                command_read_.set_exception(std::current_exception());
            }
            throw;
        }
    }

    asio::io_context io_context_;
    asio::steady_timer gate_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::promise<void> command_read_;
    std::future<void> command_read_future_;
    std::future<void> done_;
    std::vector<std::string> arguments_;
    bool closing_{false};
    bool finished_{false};
    std::thread thread_;
};

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

template <typename fn_type>
[[nodiscard]] bool throws_invalid_argument(fn_type&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

}  // namespace

RUVIA_TEST(redis_repository_owns_input_before_entity_is_destroyed) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::tracking_resource input_resource;
    ruvia::test::with_connected_redis_client(worker.attachment(), {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        const auto repository = client.get_repository<test_redis_user>(test_redis_repository_config);

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
        (void)pending;
        RUVIA_CHECK(!input_resource.deallocated_after_release());
        co_return;
    });
}

RUVIA_TEST(redis_repository_insert_owns_input_through_async_handoff) {
    redis_command_server server(redis_command_server::response::integer);
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::tracking_resource input_resource;
    ruvia::redis_config config;
    config.host_ = "127.0.0.1";
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    config.port_ = server.port();
    config.pool_size_per_worker_ = 1;
    {
        ruvia::redis_client client(worker.loop(), config);
        const std::string input_id(160, 'i');
        const std::string input_name(160, 'n');

        std::uint64_t affected_entities = 0;
        auto exercise = [&](ruvia::redis_client& connected) -> ruvia::task<void> {
            const auto repository = connected.get_repository<test_redis_user>(test_redis_repository_config);
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
            affected_entities = (co_await std::move(pending)).affected_entities();
        };
        auto root = worker.loop().start(ruvia::test::exercise_backend_client(client, exercise));
        std::jthread runner([&worker] { worker.run(); });
        std::exception_ptr peer_failure;
        try {
            server.wait_until_command_read();
        } catch (...) {
            peer_failure = std::current_exception();
            client.close();
        }
        root.wait();
        worker.stop();
        runner.join();
        server.finish();
        root.get();
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        const auto& args = server.arguments();
        bool contains_id = false;
        bool contains_name = false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& arg = args[i];
            contains_id = contains_id || arg == input_id;
            contains_name = contains_name || arg == input_name;
        }
        RUVIA_CHECK(contains_id);
        RUVIA_CHECK(contains_name);
        RUVIA_CHECK_EQ(affected_entities, std::uint64_t{1});
        RUVIA_CHECK(!input_resource.deallocated_after_release());
    }
}

RUVIA_TEST(redis_repository_pre_cancelled_operations_report_cancellation) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::with_connected_redis_client(worker.attachment(), {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        const auto handle = client.with_options({});
        ruvia::stop_source cancellation;
        cancellation.request_stop();
        const auto repository = handle.with_options({.stop_token_ = cancellation.token()})
                                    .get_repository<test_redis_user>(test_redis_repository_config);
        const std::string id(2048, 'i');

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
            }
        };

        co_await exercise();
    });
}

RUVIA_TEST(redis_repository_input_may_die_before_a_cancelled_await) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::tracking_resource input_resource;
    ruvia::test::with_connected_redis_client(worker.attachment(), {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        const auto handle = client.with_options({});
        ruvia::stop_source cancellation;
        cancellation.request_stop();
        const auto repository = handle.with_options({.stop_token_ = cancellation.token()})
                                    .get_repository<test_redis_user>(test_redis_repository_config);

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
        co_await exercise();
    });
    RUVIA_CHECK(!input_resource.deallocated_after_release());
}

RUVIA_TEST(redis_repository_inflight_operation_observes_cancellation) {
    redis_command_server server(redis_command_server::response::stalled);
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::redis_config config;
    config.host_ = "127.0.0.1";
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    config.port_ = server.port();
    config.pool_size_per_worker_ = 1;
    config.command_timeout_ = std::nullopt;
    {
        ruvia::redis_client client(worker.loop(), config);
        ruvia::stop_source cancellation;

        std::optional<ruvia::redis_error::code_type> observed_error;
        auto exercise = [&](ruvia::redis_client& connected) -> ruvia::task<void> {
            const auto repository = connected.with_options({.stop_token_ = cancellation.token()})
                                        .get_repository<test_redis_user>(test_redis_repository_config);
            try {
                auto operation = repository.exists(
                    {.where_ = test_redis_user::field<"name">() == "Alice"});
                (void)co_await std::move(operation);
            } catch (const ruvia::redis_error& error) {
                observed_error = error.code();
            }
        };
        auto root = worker.loop().start(ruvia::test::exercise_backend_client(client, exercise));
        std::jthread runner([&worker] { worker.run(); });
        std::exception_ptr peer_failure;
        try {
            server.wait_until_command_read();
        } catch (...) {
            peer_failure = std::current_exception();
            client.close();
        }
        cancellation.request_stop();
        root.wait();
        worker.stop();
        runner.join();
        server.finish();
        root.get();
        RUVIA_CHECK_EQ(observed_error,
            std::optional{ruvia::redis_error::code_type::cancelled});
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
    }
}

RUVIA_TEST(redis_repository_rejects_invalid_input_before_io) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::with_connected_redis_client(worker.attachment(), {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        const auto repository = client.with_options({}).get_repository<test_redis_user>(test_redis_repository_config);

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
        co_return;
    });
}
