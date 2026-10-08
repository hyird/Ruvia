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

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/WebSocketHandshake.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Session.h"

#include "client/HttpClientRegistry.h"
#include "context/ContextAccess.h"
#include "db/DbRegistry.h"
#include "http/SessionAccess.h"
#include "memory_resource_fixture.h"
#include "redis/RedisRegistry.h"
#include "router/RouteTable.h"
#include "router/RouterImpl.h"
#include "server/HttpResponseStreamDispatch.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "websocket/WebSocketResponseHeaders.h"

namespace {

ruvia::HttpRequest makeRequest(std::pmr::memory_resource* resource, std::string_view target = "/") {
    auto [request, parseError] = ruvia::makeParsedHttpRequest("GET", target, {}, {}, resource);
    if (parseError) {
        throw std::logic_error("invalid session middleware test request");
    }
    return std::move(request);
}

// A bounded RESP peer exercises the middleware's actual asynchronous storage
// boundary without requiring an external Redis process.
class SessionStoragePeer final {
public:
    explicit SessionStoragePeer(asio::io_context& io)
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
    bool failSet{false};
    bool failDelete{false};
    ruvia::StopSource* cancelWrite{nullptr};
    std::vector<std::vector<std::string>> commands;
    std::map<std::string, std::string, std::less<>> sessions;

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
                commands.push_back(std::move(command));
                const auto& received_command = commands.back();
                const auto& verb = received_command.front();
                if (cancelWrite != nullptr && (verb == "SET" || verb == "EVAL" || verb == "DEL")) {
                    cancelWrite->requestStop();
                    continue;
                }
                std::string reply;
                if (verb == "GET") {
                    const auto found = sessions.find(received_command[1]);
                    reply = found == sessions.end()
                                ? "$-1\r\n"
                                : "$" + std::to_string(found->second.size()) + "\r\n" + found->second + "\r\n";
                } else if (verb == "EVAL") {
                    if (failSet || failDelete) {
                        reply = "-ERR rotation failed\r\n";
                    } else if (!sessions.contains(received_command[3]) || sessions.contains(received_command[4])) {
                        reply = ":0\r\n";
                    } else {
                        sessions.emplace(received_command[4], received_command[5]);
                        sessions.erase(received_command[3]);
                        reply = ":1\r\n";
                    }
                } else if (verb == "DEL") {
                    reply = failDelete ? "-ERR delete failed\r\n" : ":" + std::to_string(sessions.erase(received_command[1])) + "\r\n";
                } else if (verb == "SET") {
                    if (failSet) {
                        reply = "-ERR storage failed\r\n";
                    } else if (sessions.contains(received_command[1])) {
                        reply = "$-1\r\n";
                    } else {
                        sessions.emplace(received_command[1], received_command[2]);
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

struct SessionFixture final {
    asio::io_context& io{ruvia::test::newTestIoContext()};
    ruvia::EventLoopAttachment attachment{ruvia::attachEventLoop(io)};
    ruvia::WorkerHandle worker{attachment.loop().handle()};
    SessionStoragePeer peer{io};
    ruvia::test::CountingMemoryResource operationMemory;
    std::array<ruvia::detail::RedisDefinition, 1> definitions{definition()};
    ruvia::detail::RedisRegistry redis{io, &operationMemory, definitions, worker};
    ruvia::detail::DbRegistry db{io, worker, &operationMemory, std::span<const ruvia::detail::DbDefinition>{}};
    ruvia::detail::HttpClientRegistry http{io, worker, &operationMemory, std::span<const ruvia::detail::HttpClientDefinition>{}};
    ruvia::StopSource stop;
    ruvia::StopToken token{stop.token()};
    ruvia::WorkerMemory memory;
    ruvia::RequestMemory requestMemory{memory};
    ruvia::HttpRequest request{makeRequest(requestMemory.resource())};
    ruvia::Context context{ruvia::detail::ContextAccess::make(requestMemory, request,
        ruvia::detail::ContextServices(worker, token, {db, redis, http}))};
    ruvia::SessionMiddleware middleware;

    ruvia::detail::RedisDefinition definition() {
        auto config = ruvia::RedisConfig{};
        config.host = "127.0.0.1";
        config.tls.mode = ruvia::client_tls_mode::disabled;
        config.port = peer.port();
        config.poolSizePerWorker = 1;
        return {std::pmr::string("default", &operationMemory),
            ruvia::detail::RedisConfigStorage(config, &operationMemory)};
    }
    void bind() {
        ruvia::detail::SessionAccess::bind(context, &middleware);
    }
    void load(std::string_view id, std::string_view data) {
        peer.sessions.emplace("sess:" + std::string(id), data);
        ruvia::detail::SessionAccess::observePresentedId(context, id);
        ruvia::detail::SessionAccess::load(context, data);
    }
    void run(ruvia::Task<void> task) {
        auto server = asio::co_spawn(io, peer.serve(), asio::use_future);
        auto execute = [&]() -> ruvia::Task<void> {
            std::exception_ptr failure;
            try {
                co_await std::move(task);
            } catch (...) {
                failure = std::current_exception();
            }
            redis.closeNow();
            peer.close();
            attachment.stop();
            if (failure) {
                std::rethrow_exception(failure);
            }
        };
        auto result = asio::co_spawn(io, ruvia::asAwaitable(execute()), asio::use_future);
        attachment.run();
        result.get();
        server.get();
    }
};

bool rejectsMutation(ruvia::Session session) {
    int rejections = 0;
    try {
        session.set("changed");
    } catch (const std::logic_error&) {
        ++rejections;
    }
    try {
        session.clear();
    } catch (const std::logic_error&) {
        ++rejections;
    }
    try {
        session.regenerate();
    } catch (const std::logic_error&) {
        ++rejections;
    }
    return rejections == 3;
}

class SessionOuterMiddleware final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        co_await next();
    }
};

class SessionMutationMiddleware final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        context.session().set("user=1");
        co_await next();
    }
};

class SessionHeadSink final {
public:
    void bindContext(ruvia::Context* context, ruvia::Task<ruvia::HttpResponse> (*head)(ruvia::Context&)) {
        context_ = context;
        head_ = head;
    }
    void releaseContext() noexcept {
        context_ = nullptr;
        head_ = nullptr;
    }
    bool committed() const noexcept {
        return committed_;
    }
    bool aborted() const noexcept {
        return false;
    }
    ruvia::Task<void> write(std::string_view) {
        co_await commit();
    }
    ruvia::Task<void> end(std::span<const ruvia::HttpHeaderView>) {
        co_await commit();
    }
    ruvia::Task<ruvia::TimerSleepResult> sleep(std::chrono::milliseconds, const ruvia::StopToken&) {
        co_return ruvia::TimerSleepResult::kElapsed;
    }
    std::string cookie;
    bool frozen{false};

private:
    ruvia::Task<void> commit() {
        if (committed_) {
            co_return;
        }
        const auto response = co_await head_(*context_);
        cookie = response.header("Set-Cookie").value_or("");
        frozen = rejectsMutation(context_->session());
        committed_ = true;
    }
    ruvia::Context* context_{nullptr};
    ruvia::Task<ruvia::HttpResponse> (*head_)(ruvia::Context&){nullptr};
    bool committed_{false};
};

}  // namespace

RUVIA_TEST(session_commit_persists_once_and_publishes_cookie_before_head) {
    SessionFixture fixture;
    fixture.bind();
    auto session = fixture.context.session();
    session.set("user=1");
    fixture.context.header("Set-Cookie", "theme=dark", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
    auto exercise = [&]() -> ruvia::Task<void> {
        // Discarding a lazy commit must neither freeze nor persist the session.
        {
            auto discarded = ruvia::detail::SessionAccess::commit(fixture.context);
        }
        session.set("user=2");
        co_await ruvia::detail::SessionAccess::commit(fixture.context);
        const auto allocations = fixture.operationMemory.liveAllocations();
        const auto head = ruvia::detail::webSocketResponseHeaders(fixture.context);
        RUVIA_CHECK_EQ(head.size(), std::size_t{2});
        RUVIA_CHECK_EQ(head[0].value(), std::string_view("theme=dark"));
        RUVIA_CHECK(head[1].value().starts_with("sid="));
        const std::string cookie(head[1].value());
        for (int i = 0; i != 32; ++i) {
            co_await ruvia::detail::SessionAccess::commit(fixture.context);
            RUVIA_CHECK_EQ(fixture.operationMemory.liveAllocations(), allocations);
            RUVIA_CHECK_EQ(head[1].value(), std::string_view(cookie));
            RUVIA_CHECK_EQ(session.data(), std::string_view("user=2"));
        }
        RUVIA_CHECK(rejectsMutation(session));
        RUVIA_CHECK_EQ(fixture.peer.commands.size(), std::size_t{1});
        RUVIA_CHECK_EQ(fixture.peer.commands[0][2], std::string("user=2"));
    };
    fixture.run(exercise());
}

RUVIA_TEST(session_commit_rotates_and_clears_before_publishing_cookie) {
    for (bool clear : {false, true}) {
        SessionFixture fixture;
        fixture.bind();
        fixture.load("deadbeef", "user=1");
        auto session = fixture.context.session();
        if (clear) {
            session.clear();
        } else {
            session.regenerate();
        }
        auto exercise = [&]() -> ruvia::Task<void> {
            co_await ruvia::detail::SessionAccess::commit(fixture.context);
            const auto& commands = fixture.peer.commands;
            RUVIA_CHECK_EQ(commands.size(), std::size_t{1});
            RUVIA_CHECK_EQ(commands.back().front(), std::string(clear ? "DEL" : "EVAL"));
            RUVIA_CHECK(commands.back()[clear ? 1 : 3].ends_with("deadbeef"));
            const auto head = ruvia::detail::webSocketResponseHeaders(fixture.context);
            RUVIA_CHECK_EQ(head.size(), std::size_t{1});
            RUVIA_CHECK_EQ(head.front().value().contains("Max-Age=0"), clear);
            RUVIA_CHECK(rejectsMutation(session));
        };
        fixture.run(exercise());
    }
}

RUVIA_TEST(session_commit_failure_and_cancellation_never_publish_or_retry) {
    // Rejection or cancellation before the peer applies a command publishes no
    // cookie and makes the failed commit terminal for writes and logout alike.
    for (int mode = 0; mode != 3; ++mode) {
        for (const bool cancel : {false, true}) {
            SessionFixture fixture;
            fixture.bind();
            fixture.peer.failSet = !cancel;
            fixture.peer.failDelete = !cancel;
            if (cancel) {
                fixture.peer.cancelWrite = &fixture.stop;
            }
            if (mode != 0) {
                fixture.load("deadbeef", "user=1");
            }
            if (mode == 2) {
                fixture.context.session().clear();
            } else {
                fixture.context.session().set("user=2");
            }
            auto exercise = [&]() -> ruvia::Task<void> {
                for (int attempt = 0; attempt != 2; ++attempt) {
                    bool failed = false;
                    try {
                        co_await ruvia::detail::SessionAccess::commit(fixture.context);
                    } catch (const ruvia::RedisError& error) {
                        failed = cancel ? error.code() == ruvia::RedisError::Code::kCancelled
                                        : error.code() == ruvia::RedisError::Code::kCommandError;
                    }
                    RUVIA_CHECK(failed);
                    RUVIA_CHECK(ruvia::detail::webSocketResponseHeaders(fixture.context).empty());
                    RUVIA_CHECK(rejectsMutation(fixture.context.session()));
                }
                RUVIA_CHECK_EQ(fixture.peer.commands.size(), std::size_t{1});
                if (mode == 0) {
                    RUVIA_CHECK(fixture.peer.sessions.empty());
                } else {
                    RUVIA_CHECK_EQ(fixture.peer.sessions.at("sess:deadbeef"), std::string("user=1"));
                }
            };
            fixture.run(exercise());
        }
    }
}

RUVIA_TEST(session_commit_rejects_updates_and_rotations_of_revoked_sessions) {
    for (const bool rotate : {false, true}) {
        SessionFixture fixture;
        fixture.bind();
        fixture.load("deadbeef", "user=1");
        fixture.context.session().set("user=2");
        if (rotate) {
            fixture.context.session().regenerate();
        }
        fixture.peer.sessions.erase("sess:deadbeef");
        auto exercise = [&]() -> ruvia::Task<void> {
            bool rejected = false;
            try {
                co_await ruvia::detail::SessionAccess::commit(fixture.context);
            } catch (const ruvia::HttpError& error) {
                rejected = error.info().status() == ruvia::http_status::kConflict;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK(ruvia::detail::webSocketResponseHeaders(fixture.context).empty());
            RUVIA_CHECK_EQ(fixture.peer.commands.size(), std::size_t{1});
            RUVIA_CHECK(fixture.peer.sessions.empty());
        };
        fixture.run(exercise());
    }
}

RUVIA_TEST(session_writes_rotate_existing_identity_and_revoke_the_old_cookie) {
    for (const bool unchanged : {false, true}) {
        SessionFixture fixture;
        const std::string initial_data = unchanged ? "user=2" : "anonymous";
        fixture.peer.sessions.emplace("sess:deadbeef", initial_data);
        auto read_only = [&](std::string_view id, std::string_view expected) -> ruvia::Task<void> {
            const std::string cookie = "sid=" + std::string(id);
            const std::array headers{ruvia::HttpHeaderView{"Cookie", cookie}};
            auto [request, error] = ruvia::makeParsedHttpRequest("GET", "/", headers, {}, fixture.requestMemory.resource());
            RUVIA_CHECK(!error);
            auto context = ruvia::detail::ContextAccess::make(fixture.requestMemory, request,
                ruvia::detail::ContextServices(fixture.worker, fixture.token, {fixture.db, fixture.redis, fixture.http}));
            ruvia::detail::NextState::Control control;
            auto next = ruvia::detail::NextAccess::make({.context = &context, .control = &control},
                [](ruvia::detail::NextState) -> ruvia::Task<void> { co_return; });
            co_await fixture.middleware.handle(context, next);
            const auto session = context.session();
            RUVIA_CHECK_EQ(session.data(), expected);
            RUVIA_CHECK(ruvia::detail::webSocketResponseHeaders(context).empty());
        };
        auto exercise = [&]() -> ruvia::Task<void> {
            co_await read_only("deadbeef", initial_data);
            const std::array headers{ruvia::HttpHeaderView{"Cookie", "sid=deadbeef"}};
            auto [request, error] = ruvia::makeParsedHttpRequest("POST", "/login", headers, {}, fixture.requestMemory.resource());
            RUVIA_CHECK(!error);
            auto context = ruvia::detail::ContextAccess::make(fixture.requestMemory, request,
                ruvia::detail::ContextServices(fixture.worker, fixture.token, {fixture.db, fixture.redis, fixture.http}));
            ruvia::detail::NextState::Control control;
            auto next = ruvia::detail::NextAccess::make({.context = &context, .control = &control},
                [](ruvia::detail::NextState state) -> ruvia::Task<void> {
                    auto session = state.context->session();
                    session.set("user=2");
                    session.set("user=2");
                    co_return;
                });
            co_await fixture.middleware.handle(context, next);
            const auto response_headers = ruvia::detail::webSocketResponseHeaders(context);
            RUVIA_CHECK_EQ(response_headers.size(), std::size_t{1});
            if (response_headers.empty()) {
                co_return;
            }
            const auto cookie = response_headers.front().value();
            RUVIA_CHECK(cookie.starts_with("sid="));
            const std::string new_id(cookie.substr(4, cookie.find(';') - 4));
            RUVIA_CHECK(new_id != "deadbeef");
            RUVIA_CHECK(ruvia::detail::isValidSessionId(new_id));
            RUVIA_CHECK_EQ(fixture.peer.sessions.size(), std::size_t{1});
            RUVIA_CHECK_EQ(fixture.peer.sessions.at("sess:" + new_id), std::string("user=2"));
            RUVIA_CHECK(!fixture.peer.sessions.contains("sess:deadbeef"));
            const auto commands = fixture.peer.commands.size();
            co_await ruvia::detail::SessionAccess::commit(context);
            RUVIA_CHECK_EQ(fixture.peer.commands.size(), commands);
            co_await read_only("deadbeef", "");
            co_await read_only(new_id, "user=2");
        };
        fixture.run(exercise());
    }
}

RUVIA_TEST(session_middleware_commits_before_stream_and_websocket_terminal) {
    // 0: stream; 1: empty stream; 2: WebSocket; 3: failed handshake preparation.
    for (int mode = 0; mode != 4; ++mode) {
        for (bool fail : {false, true}) {
            SessionFixture fixture;
            fixture.peer.failSet = fail;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            const std::array middlewares{
                ruvia::detail::makeMiddlewareDescriptor<SessionOuterMiddleware>(),
                ruvia::detail::makeMiddlewareDescriptor<ruvia::SessionMiddleware>(),
                ruvia::detail::makeMiddlewareDescriptor<SessionMutationMiddleware>()};
            struct Observation {
                int mode;
                const ruvia::HttpRequest* request;
                bool called{false};
                bool frozen{false};
                std::string cookie;
            } observation{mode, &fixture.request};
            const auto terminal = [](void* target, ruvia::Context& context) -> ruvia::Task<void> {
                auto& observed = *static_cast<Observation*>(target);
                observed.called = true;
                if (observed.mode >= 2) {
                    if (observed.mode == 3) {
                        context.header("Sec-WebSocket-Accept", "reserved");
                    }
                    const auto headers = ruvia::detail::webSocketResponseHeaders(context);
                    if (observed.mode == 3) {
                        const auto handshake = ruvia::makeWebSocketServerHandshake(
                            *observed.request, {.responseHeaders = headers});
                        (void)handshake;
                    }
                    if (!headers.empty()) {
                        observed.cookie = headers.front().value();
                    }
                    observed.frozen = rejectsMutation(context.session());
                    ruvia::detail::ContextAccess::markWebSocketHandshakeStarted(context);
                } else if (observed.mode == 0) {
                    co_await context.stream().write("first");
                    observed.frozen = rejectsMutation(context.session());
                    co_await context.stream().write("second");
                }
            };
            const ruvia::detail::RouteStreamHandler handler(&observation, terminal);
            if (mode >= 2) {
                impl.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
                    std::pmr::string("/session"), handler, {}, middlewares);
            } else {
                impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet,
                    std::pmr::string("/session"), handler, {}, middlewares);
            }
            impl.finalize();
            fixture.request = makeRequest(fixture.requestMemory.resource(), "/session");
            const auto& routes = impl.routeTable();
            const auto resolution = routes.resolve(fixture.request);
            SessionHeadSink sink;
            auto writer = ruvia::detail::makeResponseStreamWriter(sink, *fixture.memory.resource());
            auto services = ruvia::detail::ContextServices(fixture.worker, fixture.token,
                {fixture.db, fixture.redis, fixture.http});
            if (mode < 2) {
                services = services.withResponseStream(writer);
            }
            auto exercise = [&]() -> ruvia::Task<void> {
                const auto response = mode >= 2
                                          ? co_await routes.dispatchWebSocket(fixture.request, *resolution.resolved(),
                                                fixture.requestMemory, handler, services)
                                          : co_await routes.dispatchResponseStream(fixture.request, *resolution.resolved(),
                                                fixture.requestMemory, writer, services);
                RUVIA_CHECK_EQ(fixture.peer.commands.size(), std::size_t{1});
                if (fail || mode == 3) {
                    RUVIA_CHECK(response.has_value());
                    if (response) {
                        RUVIA_CHECK_EQ(response->status(), ruvia::http_status::kInternalServerError);
                        if (fail) {
                            RUVIA_CHECK(!response->header("Set-Cookie").has_value());
                        }
                    }
                    RUVIA_CHECK(!sink.committed());
                    if (mode >= 2 && fail) {
                        RUVIA_CHECK(!observation.called);
                    }
                } else {
                    RUVIA_CHECK(!response.has_value());
                    RUVIA_CHECK(observation.called);
                    RUVIA_CHECK(mode == 2 ? observation.frozen : sink.frozen);
                    RUVIA_CHECK((mode == 2 ? observation.cookie : sink.cookie).starts_with("sid="));
                }
            };
            fixture.run(exercise());
        }
    }
}

#endif
