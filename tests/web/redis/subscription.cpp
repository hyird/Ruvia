#include <array>
#include <chrono>
#include <future>
#include <istream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/redis/redis_client.h"

#include "test_harness.h"

namespace {

[[nodiscard]] std::string bulk(std::string_view value) {
    return "$" + std::to_string(value.size()) + "\r\n" + std::string(value) + "\r\n";
}

[[nodiscard]] std::string push(std::string_view kind, std::string_view name, std::size_t count) {
    return "*3\r\n" + bulk(kind) + bulk(name) + ":" + std::to_string(count) + "\r\n";
}

// Deterministic RESP Pub/Sub peer. Every session runs on the peer's own
// thread; PUBLISH from any session fans out to the sessions subscribed to the
// channel or to a matching "prefix*" pattern and replies with that count.
class pubsub_peer final {
public:
    pubsub_peer()
        : acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          port_(acceptor_.local_endpoint().port()),
          work_(asio::make_work_guard(io_)),
          done_(asio::co_spawn(io_, accept_loop(), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    ~pubsub_peer() {
        asio::post(io_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            for (auto& session : sessions_) {
                session->socket_.close(ignored);
            }
            work_.reset();
        });
        thread_.join();
        try {
            done_.get();
        } catch (const std::system_error&) {
        }
    }

    [[nodiscard]] ruvia::redis_config config() const {
        return {.host_ = "127.0.0.1", .port_ = port_, .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1, .connect_timeout_ = std::chrono::seconds(2), .command_timeout_ = std::chrono::seconds(2)};
    }

private:
    struct session final {
        explicit session(asio::io_context& io)
            : socket_(io) {}

        asio::ip::tcp::socket socket_;
        asio::streambuf buffer_;
        std::set<std::string> channels_;
        std::set<std::string> patterns_;
    };

    asio::awaitable<std::string> line(session& current) {
        co_await asio::async_read_until(current.socket_, current.buffer_, "\r\n", asio::use_awaitable);
        std::istream input(&current.buffer_);
        std::string value;
        std::getline(input, value);
        value.pop_back();
        co_return value;
    }

    asio::awaitable<std::vector<std::string>> command(session& current) {
        const auto header_value = co_await line(current);
        const auto count = std::stoi(header_value.substr(1));
        std::vector<std::string> args;
        for (int index = 0; index < count; ++index) {
            const auto size_line = co_await line(current);
            const auto size = static_cast<std::size_t>(std::stoul(size_line.substr(1)));
            if (current.buffer_.size() < size + 2) {
                co_await asio::async_read(current.socket_, current.buffer_,
                    asio::transfer_exactly(size + 2 - current.buffer_.size()), asio::use_awaitable);
            }
            std::string arg(size, '\0');
            std::istream input(&current.buffer_);
            input.read(arg.data(), static_cast<std::streamsize>(size));
            input.ignore(2);
            args.push_back(std::move(arg));
        }
        co_return args;
    }

    asio::awaitable<void> accept_loop() {
        for (;;) {
            auto current = std::make_shared<session>(io_);
            co_await acceptor_.async_accept(current->socket_, asio::use_awaitable);
            sessions_.push_back(current);
            asio::co_spawn(io_, serve(current), asio::detached);
        }
    }

    static bool matches(std::string_view pattern, std::string_view channel) {
        if (!pattern.empty() && pattern.back() == '*') {
            return channel.starts_with(pattern.substr(0, pattern.size() - 1));
        }
        return pattern == channel;
    }

    asio::awaitable<std::size_t> deliver(std::string_view channel, std::string_view payload) {
        std::size_t receivers = 0;
        // A new session may be accepted between writes; index instead of iterating.
        for (std::size_t index = 0; index < sessions_.size(); ++index) {
            const auto target = sessions_[index];
            if (!target->socket_.is_open()) {
                continue;
            }
            if (target->channels_.contains(std::string(channel))) {
                const auto frame = "*3\r\n" + bulk("message") + bulk(channel) + bulk(payload);
                co_await asio::async_write(target->socket_, asio::buffer(frame), asio::use_awaitable);
                ++receivers;
            }
            for (const auto& pattern : target->patterns_) {
                if (matches(pattern, channel)) {
                    const auto frame = "*4\r\n" + bulk("pmessage") + bulk(pattern) + bulk(channel) + bulk(payload);
                    co_await asio::async_write(target->socket_, asio::buffer(frame), asio::use_awaitable);
                    ++receivers;
                }
            }
        }
        co_return receivers;
    }

    asio::awaitable<void> serve(std::shared_ptr<session> current) {
        try {
            for (;;) {
                const auto args = co_await command(*current);
                std::string reply;
                bool close_after_reply = false;
                if (args.front() == "SELECT" || args.front() == "AUTH") {
                    reply = "+OK\r\n";
                } else if (args.front() == "PUBLISH") {
                    const auto receivers = co_await deliver(args.at(1), args.at(2));
                    reply = ":" + std::to_string(receivers) + "\r\n";
                } else if (args.front() == "SUBSCRIBE" || args.front() == "PSUBSCRIBE") {
                    const bool pattern = args.front() == "PSUBSCRIBE";
                    for (std::size_t index = 1; index < args.size(); ++index) {
                        const auto& name = args[index];
                        if (name == "denied") {
                            reply += "-NOPERM no permissions to access this channel\r\n";
                            continue;
                        }
                        if (name == "malformed") {
                            reply += "*2\r\n" + bulk("subscribe") + bulk(name);
                            continue;
                        }
                        (pattern ? current->patterns_ : current->channels_).insert(name);
                        reply += push(pattern ? "psubscribe" : "subscribe", name,
                            current->channels_.size() + current->patterns_.size());
                        if (name == "close") {
                            close_after_reply = true;
                        }
                    }
                } else if (args.front() == "UNSUBSCRIBE" || args.front() == "PUNSUBSCRIBE") {
                    const bool pattern = args.front() == "PUNSUBSCRIBE";
                    auto& names = pattern ? current->patterns_ : current->channels_;
                    const auto kind = pattern ? "punsubscribe" : "unsubscribe";
                    if (args.size() == 1 && names.empty()) {
                        reply = "*3\r\n" + bulk(kind) + "$-1\r\n:" +
                                std::to_string(current->channels_.size() + current->patterns_.size()) + "\r\n";
                    } else if (args.size() == 1) {
                        while (!names.empty()) {
                            const auto name = *names.begin();
                            names.erase(names.begin());
                            reply += push(kind, name, current->channels_.size() + current->patterns_.size());
                        }
                    } else {
                        for (std::size_t index = 1; index < args.size(); ++index) {
                            names.erase(args[index]);
                            reply += push(kind, args[index], current->channels_.size() + current->patterns_.size());
                        }
                    }
                } else {
                    reply = "-ERR unknown command\r\n";
                }
                co_await asio::async_write(current->socket_, asio::buffer(reply), asio::use_awaitable);
                if (close_after_reply) {
                    std::error_code ignored;
                    current->socket_.close(ignored);
                    co_return;
                }
            }
        } catch (const std::system_error&) {
            std::error_code ignored;
            current->socket_.close(ignored);
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::vector<std::shared_ptr<session>> sessions_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::future<void> done_;
    std::thread thread_;
};

ruvia::task<void> check_events_arrive_in_wire_order(ruvia::redis_client& client, ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    auto redis = client.handle();
    auto subscription = redis.subscription();

    bool rejected_early_read = false;
    try {
        (void)co_await subscription.read();
    } catch (const std::logic_error&) {
        rejected_early_read = true;
    }
    RUVIA_CHECK(rejected_early_read);

    co_await subscription.subscribe("news", std::string_view("bin\0ary", 7));
    co_await subscription.psubscribe("news*");

    auto first = co_await subscription.read();
    RUVIA_CHECK(first && first->kind() == ruvia::redis_pubsub_event_kind::subscribed);
    RUVIA_CHECK(first && first->channel() == "news" && first->pattern().empty() && first->subscription_count() == 1);
    auto second = co_await subscription.read();
    RUVIA_CHECK(second && second->channel() == std::string_view("bin\0ary", 7) && second->subscription_count() == 2);
    auto third = co_await subscription.read();
    RUVIA_CHECK(third && third->kind() == ruvia::redis_pubsub_event_kind::subscribed);
    RUVIA_CHECK(third && third->pattern() == "news*" && third->channel().empty() && third->subscription_count() == 3);

    // The ordinary pool publishes; the dedicated connection receives both the
    // direct and the pattern delivery, in that order.
    const auto receivers = co_await redis.publish("news", std::string_view("pay\0load", 8));
    RUVIA_CHECK(receivers == 2);
    auto message = co_await subscription.read();
    RUVIA_CHECK(message && message->kind() == ruvia::redis_pubsub_event_kind::message);
    RUVIA_CHECK(message && message->channel() == "news" && message->pattern().empty());
    RUVIA_CHECK(message && message->payload() == std::string_view("pay\0load", 8) && message->subscription_count() == 0);
    // The event borrows the current reply until the next read starts.
    const std::string retained(message->payload());
    auto pattern_message = co_await subscription.read();
    RUVIA_CHECK(pattern_message && pattern_message->kind() == ruvia::redis_pubsub_event_kind::message);
    RUVIA_CHECK(pattern_message && pattern_message->pattern() == "news*" && pattern_message->channel() == "news");
    RUVIA_CHECK(pattern_message && pattern_message->payload() == retained);

    auto pipeline = redis.pipeline();
    pipeline.publish("nobody", "x").publish("newsroom", "y");
    const auto counts = co_await std::move(pipeline).exec();
    RUVIA_CHECK(counts.size() == 2 && counts[0].integer() == 0 && counts[1].integer() == 1);
    auto room = co_await subscription.read();
    RUVIA_CHECK(room && room->pattern() == "news*" && room->channel() == "newsroom" && room->payload() == "y");

    co_await subscription.unsubscribe("news");
    auto left = co_await subscription.read();
    RUVIA_CHECK(left && left->kind() == ruvia::redis_pubsub_event_kind::unsubscribed);
    RUVIA_CHECK(left && left->channel() == "news" && left->subscription_count() == 2);
    co_await subscription.punsubscribe();
    auto left_pattern = co_await subscription.read();
    RUVIA_CHECK(left_pattern && left_pattern->kind() == ruvia::redis_pubsub_event_kind::unsubscribed);
    RUVIA_CHECK(left_pattern && left_pattern->pattern() == "news*" && left_pattern->subscription_count() == 1);
    co_await subscription.punsubscribe();
    auto nothing_left = co_await subscription.read();
    RUVIA_CHECK(nothing_left && nothing_left->kind() == ruvia::redis_pubsub_event_kind::unsubscribed);
    RUVIA_CHECK(nothing_left && nothing_left->pattern().empty() && nothing_left->channel().empty());
    RUVIA_CHECK(nothing_left && nothing_left->subscription_count() == 1);

    bool rejected_empty = false;
    try {
        (void)subscription.subscribe({});
    } catch (const std::invalid_argument&) {
        rejected_empty = true;
    }
    RUVIA_CHECK(rejected_empty);

    co_await client.shutdown();
}

ruvia::task<void> check_error_reply_keeps_connection(ruvia::redis_client& client, ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    auto redis = client.handle();
    auto subscription = redis.subscription();
    const std::array<std::string_view, 2> channels{"denied", "allowed"};
    co_await subscription.subscribe(channels);
    bool denied = false;
    try {
        (void)co_await subscription.read();
    } catch (const ruvia::redis_error& error) {
        denied = error.code() == ruvia::redis_error::code_type::command_error;
    }
    RUVIA_CHECK(denied);
    auto allowed = co_await subscription.read();
    RUVIA_CHECK(allowed && allowed->kind() == ruvia::redis_pubsub_event_kind::subscribed && allowed->channel() == "allowed");
    RUVIA_CHECK(co_await redis.publish("allowed", "still open") == 1);
    auto message = co_await subscription.read();
    RUVIA_CHECK(message && message->payload() == "still open");
    co_await client.shutdown();
}

ruvia::task<void> check_server_close_is_an_io_error(ruvia::redis_client& client, ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    auto subscription = client.handle().subscription();
    const std::array<std::string_view, 1> channels{"close"};
    co_await subscription.subscribe(channels);
    auto confirmed = co_await subscription.read();
    RUVIA_CHECK(confirmed && confirmed->channel() == "close");
    bool io_error = false;
    try {
        (void)co_await subscription.read();
    } catch (const ruvia::redis_error& error) {
        io_error = error.code() == ruvia::redis_error::code_type::io_error;
    }
    RUVIA_CHECK(io_error);
    // Closed is terminal: the stream ends and commands report it.
    RUVIA_CHECK(!(co_await subscription.read()).has_value());
    bool closed = false;
    try {
        co_await subscription.subscribe(channels);
    } catch (const ruvia::redis_error& error) {
        closed = error.code() == ruvia::redis_error::code_type::closing;
    }
    RUVIA_CHECK(closed);
    co_await client.shutdown();
}

ruvia::task<void> check_malformed_push_closes(ruvia::redis_client& client, ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    auto subscription = client.handle().subscription();
    const std::array<std::string_view, 1> channels{"malformed"};
    co_await subscription.subscribe(channels);
    bool protocol_error = false;
    try {
        (void)co_await subscription.read();
    } catch (const ruvia::redis_error& error) {
        protocol_error = error.code() == ruvia::redis_error::code_type::protocol_error;
    }
    RUVIA_CHECK(protocol_error);
    RUVIA_CHECK(!(co_await subscription.read()).has_value());
    co_await client.shutdown();
}

ruvia::task<void> check_read_timeout(ruvia::redis_client& client, ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    auto subscription = client.with_options({.timeout_ = std::chrono::milliseconds(200)}).subscription();
    const std::array<std::string_view, 1> channels{"silent"};
    co_await subscription.subscribe(channels);
    auto confirmed = co_await subscription.read();
    RUVIA_CHECK(confirmed && confirmed->channel() == "silent");
    bool timed_out = false;
    try {
        (void)co_await subscription.read();
    } catch (const ruvia::redis_error& error) {
        timed_out = error.code() == ruvia::redis_error::code_type::timeout;
    }
    RUVIA_CHECK(timed_out);
    RUVIA_CHECK(!(co_await subscription.read()).has_value());
    co_await client.shutdown();
}

ruvia::task<void> check_stop_token_ends_the_stream(ruvia::redis_client& client, ruvia::stop_source& stop,
    std::promise<void>& confirmed_promise, ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    auto subscription = client.with_options({.stop_token_ = stop.token()}).subscription();
    const std::array<std::string_view, 1> channels{"silent"};
    co_await subscription.subscribe(channels);
    auto confirmed = co_await subscription.read();
    RUVIA_CHECK(confirmed && confirmed->channel() == "silent");
    // The test thread requests the stop while this read is pending.
    confirmed_promise.set_value();
    RUVIA_CHECK(!(co_await subscription.read()).has_value());
    bool cancelled = false;
    try {
        co_await subscription.subscribe(channels);
    } catch (const ruvia::redis_error& error) {
        cancelled = error.code() == ruvia::redis_error::code_type::cancelled;
    }
    RUVIA_CHECK(cancelled);
    co_await client.shutdown();
}

ruvia::task<void> check_abort_and_concurrent_reads(ruvia::redis_client& client, ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    auto subscription = client.handle().subscription();
    const std::array<std::string_view, 1> channels{"silent"};
    co_await subscription.subscribe(channels);
    auto confirmed = co_await subscription.read();
    RUVIA_CHECK(confirmed && confirmed->channel() == "silent");

    bool pending_ended = false;
    bool second_rejected = false;
    ruvia::task_scope scope(client.worker());
    scope.spawn([](ruvia::redis_subscription& target, bool& ended) -> ruvia::task<void> {
        ended = !(co_await target.read()).has_value();
    }(subscription, pending_ended));
    scope.spawn([](ruvia::redis_subscription& target, bool& rejected) -> ruvia::task<void> {
        try {
            (void)co_await target.read();
        } catch (const std::logic_error&) {
            rejected = true;
        }
        target.abort();
    }(subscription, second_rejected));
    co_await scope.join();
    RUVIA_CHECK(second_rejected);
    RUVIA_CHECK(pending_ended);
    co_await client.shutdown();
}

ruvia::task<void> check_client_close_ends_pending_read(ruvia::redis_client& client,
    std::promise<void>& confirmed_promise, ruvia::testing::test_context& ruvia_ctx) {
    co_await client.connect();
    auto subscription = client.handle().subscription();
    const std::array<std::string_view, 1> channels{"silent"};
    co_await subscription.subscribe(channels);
    auto confirmed = co_await subscription.read();
    RUVIA_CHECK(confirmed && confirmed->channel() == "silent");
    // The test thread closes the client while this read is pending.
    confirmed_promise.set_value();
    RUVIA_CHECK(!(co_await subscription.read()).has_value());
    co_await client.shutdown();
}

}  // namespace

RUVIA_TEST(redis_subscription_delivers_confirmations_and_messages_in_wire_order) {
    pubsub_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    pool.loop(0).start(check_events_arrive_in_wire_order(client, ruvia_ctx)).get();
    pool.stop();
    pool.join();
}

RUVIA_TEST(redis_subscription_error_reply_leaves_the_connection_open) {
    pubsub_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    pool.loop(0).start(check_error_reply_keeps_connection(client, ruvia_ctx)).get();
    pool.stop();
    pool.join();
}

RUVIA_TEST(redis_subscription_reports_server_close_and_stays_closed) {
    pubsub_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    pool.loop(0).start(check_server_close_is_an_io_error(client, ruvia_ctx)).get();
    pool.stop();
    pool.join();
}

RUVIA_TEST(redis_subscription_closes_on_a_malformed_push) {
    pubsub_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    pool.loop(0).start(check_malformed_push_closes(client, ruvia_ctx)).get();
    pool.stop();
    pool.join();
}

RUVIA_TEST(redis_subscription_read_honors_the_operation_timeout) {
    pubsub_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    pool.loop(0).start(check_read_timeout(client, ruvia_ctx)).get();
    pool.stop();
    pool.join();
}

RUVIA_TEST(redis_subscription_stop_token_ends_the_stream) {
    pubsub_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    ruvia::stop_source stop;
    pool.start();
    std::promise<void> confirmed;
    auto operation = pool.loop(0).start(check_stop_token_ends_the_stream(client, stop, confirmed, ruvia_ctx));
    confirmed.get_future().get();
    stop.request_stop();
    operation.get();
    pool.stop();
    pool.join();
}

RUVIA_TEST(redis_subscription_abort_ends_a_pending_read_and_rejects_a_second) {
    pubsub_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    pool.loop(0).start(check_abort_and_concurrent_reads(client, ruvia_ctx)).get();
    pool.stop();
    pool.join();
}

RUVIA_TEST(redis_subscription_client_close_ends_a_pending_read) {
    pubsub_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::redis_client client(pool.loop(0), peer.config());
    pool.start();
    std::promise<void> confirmed;
    auto operation = pool.loop(0).start(check_client_close_ends_pending_read(client, confirmed, ruvia_ctx));
    confirmed.get_future().get();
    client.close();
    operation.get();
    pool.stop();
    pool.join();
}
