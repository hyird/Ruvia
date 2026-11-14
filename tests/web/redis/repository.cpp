#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <istream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
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
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <openssl/evp.h>

#include "backend_client_fixture.h"
#include "blocking_pool.h"
#include "event_loop.h"
#include "http_server_fixture.h"
#include "redis.h"
#include "task.h"
#include "task_scope.h"
#include "worker_task_fixture.h"

namespace {

using ruvia::redis_index_kind;

RUVIA_REDIS_ENTITY(test_redis_user, "users", RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}), RUVIA_REDIS_COLUMN(name, std::string), RUVIA_REDIS_COLUMN(active, bool), RUVIA_REDIS_COLUMN(role, std::string, ruvia::redis_column_options{.nullable_ = true}), RUVIA_REDIS_COLUMN(age, std::int32_t, ruvia::redis_column_options{.nullable_ = true}), RUVIA_REDIS_COLUMN(score, double, ruvia::redis_column_options{.nullable_ = true}));

const ruvia::redis_repository_config test_redis_repository_config{
    .prefix_ = "users",
    .indexes_ = {{.column_ = "name", .kind_ = redis_index_kind::tag}},
};

RUVIA_REDIS_ENTITY(test_redis_unsigned_pk_user, "upk_users", RUVIA_REDIS_COLUMN(id, std::uint64_t, ruvia::redis_column_options{.primary_key_ = true}), RUVIA_REDIS_COLUMN(name, std::string));

RUVIA_REDIS_ENTITY(test_redis_signed_pk_user, "spk_users", RUVIA_REDIS_COLUMN(id, std::int64_t, ruvia::redis_column_options{.primary_key_ = true}), RUVIA_REDIS_COLUMN(name, std::string));

class redis_test_worker final {
public:
    explicit redis_test_worker(asio::io_context& io_context) :
        io_context_(io_context),
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

    explicit redis_command_server(std::vector<std::string> replies, std::chrono::milliseconds reply_delay = {}) :
        redis_command_server(std::move(replies), false, reply_delay) {}

    explicit redis_command_server(stalled_tag) :
        redis_command_server(std::vector<std::string>(), true, {}) {}

    ~redis_command_server() noexcept(false) {
        finish();
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void wait_until_commands_read() {
        if (commands_read_future_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
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
    redis_command_server(std::vector<std::string> replies, bool stalled, std::chrono::milliseconds reply_delay) :
        replies_(std::move(replies)),
        reply_delay_(reply_delay),
        gate_(io_context_),
        acceptor_(io_context_, {asio::ip::tcp::v4(), 0}),
        socket_(io_context_),
        work_(asio::make_work_guard(io_context_)),
        commands_read_future_(commands_read_.get_future()),
        done_(asio::co_spawn(io_context_, serve(stalled), asio::use_future)),
        thread_([this] {
            io_context_.run();
        }) {}

    asio::awaitable<std::string> read_line() {
        std::string result{};
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
        std::vector<std::string> arguments{};
        char marker = 0;
        co_await asio::async_read(socket_, asio::buffer(&marker, 1), asio::use_awaitable);
        if (marker != '*') {
            throw std::runtime_error("invalid RESP command array");
        }
        const auto count = std::stoull(std::string(co_await read_line()));
        for (std::size_t index = 0; index != count; ++index) {
            co_await asio::async_read(socket_, asio::buffer(&marker, 1), asio::use_awaitable);
            if (marker != '$') {
                throw std::runtime_error("invalid RESP bulk string");
            }
            const auto length = std::stoull(std::string(co_await read_line()));
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
            if (error.code() != asio::error::eof && error.code() != asio::error::connection_reset && !(closing_ && (error.code() == asio::error::operation_aborted || error.code() == asio::error::bad_descriptor))) {
                throw;
            }
        } catch (...) {
            if (!commands_ready) {
                commands_read_.set_exception(std::current_exception());
            }
            throw;
        }
    }

    std::vector<std::string> replies_{};
    std::chrono::milliseconds reply_delay_;
    asio::io_context io_context_;
    asio::steady_timer gate_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::promise<void> commands_read_;
    std::future<void> commands_read_future_;
    std::future<void> done_;
    std::vector<std::vector<std::string>> commands_{};
    bool closing_{false};
    bool finished_{false};
    std::thread thread_;
};

[[nodiscard]] test_redis_user make_user(std::string_view id = "u-1") {
    test_redis_user user;
    user.set<"id">(id);
    user.set<"name">("Alice");
    user.set<"active">(true);
    user.set<"role">("admin");
    user.set<"age">(32);
    user.set<"score">(4.5);
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

[[nodiscard]] ruvia::redis_config make_redis_config(const redis_command_server& server) {
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
void exercise_with_server(redis_test_worker& worker, ruvia::redis_client& client, redis_command_server& server, exercise_type exercise) {
    auto root = worker.loop().start(ruvia::test::exercise_backend_client(client, exercise));
    std::jthread runner([&worker] {
        worker.run();
    });
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
    std::move(root).get();
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
    std::string hex{};
    for (std::size_t index = 0; index < digest_size; ++index) {
        hex.push_back("0123456789abcdef"[digest[index] >> 4]);
        hex.push_back("0123456789abcdef"[digest[index] & 0x0F]);
    }
    return hex;
}

}  // namespace

TEST(Ruvia, redis_repository_owns_input_before_entity_is_destroyed) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::with_connected_redis_client(worker.attachment(), {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        const auto repository = client.redis().repository<test_redis_user>(test_redis_repository_config);

        std::optional<test_redis_user> input;
        input.emplace();
        input->set<"id">(std::string(160, 'i'));
        input->set<"name">(std::string(160, 'n'));
        input->set<"active">(true);
        auto pending = repository.insert(*input);
        input.reset();
        // The pending command owns its copied key/field/value arguments and can
        // be discarded after the caller-owned entity and allocator are gone.
        (void)pending;
        co_return;
    });
}

TEST(Ruvia, redis_repository_insert_owns_input_through_async_handoff) {
    redis_command_server server(std::vector<std::string>({std::string(":1\r\n")}));
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    {
        ruvia::redis_client client(worker.loop(), make_redis_config(server));
        const std::string input_id(160, 'i');
        const std::string input_name(160, 'n');

        std::uint64_t affected_entities = 0;
        exercise_with_server(worker, client, server, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
            const auto repository = connected.redis().repository<test_redis_user>(test_redis_repository_config);
            std::optional<test_redis_user> input;
            input.emplace();
            input->set<"id">(input_id);
            input->set<"name">(input_name);
            input->set<"active">(true);
            input->set<"role">("admin");
            auto pending = repository.insert(*input);
            input.reset();
            // Destroying the entity before the operation starts must be safe.
            // The repository has synchronously copied every field into operation storage.
            affected_entities = (co_await std::move(pending)).affected_entities();
        });
        EXPECT_EQ(server.commands().size(), std::size_t{1});
        bool contains_id = false;
        bool contains_name = false;
        for (const auto& command : server.commands()) {
            for (const auto& arg : command) {
                contains_id = contains_id || arg == input_id;
                contains_name = contains_name || arg == input_name;
            }
        }
        EXPECT_TRUE(contains_id);
        EXPECT_TRUE(contains_name);
        EXPECT_EQ(affected_entities, std::uint64_t{1});
    }
}

TEST(Ruvia, redis_repository_scripts_run_by_digest_and_reload_on_noscript) {
    // A warm server answers EVALSHA directly; a server whose script cache
    // lacks the script replies NOSCRIPT, and the same operation reloads the
    // body through EVAL under the digest it first named.
    const std::string noscript("-NOSCRIPT No matching script. Please use EVAL.\r\n");
    for (const bool cached : {true, false}) {
        redis_command_server server(cached ? std::vector<std::string>({std::string(":1\r\n"), std::string(":1\r\n")}) : std::vector<std::string>({std::string(noscript), std::string(":1\r\n"), std::string(noscript), std::string(":1\r\n")}));
        auto& io_context = ruvia::test::new_test_io_context();
        redis_test_worker worker(io_context);
        std::uint64_t inserted = 0;
        std::uint64_t removed = 0;
        {
            ruvia::redis_client client(worker.loop(), make_redis_config(server));
            exercise_with_server(worker, client, server, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
                const auto repository = connected.redis().repository<test_redis_user>(test_redis_repository_config);
                const auto user = make_user();
                inserted = (co_await repository.insert(user)).affected_entities();
                removed = (co_await repository.remove(user)).affected_entities();
            });
        }
        EXPECT_EQ(inserted, std::uint64_t{1});
        EXPECT_EQ(removed, std::uint64_t{1});
        const auto& commands = server.commands();
        const std::size_t expected_commands = cached ? 2 : 4;
        EXPECT_EQ(commands.size(), expected_commands);
        if (commands.size() != expected_commands) {
            continue;
        }
        const std::size_t stride = cached ? 1 : 2;
        for (std::size_t operation = 0; operation < 2; ++operation) {
            const auto& by_digest = commands[operation * stride];
            EXPECT_TRUE(by_digest.size() >= 4 && by_digest[0] == "EVALSHA" && by_digest[1].size() == 40 && by_digest[2] == "1" && by_digest[3].ends_with(":u-1"));
            if (!cached) {
                const auto& reloaded = commands[operation * stride + 1];
                EXPECT_TRUE(reloaded.size() == by_digest.size() && reloaded[0] == "EVAL" && reloaded[1].find("redis.call") != std::string::npos);
                EXPECT_EQ(sha1_hex(reloaded[1]), by_digest[1]);
                EXPECT_TRUE(std::equal(reloaded.begin() + 2, reloaded.end(), by_digest.begin() + 2, by_digest.end()));
            }
        }
        // Insert and delete are distinct scripts.
        EXPECT_TRUE(commands[0][1] != commands[stride][1]);
    }
}

TEST(Ruvia, redis_repository_script_reload_shares_the_operation_timeout) {
    using namespace ruvia;
    using namespace std::chrono_literals;
    // Each reply fits a fresh one-second budget, but their combined delay
    // exceeds it. Both commands must have reached the peer before timeout.
    for (const bool remove : {false, true}) {
        redis_command_server server(std::vector<std::string>({std::string("-NOSCRIPT No matching script. Please use EVAL.\r\n"), std::string(":1\r\n")}), 650ms);
        auto& io_context = test::new_test_io_context();
        redis_test_worker worker(io_context);
        auto config = make_redis_config(server);
        config.command_timeout_ = std::nullopt;
        bool timed_out = false;
        {
            redis_client client(worker.loop(), config);
            auto exercise = [&](redis_client& connected) -> task<void> {
                const auto repository = connected.redis().with_options({.timeout_ = 1s}).repository<test_redis_user>(test_redis_repository_config);
                const auto user = make_user();
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
            std::jthread runner([&worker] {
                worker.run();
            });
            root.wait();
            worker.stop();
            runner.join();
            server.finish();
            std::move(root).get();
        }
        EXPECT_TRUE(timed_out);
        const auto& commands = server.commands();
        EXPECT_EQ(commands.size(), std::size_t{2});
        if (commands.size() == 2) {
            EXPECT_TRUE(commands[0][0] == "EVALSHA");
            EXPECT_TRUE(commands[1][0] == "EVAL");
        }
    }
}

TEST(Ruvia, redis_repository_find_without_take_rejects_a_truncated_page) {
    for (const bool paged : {false, true}) {
        // An FT.SEARCH page that reports more matches than it returns.
        redis_command_server server(std::vector<std::string>({std::string("*1\r\n:150\r\n")}));
        auto& io_context = ruvia::test::new_test_io_context();
        redis_test_worker worker(io_context);
        bool truncated = false;
        std::size_t returned_rows = 1;
        {
            ruvia::redis_client client(worker.loop(), make_redis_config(server));
            exercise_with_server(worker, client, server, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
                const auto repository = connected.redis().repository<test_redis_user>(test_redis_repository_config);
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
        EXPECT_TRUE(truncated != paged);
        EXPECT_EQ(returned_rows, std::size_t{paged ? 0U : 1U});
        EXPECT_EQ(server.commands().size(), std::size_t{1});
        if (server.commands().size() == 1) {
            const auto& args = server.commands().front();
            EXPECT_TRUE(args.size() >= 6U && args[0] == "FT.SEARCH" && args[3] == "LIMIT" && args[4] == "0" && args[5] == "100");
        }
    }
}

TEST(Ruvia, redis_repository_search_consumes_expired_documents_without_truncation) {
    const std::string reply("*3\r\n:1\r\n$7\r\nusers:1\r\n$-1\r\n");
    redis_command_server server(std::vector<std::string>({std::string(reply), std::string(reply), std::string(reply)}));
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    {
        ruvia::redis_client client(worker.loop(), make_redis_config(server));
        exercise_with_server(worker, client, server, [&](ruvia::redis_client& connected) -> ruvia::task<void> {
            const auto repository = connected.redis().repository<test_redis_user>(test_redis_repository_config);
            // A returned null document occupies the complete search page even
            // though the expired entity is omitted from the decoded rows.
            const auto rows = co_await repository.find();
            EXPECT_TRUE(rows.empty());
            const auto first = co_await repository.find_one({});
            EXPECT_TRUE(!first.has_value());
            const auto [counted_rows, count] = co_await repository.find_and_count();
            EXPECT_TRUE(counted_rows.empty());
            EXPECT_EQ(count, std::uint64_t{1});
        });
    }
    EXPECT_EQ(server.commands().size(), std::size_t{3});
}

TEST(Ruvia, redis_repository_pre_cancelled_operations_report_cancellation) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::with_connected_redis_client(worker.attachment(), {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        const auto handle = client.redis();
        ruvia::stop_source cancellation;
        cancellation.request_stop();
        const auto repository = handle.with_options({.stop_token_ = cancellation.token()}).repository<test_redis_user>(test_redis_repository_config);
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
                EXPECT_TRUE(cancelled);
            }
        };

        co_await exercise();
    });
}

TEST(Ruvia, redis_repository_input_may_die_before_a_cancelled_await) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::with_connected_redis_client(worker.attachment(), {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        const auto handle = client.redis();
        ruvia::stop_source cancellation;
        cancellation.request_stop();
        const auto repository = handle.with_options({.stop_token_ = cancellation.token()}).repository<test_redis_user>(test_redis_repository_config);

        std::optional<test_redis_user> input;
        input.emplace();
        input->set<"id">(std::string(160, 'i'));
        input->set<"name">(std::string(160, 'n'));
        input->set<"active">(true);
        auto pending = repository.insert(*input);
        input.reset();

        auto exercise = [&]() -> ruvia::task<void> {
            bool cancelled = false;
            try {
                (void)co_await std::move(pending);
            } catch (const ruvia::redis_error& error) {
                cancelled = error.code() == ruvia::redis_error::code_type::cancelled;
            }
            EXPECT_TRUE(cancelled);
        };
        co_await exercise();
    });
}

TEST(Ruvia, redis_repository_inflight_operation_observes_cancellation) {
    redis_command_server server(redis_command_server::stalled_tag{});
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    auto config = make_redis_config(server);
    config.command_timeout_ = std::nullopt;
    {
        ruvia::redis_client client(worker.loop(), config);
        ruvia::stop_source cancellation;

        std::optional<ruvia::redis_error::code_type> observed_error;
        auto exercise = [&](ruvia::redis_client& connected) -> ruvia::task<void> {
            const auto repository = connected.redis().with_options({.stop_token_ = cancellation.token()}).repository<test_redis_user>(test_redis_repository_config);
            try {
                auto operation = repository.exists({.where_ = test_redis_user::column<"name">() == "Alice"});
                (void)co_await std::move(operation);
            } catch (const ruvia::redis_error& error) {
                observed_error = error.code();
            }
        };
        auto root = worker.loop().start(ruvia::test::exercise_backend_client(client, exercise));
        std::jthread runner([&worker] {
            worker.run();
        });
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
        std::move(root).get();
        EXPECT_EQ(observed_error, std::optional{ruvia::redis_error::code_type::cancelled});
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
    }
}

TEST(Ruvia, redis_repository_rejects_invalid_input_before_io) {
    auto& io_context = ruvia::test::new_test_io_context();
    redis_test_worker worker(io_context);
    ruvia::test::with_connected_redis_client(worker.attachment(), {}, [&](ruvia::redis_client& client) -> ruvia::task<void> {
        const auto repository = client.redis().repository<test_redis_user>(test_redis_repository_config);

        test_redis_user missing_id;
        missing_id.set<"name">("Alice");
        missing_id.set<"active">(true);
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)repository.insert(missing_id);
        }));
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)repository.insert(make_user(), {.ttl_ = std::chrono::milliseconds(0)});
        }));
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)repository.insert(make_user(), {.ttl_ = std::chrono::milliseconds(10), .persist_ = true});
        }));
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)repository.find_one({.where_ = test_redis_user::column<"id">() == ""});
        }));
        for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::min() + 5, std::int64_t{-1}}) {
            EXPECT_TRUE(throws_invalid_argument([&] {
                (void)repository.find_one({.take_ = invalid});
            }));
            EXPECT_TRUE(throws_invalid_argument([&] {
                (void)repository.count({.skip_ = invalid});
            }));
        }
        for (const auto invalid : {static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1, (std::numeric_limits<std::uint64_t>::max)()}) {
            EXPECT_TRUE(throws_invalid_argument([&] {
                (void)repository.find_one({.take_ = invalid});
            }));
            EXPECT_TRUE(throws_invalid_argument([&] {
                (void)repository.count({.skip_ = invalid});
            }));
        }
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)repository.exists({.order_ = ruvia::redis_find_order{.column_ = "missing_field"}});
        }));

        const ruvia::redis_repository_config indexed_user_config{
            .prefix_ = "users",
            .indexes_ = {
                {.column_ = "id", .kind_ = redis_index_kind::tag},
                {.column_ = "age", .kind_ = redis_index_kind::numeric},
            },
        };
        const auto indexed_users = client.redis().repository<test_redis_user>(indexed_user_config);
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)indexed_users.find({.where_ = test_redis_user::column<"age">() == true});
        }));
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)indexed_users.find({.where_ = test_redis_user::column<"age">().in({true, false})});
        }));
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)indexed_users.find({.where_ = test_redis_user::column<"id">() == ""});
        }));
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)indexed_users.find({.where_ = test_redis_user::column<"id">().in({"u-1", ""})});
        }));

        const auto unsigned_repo = client.redis().repository<test_redis_unsigned_pk_user>({.prefix_ = "upk_users", .indexes_ = {{.column_ = "id", .kind_ = redis_index_kind::numeric}}});
        test_redis_unsigned_pk_user unsigned_changes;
        unsigned_changes.set<"name">("Alice");
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)unsigned_repo.find_one({.where_ = test_redis_unsigned_pk_user::column<"id">() == -1});
        }));
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)unsigned_repo.find_one({.where_ = test_redis_unsigned_pk_user::column<"id">() == -1.0});
        }));
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)unsigned_repo.update(test_redis_unsigned_pk_user::column<"id">() == -1, unsigned_changes);
        }));

        const auto signed_repo = client.redis().repository<test_redis_signed_pk_user>({.prefix_ = "spk_users", .indexes_ = {{.column_ = "id", .kind_ = redis_index_kind::numeric}}});
        test_redis_signed_pk_user signed_changes;
        signed_changes.set<"name">("Bob");
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)signed_repo.find_one({.where_ = test_redis_signed_pk_user::column<"id">() == std::numeric_limits<std::uint64_t>::max()});
        }));
        EXPECT_TRUE(throws_invalid_argument([&] {
            (void)signed_repo.update(test_redis_signed_pk_user::column<"id">() == std::numeric_limits<std::uint64_t>::max(), signed_changes);
        }));
        co_return;
    });
}

namespace {
RUVIA_REDIS_ENTITY(redis_query_user, "query-user", RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}), RUVIA_REDIS_COLUMN(email, ruvia::string), RUVIA_REDIS_COLUMN(age, std::int32_t));
}

TEST(Ruvia, redis_predicate_composition_moves_source_and_preserves_range) {
    {
        std::string text(200, 'x');
        auto filter = redis_query_user::column<"email">() == text;
        text.assign(200, 'y');
        auto range = redis_query_user::column<"age">().between(18, 65);
        ruvia::redis_find_options options{.where_ = std::move(filter) && range};
        EXPECT_TRUE(filter.empty());
        EXPECT_TRUE(!options.where_.empty());
        EXPECT_TRUE(!range.empty());
    }
}

TEST(Ruvia, redis_predicate_supports_copy_and_where_or_where_combination) {
    {
        auto base = redis_query_user::column<"email">() == "alice@example.com";
        auto copied = base;
        EXPECT_TRUE(!base.empty());
        EXPECT_TRUE(!copied.empty());

        ruvia::redis_predicate combined;
        combined.and_where(copied);
        combined.or_where(redis_query_user::column<"age">() >= 21);
        EXPECT_TRUE(!combined.empty());
    }
}

namespace {
RUVIA_REDIS_ENTITY(redis_unsigned_pk_entity, "unsigned-pk", RUVIA_REDIS_COLUMN(id, std::uint64_t, ruvia::redis_column_options{.primary_key_ = true}), RUVIA_REDIS_COLUMN(name, ruvia::string));

RUVIA_REDIS_ENTITY(redis_signed_pk_entity, "signed-pk", RUVIA_REDIS_COLUMN(id, std::int64_t, ruvia::redis_column_options{.primary_key_ = true}), RUVIA_REDIS_COLUMN(name, ruvia::string));
}  // namespace

TEST(Ruvia, redis_predicate_holds_signed_and_unsigned_primary_key_comparisons) {
    const auto unsigned_eq = redis_unsigned_pk_entity::column<"id">() == std::uint64_t{42};
    const auto signed_eq = redis_signed_pk_entity::column<"id">() == std::int64_t{-7};
    EXPECT_TRUE(!unsigned_eq.empty());
    EXPECT_TRUE(!signed_eq.empty());
}

TEST(Ruvia, redis_predicate_self_combination_preserves_operands) {
    for (const bool use_or : {false, true}) {
        auto predicate = redis_signed_pk_entity::column<"id">() == std::int64_t{1};
        if (use_or) {
            predicate.or_where(predicate);
        } else {
            predicate.and_where(predicate);
        }
        EXPECT_TRUE(!predicate.empty());
    }
}

TEST(Ruvia, redis_predicate_negation_and_entity_column_name) {
    static_assert(redis_query_user::column_name<"email">() == "email");
    const auto pred = !(redis_query_user::column<"email">() == "alice@example.com");
    EXPECT_TRUE(!pred.empty());
    const ruvia::redis_predicate empty;
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)!empty;
    }));
}

namespace {
const ruvia::redis_repository_config search_config{
    .indexes_ = {
        {.column_ = "email", .kind_ = ruvia::redis_index_kind::tag, .sortable_ = true},
        {.column_ = "age", .kind_ = ruvia::redis_index_kind::numeric},
    },
};

template <typename command_type>
concept borrows_temporary_command = requires(command_type command) { std::move(command).arguments(); };
static_assert(!borrows_temporary_command<ruvia::redis_compiled_command>);
}  // namespace

TEST(Ruvia, redis_search_compilation_owns_configuration_and_binary_predicates) {
    auto command = [&] {
        std::string prefix = "offline:users";
        std::string column = "email";
        const ruvia::redis_repository_config config{
            .prefix_ = prefix,
            .indexes_ = {{.column_ = column, .kind_ = ruvia::redis_index_kind::tag, .sortable_ = true}},
        };
        const auto mapping = ruvia::redis_search_mapping::create<redis_query_user>(config);
        prefix.assign("changed");
        column.assign("age");
        std::string text("a|{ }\0", 6);
        std::string order = "email";
        ruvia::redis_find_options options{
            .where_ = redis_query_user::column<"email">() == text,
            .order_ = ruvia::redis_find_order{.column_ = order, .direction_ = ruvia::redis_order_direction::desc},
            .skip_ = 2,
            .take_ = 3,
        };
        text.assign("changed");
        return mapping.compile_find(options);
    }();
    const auto args = command.arguments();
    ASSERT_EQ(args.size(), std::size_t{11});
    EXPECT_EQ(args[1], "offline:users:idx");
    EXPECT_EQ(args[2], "@email:{x617c7b207d00}");
    EXPECT_EQ(args[4], "2");
    EXPECT_EQ(args[5], "3");
    EXPECT_EQ(args[7], "__ruvia_sort_email");
    EXPECT_EQ(args[8], "DESC");
}

TEST(Ruvia, redis_search_mapping_rejects_invalid_schema_and_query_fields_offline) {
    using mapping_type = ruvia::redis_search_mapping;
    for (const ruvia::redis_repository_config& config : {ruvia::redis_repository_config{.indexes_ = {{.column_ = "missing"}}}, ruvia::redis_repository_config{.indexes_ = {{.column_ = "age", .kind_ = ruvia::redis_index_kind::tag}}}, ruvia::redis_repository_config{.indexes_ = {{.column_ = "email"}, {.column_ = "email"}}}}) {
        EXPECT_THROW((void)mapping_type::create<redis_query_user>(config), std::invalid_argument);
    }
    const auto mapping = mapping_type::create<redis_query_user>(search_config);
    EXPECT_THROW((void)mapping.compile_find({.where_ = redis_query_user::column<"id">() == "id"}), std::invalid_argument);
    EXPECT_THROW((void)mapping.compile_find({.where_ = redis_signed_pk_entity::column<"id">() == 1}), std::invalid_argument);
    EXPECT_THROW((void)mapping.compile_find({.where_ = redis_query_user::column<"age">() == true}), std::invalid_argument);
    EXPECT_THROW((void)mapping.compile_find({.where_ = redis_query_user::column<"age">() == std::uint64_t{9007199254740992}}), std::invalid_argument);
    EXPECT_THROW((void)mapping.compile_find({.order_ = ruvia::redis_find_order{.column_ = "missing"}}, ruvia::redis_search_mode::count), std::invalid_argument);
    EXPECT_THROW((void)mapping.compile_find({.order_ = ruvia::redis_find_order{.column_ = "age"}}), std::invalid_argument);
    const auto no_indexes = mapping_type::create<redis_query_user>();
    EXPECT_THROW((void)no_indexes.compile_create_index(), std::invalid_argument);
}

TEST(Ruvia, redis_search_compilation_preserves_empty_tag_and_zero_first_page) {
    const auto mapping = ruvia::redis_search_mapping::create<redis_query_user>(search_config);
    const auto empty = mapping.compile_find({.where_ = redis_query_user::column<"email">() == ""});
    const auto missing = mapping.compile_find({.where_ = redis_query_user::column<"email">().is_null()});
    EXPECT_EQ(empty.arguments()[2], "@email:{x}");
    EXPECT_EQ(missing.arguments()[2], "-@__ruvia_present_email:{1}");
    const auto first = mapping.compile_find({.skip_ = 9, .take_ = 0}, ruvia::redis_search_mode::first);
    EXPECT_EQ(first.arguments()[4], "9");
    EXPECT_EQ(first.arguments()[5], "0");
    const auto count = mapping.compile_find({.skip_ = 9, .take_ = 4}, ruvia::redis_search_mode::count);
    EXPECT_EQ(count.arguments()[4], "0");
    EXPECT_EQ(count.arguments()[5], "0");
}

TEST(Ruvia, redis_search_mapping_and_commands_support_copy_and_move_assignment) {
    auto destination = ruvia::redis_search_mapping::create<redis_query_user>({});
    auto command = destination.compile_drop_index();
    {
        auto source = ruvia::redis_search_mapping::create<redis_query_user>(search_config);
        const auto copy = source;
        const auto copied_command = copy.compile_create_index();
        destination = source;
        destination = std::move(source);
        command = copied_command;
        auto moved_command = copy.compile_create_index();
        command = std::move(moved_command);
    }
    const auto recreated = destination.compile_create_index();
    ASSERT_EQ(command.arguments().size(), recreated.arguments().size());
    for (std::size_t i = 0; i != command.arguments().size(); ++i) {
        EXPECT_EQ(command.arguments()[i], recreated.arguments()[i]);
    }
}

TEST(Ruvia, redis_search_index_compilation_keeps_tag_shadows_and_documents) {
    const auto mapping = ruvia::redis_search_mapping::create<redis_query_user>({
        .prefix_ = "a:b",
        .indexes_ = {{.column_ = "email", .kind_ = ruvia::redis_index_kind::tag, .sortable_ = true}},
    });
    const auto create = mapping.compile_create_index();
    const std::string_view expected[]{
        "FT.CREATE",
        "a:b:idx",
        "ON",
        "HASH",
        "PREFIX",
        "1",
        "ruvia:orm:613a62:",
        "SCHEMA",
        "__ruvia_tag_email",
        "AS",
        "email",
        "TAG",
        "CASESENSITIVE",
        "__ruvia_present_email",
        "AS",
        "__ruvia_present_email",
        "TAG",
        "CASESENSITIVE",
        "email",
        "AS",
        "__ruvia_sort_email",
        "TAG",
        "SORTABLE",
        "NOINDEX",
    };
    ASSERT_EQ(create.arguments().size(), std::size(expected));
    for (std::size_t i = 0; i != std::size(expected); ++i) {
        EXPECT_EQ(create.arguments()[i], expected[i]);
    }
    const auto drop = mapping.compile_drop_index();
    ASSERT_EQ(drop.arguments().size(), std::size_t{2});
    EXPECT_EQ(drop.arguments()[0], "FT.DROPINDEX");
    EXPECT_EQ(drop.arguments()[1], "a:b:idx");
}

namespace {

RUVIA_REDIS_ENTITY(redis_user, "user", RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}), RUVIA_REDIS_COLUMN(name, std::string), RUVIA_REDIS_COLUMN(active, bool), RUVIA_REDIS_COLUMN(age, std::int32_t, ruvia::redis_column_options{.nullable_ = true}), RUVIA_REDIS_COLUMN(score, double, ruvia::redis_column_options{.nullable_ = true}));

RUVIA_REDIS_ENTITY(nullable_redis_text, "text_value", RUVIA_REDIS_COLUMN(id, std::int64_t, ruvia::redis_column_options{.primary_key_ = true}), RUVIA_REDIS_COLUMN(text, ruvia::string, ruvia::redis_column_options{.nullable_ = true}));

RUVIA_REDIS_ENTITY(numeric_redis_entity, "numeric_input", RUVIA_REDIS_COLUMN(id, std::int32_t, ruvia::redis_column_options{.primary_key_ = true}), RUVIA_REDIS_COLUMN(score, float, ruvia::redis_column_options{.nullable_ = true}));

}  // namespace

TEST(Ruvia, redis_entity_rejects_numeric_overflow_without_changing_identity_or_field_state) {
    numeric_redis_entity value;
    value.set<"id">(7);
    const auto above_signed = static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)()) + 1;
    bool id_rejected = false;
    try {
        value.set<"id">(above_signed);
    } catch (const std::out_of_range&) {
        id_rejected = true;
    }
    EXPECT_TRUE(id_rejected);
    EXPECT_EQ(value.get<"id">(), 7);
    // 绑定层接受的有限 double，仍可能无法表示为实体的 float 字段。
    const auto oversized = ruvia::from_json<ruvia::double_value>("1.7976931348623157e308");
    EXPECT_EQ(oversized.value_, (std::numeric_limits<double>::max)());
    for (int state = 0; state != 3; ++state) {
        if (state == 1) {
            value.set_null<"score">();
        } else if (state == 2) {
            value.set<"score">(1.5);
        }
        bool score_rejected = false;
        try {
            value.set<"score">(oversized);
        } catch (const std::out_of_range&) {
            score_rejected = true;
        }
        EXPECT_TRUE(score_rejected);
        EXPECT_EQ(value.is_set<"score">(), state != 0);
        EXPECT_EQ(value.is_null<"score">(), state == 1);
        if (state == 2) {
            EXPECT_EQ(value.get<"score">(), 1.5F);
        }
    }
    value.set<"id">(ruvia::int64{(std::numeric_limits<std::int32_t>::min)()});
    EXPECT_EQ(value.get<"id">(), (std::numeric_limits<std::int32_t>::min)());
}

TEST(Ruvia, redis_entity_tracks_unset_value_and_null_states) {
    redis_user user_value;

    EXPECT_EQ(redis_user::prefix(), std::string_view("user"));
    EXPECT_TRUE(!user_value.is_set<"id">());
    EXPECT_TRUE(!user_value.is_null<"id">());
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)user_value.get<"id">();
    }));

    user_value.set<"id">("u-1");
    user_value.set<"name">("Alice");
    user_value.set<"active">(true);
    user_value.set<"age">(32);
    user_value.set<"score">(4.5);
    EXPECT_TRUE(user_value.is_set<"id">());
    EXPECT_EQ(user_value.get<"id">().view(), std::string_view("u-1"));
    EXPECT_EQ(std::string_view(user_value.get<"name">()), std::string_view("Alice"));
    EXPECT_TRUE(user_value.get<"active">());
    EXPECT_EQ(user_value.get<"age">(), 32);
    EXPECT_EQ(user_value.get<"score">(), 4.5);

    user_value.set_null<"age">();
    EXPECT_TRUE(user_value.is_set<"age">());
    EXPECT_TRUE(user_value.is_null<"age">());
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)user_value.get<"age">();
    }));

    user_value.reset<"age">();
    EXPECT_TRUE(!user_value.is_set<"age">());
    EXPECT_TRUE(!user_value.is_null<"age">());
}

TEST(Ruvia, redis_entity_move_preserves_owned_values_and_resource) {
    {
        redis_user source_value;
        source_value.set<"id">(std::string(120, 'i'));
        source_value.set<"name">(std::string(120, 'n'));
        source_value.set<"active">(true);

        redis_user moved(std::move(source_value));
        EXPECT_EQ(moved.get<"id">().size(), std::size_t{120});
        EXPECT_EQ(std::string_view(moved.get<"name">()).size(), std::size_t{120});
        EXPECT_TRUE(moved.get<"active">());
        source_value.set<"id">("reused");
        source_value.set<"name">("new name");
        EXPECT_EQ(moved.get<"id">().size(), std::size_t{120});
        EXPECT_EQ(std::string_view(moved.get<"name">()).size(), std::size_t{120});
    }
}

RUVIA_REDIS_ENTITY(redis_byte_numbers, "byte_numbers", RUVIA_REDIS_COLUMN(id, std::int8_t, ruvia::redis_column_options{.primary_key_ = true}), RUVIA_REDIS_COLUMN(flags, std::uint8_t));

TEST(Ruvia, redis_entity_supports_8bit_integers) {
    redis_byte_numbers numbers;
    numbers.set<"id">(std::int8_t{-12});
    numbers.set<"flags">(std::uint8_t{250});
    EXPECT_EQ(numbers.get<"id">(), std::int8_t{-12});
    EXPECT_EQ(numbers.get<"flags">(), std::uint8_t{250});
}

TEST(Ruvia, redis_error_uses_runtime_error_message_and_stable_code) {
    const ruvia::redis_error error(ruvia::redis_error::code_type::timeout, "redis timed out");
    EXPECT_TRUE(error.code() == ruvia::redis_error::code_type::timeout);
    EXPECT_EQ(std::string_view(error.what()), std::string_view("redis timed out"));
}

namespace {

RUVIA_REDIS_ENTITY(client_user, "client_users", RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}), RUVIA_REDIS_COLUMN(name, ruvia::string))

// Deterministic RESP peer, not an external Redis dependency. All sockets and
// cancellation run on its own thread, including cleanup after a failed test.
class redis_peer final {
public:
    enum class mode_type { normal,
        coalesced_pings,
        coalesced_arrays,
        coalesced_large,
        huge_array,
        nested_arrays,
        deep_arrays,
        valid_nested_array,
        reject_then_ping,
        batch_echo,
        barrier_success,
        barrier_stall,
        barrier_malformed,
        barrier_error,
        transaction_success,
        transaction_exec_error,
        transaction_wrong_exec,
        transaction_queue_error,
        slow_blocking_connect,
        close_after_reply };

    explicit redis_peer(mode_type mode = mode_type::normal, asio::ssl::context* tls_context = nullptr) :
        mode_(mode),
        tls_context_(tls_context),
        acceptor_(io_, {asio::ip::tcp::v4(), 0}),
        socket_(io_),
        port_(acceptor_.local_endpoint().port()),
        work_(asio::make_work_guard(io_)),
        done_(asio::co_spawn(io_, serve(), asio::use_future)),
        thread_([this] {
            io_.run();
        }) {}

    ~redis_peer() {
        asio::post(io_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
            blocking_socket_.close(ignored);
            work_.reset();
        });
        thread_.join();
        try {
            done_.get();
        } catch (const std::system_error&) {
        }
    }

    ruvia::redis_config config() const {
        return {.host_ = "127.0.0.1", .port_ = port_, .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1, .connect_timeout_ = std::chrono::seconds(2), .command_timeout_ = std::chrono::seconds(2)};
    }

    void wait_for_blocked_command() {
        auto blocked = blocked_.get_future();
        if (blocked.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("Redis test peer did not receive the command");
        }
        blocked.get();
    }

    std::vector<std::string> expiration_command() {
        return expiration_.get_future().get();
    }

    std::vector<std::vector<std::string>> batch_commands() {
        return batch_commands_.get_future().get();
    }

    void wait_for_closed_connection() {
        auto closed = closed_.get_future();
        if (closed.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("Redis test peer did not close the connection");
        }
    }

    std::vector<std::string> authentication_command() const {
        return std::vector<std::string>(authentication_);
    }

private:
    // Longer than the blocking wait in the test plus its one-second reply
    // grace, and shorter than the configured connect timeout.
    static constexpr auto slow_select_delay = std::chrono::milliseconds(1500);

    template <typename stream_type>
    asio::awaitable<std::string> line(stream_type& stream, asio::streambuf& buffer) {
        co_await asio::async_read_until(stream, buffer, "\r\n", asio::use_awaitable);
        std::istream input(&buffer);
        std::string value{};
        std::getline(input, value);
        value.pop_back();
        co_return value;
    }

    asio::awaitable<void> serve() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        if (mode_ == mode_type::slow_blocking_connect) {
            // The ordinary pool keeps the first connection; the lazy blocking
            // pool opens the second, whose SELECT reply is delayed.
            asio::co_spawn(io_, serve_connection(socket_, buffer_, false), asio::detached);
            co_await acceptor_.async_accept(blocking_socket_, asio::use_awaitable);
            co_await serve_connection(blocking_socket_, blocking_buffer_, true);
            co_return;
        }
        // A TLS idle probe can retire a connection with unread session tickets.
        // Like Redis, keep accepting after a peer end instead of leaving the
        // next connection in the listen backlog without a handshake.
        for (;;) {
            try {
                co_await serve_transport();
            } catch (const std::system_error& error) {
                if (!acceptor_.is_open()) {
                    co_return;
                }
                if (error.code() != asio::error::eof && error.code() != asio::error::connection_reset && error.code() != asio::ssl::error::stream_truncated) {
                    throw;
                }
            }
            std::error_code ignored;
            socket_.close(ignored);
            buffer_.consume(buffer_.size());
            if (mode_ == mode_type::close_after_reply && first_connection_) {
                closed_.set_value();
            }
            first_connection_ = false;
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        }
    }

    asio::awaitable<void> serve_transport() {
        if (tls_context_ != nullptr) {
            asio::ssl::stream<asio::ip::tcp::socket&> stream(socket_, *tls_context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            co_await serve_connection(stream, buffer_, false);
        } else {
            co_await serve_connection(socket_, buffer_, false);
        }
    }

    template <typename stream_type>
    asio::awaitable<void> serve_connection(stream_type& stream, asio::streambuf& buffer, bool slow_select) {
        bool in_transaction = false;
        bool write_seen = false;
        std::vector<std::vector<std::string>> transaction_commands{};
        std::vector<std::string> queued_replies{};
        for (;;) {
            const auto header_value = co_await line(stream, buffer);
            const auto count = std::stoi(std::string(std::string_view(header_value).substr(1)));
            std::vector<std::string> args{};
            for (int index = 0; index < count; ++index) {
                const auto bulk = co_await line(stream, buffer);
                const auto size = static_cast<std::size_t>(std::stoul(std::string(std::string_view(bulk).substr(1))));
                if (buffer.size() < size + 2) {
                    co_await asio::async_read(stream, buffer, asio::transfer_exactly(size + 2 - buffer.size()), asio::use_awaitable);
                }
                std::string arg(size, '\0');
                std::istream input(&buffer);
                input.read(arg.data(), static_cast<std::streamsize>(size));
                input.ignore(2);
                args.push_back(std::move(arg));
            }
            if (args.front() == "STALL" || args.front() == "AUTH") {
                if (args.front() == "AUTH") {
                    authentication_ = args;
                }
                blocked_.set_value();
                // Read again without replying; client cancellation closes this socket.
                continue;
            }
            std::string reply{};
            const bool transaction_mode = mode_ == mode_type::transaction_success || mode_ == mode_type::transaction_exec_error || mode_ == mode_type::transaction_wrong_exec || mode_ == mode_type::transaction_queue_error;
            const bool barrier_mode = mode_ == mode_type::barrier_success || mode_ == mode_type::barrier_stall || mode_ == mode_type::barrier_malformed || mode_ == mode_type::barrier_error;
            if (barrier_mode && (args.front() == "SET" || args.front() == "WAIT" || args.front() == "WAITAOF")) {
                transaction_commands.push_back(args);
                if (args.front() == "SET") {
                    write_seen = true;
                    reply = "+OK\r\n";
                } else if (mode_ == mode_type::barrier_stall || mode_ == mode_type::barrier_malformed) {
                    batch_commands_.set_value(transaction_commands);
                    blocked_.set_value();
                    if (mode_ == mode_type::barrier_stall) {
                        // Leave the barrier reply unread until cancellation closes this socket.
                        continue;
                    }
                    reply = "?invalid\r\n";
                } else if (args.front() == "WAIT") {
                    reply = mode_ == mode_type::barrier_error ? "-ERR barrier failed\r\n" : (write_seen ? ":1\r\n" : ":0\r\n");
                } else {
                    // State belongs to this accepted socket, not to the peer.
                    reply = write_seen ? "*2\r\n:1\r\n:1\r\n" : "*2\r\n:0\r\n:0\r\n";
                    batch_commands_.set_value(transaction_commands);
                }
            } else if (transaction_mode && (in_transaction || args.front() == "MULTI")) {
                transaction_commands.push_back(args);
                if (args.front() == "MULTI") {
                    in_transaction = true;
                    reply = "+OK\r\n";
                } else if (args.front() == "EXEC") {
                    in_transaction = false;
                    batch_commands_.set_value(transaction_commands);
                    if (mode_ == mode_type::transaction_exec_error) {
                        reply = "-EXECABORT discarded\r\n";
                    } else if (mode_ == mode_type::transaction_wrong_exec) {
                        reply = "+OK\r\n";
                    } else {
                        reply = "*" + std::string(std::to_string(queued_replies.size())) + "\r\n";
                        for (const auto& queued : queued_replies) {
                            reply += queued;
                        }
                    }
                } else {
                    queued_replies.push_back("$" + std::string(std::to_string(args.back().size())) + std::string("\r\n") + args.back() + "\r\n");
                    reply = mode_ == mode_type::transaction_queue_error ? "-ERR queue failed\r\n" : "+QUEUED\r\n";
                }
            } else if (mode_ == mode_type::batch_echo && args.front() != "PING") {
                reply = "$" + std::string(std::to_string(args.back().size())) + std::string("\r\n") + args.back() + "\r\n";
            } else if ((mode_ == mode_type::coalesced_pings || mode_ == mode_type::coalesced_arrays || mode_ == mode_type::coalesced_large) && args.front() == "PING") {
                if (co_await line(stream, buffer) != "*1" || co_await line(stream, buffer) != "$4" || co_await line(stream, buffer) != "PING") {
                    throw std::runtime_error("expected second pipelined PING");
                }
                if (mode_ == mode_type::coalesced_large) {
                    reply = "$65537\r\n" + std::string(65537, 'x') + "\r\n+PONG\r\n";
                } else {
                    reply = mode_ == mode_type::coalesced_arrays ? "*3\r\n+\r\n+\r\n+\r\n*3\r\n+\r\n+\r\n+\r\n" : "+PONG\r\n+PONG\r\n";
                }
            } else if ((mode_ == mode_type::huge_array || (mode_ == mode_type::reject_then_ping && first_connection_)) && args.front() == "PING") {
                reply = "*1048576\r\n";
            } else if (mode_ == mode_type::nested_arrays && args.front() == "PING") {
                for (int index = 0; index < 100; ++index) {
                    reply.append("*300\r\n");
                }
            } else if (mode_ == mode_type::deep_arrays && args.front() == "PING") {
                for (int index = 0; index < 5; ++index) {
                    reply.append("*1\r\n");
                }
            } else if (mode_ == mode_type::valid_nested_array && args.front() == "PING") {
                for (int index = 0; index < 4; ++index) {
                    reply.append("*1\r\n");
                }
                reply.append("+PONG\r\n");
            } else if (args.front() == "PING") {
                reply = args.size() == 1 ? "+PONG\r\n" : "$" + std::string(std::to_string(args[1].size())) + std::string("\r\n") + args[1] + "\r\n";
            } else if (args.front() == "NESTED") {
                reply = "*2\r\n*2\r\n$20\r\n" + std::string(20, 'a') + "\r\n$20\r\n" + std::string(20, 'b') + "\r\n$1\r\nc\r\n";
            } else if (args.front() == "GET") {
                reply = "$128\r\n" + std::string(128, 'v') + "\r\n";
            } else if (args.front() == "EXPIREAT" || args.front() == "PEXPIREAT") {
                expiration_.set_value(args);
                reply = ":1\r\n";
            } else if (args.front() == "HGETALL") {
                reply = "*6\r\n$14\r\n__ruvia_entity\r\n$1\r\n1\r\n$2\r\nid\r\n$1\r\n1\r\n$4\r\nname\r\n$128\r\n" + std::string(128, 'n') + "\r\n";
            } else if (args.front() == "SELECT") {
                if (slow_select) {
                    asio::steady_timer delay(io_, slow_select_delay);
                    co_await delay.async_wait(asio::use_awaitable);
                }
                reply = "+OK\r\n";
            } else if (args.front() == "BLPOP") {
                reply = args[1] == "filled" ? "*2\r\n$6\r\nfilled\r\n$3\r\n" + std::string("a\0b", 3) + "\r\n" : "*-1\r\n";
            } else {
                reply = "-ERR test error\r\n";
            }
            co_await asio::async_write(stream, asio::buffer(reply), asio::use_awaitable);
            if (mode_ == mode_type::close_after_reply && first_connection_) {
                co_return;
            }
        }
    }

    mode_type mode_;
    asio::ssl::context* tls_context_;
    bool first_connection_{true};
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::ip::tcp::socket blocking_socket_{io_};
    std::uint16_t port_;
    asio::streambuf buffer_;
    asio::streambuf blocking_buffer_;
    std::promise<void> blocked_;
    std::promise<void> closed_;
    // Written before blocked_ is satisfied; read after wait_for_blocked_command().
    std::vector<std::string> authentication_{};
    std::promise<std::vector<std::string>> expiration_;
    std::promise<std::vector<std::vector<std::string>>> batch_commands_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::future<void> done_;
    std::thread thread_;
};

ruvia::task<void> check_commands(ruvia::redis_client& client) {
    co_await client.connect();
    bool duplicate_rejected = false;
    try {
        co_await client.connect();
    } catch (const std::logic_error&) {
        duplicate_rejected = true;
    }
    EXPECT_TRUE(duplicate_rejected);
    auto redis = client.redis();
    // A rejected second connect must not tear down the successful first one.
    co_await redis.ping();
    auto retained = co_await redis.get("retained");
    EXPECT_TRUE(retained.has_value());
    auto users = redis.repository<client_user>();
    auto user_value = co_await users.find_one({.where_ = client_user::column<"id">() == "1"});
    EXPECT_TRUE(user_value.has_value());
    std::string mutable_ping(128, 'm');
    auto cold_ping = redis.ping(mutable_ping);
    mutable_ping.assign(128, 'n');
    const auto owned_echo = co_await std::move(cold_ping);
    EXPECT_TRUE(owned_echo == std::string_view(std::string(128, 'm')));
    for (int index = 0; index < 32; ++index) {
        auto cold = redis.get(std::string(128, 'k'));
        (void)cold;
        auto echo = co_await redis.ping(std::string(128, 'x'));
        EXPECT_TRUE(echo == std::string_view(std::string(128, 'x')));
        EXPECT_TRUE(*retained == std::string_view(std::string(128, 'v')));
        EXPECT_TRUE(user_value->get<"name">().view() == std::string_view(std::string(128, 'n')));
    }
    bool failed = false;
    try {
        (void)co_await redis.set("bad", "value");
    } catch (const ruvia::redis_error&) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    co_await redis.ping();
    auto cold = redis.get("not-started");
    co_await client.shutdown();
    co_await client.shutdown();
    bool expired = false;
    try {
        (void)redis.get("closed");
    } catch (const std::logic_error&) {
        expired = true;
    }
    EXPECT_TRUE(expired);
    EXPECT_TRUE(*retained == std::string_view(std::string(128, 'v')));
}

ruvia::task<void> check_blocking_wait_starts_at_write(ruvia::redis_client& client) {
    co_await client.connect();
    auto redis = client.redis();
    const std::array<std::string_view, 1> keys{"filled"};
    const auto wait = ruvia::redis_block_wait::for_duration(std::chrono::milliseconds(100));
    // The lazy blocking connection spends longer in SELECT than the wait plus
    // its reply grace; that time must not count against the server-side wait.
    const auto first = co_await redis.blpop(keys, wait);
    EXPECT_TRUE(first && first->key() == "filled" && first->value() == std::string_view("a\0b", 3));
    // The connection stayed healthy: the peer accepts no third connection.
    const std::array<std::string_view, 1> empty_keys{"queue"};
    const auto second = co_await redis.blpop(empty_keys, wait);
    EXPECT_TRUE(!second.has_value());
    EXPECT_TRUE(first && first->value() == std::string_view("a\0b", 3));
    co_await client.shutdown();
}

ruvia::task<void> check_coalesced_replies(ruvia::redis_client& client, bool arrays = false) {
    co_await client.connect();
    auto pipeline = client.redis().pipeline();
    pipeline.command("PING").command("PING");
    bool accepted = false;
    try {
        const auto replies = co_await std::move(pipeline).exec();
        accepted = replies.size() == 2;
        if (accepted && arrays) {
            accepted = replies[0].kind() == ruvia::redis_value::kind_type::array && replies[1].kind() == ruvia::redis_value::kind_type::array && replies[0].array().size() == 3 && replies[1].array().size() == 3;
        } else if (accepted) {
            accepted = replies[0].string() == "PONG" && replies[1].string() == "PONG";
        }
    } catch (const ruvia::redis_error&) {
    }
    EXPECT_TRUE(accepted);
    co_await client.shutdown();
}

ruvia::task<void> check_owned_pipeline(ruvia::redis_client& client) {
    co_await client.connect();
    const std::string binary_key("key\0binary", 10);
    std::string binary_value(64 * 1024, 'v');
    binary_value[5] = '\0';
    auto cold = [&] {
        auto pipeline = client.redis().pipeline();
        auto key = decltype(binary_key)(binary_key);
        auto value = decltype(binary_value)(binary_value);
        pipeline.get(key).set(key, value).incr_by(key, std::numeric_limits<std::int64_t>::min()).zadd(key, 1.5, "member");
        key.assign(128, 'x');
        value.assign(128, 'y');
        return std::move(pipeline).exec();
    }();
    const auto retained = co_await std::move(cold);
    EXPECT_EQ(retained.size(), std::size_t{4});
    EXPECT_TRUE(retained[0].string() == std::string_view(binary_key));
    EXPECT_TRUE(retained[1].string() == std::string_view(binary_value));
    EXPECT_TRUE(retained[2].string() == "-9223372036854775808");
    EXPECT_TRUE(retained[3].string() == "member");
    auto next_value = client.redis().pipeline();
    next_value.get(std::string(256, 'n'));
    const auto replies = co_await std::move(next_value).exec();
    EXPECT_EQ(replies.size(), std::size_t{1});
    EXPECT_TRUE(retained[1].string() == std::string_view(binary_value));
    co_await client.shutdown();
    EXPECT_TRUE(retained[0].string() == std::string_view(binary_key));
}

ruvia::task<void> check_owned_transaction(ruvia::redis_client& client, redis_peer::mode_type mode) {
    co_await client.connect();
    const std::string binary_key("key\0binary", 10);
    std::string binary_value(64 * 1024, 'v');
    binary_value[5] = '\0';
    auto cold = [&] {
        auto transaction = client.redis().transaction();
        auto key = decltype(binary_key)(binary_key);
        auto value = decltype(binary_value)(binary_value);
        transaction.set(key, value).get(key);
        key.assign(128, 'x');
        value.assign(128, 'y');
        return std::move(transaction).exec();
    }();
    bool accepted = false;
    try {
        const auto retained = co_await std::move(cold);
        EXPECT_TRUE(mode == redis_peer::mode_type::transaction_success);
        EXPECT_EQ(retained.size(), std::size_t{2});
        EXPECT_TRUE(retained[0].string() == std::string_view(binary_value));
        EXPECT_TRUE(retained[1].string() == std::string_view(binary_key));
        co_await client.redis().ping();
        co_await client.shutdown();
        EXPECT_TRUE(retained[0].string() == std::string_view(binary_value));
        accepted = true;
    } catch (const ruvia::redis_error& error) {
        EXPECT_TRUE(mode != redis_peer::mode_type::transaction_success);
        EXPECT_TRUE(error.code() == ruvia::redis_error::code_type::command_error);
        if (mode == redis_peer::mode_type::transaction_queue_error) {
            EXPECT_TRUE((std::string_view(error.what()).find("reply 1") != std::string_view::npos));
            EXPECT_TRUE((std::string_view(error.what()).find("queue failed") != std::string_view::npos));
        } else if (mode == redis_peer::mode_type::transaction_exec_error) {
            EXPECT_TRUE((std::string_view(error.what()).find("EXECABORT") != std::string_view::npos));
        }
        accepted = true;
    }
    EXPECT_TRUE(accepted);
    co_await client.shutdown();
}

ruvia::task<void> check_oversized_reply(ruvia::redis_client& client) {
    co_await client.connect();
    bool rejected = false;
    try {
        (void)co_await client.redis().get("large");
    } catch (const ruvia::redis_error& error) {
        rejected = error.code() == ruvia::redis_error::code_type::protocol_error;
    }
    EXPECT_TRUE(rejected);
    co_await client.shutdown();
}

ruvia::task<void> check_huge_declared_array(ruvia::redis_client& client) {
    co_await client.connect();
    bool rejected = false;
    try {
        co_await client.redis().ping();
    } catch (const ruvia::redis_error& error) {
        rejected = error.code() == ruvia::redis_error::code_type::protocol_error;
    }
    EXPECT_TRUE(rejected);
    co_await client.shutdown();
}

ruvia::task<void> check_nested_array_at_depth_limit(ruvia::redis_client& client) {
    co_await client.connect();
    const std::string_view command[]{"PING"};
    const auto reply = co_await client.redis().command(std::span<const std::string_view>(command));
    const ruvia::redis_value* value = &reply;
    bool valid = true;
    for (int index = 0; index < 4; ++index) {
        if (value->kind() != ruvia::redis_value::kind_type::array || value->array().size() != 1) {
            valid = false;
            break;
        }
        value = &value->array()[0];
    }
    EXPECT_TRUE(valid && value->kind() == ruvia::redis_value::kind_type::string && value->string() == "PONG");
    co_await client.shutdown();
}

ruvia::task<void> check_reconnect_after_budget_rejection(ruvia::redis_client& client) {
    co_await client.connect();
    auto redis = client.redis();
    bool rejected = false;
    try {
        co_await redis.ping();
    } catch (const ruvia::redis_error& error) {
        rejected = error.code() == ruvia::redis_error::code_type::protocol_error;
    }
    EXPECT_TRUE(rejected);
    co_await redis.ping();
    co_await client.shutdown();
}

ruvia::task<void> blocked_command(ruvia::redis_client& client, ruvia::stop_token stop = {}) {
    co_await client.connect();
    auto handle = client.redis().with_options({.stop_token_ = std::move(stop)});
    auto copied = handle;
    (void)co_await copied.with_options({}).command("STALL");
}

ruvia::task<void> connect_until_stopped(ruvia::redis_client& client, bool& cancelled) {
    try {
        co_await client.connect();
    } catch (const ruvia::redis_error&) {
        cancelled = true;
    }
}

ruvia::task<void> check_expiration(ruvia::redis_client& client, std::chrono::system_clock::time_point expires_at) {
    co_await client.connect();
    const bool applied = co_await client.redis().expire_at("session", expires_at);
    EXPECT_TRUE(applied);
    co_await client.shutdown();
}

ruvia::task<void> check_runtime_handle_timeout(ruvia::event_loop loop, ruvia::redis_config config) {
    config.command_timeout_.reset();
    auto worker_value = loop.worker();
    ruvia::redis_client client(loop, config);
    co_await client.connect();
    auto handle = client.redis().with_options({.timeout_ = std::chrono::seconds(5)});
    auto copied = handle;
    auto inherited = copied.with_options({});
    auto shortened = inherited.with_options({.timeout_ = std::chrono::milliseconds(100)});
    auto derived = shortened.with_options({.timeout_ = std::chrono::seconds(5)});
    auto cold_operation = derived.command("STALL");
    (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(250));
    const auto started = std::chrono::steady_clock::now();
    bool command_timed_out = false;
    try {
        (void)co_await std::move(cold_operation);
    } catch (const ruvia::redis_error& error) {
        command_timed_out = error.code() == ruvia::redis_error::code_type::timeout;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(command_timed_out);
    // Cold time must not consume the deadline; a longer override cannot relax it.
    EXPECT_TRUE(elapsed >= std::chrono::milliseconds(50));
    EXPECT_TRUE(elapsed < std::chrono::seconds(2));
    co_await client.shutdown();
}

ruvia::task<void> check_runtime_handle_cancellation_group(ruvia::event_loop loop, ruvia::redis_config config, ruvia::stop_source& base_stop, ruvia::stop_source& override_stop) {
    config.command_timeout_.reset();
    ruvia::redis_client client(loop, config);
    co_await client.connect();
    ruvia::stop_group group{base_stop.token(), override_stop.token()};
    auto configured = client.redis().with_options({.stop_token_ = base_stop.token()});
    auto copied = configured;
    auto derived = copied.with_options({.stop_token_ = group.token()});
    bool cancelled = false;
    try {
        (void)co_await derived.command("STALL");
    } catch (const ruvia::redis_error& error) {
        cancelled = error.code() == ruvia::redis_error::code_type::cancelled;
    }
    EXPECT_TRUE(cancelled);
    co_await client.shutdown();
}

}  // namespace

TEST(Ruvia, redis_client_handle_options_survive_copy_and_derivation) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    pool.loop(0).start(check_runtime_handle_timeout(pool.loop(0), peer.config())).get();
    pool.join();
}

TEST(Ruvia, redis_client_handle_uses_explicit_stop_group_after_copy_and_derivation) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    for (const bool stop_base : {true, false}) {
        redis_peer peer;
        ruvia::stop_source base_stop;
        ruvia::stop_source override_stop;
        auto operation = pool.loop(0).start(check_runtime_handle_cancellation_group(pool.loop(0), peer.config(), base_stop, override_stop));
        peer.wait_for_blocked_command();
        (stop_base ? base_stop : override_stop).request_stop();
        std::move(operation).get();
    }
    pool.join();
}

TEST(Ruvia, redis_client_accepts_many_user_sources_and_replaces_old_handle_cancellation) {
    for (std::size_t selected = 0; selected != 6; ++selected) {
        redis_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        auto config = peer.config();
        config.command_timeout_.reset();
        ruvia::redis_client client(pool.loop(0), config);
        std::array<ruvia::stop_source, 6> sources;
        std::array<ruvia::stop_token, 6> tokens;
        for (std::size_t index = 0; index != sources.size(); ++index) {
            tokens[index] = sources[index].token();
        }
        ruvia::stop_group group{tokens};
        ruvia::stop_source old;
        old.request_stop();
        auto run = [&]() -> ruvia::task<void> {
            co_await client.connect();
            auto original = client.redis().with_options({.stop_token_ = old.token()});
            auto copied = original;
            auto configured = copied.with_options({.stop_token_ = group.token()}).with_options({});
            auto cold = configured.command("STALL");
            bool cancelled = false;
            try {
                (void)co_await std::move(cold);
            } catch (const ruvia::redis_error& error) {
                cancelled = error.code() == ruvia::redis_error::code_type::cancelled;
            }
            EXPECT_TRUE(cancelled);
            co_await client.shutdown();
        };
        pool.start();
        auto pending = pool.loop(0).start(run());
        peer.wait_for_blocked_command();
        sources[selected].request_stop();
        std::move(pending).get();
        pool.join();
    }
}

TEST(Ruvia, redis_client_cold_operation_retains_explicit_group_after_handle_destruction) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    auto run = [&]() -> ruvia::task<void> {
        co_await client.connect();
        ruvia::stop_source first;
        ruvia::stop_source second;
        ruvia::stop_group cancellation{first.token(), second.token()};
        auto cold = [&] {
            auto configured = client.redis().with_options({.stop_token_ = cancellation.token()});
            auto copied = configured;
            return copied.with_options({}).command("PING");
        }();
        second.request_stop();
        bool cancelled = false;
        try {
            (void)co_await std::move(cold);
        } catch (const ruvia::redis_error& error) {
            cancelled = error.code() == ruvia::redis_error::code_type::cancelled;
        }
        EXPECT_TRUE(cancelled);
        co_await client.redis().ping();
        co_await client.shutdown();
    };
    pool.start();
    pool.loop(0).start(run()).get();
    pool.join();
}

TEST(Ruvia, redis_client_runs_on_its_event_loop_and_retains_results) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 2});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    auto wrong_loop = pool.loop(1).start(client.connect());
    bool rejected = false;
    try {
        std::move(wrong_loop).get();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    pool.loop(0).start(check_commands(client)).get();
    pool.stop();
    pool.join();
}

TEST(Ruvia, redis_client_needs_a_loop_with_a_blocking_pool_to_look_up_a_dns_host) {
    const ruvia::redis_config config{.host_ = "localhost", .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}};
    bool rejected = false;
    {
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        try {
            ruvia::redis_client client(pool.loop(0), config);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        pool.join();
    }
    EXPECT_TRUE(rejected);
    // The same configuration is accepted once the loop borrows a pool.
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    ruvia::event_loop_pool pool({.loop_count_ = 1, .blocking_pool_ = &lookups});
    {
        ruvia::redis_client client(pool.loop(0), config);
    }
    pool.join();
    lookups.join();
}

ruvia::task<void> check_self_nested_copy(ruvia::redis_client& client) {
    co_await client.connect();
    auto redis = client.redis();
    const auto reply = co_await redis.command("NESTED");
    {
        ruvia::redis_value value(reply);
        // The source is a subtree of the destination; assigning must not read
        // storage the assignment already released.
        value = value.array()[0];
        EXPECT_TRUE(value.kind() == ruvia::redis_value::kind_type::array);
        EXPECT_TRUE(value.array().size() == 2);
        EXPECT_TRUE(value.array()[0].string() == std::string(20, 'a'));
        EXPECT_TRUE(value.array()[1].string() == std::string(20, 'b'));
    }
    co_await client.shutdown();
}

TEST(Ruvia, redis_value_copy_assignment_from_its_own_subtree) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    pool.loop(0).start(check_self_nested_copy(client)).get();
    pool.stop();
    pool.join();
}

ruvia::task<void> check_reconnect_after_peer_close(ruvia::redis_client& client, redis_peer& peer) {
    co_await client.connect();
    auto redis = client.redis();
    EXPECT_TRUE(co_await redis.ping("first") == "first");
    peer.wait_for_closed_connection();
    // The idle connection the peer closed must be replaced, not written to.
    EXPECT_TRUE(co_await redis.ping("second") == "second");
    co_await client.shutdown();
}

TEST(Ruvia, redis_pool_replaces_an_idle_connection_the_peer_closed) {
    redis_peer peer(redis_peer::mode_type::close_after_reply);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    pool.loop(0).start(check_reconnect_after_peer_close(client, peer)).get();
    pool.stop();
    pool.join();
}

TEST(Ruvia, redis_blocking_wait_is_not_consumed_by_a_slow_lazy_connect) {
    redis_peer peer(redis_peer::mode_type::slow_blocking_connect);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.database_ = 1;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_blocking_wait_starts_at_write(client)).get();
    pool.join();
}

TEST(Ruvia, redis_pipeline_owns_binary_input_and_retains_ordered_replies) {
    redis_peer peer(redis_peer::mode_type::batch_echo);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    pool.loop(0).start(check_owned_pipeline(client)).get();
    pool.join();
}

TEST(Ruvia, redis_pipeline_barriers_follow_writes_on_the_same_connection_and_drain_command_errors) {
    for (const auto mode : {redis_peer::mode_type::barrier_success, redis_peer::mode_type::barrier_error}) {
        redis_peer peer(mode);
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::redis_client client(pool.loop(0), peer.config());
        auto exercise = [&]() -> ruvia::task<void> {
            co_await client.connect();
            auto pipeline = client.redis().with_options({.timeout_ = std::chrono::seconds(1)}).pipeline();
            pipeline.set("barrier-key", "value").command("WAIT", "1", "100").command("WAITAOF", "1", "1", "100");
            const auto replies = co_await std::move(pipeline).exec();
            EXPECT_EQ(replies.size(), std::size_t{3});
            if (replies.size() == 3) {
                EXPECT_EQ(replies[0].string(), std::string_view("OK"));
                if (mode == redis_peer::mode_type::barrier_error) {
                    EXPECT_TRUE(replies[1].kind() == ruvia::redis_value::kind_type::error);
                } else {
                    EXPECT_EQ(replies[1].integer(), std::int64_t{1});
                }
                const auto fsynced = replies[2].array();
                EXPECT_EQ(fsynced.size(), std::size_t{2});
                if (fsynced.size() == 2) {
                    EXPECT_EQ(fsynced[0].integer(), std::int64_t{1});
                    EXPECT_EQ(fsynced[1].integer(), std::int64_t{1});
                }
            }
            co_await client.redis().ping();
            co_await client.shutdown();
        };
        pool.start();
        pool.loop(0).start(exercise()).get();
        pool.join();
        const std::vector<std::vector<std::string>> expected({std::vector<std::string>({std::string("SET"), std::string("barrier-key"), std::string("value")}), std::vector<std::string>({std::string("WAIT"), std::string("1"), std::string("100")}), std::vector<std::string>({std::string("WAITAOF"), std::string("1"), std::string("1"), std::string("100")})});
        EXPECT_TRUE(peer.batch_commands() == expected);
    }
}

TEST(Ruvia, redis_transaction_owns_framing_and_handles_exec_outcomes) {
    for (const auto mode : {redis_peer::mode_type::transaction_success, redis_peer::mode_type::transaction_exec_error, redis_peer::mode_type::transaction_wrong_exec, redis_peer::mode_type::transaction_queue_error}) {
        redis_peer peer(mode);
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::redis_client client(pool.loop(0), peer.config());
        pool.start();
        pool.loop(0).start(check_owned_transaction(client, mode)).get();
        pool.join();
        const std::string key("key\0binary", 10);
        std::string value(64 * 1024, 'v');
        value[5] = '\0';
        const std::vector<std::vector<std::string>> expected({
            std::vector<std::string>({std::string("MULTI")}),
            std::vector<std::string>({std::string("SET"), std::string(key), std::string(value)}),
            std::vector<std::string>({std::string("GET"), std::string(key)}),
            std::vector<std::string>({std::string("EXEC")}),
        });
        EXPECT_TRUE(peer.batch_commands() == expected);
    }
}

TEST(Ruvia, redis_pipeline_reply_limit_counts_each_reply_not_the_tcp_batch) {
    redis_peer peer(redis_peer::mode_type::coalesced_pings);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 7;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_coalesced_replies(client)).get();
    pool.join();
}

TEST(Ruvia, redis_pipeline_preserves_large_and_following_coalesced_replies) {
    redis_peer peer(redis_peer::mode_type::coalesced_large);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    auto exercise = [&]() -> ruvia::task<void> {
        co_await client.connect();
        auto pipeline = client.redis().pipeline();
        pipeline.command("PING").command("PING");
        const auto replies = co_await std::move(pipeline).exec();
        EXPECT_EQ(replies.size(), std::size_t{2});
        if (replies.size() == 2) {
            EXPECT_TRUE(replies[0].string() == std::string(65537, 'x'));
            EXPECT_TRUE(replies[1].string() == "PONG");
        }
        co_await client.shutdown();
    };
    pool.start();
    pool.loop(0).start(exercise()).get();
    pool.join();
}

TEST(Ruvia, redis_pipeline_array_element_budget_resets_for_each_reply) {
    redis_peer peer(redis_peer::mode_type::coalesced_arrays);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 13;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_coalesced_replies(client, true)).get();
    pool.join();
}

TEST(Ruvia, redis_client_reply_limit_still_rejects_oversized_single_reply) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 7;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_oversized_reply(client)).get();
    pool.join();
}

TEST(Ruvia, redis_reply_budget_rejects_impossible_declared_array_before_its_elements_arrive) {
    redis_peer peer(redis_peer::mode_type::huge_array);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 1024;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_huge_declared_array(client)).get();
    pool.join();
}

TEST(Ruvia, redis_reply_budget_rejects_aggregate_nested_array_declarations) {
    redis_peer peer(redis_peer::mode_type::nested_arrays);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 1024;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_huge_declared_array(client)).get();
    pool.join();
}

TEST(Ruvia, redis_reply_depth_rejects_incomplete_nested_array) {
    redis_peer peer(redis_peer::mode_type::deep_arrays);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_array_depth_ = 4;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_huge_declared_array(client)).get();
    pool.join();
}

TEST(Ruvia, redis_reply_array_at_the_depth_limit_is_accepted) {
    redis_peer peer(redis_peer::mode_type::valid_nested_array);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_array_depth_ = 4;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_nested_array_at_depth_limit(client)).get();
    pool.join();
}

TEST(Ruvia, redis_reader_budget_rebinds_after_protocol_failure_and_reconnect) {
    redis_peer peer(redis_peer::mode_type::reject_then_ping);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.max_reply_bytes_ = 1024;
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_reconnect_after_budget_rejection(client)).get();
    pool.join();
}

TEST(Ruvia, redis_client_expire_at_preserves_the_requested_deadline) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    const auto expires_at = std::chrono::system_clock::time_point{std::chrono::seconds(2'000'000'000)} + std::chrono::microseconds(1'001);
    pool.loop(0).start(check_expiration(client, expires_at)).get();
    const auto command = peer.expiration_command();
    pool.join();
    EXPECT_EQ(command, (std::vector<std::string>({std::string("PEXPIREAT"), std::string("session"), std::string("2000000000002")})));
}

ruvia::task<void> check_pipeline_abort_and_reconnect(ruvia::redis_client& client, ruvia::operation_options options, ruvia::redis_error::code_type expected, std::string_view barrier = {}) {
    co_await client.connect();
    std::exception_ptr failure;
    try {
        auto pipeline = client.redis().with_options(std::move(options)).pipeline();
        if (barrier.empty()) {
            pipeline.command("STALL");
        } else {
            pipeline.set("barrier-key", "value");
            if (barrier == "WAIT") {
                pipeline.command(barrier, "1", "0");
            } else {
                pipeline.command(barrier, "1", "1", "0");
            }
        }
        bool rejected = false;
        try {
            (void)co_await std::move(pipeline).exec();
        } catch (const ruvia::redis_error& error) {
            rejected = error.code() == expected;
        }
        EXPECT_TRUE(rejected);
        // The failed pipeline must return its only slot and retire the partial
        // exchange before a fresh command establishes a new connection.
        co_await client.redis().ping();
        auto next = client.redis().pipeline();
        next.command("PING");
        const auto replies = co_await std::move(next).exec();
        EXPECT_EQ(replies.size(), std::size_t{1});
        if (!replies.empty()) {
            EXPECT_EQ(replies[0].string(), std::string_view("PONG"));
        }
    } catch (...) {
        failure = std::current_exception();
    }
    co_await client.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, redis_pipeline_timeout_and_cancellation_release_the_slot_for_reconnect) {
    for (const bool cancel : {false, true}) {
        redis_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::redis_client client(pool.loop(0), peer.config());
        ruvia::stop_source stop;
        pool.start();
        auto pending = pool.loop(0).start(check_pipeline_abort_and_reconnect(client, {.timeout_ = cancel ? std::chrono::seconds(5) : std::chrono::milliseconds(100), .stop_token_ = stop.token()}, cancel ? ruvia::redis_error::code_type::cancelled : ruvia::redis_error::code_type::timeout));
        peer.wait_for_blocked_command();
        if (cancel) {
            stop.request_stop();
        }
        std::move(pending).get();
        pool.join();
    }
}

TEST(Ruvia, redis_pipeline_barrier_timeout_cancellation_and_protocol_failure_retire_the_connection) {
    for (const auto barrier : {"WAIT", "WAITAOF"}) {
        for (const auto failure : {0, 1, 2, 3}) {
            const bool cancel = failure == 1;
            const bool pool_timeout = failure == 2;
            const bool malformed = failure == 3;
            redis_peer peer(malformed ? redis_peer::mode_type::barrier_malformed : redis_peer::mode_type::barrier_stall);
            ruvia::event_loop_pool pool({.loop_count_ = 1});
            auto config = peer.config();
            config.command_timeout_ = pool_timeout ? std::chrono::milliseconds(100) : std::chrono::seconds(5);
            ruvia::redis_client client(pool.loop(0), config);
            ruvia::stop_source stop;
            ruvia::operation_options options{.stop_token_ = stop.token()};
            if (!pool_timeout) {
                options.timeout_ = cancel || malformed ? std::chrono::seconds(5) : std::chrono::milliseconds(100);
            }
            const auto expected = malformed ? ruvia::redis_error::code_type::protocol_error : (cancel ? ruvia::redis_error::code_type::cancelled : ruvia::redis_error::code_type::timeout);
            pool.start();
            auto pending = pool.loop(0).start(check_pipeline_abort_and_reconnect(client, options, expected, barrier));
            peer.wait_for_blocked_command();
            if (cancel) {
                stop.request_stop();
            }
            std::move(pending).get();
            pool.join();
            std::vector<std::vector<std::string>> commands({std::vector<std::string>({std::string("SET"), std::string("barrier-key"), std::string("value")}), std::vector<std::string>({std::string(barrier), std::string("1")})});
            if (std::string_view(barrier) == "WAITAOF") {
                commands.back().push_back("1");
            }
            commands.back().push_back("0");
            EXPECT_TRUE(peer.batch_commands() == commands);
        }
    }
}

TEST(Ruvia, redis_client_loop_stop_cancels_and_joins_pending_commands) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    auto pending = pool.loop(0).start(blocked_command(client));
    peer.wait_for_blocked_command();
    pool.stop();
    bool cancelled = false;
    try {
        std::move(pending).get();
    } catch (const ruvia::redis_error&) {
        cancelled = true;
    }
    pool.join();
    EXPECT_TRUE(cancelled);
}

TEST(Ruvia, redis_client_close_and_shutdown_waiters_join_pending_commands) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    std::array<ruvia::stop_source, 6> sources;
    ruvia::stop_group group{sources[0].token(), sources[1].token(), sources[2].token(), sources[3].token(), sources[4].token(), sources[5].token()};
    pool.start();
    auto pending = pool.loop(0).start(blocked_command(client, group.token()));
    peer.wait_for_blocked_command();
    client.close();
    auto closing = pool.loop(0).start(client.shutdown());
    auto also_closing = pool.loop(0).start(client.shutdown());
    bool cancelled = false;
    try {
        std::move(pending).get();
    } catch (const ruvia::redis_error&) {
        cancelled = true;
    }
    std::move(closing).get();
    std::move(also_closing).get();
    pool.loop(0).start(client.shutdown()).get();
    pool.stop();
    pool.join();
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(!group.token().stop_requested());
}

TEST(Ruvia, redis_client_event_loop_stop_awaits_retirement_of_pending_authentication) {
    for (const bool use_attachment_run : {false, true}) {
        redis_peer peer;
        auto config = peer.config();
        config.password_ = "stall-authentication";
        asio::io_context io;
        auto attachment = ruvia::attach_event_loop(io);
        ruvia::redis_client client(attachment.loop(), config);
        bool cancelled = false;
        auto root = attachment.loop().start(connect_until_stopped(client, cancelled));
        std::thread driver([&] {
            if (use_attachment_run) {
                attachment.run();
            } else {
                io.run();
            }
        });
        peer.wait_for_blocked_command();
        attachment.stop();
        driver.join();
        std::move(root).get();
        EXPECT_TRUE(cancelled);
        EXPECT_TRUE(!client.worker().accepting());
    }
}

TEST(Ruvia, redis_client_shutdown_joins_an_inflight_connect) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.password_ = "stall-authentication";
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    auto connecting = pool.loop(0).start(client.connect());
    peer.wait_for_blocked_command();
    bool duplicate_rejected = false;
    try {
        pool.loop(0).start(client.connect()).get();
    } catch (const std::logic_error&) {
        duplicate_rejected = true;
    }
    EXPECT_TRUE(duplicate_rejected);
    auto closing = pool.loop(0).start(client.shutdown());
    bool cancelled = false;
    try {
        std::move(connecting).get();
    } catch (const ruvia::redis_error&) {
        cancelled = true;
    }
    std::move(closing).get();
    pool.join();
    EXPECT_TRUE(cancelled);
}

// An ACL username must reach the server even without a password; otherwise the
// connection silently runs as the default user.
TEST(Ruvia, redis_client_authenticates_a_username_without_password) {
    redis_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto config = peer.config();
    config.username_ = "app";
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    auto connecting = pool.loop(0).start(client.connect());
    peer.wait_for_blocked_command();
    const std::vector<std::string> expected({std::string("AUTH"), std::string("app"), std::string("")});
    EXPECT_TRUE(peer.authentication_command() == expected);
    auto closing = pool.loop(0).start(client.shutdown());
    bool cancelled = false;
    try {
        std::move(connecting).get();
    } catch (const ruvia::redis_error&) {
        cancelled = true;
    }
    std::move(closing).get();
    pool.join();
    EXPECT_TRUE(cancelled);
}

TEST(Ruvia, redis_client_fresh_close_and_loop_stop_share_worker_completion) {
    for (int iteration = 0; iteration < 64; ++iteration) {
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::redis_client client(pool.loop(0), {.pool_size_per_worker_ = 1});
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
        EXPECT_TRUE(!client.worker().accepting());
    }
}

TEST(Ruvia, redis_client_cold_connect_can_be_discarded_without_starting_the_pool) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    {
        ruvia::redis_client client(pool.loop(0), {.host_ = "127.0.0.1"});
        auto discarded = client.connect();
        (void)discarded;
    }
    pool.join();
}

// The host feeds the shared client transport and TLS SNI path, so it must be
// an unbracketed IP literal or an ASCII DNS name.
TEST(Ruvia, redis_client_rejects_invalid_hosts_at_configuration) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    for (const std::string_view host :
        {std::string_view("a::b::c"), std::string_view("-leading.example"),
            std::string_view("b\xc3\xbc"
                             "cher.example"),
            std::string_view("host:6379")}) {
        std::string message{};
        try {
            ruvia::redis_client client(pool.loop(0), {.host_ = std::string(host)});
        } catch (const std::invalid_argument& error) {
            message = error.what();
        }
        EXPECT_EQ(message, std::string("redis host is invalid"));
    }
    pool.join();
}

TEST(Ruvia, redis_client_failed_connect_can_be_shutdown) {
    asio::io_context io;
    // Bound but not listening: no other process can claim this port.
    asio::ip::tcp::acceptor unavailable(io);
    unavailable.open(asio::ip::tcp::v4());
    unavailable.bind({asio::ip::tcp::v4(), 0});
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), {.host_ = "127.0.0.1", .port_ = unavailable.local_endpoint().port(), .pool_size_per_worker_ = 1, .connect_timeout_ = std::chrono::seconds(1)});
    pool.start();
    bool failed = false;
    try {
        pool.loop(0).start(client.connect()).get();
    } catch (const ruvia::redis_error&) {
        failed = true;
    }
    pool.loop(0).start(client.shutdown()).get();
    pool.join();
    EXPECT_TRUE(failed);
}

TEST(Ruvia, redis_tls_authenticates_identity_and_retains_public_results) {
    ruvia::test::tls_identity identity("redis.test");
    redis_peer peer(redis_peer::mode_type::normal, &identity.context_);
    auto config = peer.config();
    config.tls_ = {.ca_file_ = identity.ca_file_, .server_name_ = "redis.test"};
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), config);
    pool.start();
    pool.loop(0).start(check_commands(client)).get();
    pool.join();
}

TEST(Ruvia, redis_tls_rejects_untrusted_and_wrong_name_before_authentication) {
    ruvia::test::tls_identity identity("redis.test");
    for (const bool trust_ca : {false, true}) {
        redis_peer peer(redis_peer::mode_type::normal, &identity.context_);
        auto config = peer.config();
        config.password_ = "must-not-send";
        config.tls_ = {.ca_file_ = trust_ca ? std::optional<std::filesystem::path>(identity.ca_file_) : std::nullopt, .server_name_ = trust_ca ? "wrong.test" : "redis.test"};
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::redis_client client(pool.loop(0), config);
        pool.start();
        bool rejected = false;
        try {
            pool.loop(0).start(client.connect()).get();
        } catch (const ruvia::redis_error& error) {
            rejected = error.code() == ruvia::redis_error::code_type::connect_failed;
        }
        EXPECT_TRUE(rejected);
        pool.loop(0).start(client.shutdown()).get();
        pool.join();
    }
}

TEST(Ruvia, redis_tls_shutdown_joins_pending_authenticated_transport_io) {
    ruvia::test::tls_identity identity("redis.test");
    redis_peer peer(redis_peer::mode_type::normal, &identity.context_);
    auto config = peer.config();
    config.tls_ = {.ca_file_ = identity.ca_file_, .server_name_ = "redis.test"};
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), config);
    {
        auto cold = client.connect();
    }
    pool.start();
    auto pending = pool.loop(0).start(blocked_command(client));
    peer.wait_for_blocked_command();
    client.close();
    bool cancelled = false;
    try {
        std::move(pending).get();
    } catch (const ruvia::redis_error&) {
        cancelled = true;
    }
    pool.loop(0).start(client.shutdown()).get();
    pool.join();
    EXPECT_TRUE(cancelled);
}
