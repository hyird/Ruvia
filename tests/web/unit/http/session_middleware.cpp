#ifdef RUVIA_ENABLE_REDIS

#include <array>
#include <future>
#include <map>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/read_until.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/context.h"
#include "ruvia/web/session.h"

#include "client/http_client_registry.h"
#include "context/context_access.h"
#include "db/db_registry.h"
#include "http/session_access.h"
#include "memory_resource_fixture.h"
#include "redis/redis_registry.h"
#include "router/route_table.h"
#include "router/router_impl.h"
#include "server/http_response_stream_dispatch.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "websocket/websocket_response_headers.h"

namespace {

ruvia::http_request make_request(std::pmr::memory_resource* resource, std::string_view target = "/") {
    auto [request, parse_error] = ruvia::make_parsed_http_request("GET", target, {}, {}, resource);
    if (parse_error) {
        throw std::logic_error("invalid session middleware test request");
    }
    return std::move(request);
}

// A bounded RESP peer exercises the middleware's actual asynchronous storage
// boundary without requiring an external Redis process.
class session_storage_peer final {
public:
    explicit session_storage_peer(asio::io_context& io)
        : acceptor_(io, {asio::ip::tcp::v4(), 0}),
          socket_(io) {}

    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void close() {
        std::error_code ignored;
        acceptor_.close(ignored);
        socket_.close(ignored);
    }
    bool fail_set_{false};
    bool fail_delete_{false};
    ruvia::stop_source* cancel_write_{nullptr};
    std::vector<std::vector<std::string>> commands_;
    std::map<std::string, std::string, std::less<>> sessions_;

    asio::awaitable<void> serve() {
        try {
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            for (;;) {
                const auto prefix = co_await line();
                const auto count = std::stoul(prefix.substr(1));
                std::vector<std::string> command;
                for (std::size_t i = 0; i != count; ++i) {
                    const auto size = std::stoul((co_await line()).substr(1));
                    auto value = co_await line();
                    if (value.size() != size) {
                        throw std::logic_error("invalid test RESP argument");
                    }
                    command.push_back(std::move(value));
                }
                commands_.push_back(std::move(command));
                const auto& received_command = commands_.back();
                const auto& verb = received_command.front();
                if (cancel_write_ != nullptr && (verb == "SET" || verb == "EVAL" || verb == "DEL")) {
                    cancel_write_->request_stop();
                    continue;
                }
                std::string reply;
                if (verb == "GET") {
                    const auto found = sessions_.find(received_command[1]);
                    reply = found == sessions_.end()
                                ? "$-1\r\n"
                                : "$" + std::to_string(found->second.size()) + "\r\n" + found->second + "\r\n";
                } else if (verb == "EVAL") {
                    if (fail_set_ || fail_delete_) {
                        reply = "-ERR rotation failed\r\n";
                    } else if (!sessions_.contains(received_command[3]) || sessions_.contains(received_command[4])) {
                        reply = ":0\r\n";
                    } else {
                        sessions_.emplace(received_command[4], received_command[5]);
                        sessions_.erase(received_command[3]);
                        reply = ":1\r\n";
                    }
                } else if (verb == "DEL") {
                    reply = fail_delete_ ? "-ERR delete failed\r\n" : ":" + std::to_string(sessions_.erase(received_command[1])) + "\r\n";
                } else if (verb == "SET") {
                    if (fail_set_) {
                        reply = "-ERR storage failed\r\n";
                    } else if (sessions_.contains(received_command[1])) {
                        reply = "$-1\r\n";
                    } else {
                        sessions_.emplace(received_command[1], received_command[2]);
                        reply = "+OK\r\n";
                    }
                } else {
                    throw std::logic_error("unexpected session storage command");
                }
                co_await asio::async_write(socket_, asio::buffer(reply), asio::use_awaitable);
            }
        } catch (const std::system_error& error) {
            if (error.code() != asio::error::eof && error.code() != asio::error::operation_aborted &&
                error.code() != asio::error::bad_descriptor && error.code() != asio::error::connection_reset) {
                throw;
            }
        }
    }

private:
    asio::awaitable<std::string> line() {
        const auto size = co_await asio::async_read_until(socket_, asio::dynamic_buffer(input_), "\r\n", asio::use_awaitable);
        auto value = input_.substr(0, size - 2);
        input_.erase(0, size);
        co_return value;
    }
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    std::string input_;
};

struct session_fixture final {
    asio::io_context& io_{ruvia::test::new_test_io_context()};
    ruvia::event_loop_attachment attachment_{ruvia::attach_event_loop(io_)};
    ruvia::worker_handle worker_{attachment_.loop().handle()};
    session_storage_peer peer_{io_};
    ruvia::test::counting_memory_resource operation_memory_;
    std::array<ruvia::detail::redis_definition_type, 1> definitions_{definition()};
    ruvia::detail::redis_registry redis_{io_, &operation_memory_, definitions_, worker_};
    ruvia::detail::db_registry db_{io_, worker_, &operation_memory_, std::span<const ruvia::detail::db_definition>{}};
    ruvia::detail::http_client_registry http_{io_, worker_, &operation_memory_, std::span<const ruvia::detail::http_client_definition_type>{}};
    ruvia::stop_source stop_;
    ruvia::stop_token token_{stop_.token()};
    ruvia::worker_memory memory_;
    ruvia::request_memory request_memory_{memory_};
    ruvia::http_request request_{make_request(request_memory_.resource())};
    ruvia::context context_{ruvia::detail::context_access::make(request_memory_, request_,
        ruvia::detail::context_services(worker_, token_, {db_, redis_, http_}))};
    ruvia::session_middleware middleware_;

    ruvia::detail::redis_definition_type definition() {
        auto config = ruvia::redis_config{};
        config.host_ = "127.0.0.1";
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.port_ = peer_.port();
        config.pool_size_per_worker_ = 1;
        return {std::pmr::string("default", &operation_memory_),
            ruvia::detail::redis_config_storage(config, &operation_memory_)};
    }
    void bind() {
        ruvia::detail::session_access::bind(context_, &middleware_);
    }
    void load(std::string_view id, std::string_view data) {
        peer_.sessions_.emplace("sess:" + std::string(id), data);
        ruvia::detail::session_access::observe_presented_id(context_, id);
        ruvia::detail::session_access::load(context_, data);
    }
    void run(ruvia::task<void> task_value) {
        auto server = asio::co_spawn(io_, peer_.serve(), asio::use_future);
        auto execute = [&]() -> ruvia::task<void> {
            std::exception_ptr failure;
            try {
                co_await std::move(task_value);
            } catch (...) {
                failure = std::current_exception();
            }
            redis_.close_now();
            peer_.close();
            attachment_.stop();
            if (failure) {
                std::rethrow_exception(failure);
            }
        };
        auto result_value = asio::co_spawn(io_, ruvia::as_awaitable(execute()), asio::use_future);
        attachment_.run();
        result_value.get();
        server.get();
    }
};

bool rejects_mutation(ruvia::session session_value) {
    int rejections = 0;
    try {
        session_value.set("changed");
    } catch (const std::logic_error&) {
        ++rejections;
    }
    try {
        session_value.clear();
    } catch (const std::logic_error&) {
        ++rejections;
    }
    try {
        session_value.regenerate();
    } catch (const std::logic_error&) {
        ++rejections;
    }
    return rejections == 3;
}

class session_outer_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
    }
};

class session_mutation_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next& next_value) {
        context_value.session().set("user=1");
        co_await next_value();
    }
};

class session_head_sink final {
public:
    void bind_context(ruvia::context* context_value, ruvia::task<ruvia::http_response> (*head)(ruvia::context&)) {
        context_ = context_value;
        head_ = head;
    }
    void release_context() noexcept {
        context_ = nullptr;
        head_ = nullptr;
    }
    bool committed() const noexcept {
        return committed_;
    }
    bool aborted() const noexcept {
        return false;
    }
    ruvia::task<void> write(std::string_view) {
        co_await commit();
    }
    ruvia::task<void> end(std::span<const ruvia::http_header_view>) {
        co_await commit();
    }
    ruvia::task<ruvia::timer_sleep_result> sleep(std::chrono::milliseconds, const ruvia::stop_token&) {
        co_return ruvia::timer_sleep_result::elapsed;
    }
    std::string cookie_;
    bool frozen_{false};

private:
    ruvia::task<void> commit() {
        if (committed_) {
            co_return;
        }
        const auto response = co_await head_(*context_);
        cookie_ = response.header("Set-Cookie").value_or("");
        frozen_ = rejects_mutation(context_->session());
        committed_ = true;
    }
    ruvia::context* context_{nullptr};
    ruvia::task<ruvia::http_response> (*head_)(ruvia::context&){nullptr};
    bool committed_{false};
};

}  // namespace

RUVIA_TEST(session_commit_persists_once_and_publishes_cookie_before_head) {
    session_fixture fixture;
    fixture.bind();
    auto session_value = fixture.context_.session();
    session_value.set("user=1");
    fixture.context_.header("Set-Cookie", "theme=dark", {.mode_ = ruvia::http_response_header_mode::append});
    auto exercise = [&]() -> ruvia::task<void> {
        // Discarding a lazy commit must neither freeze nor persist the session.
        {
            auto discarded = ruvia::detail::session_access::commit(fixture.context_);
        }
        session_value.set("user=2");
        co_await ruvia::detail::session_access::commit(fixture.context_);
        const auto allocations = fixture.operation_memory_.live_allocations();
        const auto head = ruvia::detail::websocket_response_headers(fixture.context_);
        RUVIA_CHECK_EQ(head.size(), std::size_t{2});
        RUVIA_CHECK_EQ(head[0].value(), std::string_view("theme=dark"));
        RUVIA_CHECK(head[1].value().starts_with("sid="));
        const std::string cookie(head[1].value());
        for (int i = 0; i != 32; ++i) {
            co_await ruvia::detail::session_access::commit(fixture.context_);
            RUVIA_CHECK_EQ(fixture.operation_memory_.live_allocations(), allocations);
            RUVIA_CHECK_EQ(head[1].value(), std::string_view(cookie));
            RUVIA_CHECK_EQ(session_value.data(), std::string_view("user=2"));
        }
        RUVIA_CHECK(rejects_mutation(session_value));
        RUVIA_CHECK_EQ(fixture.peer_.commands_.size(), std::size_t{1});
        RUVIA_CHECK_EQ(fixture.peer_.commands_[0][2], std::string("user=2"));
    };
    fixture.run(exercise());
}

RUVIA_TEST(session_commit_rotates_and_clears_before_publishing_cookie) {
    for (bool clear : {false, true}) {
        session_fixture fixture;
        fixture.bind();
        fixture.load("deadbeef", "user=1");
        auto session_value = fixture.context_.session();
        if (clear) {
            session_value.clear();
        } else {
            session_value.regenerate();
        }
        auto exercise = [&]() -> ruvia::task<void> {
            co_await ruvia::detail::session_access::commit(fixture.context_);
            const auto& commands = fixture.peer_.commands_;
            RUVIA_CHECK_EQ(commands.size(), std::size_t{1});
            RUVIA_CHECK_EQ(commands.back().front(), std::string(clear ? "DEL" : "EVAL"));
            RUVIA_CHECK(commands.back()[clear ? 1 : 3].ends_with("deadbeef"));
            const auto head = ruvia::detail::websocket_response_headers(fixture.context_);
            RUVIA_CHECK_EQ(head.size(), std::size_t{1});
            RUVIA_CHECK_EQ((head.front().value().find("Max-Age=0") != std::string_view::npos), clear);
            RUVIA_CHECK(rejects_mutation(session_value));
        };
        fixture.run(exercise());
    }
}

RUVIA_TEST(session_commit_failure_and_cancellation_never_publish_or_retry) {
    // Rejection or cancellation before the peer applies a command publishes no
    // cookie and makes the failed commit terminal for writes and logout alike.
    for (int mode = 0; mode != 3; ++mode) {
        for (const bool cancel : {false, true}) {
            session_fixture fixture;
            fixture.bind();
            fixture.peer_.fail_set_ = !cancel;
            fixture.peer_.fail_delete_ = !cancel;
            if (cancel) {
                fixture.peer_.cancel_write_ = &fixture.stop_;
            }
            if (mode != 0) {
                fixture.load("deadbeef", "user=1");
            }
            if (mode == 2) {
                fixture.context_.session().clear();
            } else {
                fixture.context_.session().set("user=2");
            }
            auto exercise = [&]() -> ruvia::task<void> {
                for (int attempt_value = 0; attempt_value != 2; ++attempt_value) {
                    bool failed = false;
                    try {
                        co_await ruvia::detail::session_access::commit(fixture.context_);
                    } catch (const ruvia::redis_error& error) {
                        failed = cancel ? error.code() == ruvia::redis_error::code_type::cancelled
                                        : error.code() == ruvia::redis_error::code_type::command_error;
                    }
                    RUVIA_CHECK(failed);
                    RUVIA_CHECK(ruvia::detail::websocket_response_headers(fixture.context_).empty());
                    RUVIA_CHECK(rejects_mutation(fixture.context_.session()));
                }
                RUVIA_CHECK_EQ(fixture.peer_.commands_.size(), std::size_t{1});
                if (mode == 0) {
                    RUVIA_CHECK(fixture.peer_.sessions_.empty());
                } else {
                    RUVIA_CHECK_EQ(fixture.peer_.sessions_.at("sess:deadbeef"), std::string("user=1"));
                }
            };
            fixture.run(exercise());
        }
    }
}

RUVIA_TEST(session_commit_rejects_updates_and_rotations_of_revoked_sessions) {
    for (const bool rotate : {false, true}) {
        session_fixture fixture;
        fixture.bind();
        fixture.load("deadbeef", "user=1");
        fixture.context_.session().set("user=2");
        if (rotate) {
            fixture.context_.session().regenerate();
        }
        fixture.peer_.sessions_.erase("sess:deadbeef");
        auto exercise = [&]() -> ruvia::task<void> {
            bool rejected = false;
            try {
                co_await ruvia::detail::session_access::commit(fixture.context_);
            } catch (const ruvia::http_error& error) {
                rejected = error.info().status() == ruvia::http_status::conflict;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK(ruvia::detail::websocket_response_headers(fixture.context_).empty());
            RUVIA_CHECK_EQ(fixture.peer_.commands_.size(), std::size_t{1});
            RUVIA_CHECK(fixture.peer_.sessions_.empty());
        };
        fixture.run(exercise());
    }
}

RUVIA_TEST(session_writes_rotate_existing_identity_and_revoke_the_old_cookie) {
    for (const bool unchanged : {false, true}) {
        session_fixture fixture;
        const std::string initial_data = unchanged ? "user=2" : "anonymous";
        fixture.peer_.sessions_.emplace("sess:deadbeef", initial_data);
        auto read_only = [&](std::string_view id, std::string_view expected) -> ruvia::task<void> {
            const std::string cookie = "sid=" + std::string(id);
            const std::array headers{ruvia::http_header_view{"Cookie", cookie}};
            auto [request, error] = ruvia::make_parsed_http_request("GET", "/", headers, {}, fixture.request_memory_.resource());
            RUVIA_CHECK(!error);
            auto context_value = ruvia::detail::context_access::make(fixture.request_memory_, request,
                ruvia::detail::context_services(fixture.worker_, fixture.token_, {fixture.db_, fixture.redis_, fixture.http_}));
            ruvia::detail::next_state::control_type control;
            auto next_value = ruvia::detail::next_access::make({.context_ = &context_value, .control_ = &control},
                [](ruvia::detail::next_state) -> ruvia::task<void> { co_return; });
            co_await fixture.middleware_.handle(context_value, next_value);
            const auto session_value = context_value.session();
            RUVIA_CHECK_EQ(session_value.data(), expected);
            RUVIA_CHECK(ruvia::detail::websocket_response_headers(context_value).empty());
        };
        auto exercise = [&]() -> ruvia::task<void> {
            co_await read_only("deadbeef", initial_data);
            const std::array headers{ruvia::http_header_view{"Cookie", "sid=deadbeef"}};
            auto [request, error] = ruvia::make_parsed_http_request("POST", "/login", headers, {}, fixture.request_memory_.resource());
            RUVIA_CHECK(!error);
            auto context_value = ruvia::detail::context_access::make(fixture.request_memory_, request,
                ruvia::detail::context_services(fixture.worker_, fixture.token_, {fixture.db_, fixture.redis_, fixture.http_}));
            ruvia::detail::next_state::control_type control;
            auto next_value = ruvia::detail::next_access::make({.context_ = &context_value, .control_ = &control},
                [](ruvia::detail::next_state state_value) -> ruvia::task<void> {
                    auto session_value = state_value.context_->session();
                    session_value.set("user=2");
                    session_value.set("user=2");
                    co_return;
                });
            co_await fixture.middleware_.handle(context_value, next_value);
            const auto response_headers_value = ruvia::detail::websocket_response_headers(context_value);
            RUVIA_CHECK_EQ(response_headers_value.size(), std::size_t{1});
            if (response_headers_value.empty()) {
                co_return;
            }
            const auto cookie = response_headers_value.front().value();
            RUVIA_CHECK(cookie.starts_with("sid="));
            const std::string new_id(cookie.substr(4, cookie.find(';') - 4));
            RUVIA_CHECK(new_id != "deadbeef");
            RUVIA_CHECK(ruvia::detail::is_valid_session_id(new_id));
            RUVIA_CHECK_EQ(fixture.peer_.sessions_.size(), std::size_t{1});
            RUVIA_CHECK_EQ(fixture.peer_.sessions_.at("sess:" + new_id), std::string("user=2"));
            RUVIA_CHECK(!fixture.peer_.sessions_.contains("sess:deadbeef"));
            const auto commands = fixture.peer_.commands_.size();
            co_await ruvia::detail::session_access::commit(context_value);
            RUVIA_CHECK_EQ(fixture.peer_.commands_.size(), commands);
            co_await read_only("deadbeef", "");
            co_await read_only(new_id, "user=2");
        };
        fixture.run(exercise());
    }
}

RUVIA_TEST(session_middleware_commits_before_stream_and_websocket_terminal) {
    // 0: stream; 1: empty stream; 2: websocket; 3: failed handshake preparation.
    for (int mode = 0; mode != 4; ++mode) {
        for (bool fail : {false, true}) {
            session_fixture fixture;
            fixture.peer_.fail_set_ = fail;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            const std::array middlewares{
                ruvia::detail::make_middleware_descriptor<session_outer_middleware>(),
                ruvia::detail::make_middleware_descriptor<ruvia::session_middleware>(),
                ruvia::detail::make_middleware_descriptor<session_mutation_middleware>()};
            struct observation {
                int mode_;
                const ruvia::http_request* request_;
                bool called_{false};
                bool frozen_{false};
                std::string cookie_;
            } observation_value{mode, &fixture.request_};
            const auto terminal = [](void* target, ruvia::context& context_value) -> ruvia::task<void> {
                auto& observed_value = *static_cast<observation*>(target);
                observed_value.called_ = true;
                if (observed_value.mode_ >= 2) {
                    if (observed_value.mode_ == 3) {
                        context_value.header("Sec-WebSocket-Accept", "reserved");
                    }
                    const auto headers = ruvia::detail::websocket_response_headers(context_value);
                    if (observed_value.mode_ == 3) {
                        const auto handshake = ruvia::make_websocket_server_handshake(
                            *observed_value.request_, {.response_headers_ = headers});
                        (void)handshake;
                    }
                    if (!headers.empty()) {
                        observed_value.cookie_ = headers.front().value();
                    }
                    observed_value.frozen_ = rejects_mutation(context_value.session());
                    ruvia::detail::context_access::mark_websocket_handshake_started(context_value);
                } else if (observed_value.mode_ == 0) {
                    co_await context_value.stream().write("first");
                    observed_value.frozen_ = rejects_mutation(context_value.session());
                    co_await context_value.stream().write("second");
                }
            };
            const ruvia::detail::route_stream_handler_type handler(&observation_value, terminal);
            if (mode >= 2) {
                impl.register_websocket_route(ruvia::http_known_method::get,
                    std::pmr::string("/session"), handler, {}, middlewares);
            } else {
                impl.register_response_stream_route(ruvia::http_known_method::get,
                    std::pmr::string("/session"), handler, {}, middlewares);
            }
            impl.finalize();
            fixture.request_ = make_request(fixture.request_memory_.resource(), "/session");
            const auto& routes_value = impl.route_table();
            const auto resolution = routes_value.resolve(fixture.request_);
            session_head_sink sink;
            auto writer = ruvia::detail::make_response_stream_writer(sink, *fixture.memory_.resource());
            auto services = ruvia::detail::context_services(fixture.worker_, fixture.token_,
                {fixture.db_, fixture.redis_, fixture.http_});
            if (mode < 2) {
                services = services.with_response_stream(writer);
            }
            auto exercise = [&]() -> ruvia::task<void> {
                const auto response = mode >= 2
                                          ? co_await routes_value.dispatch_websocket(fixture.request_, *resolution.resolved(),
                                                fixture.request_memory_, handler, services)
                                          : co_await routes_value.dispatch_response_stream(fixture.request_, *resolution.resolved(),
                                                fixture.request_memory_, writer, services);
                RUVIA_CHECK_EQ(fixture.peer_.commands_.size(), std::size_t{1});
                if (fail || mode == 3) {
                    RUVIA_CHECK(response.has_value());
                    if (response) {
                        RUVIA_CHECK_EQ(response->status(), ruvia::http_status::internal_server_error);
                        if (fail) {
                            RUVIA_CHECK(!response->header("Set-Cookie").has_value());
                        }
                    }
                    RUVIA_CHECK(!sink.committed());
                    if (mode >= 2 && fail) {
                        RUVIA_CHECK(!observation_value.called_);
                    }
                } else {
                    RUVIA_CHECK(!response.has_value());
                    RUVIA_CHECK(observation_value.called_);
                    RUVIA_CHECK(mode == 2 ? observation_value.frozen_ : sink.frozen_);
                    RUVIA_CHECK((mode == 2 ? observation_value.cookie_ : sink.cookie_).starts_with("sid="));
                }
            };
            fixture.run(exercise());
        }
    }
}

#endif
