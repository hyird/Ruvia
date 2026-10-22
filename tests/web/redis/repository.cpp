#include <algorithm>
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
#include <asio/steady_timer.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <openssl/evp.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/web/redis/redis_handle.h"
#include "ruvia/web/redis/redis_repository.h"

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

RUVIA_REDIS_ENTITY(test_redis_unsigned_pk_user, "upk_users",
    RUVIA_REDIS_COLUMN(id, std::uint64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, std::pmr::string));

RUVIA_REDIS_ENTITY(test_redis_signed_pk_user, "spk_users",
    RUVIA_REDIS_COLUMN(id, std::int64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, std::pmr::string));

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

// Deterministic RESP2 peer: answers each command array with the next canned
// reply and records every received command. A stalled peer leaves its first
// command unanswered until the client closes the connection.
class redis_command_server final {
public:
    struct stalled_tag final {};

    explicit redis_command_server(std::vector<std::string> replies, std::chrono::milliseconds reply_delay = {})
        : redis_command_server(std::move(replies), false, reply_delay) {}

    explicit redis_command_server(stalled_tag)
        : redis_command_server({}, true, {}) {}

    ~redis_command_server() noexcept(false) {
        finish();
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void wait_until_commands_read() {
        if (commands_read_future_.wait_for(std::chrono::seconds(5)) !=
            std::future_status::ready) {
            throw std::runtime_error("Redis peer did not receive its commands");
        }
        commands_read_future_.get();
    }

    [[nodiscard]] const std::vector<std::vector<std::string>>& commands() const noexcept {
        return commands_;
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
    redis_command_server(std::vector<std::string> replies, bool stalled, std::chrono::milliseconds reply_delay)
        : replies_(std::move(replies)),
          reply_delay_(reply_delay),
          gate_(io_context_),
          acceptor_(io_context_, {asio::ip::tcp::v4(), 0}),
          socket_(io_context_),
          work_(asio::make_work_guard(io_context_)),
          commands_read_future_(commands_read_.get_future()),
          done_(asio::co_spawn(io_context_, serve(stalled), asio::use_future)),
          thread_([this] { io_context_.run(); }) {}

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
        std::vector<std::string> arguments;
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
            arguments.push_back(std::move(value));
        }
        commands_.push_back(std::move(arguments));
    }

    asio::awaitable<void> serve(bool stalled) {
        bool commands_ready = false;
        try {
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            if (stalled) {
                // Leave the command unanswered until public cancellation closes
                // the connection; no pool-buffer warm-up is required.
                char marker = 0;
                co_await asio::async_read(socket_, asio::buffer(&marker, 1), asio::use_awaitable);
                gate_.expires_at(asio::steady_timer::time_point::max());
                commands_read_.set_value();
                commands_ready = true;
                std::error_code error;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, error));
            } else {
                for (const auto& reply : replies_) {
                    co_await read_command();
                    if (reply_delay_.count() > 0) {
                        gate_.expires_after(reply_delay_);
                        co_await gate_.async_wait(asio::use_awaitable);
                    }
                    co_await asio::async_write(socket_, asio::buffer(reply), asio::use_awaitable);
                }
                commands_read_.set_value();
                commands_ready = true;
                std::array<char, 64> buffer{};
                for (;;) {
                    co_await socket_.async_read_some(asio::buffer(buffer), asio::use_awaitable);
                }
            }
        } catch (const std::system_error& error) {
            if (!commands_ready && !closing_) {
                commands_read_.set_exception(std::current_exception());
            }
            if (error.code() != asio::error::eof &&
                error.code() != asio::error::connection_reset &&
                !(closing_ && (error.code() == asio::error::operation_aborted ||
                                  error.code() == asio::error::bad_descriptor))) {
                throw;
            }
        } catch (...) {
            if (!commands_ready) {
                commands_read_.set_exception(std::current_exception());
            }
            throw;
        }
    }

    std::vector<std::string> replies_;
    std::chrono::milliseconds reply_delay_;
    asio::io_context io_context_;
    asio::steady_timer gate_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::promise<void> commands_read_;
    std::future<void> commands_read_future_;
    std::future<void> done_;
    std::vector<std::vector<std::string>> commands_;
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

[[nodiscard]] ruvia::redis_config server_config(const redis_command_server& server) {
    ruvia::redis_config config;
    config.host_ = "127.0.0.1";
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    config.port_ = server.port();
    config.pool_size_per_worker_ = 1;
    return config;
}

// Runs `exercise` against the connected client on the worker thread while the
// peer answers; a peer failure surfaces after the worker has stopped.
template <typename exercise_type>
void exercise_with_server(redis_test_worker& worker, ruvia::redis_client& client,
    redis_command_server& server, exercise_type exercise) {
    auto root = worker.loop().start(ruvia::test::exercise_backend_client(client, exercise));
    std::jthread runner([&worker] { worker.run(); });
    std::exception_ptr peer_failure;
    try {
        server.wait_until_commands_read();
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
}

[[nodiscard]] std::string sha1_hex(std::string_view text) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    std::size_t digest_size = 0;
    if (EVP_Q_digest(nullptr, "SHA1", nullptr, text.data(), text.size(), digest.data(), &digest_size) != 1) {
        throw std::runtime_error("test SHA1 digest failed");
    }
    std::string hex;
    for (std::size_t index = 0; index < digest_size; ++index) {
        hex.push_back("0123456789abcdef"[digest[index] >> 4]);
        hex.push_back("0123456789abcdef"[digest[index] & 0x0F]);
    }
    return hex;
}

}  // namespace

RUVIA_TEST(redis_repository_owns_input_before_entity_is_destroyed) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::tracking_resource input_resource;
    ruvia::test::with_connected_redis_client(worker.attachment(), {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        const auto repository = client.handle().repository<test_redis_user>(test_redis_repository_config);

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
    redis_command_server server({":1\r\n"});
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::tracking_resource input_resource;
    {
        ruvia::redis_client client(worker.loop(), server_config(server));
        const std::string input_id(160, 'i');
        const std::string input_name(160, 'n');

        std::uint64_t affected_entities = 0;
        exercise_with_server(worker, client, server, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
            const auto repository = connected.handle().repository<test_redis_user>(test_redis_repository_config);
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
        });
        RUVIA_CHECK_EQ(server.commands().size(), std::size_t{1});
        bool contains_id = false;
        bool contains_name = false;
        for (const auto& command : server.commands()) {
            for (const auto& arg : command) {
                contains_id = contains_id || arg == input_id;
                contains_name = contains_name || arg == input_name;
            }
        }
        RUVIA_CHECK(contains_id);
        RUVIA_CHECK(contains_name);
        RUVIA_CHECK_EQ(affected_entities, std::uint64_t{1});
        RUVIA_CHECK(!input_resource.deallocated_after_release());
    }
}

RUVIA_TEST(redis_repository_scripts_run_by_digest_and_reload_on_noscript) {
    // A warm server answers EVALSHA directly; a server whose script cache
    // lacks the script replies NOSCRIPT, and the same operation reloads the
    // body through EVAL under the digest it first named.
    const std::string noscript = "-NOSCRIPT No matching script. Please use EVAL.\r\n";
    for (const bool cached : {true, false}) {
        redis_command_server server(cached ? std::vector<std::string>{":1\r\n", ":1\r\n"}
                                           : std::vector<std::string>{noscript, ":1\r\n", noscript, ":1\r\n"});
        auto& io_context = ruvia::test::new_test_io_context();
        redis_test_worker worker(io_context);
        std::uint64_t inserted = 0;
        std::uint64_t removed = 0;
        {
            ruvia::redis_client client(worker.loop(), server_config(server));
            exercise_with_server(worker, client, server, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
                const auto repository = connected.handle().repository<test_redis_user>(test_redis_repository_config);
                const auto user = make_user(std::pmr::get_default_resource());
                inserted = (co_await repository.insert(user)).affected_entities();
                removed = (co_await repository.remove(user)).affected_entities();
            });
        }
        RUVIA_CHECK_EQ(inserted, std::uint64_t{1});
        RUVIA_CHECK_EQ(removed, std::uint64_t{1});
        const auto& commands = server.commands();
        const std::size_t expected_commands = cached ? 2 : 4;
        RUVIA_CHECK_EQ(commands.size(), expected_commands);
        if (commands.size() != expected_commands) {
            continue;
        }
        const std::size_t stride = cached ? 1 : 2;
        for (std::size_t operation = 0; operation < 2; ++operation) {
            const auto& by_digest = commands[operation * stride];
            RUVIA_CHECK(by_digest.size() >= 4 && by_digest[0] == "EVALSHA" && by_digest[1].size() == 40 &&
                        by_digest[2] == "1" && by_digest[3].ends_with(":u-1"));
            if (!cached) {
                const auto& reloaded = commands[operation * stride + 1];
                RUVIA_CHECK(reloaded.size() == by_digest.size() && reloaded[0] == "EVAL" &&
                            reloaded[1].find("redis.call") != std::string::npos);
                RUVIA_CHECK_EQ(sha1_hex(reloaded[1]), by_digest[1]);
                RUVIA_CHECK(std::equal(reloaded.begin() + 2, reloaded.end(), by_digest.begin() + 2, by_digest.end()));
            }
        }
        // Insert and delete are distinct scripts.
        RUVIA_CHECK(commands[0][1] != commands[stride][1]);
    }
}

RUVIA_TEST(redis_repository_script_reload_shares_the_operation_timeout) {
    using namespace ruvia;
    using namespace std::chrono_literals;
    // Each reply fits a fresh one-second budget, but their combined delay
    // exceeds it. Both commands must have reached the peer before timeout.
    for (const bool remove : {false, true}) {
        redis_command_server server({"-NOSCRIPT No matching script. Please use EVAL.\r\n", ":1\r\n"}, 650ms);
        auto& io_context = test::new_test_io_context();
        redis_test_worker worker(io_context);
        auto config = server_config(server);
        config.command_timeout_ = std::nullopt;
        bool timed_out = false;
        {
            redis_client client(worker.loop(), config);
            auto exercise = [&](redis_client& connected) -> task<void> {
                const auto repository = connected.handle({.timeout_ = 1s}).repository<test_redis_user>(test_redis_repository_config);
                const auto user = make_user(std::pmr::get_default_resource());
                try {
                    if (remove) {
                        (void)co_await repository.remove(user);
                    } else {
                        (void)co_await repository.insert(user);
                    }
                } catch (const redis_error& error) {
                    timed_out = error.code() == redis_error::code_type::timeout;
                }
            };
            auto root = worker.loop().start(test::exercise_backend_client(client, exercise));
            std::jthread runner([&worker] { worker.run(); });
            root.wait();
            worker.stop();
            runner.join();
            server.finish();
            root.get();
        }
        RUVIA_CHECK(timed_out);
        const auto& commands = server.commands();
        RUVIA_CHECK_EQ(commands.size(), std::size_t{2});
        if (commands.size() == 2) {
            RUVIA_CHECK(commands[0][0] == "EVALSHA");
            RUVIA_CHECK(commands[1][0] == "EVAL");
        }
    }
}

RUVIA_TEST(redis_repository_find_without_take_rejects_a_truncated_page) {
    for (const bool paged : {false, true}) {
        // An FT.SEARCH page that reports more matches than it returns.
        redis_command_server server({"*1\r\n:150\r\n"});
        auto& io_context = ruvia::test::new_test_io_context();
        redis_test_worker worker(io_context);
        bool truncated = false;
        std::size_t returned_rows = 1;
        {
            ruvia::redis_client client(worker.loop(), server_config(server));
            exercise_with_server(worker, client, server, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
                const auto repository =
                    connected.handle().repository<test_redis_user>(test_redis_repository_config);
                ruvia::redis_find_options options;
                if (paged) {
                    options.take_ = ruvia::redis_find_default_take;
                }
                try {
                    returned_rows = (co_await repository.find(options)).size();
                } catch (const ruvia::redis_error& error) {
                    truncated = error.code() == ruvia::redis_error::code_type::command_error;
                }
            });
        }
        // 150 matches with one page of 100 must never read as a complete result;
        // an explicit page accepts the same reply.
        RUVIA_CHECK(truncated != paged);
        RUVIA_CHECK_EQ(returned_rows, std::size_t{paged ? 0U : 1U});
        RUVIA_CHECK_EQ(server.commands().size(), std::size_t{1});
        if (server.commands().size() == 1) {
            const auto& args = server.commands().front();
            RUVIA_CHECK(args.size() >= 6U && args[0] == "FT.SEARCH" && args[3] == "LIMIT" && args[4] == "0" &&
                        args[5] == "100");
        }
    }
}

RUVIA_TEST(redis_repository_search_consumes_expired_documents_without_truncation) {
    const std::string reply = "*3\r\n:1\r\n$7\r\nusers:1\r\n$-1\r\n";
    redis_command_server server({reply, reply, reply});
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    {
        ruvia::redis_client client(worker.loop(), server_config(server));
        exercise_with_server(worker, client, server, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
            const auto repository =
                connected.handle().repository<test_redis_user>(test_redis_repository_config);
            // A returned null document occupies the complete search page even
            // though the expired entity is omitted from the decoded rows.
            const auto rows = co_await repository.find();
            RUVIA_CHECK(rows.empty());
            const auto first = co_await repository.find_one({});
            RUVIA_CHECK(!first.has_value());
            const auto [counted_rows, count] = co_await repository.find_and_count();
            RUVIA_CHECK(counted_rows.empty());
            RUVIA_CHECK_EQ(count, std::uint64_t{1});
        });
    }
    RUVIA_CHECK_EQ(server.commands().size(), std::size_t{3});
}

RUVIA_TEST(redis_repository_pre_cancelled_operations_report_cancellation) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::with_connected_redis_client(worker.attachment(), {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        const auto handle = client.handle();
        ruvia::stop_source cancellation;
        cancellation.request_stop();
        const auto repository = handle.with_options({.stop_token_ = cancellation.token()})
                                    .repository<test_redis_user>(test_redis_repository_config);
        const std::string id(2048, 'i');

        auto exercise = [&]() -> ruvia::task<void> {
            for (int index = 0; index != 64; ++index) {
                bool cancelled = false;
                try {
                    auto operation = repository.exists({.where_ = test_redis_user::column<"name">() == id});
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
        const auto handle = client.handle();
        ruvia::stop_source cancellation;
        cancellation.request_stop();
        const auto repository = handle.with_options({.stop_token_ = cancellation.token()})
                                    .repository<test_redis_user>(test_redis_repository_config);

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
    redis_command_server server(redis_command_server::stalled_tag{});
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    auto config = server_config(server);
    config.command_timeout_ = std::nullopt;
    {
        ruvia::redis_client client(worker.loop(), config);
        ruvia::stop_source cancellation;

        std::optional<ruvia::redis_error::code_type> observed_error;
        auto exercise = [&](ruvia::redis_client& connected) -> ruvia::task<void> {
            const auto repository = connected.handle({.stop_token_ = cancellation.token()})
                                        .repository<test_redis_user>(test_redis_repository_config);
            try {
                auto operation = repository.exists(
                    {.where_ = test_redis_user::column<"name">() == "Alice"});
                (void)co_await std::move(operation);
            } catch (const ruvia::redis_error& error) {
                observed_error = error.code();
            }
        };
        auto root = worker.loop().start(ruvia::test::exercise_backend_client(client, exercise));
        std::jthread runner([&worker] { worker.run(); });
        std::exception_ptr peer_failure;
        try {
            server.wait_until_commands_read();
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
        const auto repository = client.handle().repository<test_redis_user>(test_redis_repository_config);

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
            (void)repository.find_one({.where_ = test_redis_user::column<"id">() == ""});
        }));
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)repository.find_one({.take_ = std::numeric_limits<std::uint64_t>::max()});
        }));
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)repository.count({.skip_ = std::numeric_limits<std::uint64_t>::max()});
        }));
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)repository.exists({.order_ = ruvia::redis_find_order{.field_ = "missing_field"}});
        }));

        const ruvia::redis_repository_config indexed_user_config{
            .prefix_ = "users",
            .indexes_ = {
                {.field_ = "id", .kind_ = redis_index_kind::tag},
                {.field_ = "age", .kind_ = redis_index_kind::numeric},
            },
        };
        const auto indexed_users = client.handle().repository<test_redis_user>(indexed_user_config);
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)indexed_users.find({.where_ = test_redis_user::column<"age">() == true});
        }));
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)indexed_users.find({.where_ = test_redis_user::column<"age">().in({true, false})});
        }));
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)indexed_users.find({.where_ = test_redis_user::column<"id">() == ""});
        }));
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)indexed_users.find({.where_ = test_redis_user::column<"id">().in({"u-1", ""})});
        }));

        const auto unsigned_repo = client.handle().repository<test_redis_unsigned_pk_user>(
            {.prefix_ = "upk_users", .indexes_ = {{.field_ = "id", .kind_ = redis_index_kind::numeric}}});
        test_redis_unsigned_pk_user unsigned_changes;
        unsigned_changes.set<"name">("Alice");
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)unsigned_repo.find_one({.where_ = test_redis_unsigned_pk_user::column<"id">() == -1});
        }));
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)unsigned_repo.find_one({.where_ = test_redis_unsigned_pk_user::column<"id">() == -1.0});
        }));
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)unsigned_repo.update(test_redis_unsigned_pk_user::column<"id">() == -1, unsigned_changes);
        }));

        const auto signed_repo = client.handle().repository<test_redis_signed_pk_user>(
            {.prefix_ = "spk_users", .indexes_ = {{.field_ = "id", .kind_ = redis_index_kind::numeric}}});
        test_redis_signed_pk_user signed_changes;
        signed_changes.set<"name">("Bob");
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)signed_repo.find_one({.where_ = test_redis_signed_pk_user::column<"id">() ==
                                                  std::numeric_limits<std::uint64_t>::max()});
        }));
        RUVIA_CHECK(throws_invalid_argument([&] {
            (void)signed_repo.update(
                test_redis_signed_pk_user::column<"id">() == std::numeric_limits<std::uint64_t>::max(),
                signed_changes);
        }));
        co_return;
    });
}
