#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <asio/io_context.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/web/Context.h"

#include "http3/Http3ServerConnection.h"
#include "http3/http3_connection_state.h"
#include "http3/http3_stream_buffer.h"
#include "http3/http3_worker_server.h"
#include "integration/WorkerCapabilities.h"
#include "memory_resource_fixture.h"
#include "router/Router.h"
#include "router/RouterImpl.h"
#include "routing_fixture.h"
#include "server/HttpServerOptions.h"
#include "test_harness.h"

namespace ruvia::detail {

struct http3_worker_server_test_access final {
    [[nodiscard]] static bool pump_once(http3_worker_server& server) noexcept {
        return server.pump();
    }

    [[nodiscard]] static Http3ServerConnection* connection(
        http3_worker_server& server, http3_connection_identity identity) noexcept {
        auto* slot = server.find_slot(identity);
        return slot != nullptr ? slot->connection.get() : nullptr;
    }
};

}  // namespace ruvia::detail

namespace {

using Buffer = ruvia::detail::http3_stream_buffer;
using State = ruvia::detail::http3_connection_state;
using Server = ruvia::detail::http3_worker_server;
using Connection = ruvia::detail::Http3ServerConnection;
using Access = ruvia::detail::http3_worker_server_test_access;

struct LocalTransport final {
    State* state{};
    std::size_t executions{};
    std::size_t closes{};

    static State::intent_execution_result execute(void* context,
        ruvia::detail::http3_connection_identity identity,
        const Connection::TransportIntent& intent) noexcept {
        auto& transport = *static_cast<LocalTransport*>(context);
        ++transport.executions;
        if (intent.token.kind == Connection::TransportIntentKind::kConnectionClose) {
            ++transport.closes;
            if (transport.state->mark_transport_retired(identity) != State::status::changed) {
                std::terminate();
            }
            return {.outcome = State::execution_outcome::executed};
        }
        return {.outcome = State::execution_outcome::executed};
    }
};

struct Fixture final {
    ruvia::WorkerMemory memory;
    ruvia::detail::HttpServerOptions options;
    ruvia::detail::Router router;
    ruvia::detail::RouterImpl& routes{ruvia::detail::RouterImpl::from(router)};
    ruvia::detail::WorkerCapabilities capabilities;
    ruvia::ConnectionScanner scanner;
    ruvia::StopSource stop;
    ruvia::StopToken stop_token;
    std::atomic<std::size_t> active{};
    std::atomic<std::size_t> refused{};
    Server server;
    ruvia::WorkerSignal protocol_ready;
    ruvia::WorkerSignal handler_started;
    ruvia::WorkerSignal handler_release;
    bool handler_left{};
    Buffer requests;

    Fixture(const ruvia::WorkerHandle& worker, asio::io_context& io,
        std::pmr::memory_resource& upstream, std::size_t capacity = 1)
        : memory(upstream),
          capabilities(io, worker, memory.resource(), {}, {}),
          scanner(worker, {}),
          stop_token(stop.token()),
          server(worker, memory, finalize_routes(), capabilities, scanner,
              io.get_executor(), options, stop_token, capacity, 8, active, refused),
          protocol_ready(worker),
          handler_started(worker),
          handler_release(worker),
          requests(8, 8, 8, memory.resource()) {
        requests.set_local_notifications({.ready = {
                                              &server, [](void* context, std::uint8_t) noexcept {
                                                  static_cast<Server*>(context)->wake();
                                              }}});
    }

    [[nodiscard]] const ruvia::detail::RouteTable& finalize_routes() {
        routes.registerRoute(ruvia::HttpKnownMethod::kGet, routing_test::path("/stall"),
            ruvia::detail::RouteHandler(this, &Fixture::held_handler),
            ruvia::detail::RequestBodyMode::kBuffered,
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
        routes.finalize();
        return routes.routeTable();
    }

    static ruvia::Task<ruvia::HttpResponse> held_handler(void* context, ruvia::Context& request) {
        auto& fixture = *static_cast<Fixture*>(context);
        fixture.handler_started.notify();
        co_await fixture.handler_release.wait();
        fixture.handler_left = true;
        co_return request.text("retired");
    }

    [[nodiscard]] bool stage(std::span<State* const> states) noexcept {
        return server.stage_install({.request_buffer = &requests,
            .connections = states,
            .protocol_ready = {&protocol_ready, [](void* context, std::uint8_t) noexcept {
                                   static_cast<ruvia::WorkerSignal*>(context)->notify();
                               }}});
    }

    [[nodiscard]] State::local_change_callback state_wake() noexcept {
        return {this, [](void* context) noexcept {
                    auto& fixture = *static_cast<Fixture*>(context);
                    fixture.server.wake();
                    fixture.protocol_ready.notify();
                }};
    }
};

ruvia::Task<void> stop_after(ruvia::EventLoopAttachment& attachment,
    ruvia::Task<void> operation) {
    try {
        co_await std::move(operation);
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

void run_task(ruvia::EventLoopAttachment& attachment, ruvia::Task<void> operation) {
    auto root = attachment.loop().start(stop_after(attachment, std::move(operation)));
    attachment.run();
    root.get();
}

std::string request_headers(ruvia::WorkerMemory& memory) {
    const auto fields = ruvia::encodeHttp3ClientRequestHead(
        {.method = "GET", .scheme = "https", .authority = "example.test", .path = "/stall"},
        {}, memory.resource());
    if ((fields.index() != 0)) {
        throw std::runtime_error("HTTP/3 request field section encoding failed");
    }
    std::array<char, ruvia::kHttp3FrameHeaderMaxBytes> header{};
    const auto size = ruvia::encodeHttp3FrameHeader(header,
        static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders), std::get<0>(fields).fieldSection.size());
    if ((size.index() != 0)) {
        throw std::runtime_error("HTTP/3 request frame encoding failed");
    }
    std::string wire(header.data(), std::get<0>(size));
    wire.append(std::get<0>(fields).fieldSection.data(), std::get<0>(fields).fieldSection.size());
    return wire;
}

template <typename Scenario>
void with_worker(Scenario&& scenario) {
    asio::io_context io;
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    run_task(attachment, scenario(worker, io));
}

}  // namespace

RUVIA_TEST(http3_worker_server_drains_unattached_bound_connection_locally) {
    ruvia::test::CountingMemoryResource upstream;
    with_worker([&](const ruvia::WorkerHandle& worker, asio::io_context& io) -> ruvia::Task<void> {
        Fixture fixture(worker, io, upstream);
        State state(fixture.state_wake());
        const std::array states{&state};
        RUVIA_CHECK(fixture.stage(states));
        RUVIA_CHECK(fixture.server.install());
        RUVIA_CHECK(Access::pump_once(fixture.server));
        const auto identity = state.available_identity();
        RUVIA_CHECK(identity.has_value());
        RUVIA_CHECK_EQ(state.bind(*identity), State::status::changed);
        // Stop before consuming the binding; rejection and physical retirement
        // release the reservation without constructing a handler connection.
        fixture.server.request_stop();
        RUVIA_CHECK(Access::pump_once(fixture.server));
        RUVIA_CHECK(state.rejection() == State::reject_reason::stopping);
        RUVIA_CHECK_EQ(state.mark_transport_retired(*identity), State::status::changed);
        co_await fixture.server.run();
        RUVIA_CHECK(fixture.server.drained());
        RUVIA_CHECK(state.worker_finalized() && state.slot_reusable());
        RUVIA_CHECK_EQ(fixture.active.load(), std::size_t{0});
        RUVIA_CHECK(fixture.requests.stopped());
        RUVIA_CHECK(fixture.server.response_buffer().stopped());
    });
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3_worker_server_transport_retirement_waits_for_active_handler_join) {
    ruvia::test::CountingMemoryResource upstream;
    with_worker([&](const ruvia::WorkerHandle& worker, asio::io_context& io) -> ruvia::Task<void> {
        Fixture fixture(worker, io, upstream, 2);
        LocalTransport transport;
        State state(fixture.state_wake());
        transport.state = &state;
        state.set_transport_executor({&transport, &LocalTransport::execute});
        LocalTransport drain_transport;
        State drain_state(fixture.state_wake());
        drain_transport.state = &drain_state;
        drain_state.set_transport_executor({&drain_transport, &LocalTransport::execute});
        const std::array states{&state, &drain_state};
        RUVIA_CHECK(fixture.stage(states));
        RUVIA_CHECK(fixture.server.install());
        RUVIA_CHECK(Access::pump_once(fixture.server));
        const auto identity = state.available_identity();
        RUVIA_CHECK(identity.has_value());
        const auto drain_identity = drain_state.available_identity();
        RUVIA_CHECK(drain_identity.has_value());
        RUVIA_CHECK_EQ(state.bind(*identity, {.remote_address = "127.0.0.1", .remote_port = 43210}),
            State::status::changed);
        RUVIA_CHECK_EQ(drain_state.bind(*drain_identity,
                           {.remote_address = "127.0.0.1", .remote_port = 43211}),
            State::status::changed);
        RUVIA_CHECK(Access::pump_once(fixture.server));
        RUVIA_CHECK(state.admission() == State::admission_phase::handler_attached);
        const ruvia::detail::http3_stream_id stream{
            identity->epoch, identity->connection_generation, 0};
        const auto wire = request_headers(fixture.memory);
        RUVIA_CHECK(fixture.requests.try_send(stream, std::as_bytes(std::span(wire))) ==
                    Buffer::send_result::sent);
        RUVIA_CHECK(fixture.requests.try_send_control(
                        {ruvia::detail::http3_stream_control::kind::stream_fin, stream, wire.size()}) ==
                    Buffer::control_result::sent);
        RUVIA_CHECK(Access::pump_once(fixture.server));
        co_await fixture.handler_started.wait();
        RUVIA_CHECK(!fixture.handler_left);
        auto* connection = Access::connection(fixture.server, *identity);
        RUVIA_CHECK(connection != nullptr);
        RUVIA_CHECK_EQ(connection->activeTaskCount(), std::size_t{1});
        RUVIA_CHECK_EQ(fixture.active.load(), std::size_t{2});
        RUVIA_CHECK_EQ(state.seal_admission(*identity, 1, 0), State::status::changed);
        RUVIA_CHECK_EQ(drain_state.seal_admission(*drain_identity, 0, 0), State::status::changed);
        RUVIA_CHECK(Access::pump_once(fixture.server));
        RUVIA_CHECK(drain_state.worker_drained());
        RUVIA_CHECK(!state.worker_drained());
        fixture.server.request_stop();
        const auto close = connection->peekTransportIntent();
        RUVIA_CHECK(close.has_value());
        RUVIA_CHECK_EQ(state.mark_transport_retired(*identity), State::status::changed);
        RUVIA_CHECK(state.execute_intent(*identity, *close).completed());
        RUVIA_CHECK_EQ(transport.executions, std::size_t{0});
        RUVIA_CHECK(Access::pump_once(fixture.server));
        RUVIA_CHECK(Access::pump_once(fixture.server));
        RUVIA_CHECK(drain_state.worker_finalized() && drain_state.slot_reusable());
        // Physical retirement settles intent debt; handler finalization and
        // scheduler token retirement must remain behind the actual join.
        RUVIA_CHECK(!state.worker_finalized() && !state.slot_reusable());
        RUVIA_CHECK_EQ(fixture.active.load(), std::size_t{1});
        RUVIA_CHECK(!fixture.handler_left);
        RUVIA_CHECK(!fixture.server.drained());
        fixture.handler_release.notify();
        co_await fixture.server.run();
        RUVIA_CHECK(fixture.handler_left);
        RUVIA_CHECK(state.worker_finalized() && state.slot_reusable());
        RUVIA_CHECK(drain_state.worker_finalized() && drain_state.slot_reusable());
        RUVIA_CHECK(fixture.server.drained());
        RUVIA_CHECK_EQ(fixture.active.load(), std::size_t{0});
    });
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3_worker_server_batch_retires_local_connections) {
    ruvia::test::CountingMemoryResource upstream;
    with_worker([&](const ruvia::WorkerHandle& worker, asio::io_context& io) -> ruvia::Task<void> {
        Fixture fixture(worker, io, upstream, 4);
        std::array<LocalTransport, 4> transports{};
        State first(fixture.state_wake());
        State second(fixture.state_wake());
        State third(fixture.state_wake());
        State fourth(fixture.state_wake());
        const std::array states{&first, &second, &third, &fourth};
        for (std::size_t index = 0; index < states.size(); ++index) {
            transports[index].state = states[index];
            states[index]->set_transport_executor({&transports[index], &LocalTransport::execute});
        }
        RUVIA_CHECK(fixture.stage(states));
        RUVIA_CHECK(fixture.server.install());
        RUVIA_CHECK(Access::pump_once(fixture.server));
        for (auto* state : states) {
            const auto identity = state->available_identity();
            RUVIA_CHECK(identity.has_value());
            RUVIA_CHECK_EQ(state->bind(*identity, {.remote_address = "127.0.0.1", .remote_port = 43210}),
                State::status::changed);
        }
        RUVIA_CHECK(Access::pump_once(fixture.server));
        RUVIA_CHECK_EQ(fixture.active.load(), states.size());
        fixture.server.request_stop();
        co_await fixture.server.run();
        RUVIA_CHECK(fixture.server.drained());
        RUVIA_CHECK_EQ(fixture.active.load(), std::size_t{0});
        for (std::size_t index = 0; index < states.size(); ++index) {
            RUVIA_CHECK(states[index]->worker_finalized() && states[index]->slot_reusable());
            RUVIA_CHECK_EQ(transports[index].closes, std::size_t{1});
        }
    });
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(http3_worker_server_failed_install_rolls_back_before_launch) {
    with_worker([&](const ruvia::WorkerHandle& worker, asio::io_context& io) -> ruvia::Task<void> {
        ruvia::test::RejectingMemoryResource upstream;
        // Scheduler startup storage exceeds the pool's small-block cache.
        Fixture fixture(worker, io, upstream, 4096);
        std::array<State, 4096> states;
        std::array<State*, 4096> views{};
        for (std::size_t index = 0; index < states.size(); ++index) {
            views[index] = &states[index];
        }
        RUVIA_CHECK(!fixture.server.stage_install({.request_buffer = &fixture.requests}));
        RUVIA_CHECK(fixture.stage(views));
        upstream.rejectAllocations();
        RUVIA_CHECK(!fixture.server.install());
        upstream.rejectAllocations(false);
        RUVIA_CHECK(!fixture.server.installed());
        fixture.server.abandon_before_launch();
        RUVIA_CHECK(fixture.server.drained());
        RUVIA_CHECK(fixture.requests.stopped());
        RUVIA_CHECK(fixture.server.response_buffer().stopped());
        for (auto& state : states) {
            RUVIA_CHECK(state.ready_to_destroy());
            RUVIA_CHECK(!state.available_identity());
        }
        RUVIA_CHECK_EQ(fixture.active.load(), std::size_t{0});
        fixture.server.abandon_before_launch();
        co_return;
    });
}

RUVIA_TEST(http3_worker_server_failed_task_spawn_rolls_back_installed_local_state) {
    ruvia::test::CountingMemoryResource upstream;
    with_worker([&](const ruvia::WorkerHandle& worker, asio::io_context& io) -> ruvia::Task<void> {
        Fixture fixture(worker, io, upstream);
        State state(fixture.state_wake());
        const std::array states{&state};
        RUVIA_CHECK(fixture.stage(states));
        RUVIA_CHECK(fixture.server.install());
        ruvia::test::RejectingMemoryResource scope_resource;
        ruvia::TaskScope scope(worker, {.resource = &scope_resource});
        scope_resource.rejectAllocations();
        bool failed = false;
        try {
            scope.spawn(fixture.server.run());
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        scope_resource.rejectAllocations(false);
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(scope.size(), std::size_t{0});
        fixture.server.abandon_before_launch();
        RUVIA_CHECK(fixture.server.drained());
        RUVIA_CHECK(fixture.requests.stopped());
        RUVIA_CHECK(fixture.server.response_buffer().stopped());
        RUVIA_CHECK(state.ready_to_destroy());
        RUVIA_CHECK(!state.available_identity());
        RUVIA_CHECK_EQ(fixture.active.load(), std::size_t{0});
        co_await scope.join();
    });
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
