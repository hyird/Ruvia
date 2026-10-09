#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/callback_ref.h"

#include "http3/http3_ready_scheduler.h"
#include "http3/http3_server_connection.h"
#include "memory_resource_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "routing_fixture.h"
#include "server/http_server_options.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using connection_type = ruvia::detail::http3_server_connection;
using connection_config = ruvia::detail::http3_server_connection_config;
using scheduler_type = ruvia::detail::http3_ready_scheduler;
using stream_buffer = ruvia::detail::http3_stream_buffer;
using stream_control = ruvia::detail::http3_stream_control;
using stream_id = ruvia::detail::http3_stream_id;
using namespace std::chrono_literals;

constexpr std::uint64_t base_epoch = 109;
constexpr std::uint64_t base_generation = 211;

struct route_state final {
    std::string large_body_ = std::string(48 * 1024, 'x');
};

ruvia::task<ruvia::http_response> scheduler_handler(void* raw, ruvia::context& context_value) {
    auto& state_value = *static_cast<route_state*>(raw);
    if (context_value.req().path() == "/large") {
        co_return context_value.text(std::string_view(state_value.large_body_));
    }
    co_return context_value.text(context_value.req().path());
}

struct route_table_fixture final {
    route_state state_;
    ruvia::detail::router router_;
    ruvia::detail::router_impl& implementation_{ruvia::detail::router_impl::from(router_)};

    route_table_fixture() {
        add(ruvia::http_known_method::get, "/small");
        add(ruvia::http_known_method::get, "/large");
        add(ruvia::http_known_method::get, "/deadline");
        add(ruvia::http_known_method::head, "/head");
        implementation_.finalize();
    }

    void add(ruvia::http_known_method method, std::string_view path) {
        implementation_.register_route(method, routing_test::path(path),
            ruvia::detail::route_handler_type(&state_, &scheduler_handler),
            ruvia::detail::request_body_mode::buffered,
            std::span<const ruvia::detail::controller_middleware_descriptor>{},
            std::span<const ruvia::detail::controller_middleware_descriptor>{});
    }
};

struct fixture_type final {
    route_table_fixture routes_;
    ruvia::worker_memory worker_;
    ruvia::stop_source stop_source_;
    ruvia::stop_token stop_token_;
    ruvia::detail::context_services services_;
    ruvia::detail::http_server_options options_;

    fixture_type(const ruvia::worker_handle& worker_handle_value,
        std::pmr::memory_resource& upstream)
        : worker_(upstream),
          stop_token_(stop_source_.token()),
          services_(worker_handle_value, stop_token_) {}
};

bool accepted(stream_buffer::send_result result_value) noexcept {
    return result_value == stream_buffer::send_result::sent;
}

bool accepted(stream_buffer::control_result result_value) noexcept {
    return result_value == stream_buffer::control_result::sent;
}

std::string frame(std::uint64_t type, std::string_view payload_value) {
    std::array<char, ruvia::http3_frame_header_max_bytes> header_value{};
    const auto size = ruvia::encode_http3_frame_header(header_value, type, payload_value.size());
    if ((size.index() != 0)) {
        throw std::runtime_error("HTTP/3 scheduler test frame encoding failed");
    }
    std::string wire(header_value.data(), std::get<0>(size));
    wire.append(payload_value);
    return wire;
}

std::string request_wire(fixture_type& fixture_value, std::string_view method, std::string_view path) {
    const auto encoded = ruvia::encode_http3_client_request_head({.method_ = method,
                                                                     .scheme_ = "https",
                                                                     .authority_ = "example.test",
                                                                     .path_ = path},
        {}, fixture_value.worker_.resource());
    if ((encoded.index() != 0)) {
        throw std::runtime_error("HTTP/3 scheduler request encoding failed");
    }
    return frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(encoded).field_section_.data(), std::get<0>(encoded).field_section_.size()));
}

connection_type::event_result_type feed_request(connection_type& owner_value, stream_buffer& inbound,
    fixture_type& fixture_value, stream_id id, std::string_view method, std::string_view path) {
    const auto wire = request_wire(fixture_value, method, path);
    const auto bytes_value = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!accepted(inbound.try_send(id, bytes_value)) ||
        !accepted(inbound.try_send_control(
            {stream_control::kind::stream_fin, id, static_cast<std::uint64_t>(wire.size())}))) {
        throw std::runtime_error("HTTP/3 scheduler input buffer unexpectedly full");
    }
    stream_buffer::borrowed_block block;
    if (!inbound.try_receive(block)) {
        throw std::runtime_error("HTTP/3 scheduler input block was not published");
    }
    const auto result_value = owner_value.accept_data(block);
    block.release();
    stream_control fin;
    if (!inbound.try_receive_control(fin)) {
        throw std::runtime_error("HTTP/3 scheduler input FIN was not published");
    }
    const auto fin_result = owner_value.accept_control(fin);
    if (result_value.status_ != connection_type::event_status_type::accepted) {
        return result_value;
    }
    return fin_result;
}

connection_type::event_result_type feed_malformed_headers(connection_type& owner_value, stream_buffer& inbound,
    fixture_type& fixture_value, stream_id id) {
    const std::array<ruvia::http3_field_section_field_view, 4> fields_value{{{":method", "GET"}, {":scheme", "https"}, {"x-before-path", "bad"}, {":path", "/"}}};
    const auto section = ruvia::encode_http3_field_section(fields_value, fixture_value.worker_.resource());
    if ((section.index() != 0)) {
        throw std::runtime_error("HTTP/3 malformed scheduler request encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(section).data(), std::get<0>(section).size()));
    const auto bytes_value = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!accepted(inbound.try_send(id, bytes_value))) {
        throw std::runtime_error("HTTP/3 malformed scheduler request buffer full");
    }
    stream_buffer::borrowed_block block;
    if (!inbound.try_receive(block)) {
        throw std::runtime_error("HTTP/3 malformed scheduler block was not published");
    }
    const auto result_value = owner_value.accept_data(block);
    block.release();
    return result_value;
}

ruvia::task<void> wait_for_ready(std::span<connection_type* const> connections,
    const ruvia::worker_handle& worker_value, const ruvia::stop_token& stop_token_value) {
    for (std::size_t attempt_value = 0; attempt_value < 2000; ++attempt_value) {
        bool ready = true;
        for (const auto* connection : connections) {
            ready = ready && connection != nullptr && connection->ready_request_count() != 0;
        }
        if (ready) {
            co_return;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms, stop_token_value) !=
            ruvia::timer_sleep_result::elapsed) {
            break;
        }
    }
    throw std::runtime_error("HTTP/3 scheduler requests did not become publishable");
}

ruvia::task<void> wait_for_no_tasks(std::span<connection_type* const> connections,
    const ruvia::worker_handle& worker_value, const ruvia::stop_token& stop_token_value) {
    for (std::size_t attempt_value = 0; attempt_value < 2000; ++attempt_value) {
        bool idle = true;
        for (const auto* connection : connections) {
            idle = idle && connection != nullptr && connection->active_task_count() == 0;
        }
        if (idle) {
            co_return;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms, stop_token_value) !=
            ruvia::timer_sleep_result::elapsed) {
            break;
        }
    }
    throw std::runtime_error("HTTP/3 scheduler request tasks did not retire");
}

ruvia::task<void> stop_and_retire(scheduler_type& scheduler, scheduler_type::connection_token token,
    connection_type& connection, ruvia::testing::test_context& ruvia_ctx);

void drain_buffer(stream_buffer& buffer, unsigned& data_blocks, unsigned& controls) {
    stream_buffer::borrowed_block block;
    while (buffer.try_receive(block)) {
        ++data_blocks;
        block.release();
    }
    stream_control control;
    while (buffer.try_receive_control(control)) {
        ++controls;
    }
}

ruvia::task<void> stop_and_retire(scheduler_type& scheduler, scheduler_type::connection_token token,
    connection_type& connection, ruvia::testing::test_context& ruvia_ctx) {
    RUVIA_CHECK(connection.request_stop());
    RUVIA_CHECK(scheduler.begin_retirement(token));
    std::optional<scheduler_type::step_result> close;
    for (std::size_t turn = 0; turn < 1024; ++turn) {
        const auto step = scheduler.step();
        if (step.kind_ == scheduler_type::step_kind::idle) {
            break;
        }
        if (step.kind_ != scheduler_type::step_kind::transport_intent) {
            continue;
        }
        if (step.connection_ == token &&
            step.intent_.token_.kind_ == connection_type::transport_intent_kind_type::connection_close) {
            close = step;
            break;
        }
        if (!scheduler.acknowledge_intent(step.connection_, step.intent_.token_)) {
            throw std::runtime_error("scheduler intent handoff could not be settled");
        }
    }
    RUVIA_CHECK(close.has_value());
    if (!close) {
        throw std::runtime_error("HTTP/3 close intent was not offered during retirement");
    }
    co_await connection.join();
    RUVIA_CHECK(connection.take_over_transport_retirement(
        {.epoch_ = token.epoch_, .connection_generation_ = token.connection_generation_}));
    RUVIA_CHECK(scheduler.acknowledge_intent(token, close->intent_.token_));
    while (connection.pending_transport_intent_count() != 0) {
        const auto pending = scheduler.step();
        if (pending.kind_ != scheduler_type::step_kind::transport_intent ||
            !scheduler.acknowledge_intent(token, pending.intent_.token_)) {
            throw std::runtime_error("retirement intent debt could not be settled");
        }
    }
    RUVIA_CHECK(scheduler.retire(token));
}

ruvia::task<void> exercise_retirement_waits_for_intent_acknowledgement(
    fixture_type& fixture_value, const ruvia::worker_handle& worker_value,
    ruvia::testing::test_context& ruvia_ctx) {
    scheduler_type scheduler(worker_value, 1, fixture_value.worker_.resource());
    stream_buffer outbound(2, 2, 2, fixture_value.worker_.resource());
    const auto registration = scheduler.reserve(base_epoch, base_generation);
    RUVIA_CHECK(registration.has_value());
    if (!registration) {
        throw std::runtime_error("scheduler retirement fixture slot unavailable");
    }
    connection_type owner_value(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, registration->activation_,
        connection_config{.epoch_ = base_epoch,
            .connection_generation_ = base_generation,
            .max_tracked_streams_ = 8});
    RUVIA_CHECK(scheduler.attach(registration->token_, owner_value));
    RUVIA_CHECK(owner_value.request_stop());
    const auto close = scheduler.step();
    RUVIA_CHECK(close.kind_ == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(close.intent_.token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
    RUVIA_CHECK(scheduler.begin_retirement(registration->token_));
    co_await owner_value.join();
    RUVIA_CHECK(owner_value.confirm_transport_retired({.epoch_ = registration->token_.epoch_,
        .connection_generation_ = registration->token_.connection_generation_}));
    RUVIA_CHECK(!scheduler.retire(registration->token_));
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token_, close.intent_.token_));
    RUVIA_CHECK(scheduler.retire(registration->token_));
    RUVIA_CHECK(outbound.stop());
    static_cast<void>(ruvia_ctx);
}

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

void run_worker_task(ruvia::event_loop_attachment& attachment, ruvia::task<void> operation) {
    auto root = attachment.loop().start(stop_after(attachment, std::move(operation)));
    attachment.run();
}

ruvia::task<void> exercise_cross_connection_lane_rotation(fixture_type& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    constexpr std::size_t head_connections = 6;
    constexpr std::size_t connections = head_connections + 1;
    scheduler_type scheduler(worker_value, connections, fixture_value.worker_.resource());
    stream_buffer outbound(32, 32, 16, fixture_value.worker_.resource());
    stream_buffer inbound(2, 2, 2, fixture_value.worker_.resource());
    std::array<std::optional<connection_type>, connections> owners;
    std::array<scheduler_type::connection_token, connections> tokens{};
    std::array<connection_type*, connections> owner_refs{};

    for (std::size_t index = 0; index < connections; ++index) {
        const auto epoch = base_epoch + index;
        const auto generation = base_generation + index;
        const auto registration = scheduler.reserve(epoch, generation);
        RUVIA_CHECK(registration.has_value());
        if (!registration) {
            throw std::runtime_error("scheduler connection slots unexpectedly exhausted");
        }
        tokens[index] = registration->token_;
        owners[index].emplace(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
            fixture_value.services_, fixture_value.options_, outbound, registration->activation_,
            connection_config{.epoch_ = epoch,
                .connection_generation_ = generation,
                .max_tracked_streams_ = 8});
        RUVIA_CHECK(scheduler.attach(tokens[index], *owners[index]));
        owner_refs[index] = &*owners[index];
        const auto request = index < head_connections
                                 ? feed_request(*owners[index], inbound, fixture_value,
                                       {epoch, generation, 0}, "HEAD", "/head")
                                 : feed_request(*owners[index], inbound, fixture_value,
                                       {epoch, generation, 0}, "GET", "/large");
        RUVIA_CHECK(request.status_ == connection_type::event_status_type::dispatched);
    }
    co_await wait_for_ready(owner_refs, worker_value, fixture_value.stop_token_);

    unsigned data_blocks = 0;
    unsigned controls = 0;
    std::size_t consecutive_control_turns = 0;
    std::size_t max_consecutive_control_turns = 0;
    bool data_served_while_control_pending = false;
    bool large_served = false;
    std::size_t turns = 0;
    for (; turns < 256; ++turns) {
        const auto step = scheduler.step();
        if (step.kind_ == scheduler_type::step_kind::idle) {
            break;
        }
        if (step.kind_ == scheduler_type::step_kind::wrong_worker) {
            RUVIA_CHECK(false);
            break;
        }
        if (step.kind_ != scheduler_type::step_kind::publication) {
            continue;
        }
        if (step.connection_ == tokens.back() &&
            step.publication_.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published) {
            large_served = true;
        }
        if (step.publication_.publication_.status_ ==
            connection_type::dispatch_type::publish_status_type::fin_published) {
            ++consecutive_control_turns;
            max_consecutive_control_turns =
                std::max(max_consecutive_control_turns, consecutive_control_turns);
        } else {
            consecutive_control_turns = 0;
            const auto lanes = scheduler.snapshot();
            data_served_while_control_pending = data_served_while_control_pending ||
                                                (step.publication_.publication_.status_ ==
                                                        connection_type::dispatch_type::publish_status_type::bytes_published &&
                                                    lanes.runnable_[0] != 0 && lanes.runnable_[1] != 0);
        }
        drain_buffer(outbound, data_blocks, controls);
    }
    RUVIA_CHECK(turns < 256);
    RUVIA_CHECK(large_served);
    RUVIA_CHECK(max_consecutive_control_turns <= scheduler_type::default_control_burst_limit);
    RUVIA_CHECK(data_served_while_control_pending);
    RUVIA_CHECK(data_blocks > head_connections);
    RUVIA_CHECK(controls >= head_connections);
    co_await wait_for_no_tasks(owner_refs, worker_value, fixture_value.stop_token_);

    for (std::size_t index = 0; index < connections; ++index) {
        co_await stop_and_retire(scheduler, tokens[index], *owners[index], ruvia_ctx);
        owners[index].reset();
    }
    RUVIA_CHECK(outbound.stop());
}

ruvia::task<void> exercise_local_deadline_without_capacity_notification(
    fixture_type& fixture_value, const ruvia::worker_handle& worker_value,
    ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.options_.deadline_ = ruvia::deadline_config{.handler_ = 60ms};
    scheduler_type scheduler(worker_value, 1, fixture_value.worker_.resource());
    stream_buffer outbound(2, 1, 1, fixture_value.worker_.resource());
    stream_buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    const auto registration = scheduler.reserve(base_epoch + 50, base_generation + 50);
    RUVIA_CHECK(registration.has_value());
    if (!registration) {
        throw std::runtime_error("scheduler local deadline slot unavailable");
    }
    std::optional<connection_type> owner;
    owner.emplace(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, registration->activation_,
        connection_config{.epoch_ = base_epoch + 50,
            .connection_generation_ = base_generation + 50,
            .max_tracked_streams_ = 8});
    RUVIA_CHECK(scheduler.attach(registration->token_, *owner));
    const std::array<std::byte, 1> filler{std::byte{0x23}};
    RUVIA_CHECK(accepted(outbound.try_send(
        {base_epoch + 80, base_generation + 80, 0}, filler)));
    const auto request = feed_request(*owner, inbound, fixture_value,
        {base_epoch + 50, base_generation + 50, 0}, "GET", "/deadline");
    RUVIA_CHECK(request.status_ == connection_type::event_status_type::dispatched);
    std::array<connection_type*, 1> references{&*owner};
    co_await wait_for_ready(references, worker_value, fixture_value.stop_token_);

    const auto backpressure = scheduler.step();
    RUVIA_CHECK(backpressure.kind_ == scheduler_type::step_kind::publication);
    RUVIA_CHECK(backpressure.publication_.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(backpressure.publication_.publication_.block_reason_ ==
                connection_type::dispatch_type::publish_block_reason_type::data);
    RUVIA_CHECK(scheduler.step().kind_ == scheduler_type::step_kind::idle);

    bool local_ready = false;
    for (std::size_t attempt_value = 0; attempt_value < 1000; ++attempt_value) {
        const auto state_value = scheduler.snapshot();
        if (state_value.runnable_[2] != 0) {
            local_ready = true;
            break;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms, fixture_value.stop_token_) !=
            ruvia::timer_sleep_result::elapsed) {
            break;
        }
    }
    RUVIA_CHECK(local_ready);
    const auto cancelled = scheduler.step();
    RUVIA_CHECK(cancelled.kind_ == scheduler_type::step_kind::publication);
    RUVIA_CHECK(cancelled.publication_.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::cancelled);
    const auto reset = scheduler.step();
    RUVIA_CHECK(reset.kind_ == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(reset.intent_.token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
    RUVIA_CHECK(reset.intent_.stream_reset_error_code_ ==
                ruvia::http3_connection_error_code::request_cancelled);
    RUVIA_CHECK(owner->request_stop());
    RUVIA_CHECK(scheduler.step().kind_ == scheduler_type::step_kind::idle);
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token_, reset.intent_.token_));
    const auto close = scheduler.step();
    RUVIA_CHECK(close.kind_ == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(close.intent_.token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
    RUVIA_CHECK(scheduler.begin_retirement(registration->token_));
    co_await owner->join();
    RUVIA_CHECK(owner->take_over_transport_retirement(
        {.epoch_ = registration->token_.epoch_,
            .connection_generation_ = registration->token_.connection_generation_}));
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token_, close.intent_.token_));
    RUVIA_CHECK(scheduler.retire(registration->token_));
    owner.reset();
    stream_control unused;
    while (outbound.try_receive_control(unused)) {
    }
    stream_buffer::borrowed_block block;
    while (outbound.try_receive(block)) {
        block.release();
    }
    RUVIA_CHECK(outbound.stop());
}

ruvia::task<void> exercise_stale_slot_activation_after_join(
    fixture_type& fixture_value, const ruvia::worker_handle& worker_value,
    ruvia::testing::test_context& ruvia_ctx) {
    scheduler_type scheduler(worker_value, 1, fixture_value.worker_.resource());
    stream_buffer outbound(2, 2, 2, fixture_value.worker_.resource());
    const auto old_registration = scheduler.reserve(base_epoch + 60, base_generation + 60);
    RUVIA_CHECK(old_registration.has_value());
    if (!old_registration) {
        throw std::runtime_error("scheduler ABA fixture slot unavailable");
    }
    std::optional<connection_type> old_owner;
    old_owner.emplace(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, old_registration->activation_,
        connection_config{.epoch_ = base_epoch + 60,
            .connection_generation_ = base_generation + 60,
            .max_tracked_streams_ = 8});
    RUVIA_CHECK(scheduler.attach(old_registration->token_, *old_owner));
    RUVIA_CHECK(!scheduler.reserve(base_epoch + 61, base_generation + 61).has_value());
    RUVIA_CHECK(old_owner->request_stop());
    const auto old_close = scheduler.step();
    RUVIA_CHECK(old_close.kind_ == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(scheduler.begin_retirement(old_registration->token_));
    co_await old_owner->join();
    RUVIA_CHECK(old_owner->take_over_transport_retirement(
        {.epoch_ = old_registration->token_.epoch_,
            .connection_generation_ = old_registration->token_.connection_generation_}));
    RUVIA_CHECK(scheduler.acknowledge_intent(old_registration->token_, old_close.intent_.token_));
    RUVIA_CHECK(scheduler.retire(old_registration->token_));
    old_owner.reset();

    const auto current_registration = scheduler.reserve(base_epoch + 62, base_generation + 62);
    RUVIA_CHECK(current_registration.has_value());
    if (!current_registration) {
        throw std::runtime_error("scheduler did not return a retired slot");
    }
    RUVIA_CHECK_EQ(current_registration->token_.slot_, old_registration->token_.slot_);
    RUVIA_CHECK(current_registration->token_.slot_generation_ !=
                old_registration->token_.slot_generation_);
    std::optional<connection_type> current_owner;
    current_owner.emplace(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, current_registration->activation_,
        connection_config{.epoch_ = base_epoch + 62,
            .connection_generation_ = base_generation + 62,
            .max_tracked_streams_ = 8});
    RUVIA_CHECK(scheduler.attach(current_registration->token_, *current_owner));

    connection_type::worker_activation_type stale{
        .work_ = {.runnable_ = {.data_ = true}}};
    old_registration->activation_.activate_(old_registration->activation_.context_,
        old_registration->token_.epoch_, old_registration->token_.connection_generation_,
        old_registration->token_.slot_generation_, stale);
    const auto state_value = scheduler.snapshot();
    RUVIA_CHECK_EQ(state_value.runnable_[0], std::size_t{0});
    RUVIA_CHECK_EQ(state_value.attached_connections_, std::size_t{1});

    RUVIA_CHECK(current_owner->request_stop());
    const auto current_close = scheduler.step();
    RUVIA_CHECK(current_close.kind_ == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(scheduler.begin_retirement(current_registration->token_));
    co_await current_owner->join();
    RUVIA_CHECK(current_owner->take_over_transport_retirement(
        {.epoch_ = current_registration->token_.epoch_,
            .connection_generation_ = current_registration->token_.connection_generation_}));
    RUVIA_CHECK(scheduler.acknowledge_intent(
        current_registration->token_, current_close.intent_.token_));
    RUVIA_CHECK(scheduler.retire(current_registration->token_));
    current_owner.reset();
    RUVIA_CHECK(outbound.stop());
}

ruvia::task<void> exercise_control_burst_one_preserves_intent_turn(fixture_type& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    scheduler_type scheduler(worker_value, 4, fixture_value.worker_.resource(), {}, 1);
    stream_buffer outbound(16, 16, 16, fixture_value.worker_.resource());
    stream_buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    std::array<std::optional<connection_type>, 4> owners;
    std::array<scheduler_type::registration, 4> registrations{};
    for (std::size_t i = 0; i < owners.size(); ++i) {
        const auto epoch = base_epoch + 150 + i;
        const auto generation = base_generation + 150 + i;
        const auto registration = scheduler.reserve(epoch, generation);
        if (!registration) {
            throw std::runtime_error("scheduler control-burst slot unavailable");
        }
        registrations[i] = *registration;
        owners[i].emplace(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
            fixture_value.services_, fixture_value.options_, outbound, registration->activation_,
            connection_config{.epoch_ = epoch,
                .connection_generation_ = generation,
                .max_tracked_streams_ = 8});
        RUVIA_CHECK(scheduler.attach(registration->token_, *owners[i]));
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& token = registrations[i].token_;
        const auto request = feed_request(*owners[i], inbound, fixture_value,
            {token.epoch_, token.connection_generation_, 0}, i == 2 ? "GET" : "HEAD",
            i == 2 ? "/large" : "/head");
        RUVIA_CHECK(request.status_ == connection_type::event_status_type::dispatched);
    }
    std::array<connection_type*, 3> ready{&*owners[0], &*owners[1], &*owners[2]};
    co_await wait_for_ready(ready, worker_value, fixture_value.stop_token_);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto head = owners[i]->publish_one({.data_ = true});
        RUVIA_CHECK(head.publication_.status_ ==
                    connection_type::dispatch_type::publish_status_type::bytes_published);
        const auto& registration = registrations[i];
        registration.activation_.activate_(registration.activation_.context_,
            registration.token_.epoch_, registration.token_.connection_generation_,
            registration.token_.slot_generation_, {.work_ = owners[i]->work_state()});
    }
    const auto& error_token = registrations[3].token_;
    const auto malformed = feed_malformed_headers(*owners[3], inbound, fixture_value,
        {error_token.epoch_, error_token.connection_generation_, 0});
    RUVIA_CHECK(malformed.status_ == connection_type::event_status_type::protocol_error);
    const auto initially = scheduler.snapshot();
    RUVIA_CHECK(initially.runnable_[0] != 0);
    RUVIA_CHECK(initially.runnable_[1] >= 2);
    RUVIA_CHECK(initially.runnable_[3] != 0);
    const auto data = scheduler.step();
    const auto control = scheduler.step();
    const auto forced_data = scheduler.step();
    RUVIA_CHECK(data.kind_ == scheduler_type::step_kind::publication);
    RUVIA_CHECK(control.kind_ == scheduler_type::step_kind::publication);
    RUVIA_CHECK(forced_data.kind_ == scheduler_type::step_kind::publication);
    RUVIA_CHECK(scheduler.snapshot().runnable_[1] != 0);
    const auto intent = scheduler.step();
    RUVIA_CHECK(intent.kind_ == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(intent.intent_.token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
    RUVIA_CHECK(intent.connection_ == error_token);
    RUVIA_CHECK(scheduler.acknowledge_intent(intent.connection_, intent.intent_.token_));

    for (std::size_t i = 0; i < owners.size(); ++i) {
        co_await stop_and_retire(scheduler, registrations[i].token_, *owners[i], ruvia_ctx);
        owners[i].reset();
    }
    stream_control control_event;
    while (outbound.try_receive_control(control_event)) {
    }
    stream_buffer::borrowed_block block;
    while (outbound.try_receive(block)) {
        block.release();
    }
    RUVIA_CHECK(!outbound.has_pending());
    RUVIA_CHECK(outbound.stop());
}

struct ready_probe final {
    std::size_t signals_{};
    static void ready(void* context_value) noexcept {
        ++static_cast<ready_probe*>(context_value)->signals_;
    }
};

ruvia::task<void> exercise_local_capacity_recovery(fixture_type& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    ready_probe probe;
    scheduler_type scheduler(worker_value, 4, fixture_value.worker_.resource(), {&probe, &ready_probe::ready});
    stream_buffer outbound(4, 1, 1, fixture_value.worker_.resource());
    stream_buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    std::array<std::optional<connection_type>, 4> owners;
    std::array<scheduler_type::registration, 4> registrations{};
    for (std::size_t index = 0; index < owners.size(); ++index) {
        const auto registration = scheduler.reserve(base_epoch + 300 + index, base_generation + 300 + index);
        if (!registration) {
            throw std::runtime_error("local capacity recovery slot unavailable");
        }
        registrations[index] = *registration;
        owners[index].emplace(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
            fixture_value.services_, fixture_value.options_, outbound, registration->activation_,
            connection_config{.epoch_ = registration->token_.epoch_,
                .connection_generation_ = registration->token_.connection_generation_,
                .max_tracked_streams_ = 8});
        RUVIA_CHECK(scheduler.attach(registration->token_, *owners[index]));
    }
    const auto id = [&](std::size_t index) {
        const auto& token = registrations[index].token_;
        return stream_id{token.epoch_, token.connection_generation_, 0};
    };
    const std::array<std::byte, 1> filler{std::byte{0x42}};
    const stream_id filler_id{base_epoch + 399, base_generation + 399, 0};
    RUVIA_CHECK(accepted(outbound.try_send_control({stream_control::kind::writable, filler_id, 0})));

    // Put a HEAD response on the CONTROL blocked list without occupying DATA.
    RUVIA_CHECK(feed_request(*owners[2], inbound, fixture_value, id(2), "HEAD", "/head").status_ ==
                connection_type::event_status_type::dispatched);
    std::array<connection_type*, 1> head_ready{&*owners[2]};
    co_await wait_for_ready(head_ready, worker_value, fixture_value.stop_token_);
    RUVIA_CHECK(scheduler.step().publication_.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);
    stream_buffer::borrowed_block block;
    RUVIA_CHECK(outbound.try_receive(block));
    block.release();
    RUVIA_CHECK(scheduler.step().publication_.publication_.block_reason_ ==
                connection_type::dispatch_type::publish_block_reason_type::control);
    RUVIA_CHECK(scheduler.step().kind_ == scheduler_type::step_kind::idle);

    RUVIA_CHECK(accepted(outbound.try_send(filler_id, filler)));
    for (std::size_t index = 0; index < 2; ++index) {
        RUVIA_CHECK(feed_request(*owners[index], inbound, fixture_value, id(index), "GET", "/small").status_ ==
                    connection_type::event_status_type::dispatched);
    }
    std::array<connection_type*, 2> data_ready{&*owners[0], &*owners[1]};
    co_await wait_for_ready(data_ready, worker_value, fixture_value.stop_token_);
    RUVIA_CHECK(probe.signals_ != 0);
    RUVIA_CHECK(feed_malformed_headers(*owners[3], inbound, fixture_value, id(3)).status_ ==
                connection_type::event_status_type::protocol_error);
    std::size_t data_attempts = 0;
    std::size_t reset_intents = 0;
    for (std::size_t turn = 0; turn < 8; ++turn) {
        const auto step = scheduler.step();
        if (step.kind_ == scheduler_type::step_kind::idle) {
            break;
        }
        if (step.kind_ == scheduler_type::step_kind::transport_intent) {
            RUVIA_CHECK(step.connection_ == registrations[3].token_);
            RUVIA_CHECK(step.intent_.token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
            RUVIA_CHECK(step.intent_.stream_reset_error_code_ == ruvia::http3_connection_error_code::message_error);
            // Local execution is independent of response CONTROL capacity.
            RUVIA_CHECK(scheduler.acknowledge_intent(step.connection_, step.intent_.token_));
            RUVIA_CHECK(!scheduler.acknowledge_intent(step.connection_, step.intent_.token_));
            ++reset_intents;
        } else if (step.kind_ == scheduler_type::step_kind::publication) {
            RUVIA_CHECK(step.publication_.publication_.block_reason_ == connection_type::dispatch_type::publish_block_reason_type::data);
            ++data_attempts;
        }
    }
    RUVIA_CHECK_EQ(data_attempts, std::size_t{2});
    RUVIA_CHECK_EQ(reset_intents, std::size_t{1});
    RUVIA_CHECK_EQ(scheduler.snapshot().blocked_[0], std::size_t{2});
    RUVIA_CHECK_EQ(scheduler.snapshot().blocked_[1], std::size_t{1});

    // A capacity event with no actual space is still finite and fair. Each
    // blocked DATA owner gets one attempt, never the blocked CONTROL owner.
    const auto signals_before_capacity = probe.signals_;
    scheduler.receive_capacity(scheduler_type::capacity_data);
    RUVIA_CHECK(probe.signals_ > signals_before_capacity);
    std::array<std::size_t, 2> failed_attempts{};
    for (std::size_t turn = 0; turn < 8; ++turn) {
        const auto step = scheduler.step();
        if (step.kind_ == scheduler_type::step_kind::idle) {
            break;
        }
        RUVIA_CHECK(step.kind_ == scheduler_type::step_kind::publication);
        RUVIA_CHECK(step.publication_.publication_.block_reason_ == connection_type::dispatch_type::publish_block_reason_type::data);
        for (std::size_t index = 0; index < failed_attempts.size(); ++index) {
            if (step.connection_ == registrations[index].token_) {
                ++failed_attempts[index];
            }
        }
    }
    RUVIA_CHECK_EQ(failed_attempts[0], std::size_t{1});
    RUVIA_CHECK_EQ(failed_attempts[1], std::size_t{1});
    RUVIA_CHECK(scheduler.step().kind_ == scheduler_type::step_kind::idle);
    RUVIA_CHECK_EQ(scheduler.snapshot().capacity_pass_lanes_, std::uint8_t{0});

    // Returning a borrow permits one connection to publish, not all blocked
    // owners. The second stays blocked until the next explicit capacity event.
    RUVIA_CHECK(outbound.try_receive(block));
    block.release();
    scheduler.receive_capacity(scheduler_type::capacity_data);
    bool first_owner_published = false;
    bool second_owner_blocked = false;
    for (std::size_t turn = 0; turn < 8; ++turn) {
        const auto step = scheduler.step();
        if (step.kind_ == scheduler_type::step_kind::idle) {
            break;
        }
        if (step.kind_ != scheduler_type::step_kind::publication) {
            continue;
        }
        if (step.publication_.publication_.status_ == connection_type::dispatch_type::publish_status_type::bytes_published) {
            first_owner_published |= step.connection_ == registrations[0].token_;
        }
        if (step.publication_.publication_.block_reason_ == connection_type::dispatch_type::publish_block_reason_type::data) {
            second_owner_blocked |= step.connection_ == registrations[1].token_;
        }
    }
    RUVIA_CHECK(first_owner_published);
    RUVIA_CHECK(second_owner_blocked);
    RUVIA_CHECK(scheduler.snapshot().blocked_[1] != 0);

    unsigned data_blocks = 0;
    unsigned controls = 0;
    // Drain both independent lanes and drive each finite recovery to idle.
    for (std::size_t pass = 0; pass < 32; ++pass) {
        drain_buffer(outbound, data_blocks, controls);
        scheduler.receive_capacity(scheduler_type::capacity_all);
        for (std::size_t turn = 0; turn < 16; ++turn) {
            const auto step = scheduler.step();
            if (step.kind_ == scheduler_type::step_kind::idle) {
                break;
            }
            if (step.kind_ == scheduler_type::step_kind::transport_intent) {
                RUVIA_CHECK(scheduler.acknowledge_intent(step.connection_, step.intent_.token_));
            }
        }
        const auto state_value = scheduler.snapshot();
        if (state_value.blocked_[0] == 0 && state_value.blocked_[1] == 0 && !outbound.has_pending()) {
            break;
        }
    }
    RUVIA_CHECK_EQ(scheduler.snapshot().blocked_[0], std::size_t{0});
    RUVIA_CHECK_EQ(scheduler.snapshot().blocked_[1], std::size_t{0});
    RUVIA_CHECK(controls >= 3);
    std::array<connection_type*, 4> all_owners{&*owners[0], &*owners[1], &*owners[2], &*owners[3]};
    co_await wait_for_no_tasks(all_owners, worker_value, fixture_value.stop_token_);
    for (std::size_t index = 0; index < owners.size(); ++index) {
        co_await stop_and_retire(scheduler, registrations[index].token_, *owners[index], ruvia_ctx);
        owners[index].reset();
    }
    drain_buffer(outbound, data_blocks, controls);
    RUVIA_CHECK(outbound.stop());
}

}  // namespace
RUVIA_TEST(http3_ready_scheduler_waits_for_intent_ack_before_retirement) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture_type fixture_value(worker_value, upstream);
        run_worker_task(attachment,
            exercise_retirement_waits_for_intent_acknowledgement(fixture_value, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_ready_scheduler_rotates_connections_and_publication_lanes) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture_type fixture_value(worker_value, upstream);
        run_worker_task(attachment,
            exercise_cross_connection_lane_rotation(fixture_value, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}
RUVIA_TEST(http3_ready_scheduler_control_burst_one_preserves_intent_turn) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture_type fixture_value(worker_value, upstream);
        run_worker_task(attachment,
            exercise_control_burst_one_preserves_intent_turn(fixture_value, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_ready_scheduler_keeps_local_deadline_activation_without_capacity_notification) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture_type fixture_value(worker_value, upstream);
        run_worker_task(attachment,
            exercise_local_deadline_without_capacity_notification(fixture_value, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_ready_scheduler_rejects_stale_activation_after_joined_slot_reuse) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture_type fixture_value(worker_value, upstream);
        run_worker_task(attachment,
            exercise_stale_slot_activation_after_join(fixture_value, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_ready_scheduler_recovers_blocked_publications_from_local_capacity) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture_type fixture_value(worker_value, upstream);
        run_worker_task(attachment, exercise_local_capacity_recovery(fixture_value, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}
