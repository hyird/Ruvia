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

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/web/context.h"

#include "http3/http3_connection_state.h"
#include "http3/http3_server_connection.h"
#include "http3/http3_stream_buffer.h"
#include "http3/http3_worker_server.h"
#include "integration/worker_capabilities.h"
#include "memory_resource_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "routing_fixture.h"
#include "server/http_server_options.h"
#include "test_harness.h"

namespace ruvia::detail {

struct http3_worker_server_test_access final {
    [[nodiscard]] static bool pump_once(http3_worker_server& server) noexcept {
        return server.pump();
    }

    [[nodiscard]] static http3_server_connection* connection(
        http3_worker_server& server, http3_connection_identity identity) noexcept {
        auto* slot = server.find_slot(identity);
        return slot != nullptr ? slot->connection_.get() : nullptr;
    }
};

}  // namespace ruvia::detail

namespace {

using buffer_type = ruvia::detail::http3_stream_buffer;
using state_type = ruvia::detail::http3_connection_state;
using server_type = ruvia::detail::http3_worker_server;
using connection_type = ruvia::detail::http3_server_connection;
using access_type = ruvia::detail::http3_worker_server_test_access;

struct local_transport final {
    state_type* state_{};
    std::size_t executions_{};
    std::size_t closes_{};

    static state_type::intent_execution_result execute(void* context_value,
        ruvia::detail::http3_connection_identity identity,
        const connection_type::transport_intent_type& intent) noexcept {
        auto& transport = *static_cast<local_transport*>(context_value);
        ++transport.executions_;
        if (intent.token_.kind_ == connection_type::transport_intent_kind_type::connection_close) {
            ++transport.closes_;
            if (transport.state_->mark_transport_retired(identity) != state_type::status::changed) {
                std::terminate();
            }
            return {.outcome_ = state_type::execution_outcome::executed};
        }
        return {.outcome_ = state_type::execution_outcome::executed};
    }
};

struct fixture final {
    ruvia::worker_memory memory_;
    ruvia::detail::http_server_options options_;
    ruvia::detail::router router_;
    ruvia::detail::router_impl& routes_{ruvia::detail::router_impl::from(router_)};
    ruvia::detail::worker_capabilities capabilities_;
    ruvia::connection_scanner scanner_;
    ruvia::stop_source stop_;
    ruvia::stop_token stop_token_;
    std::atomic<std::size_t> active_{};
    std::atomic<std::size_t> refused_{};
    server_type server_;
    ruvia::worker_signal protocol_ready_;
    ruvia::worker_signal handler_started_;
    ruvia::worker_signal handler_release_;
    bool handler_left_{};
    buffer_type requests_;

    fixture(const ruvia::worker_handle& worker_value, asio::io_context& io,
        std::pmr::memory_resource& upstream, std::size_t capacity = 1)
        : memory_(upstream),
          capabilities_(io, worker_value, memory_.resource(), {}, {}),
          scanner_(worker_value, {}),
          stop_token_(stop_.token()),
          server_(worker_value, memory_, finalize_routes(), capabilities_, scanner_,
              io.get_executor(), options_, stop_token_, capacity, 8, active_, refused_),
          protocol_ready_(worker_value),
          handler_started_(worker_value),
          handler_release_(worker_value),
          requests_(8, 8, 8, memory_.resource()) {
        requests_.set_local_notifications({.ready_ = {
                                               &server_, [](void* context_value, std::uint8_t) noexcept {
                                                   static_cast<server_type*>(context_value)->wake();
                                               }}});
    }

    [[nodiscard]] const ruvia::detail::route_table& finalize_routes() {
        routes_.register_route(ruvia::http_known_method::get, routing_test::path("/stall"),
            ruvia::detail::route_handler_type(this, &fixture::held_handler),
            ruvia::detail::request_body_mode::buffered,
            std::span<const ruvia::detail::controller_middleware_descriptor>{},
            std::span<const ruvia::detail::controller_middleware_descriptor>{});
        routes_.finalize();
        return routes_.route_table();
    }

    static ruvia::task<ruvia::http_response> held_handler(void* context_value, ruvia::context& request) {
        auto& fixture_value = *static_cast<fixture*>(context_value);
        fixture_value.handler_started_.notify();
        co_await fixture_value.handler_release_.wait();
        fixture_value.handler_left_ = true;
        co_return request.text("retired");
    }

    [[nodiscard]] bool stage(std::span<state_type* const> states) noexcept {
        return server_.stage_install({.request_buffer_ = &requests_,
            .connections_ = states,
            .protocol_ready_ = {&protocol_ready_, [](void* context_value, std::uint8_t) noexcept {
                                    static_cast<ruvia::worker_signal*>(context_value)->notify();
                                }}});
    }

    [[nodiscard]] state_type::local_change_callback state_wake() noexcept {
        return {this, [](void* context_value) noexcept {
                    auto& fixture_value = *static_cast<fixture*>(context_value);
                    fixture_value.server_.wake();
                    fixture_value.protocol_ready_.notify();
                }};
    }
};

ruvia::task<void> stop_after(ruvia::event_loop_attachment& attachment,
    ruvia::task<void> operation) {
    try {
        co_await std::move(operation);
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

void run_task(ruvia::event_loop_attachment& attachment, ruvia::task<void> operation) {
    auto root = attachment.loop().start(stop_after(attachment, std::move(operation)));
    attachment.run();
    root.get();
}

std::string request_headers(ruvia::worker_memory& memory) {
    const auto fields_value = ruvia::encode_http3_client_request_head(
        {.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .path_ = "/stall"},
        {}, memory.resource());
    if ((fields_value.index() != 0)) {
        throw std::runtime_error("HTTP/3 request field section encoding failed");
    }
    std::array<char, ruvia::http3_frame_header_max_bytes> header_value{};
    const auto size = ruvia::encode_http3_frame_header(header_value,
        static_cast<std::uint64_t>(ruvia::http3_frame_type::headers), std::get<0>(fields_value).field_section_.size());
    if ((size.index() != 0)) {
        throw std::runtime_error("HTTP/3 request frame encoding failed");
    }
    std::string wire(header_value.data(), std::get<0>(size));
    wire.append(std::get<0>(fields_value).field_section_.data(), std::get<0>(fields_value).field_section_.size());
    return wire;
}

template <typename scenario_type>
void with_worker(scenario_type&& scenario) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    run_task(attachment, scenario(worker_value, io));
}

}  // namespace

RUVIA_TEST(http3_worker_server_drains_unattached_bound_connection_locally) {
    ruvia::test::counting_memory_resource upstream;
    with_worker([&](const ruvia::worker_handle& worker_value, asio::io_context& io) -> ruvia::task<void> {
        fixture fixture(worker_value, io, upstream);
        state_type state_value(fixture.state_wake());
        const std::array states{&state_value};
        RUVIA_CHECK(fixture.stage(states));
        RUVIA_CHECK(fixture.server_.install());
        RUVIA_CHECK(access_type::pump_once(fixture.server_));
        const auto identity = state_value.available_identity();
        RUVIA_CHECK(identity.has_value());
        RUVIA_CHECK_EQ(state_value.bind(*identity), state_type::status::changed);
        // Stop before consuming the binding; rejection and physical retirement
        // release the reservation without constructing a handler connection.
        fixture.server_.request_stop();
        RUVIA_CHECK(access_type::pump_once(fixture.server_));
        RUVIA_CHECK(state_value.rejection() == state_type::reject_reason::stopping);
        RUVIA_CHECK_EQ(state_value.mark_transport_retired(*identity), state_type::status::changed);
        co_await fixture.server_.run();
        RUVIA_CHECK(fixture.server_.drained());
        RUVIA_CHECK(state_value.worker_finalized() && state_value.slot_reusable());
        RUVIA_CHECK_EQ(fixture.active_.load(), std::size_t{0});
        RUVIA_CHECK(fixture.requests_.stopped());
        RUVIA_CHECK(fixture.server_.response_buffer().stopped());
    });
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_worker_server_transport_retirement_waits_for_active_handler_join) {
    ruvia::test::counting_memory_resource upstream;
    with_worker([&](const ruvia::worker_handle& worker_value, asio::io_context& io) -> ruvia::task<void> {
        fixture fixture_value(worker_value, io, upstream, 2);
        local_transport transport;
        state_type state_value(fixture_value.state_wake());
        transport.state_ = &state_value;
        state_value.set_transport_executor({&transport, &local_transport::execute});
        local_transport drain_transport;
        state_type drain_state(fixture_value.state_wake());
        drain_transport.state_ = &drain_state;
        drain_state.set_transport_executor({&drain_transport, &local_transport::execute});
        const std::array states{&state_value, &drain_state};
        RUVIA_CHECK(fixture_value.stage(states));
        RUVIA_CHECK(fixture_value.server_.install());
        RUVIA_CHECK(access_type::pump_once(fixture_value.server_));
        const auto identity = state_value.available_identity();
        RUVIA_CHECK(identity.has_value());
        const auto drain_identity = drain_state.available_identity();
        RUVIA_CHECK(drain_identity.has_value());
        RUVIA_CHECK_EQ(state_value.bind(*identity, {.remote_address_ = "127.0.0.1", .remote_port_ = 43210}),
            state_type::status::changed);
        RUVIA_CHECK_EQ(drain_state.bind(*drain_identity,
                           {.remote_address_ = "127.0.0.1", .remote_port_ = 43211}),
            state_type::status::changed);
        RUVIA_CHECK(access_type::pump_once(fixture_value.server_));
        RUVIA_CHECK(state_value.admission() == state_type::admission_phase::handler_attached);
        const ruvia::detail::http3_stream_id stream{
            identity->epoch_, identity->connection_generation_, 0};
        const auto wire = request_headers(fixture_value.memory_);
        RUVIA_CHECK(fixture_value.requests_.try_send(stream, std::as_bytes(std::span(wire))) ==
                    buffer_type::send_result::sent);
        RUVIA_CHECK(fixture_value.requests_.try_send_control(
                        {ruvia::detail::http3_stream_control::kind::stream_fin, stream, wire.size()}) ==
                    buffer_type::control_result::sent);
        RUVIA_CHECK(access_type::pump_once(fixture_value.server_));
        co_await fixture_value.handler_started_.wait();
        RUVIA_CHECK(!fixture_value.handler_left_);
        auto* connection = access_type::connection(fixture_value.server_, *identity);
        RUVIA_CHECK(connection != nullptr);
        RUVIA_CHECK_EQ(connection->active_task_count(), std::size_t{1});
        RUVIA_CHECK_EQ(fixture_value.active_.load(), std::size_t{2});
        RUVIA_CHECK_EQ(state_value.seal_admission(*identity, 1, 0), state_type::status::changed);
        RUVIA_CHECK_EQ(drain_state.seal_admission(*drain_identity, 0, 0), state_type::status::changed);
        RUVIA_CHECK(access_type::pump_once(fixture_value.server_));
        RUVIA_CHECK(drain_state.worker_drained());
        RUVIA_CHECK(!state_value.worker_drained());
        fixture_value.server_.request_stop();
        const auto close = connection->peek_transport_intent();
        RUVIA_CHECK(close.has_value());
        RUVIA_CHECK_EQ(state_value.mark_transport_retired(*identity), state_type::status::changed);
        RUVIA_CHECK(state_value.execute_intent(*identity, *close).completed());
        RUVIA_CHECK_EQ(transport.executions_, std::size_t{0});
        RUVIA_CHECK(access_type::pump_once(fixture_value.server_));
        RUVIA_CHECK(access_type::pump_once(fixture_value.server_));
        RUVIA_CHECK(drain_state.worker_finalized() && drain_state.slot_reusable());
        // Physical retirement settles intent debt; handler finalization and
        // scheduler token retirement must remain behind the actual join.
        RUVIA_CHECK(!state_value.worker_finalized() && !state_value.slot_reusable());
        RUVIA_CHECK_EQ(fixture_value.active_.load(), std::size_t{1});
        RUVIA_CHECK(!fixture_value.handler_left_);
        RUVIA_CHECK(!fixture_value.server_.drained());
        fixture_value.handler_release_.notify();
        co_await fixture_value.server_.run();
        RUVIA_CHECK(fixture_value.handler_left_);
        RUVIA_CHECK(state_value.worker_finalized() && state_value.slot_reusable());
        RUVIA_CHECK(drain_state.worker_finalized() && drain_state.slot_reusable());
        RUVIA_CHECK(fixture_value.server_.drained());
        RUVIA_CHECK_EQ(fixture_value.active_.load(), std::size_t{0});
    });
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_worker_server_batch_retires_local_connections) {
    ruvia::test::counting_memory_resource upstream;
    with_worker([&](const ruvia::worker_handle& worker_value, asio::io_context& io) -> ruvia::task<void> {
        fixture fixture_value(worker_value, io, upstream, 4);
        std::array<local_transport, 4> transports{};
        state_type first(fixture_value.state_wake());
        state_type second(fixture_value.state_wake());
        state_type third(fixture_value.state_wake());
        state_type fourth(fixture_value.state_wake());
        const std::array states{&first, &second, &third, &fourth};
        for (std::size_t index = 0; index < states.size(); ++index) {
            transports[index].state_ = states[index];
            states[index]->set_transport_executor({&transports[index], &local_transport::execute});
        }
        RUVIA_CHECK(fixture_value.stage(states));
        RUVIA_CHECK(fixture_value.server_.install());
        RUVIA_CHECK(access_type::pump_once(fixture_value.server_));
        for (auto* state : states) {
            const auto identity = state->available_identity();
            RUVIA_CHECK(identity.has_value());
            RUVIA_CHECK_EQ(state->bind(*identity, {.remote_address_ = "127.0.0.1", .remote_port_ = 43210}),
                state_type::status::changed);
        }
        RUVIA_CHECK(access_type::pump_once(fixture_value.server_));
        RUVIA_CHECK_EQ(fixture_value.active_.load(), states.size());
        fixture_value.server_.request_stop();
        co_await fixture_value.server_.run();
        RUVIA_CHECK(fixture_value.server_.drained());
        RUVIA_CHECK_EQ(fixture_value.active_.load(), std::size_t{0});
        for (std::size_t index = 0; index < states.size(); ++index) {
            RUVIA_CHECK(states[index]->worker_finalized() && states[index]->slot_reusable());
            RUVIA_CHECK_EQ(transports[index].closes_, std::size_t{1});
        }
    });
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http3_worker_server_failed_install_rolls_back_before_launch) {
    with_worker([&](const ruvia::worker_handle& worker_value, asio::io_context& io) -> ruvia::task<void> {
        ruvia::test::rejecting_memory_resource upstream;
        // Scheduler startup storage exceeds the pool's small-block cache.
        fixture fixture_value(worker_value, io, upstream, 4096);
        std::array<state_type, 4096> states;
        std::array<state_type*, 4096> views{};
        for (std::size_t index = 0; index < states.size(); ++index) {
            views[index] = &states[index];
        }
        RUVIA_CHECK(!fixture_value.server_.stage_install({.request_buffer_ = &fixture_value.requests_}));
        RUVIA_CHECK(fixture_value.stage(views));
        upstream.reject_allocations();
        RUVIA_CHECK(!fixture_value.server_.install());
        upstream.reject_allocations(false);
        RUVIA_CHECK(!fixture_value.server_.installed());
        fixture_value.server_.abandon_before_launch();
        RUVIA_CHECK(fixture_value.server_.drained());
        RUVIA_CHECK(fixture_value.requests_.stopped());
        RUVIA_CHECK(fixture_value.server_.response_buffer().stopped());
        for (auto& state : states) {
            RUVIA_CHECK(state.ready_to_destroy());
            RUVIA_CHECK(!state.available_identity());
        }
        RUVIA_CHECK_EQ(fixture_value.active_.load(), std::size_t{0});
        fixture_value.server_.abandon_before_launch();
        co_return;
    });
}

RUVIA_TEST(http3_worker_server_failed_task_spawn_rolls_back_installed_local_state) {
    ruvia::test::counting_memory_resource upstream;
    with_worker([&](const ruvia::worker_handle& worker_value, asio::io_context& io) -> ruvia::task<void> {
        fixture fixture(worker_value, io, upstream);
        state_type state_value(fixture.state_wake());
        const std::array states{&state_value};
        RUVIA_CHECK(fixture.stage(states));
        RUVIA_CHECK(fixture.server_.install());
        ruvia::test::rejecting_memory_resource scope_resource;
        ruvia::task_scope scope(worker_value, {.resource_ = &scope_resource});
        scope_resource.reject_allocations();
        bool failed = false;
        try {
            scope.spawn(fixture.server_.run());
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        scope_resource.reject_allocations(false);
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(scope.size(), std::size_t{0});
        fixture.server_.abandon_before_launch();
        RUVIA_CHECK(fixture.server_.drained());
        RUVIA_CHECK(fixture.requests_.stopped());
        RUVIA_CHECK(fixture.server_.response_buffer().stopped());
        RUVIA_CHECK(state_value.ready_to_destroy());
        RUVIA_CHECK(!state_value.available_identity());
        RUVIA_CHECK_EQ(fixture.active_.load(), std::size_t{0});
        co_await scope.join();
    });
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}
