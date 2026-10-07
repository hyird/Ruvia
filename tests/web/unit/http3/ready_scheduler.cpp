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

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/CallbackRef.h"
#include "ruvia/web/detail/http3/Http3ServerConnection.h"
#include "ruvia/web/detail/http3/http3_ready_scheduler.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

#include "memory_resource_fixture.h"
#include "routing_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using connection_type = ruvia::detail::Http3ServerConnection;
using connection_config = ruvia::detail::Http3ServerConnectionConfig;
using scheduler_type = ruvia::detail::http3_ready_scheduler;
using stream_buffer = ruvia::detail::http3_stream_buffer;
using stream_control = ruvia::detail::http3_stream_control;
using stream_id = ruvia::detail::http3_stream_id;
using namespace std::chrono_literals;

constexpr std::uint64_t base_epoch = 109;
constexpr std::uint64_t base_generation = 211;

struct route_state final {
    std::string large_body = std::string(48 * 1024, 'x');
};

ruvia::Task<ruvia::HttpResponse> scheduler_handler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<route_state*>(raw);
    if (context.req().path() == "/large") {
        co_return context.text(std::string_view(state.large_body));
    }
    co_return context.text(context.req().path());
}

struct route_table_fixture final {
    route_state state;
    ruvia::detail::Router router;
    ruvia::detail::RouterImpl& implementation{ruvia::detail::RouterImpl::from(router)};

    route_table_fixture() {
        add(ruvia::HttpKnownMethod::kGet, "/small");
        add(ruvia::HttpKnownMethod::kGet, "/large");
        add(ruvia::HttpKnownMethod::kGet, "/deadline");
        add(ruvia::HttpKnownMethod::kHead, "/head");
        implementation.finalize();
    }

    void add(ruvia::HttpKnownMethod method, std::string_view path) {
        implementation.registerRoute(method, routing_test::path(path),
            ruvia::detail::RouteHandler(&state, &scheduler_handler),
            ruvia::detail::RequestBodyMode::kBuffered,
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
    }
};

struct fixture final {
    route_table_fixture routes;
    ruvia::WorkerMemory worker;
    ruvia::StopSource stop_source;
    ruvia::StopToken stop_token;
    ruvia::detail::ContextServices services;
    ruvia::detail::HttpServerOptions options;

    fixture(const ruvia::WorkerHandle& worker_handle,
        std::pmr::memory_resource& upstream)
        : worker(upstream),
          stop_token(stop_source.token()),
          services(worker_handle, stop_token) {}
};

bool accepted(stream_buffer::send_result result) noexcept {
    return result == stream_buffer::send_result::sent;
}

bool accepted(stream_buffer::control_result result) noexcept {
    return result == stream_buffer::control_result::sent;
}

std::string frame(std::uint64_t type, std::string_view payload) {
    std::array<char, ruvia::kHttp3FrameHeaderMaxBytes> header{};
    const auto size = ruvia::encodeHttp3FrameHeader(header, type, payload.size());
    if (!size) {
        throw std::runtime_error("HTTP/3 scheduler test frame encoding failed");
    }
    std::string wire(header.data(), *size);
    wire.append(payload);
    return wire;
}

std::string request_wire(fixture& fixture, std::string_view method, std::string_view path) {
    const auto encoded = ruvia::encodeHttp3ClientRequestHead({.method = method,
                                                                 .scheme = "https",
                                                                 .authority = "example.test",
                                                                 .path = path},
        {}, fixture.worker.resource());
    if (!encoded) {
        throw std::runtime_error("HTTP/3 scheduler request encoding failed");
    }
    return frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(encoded->fieldSection.data(), encoded->fieldSection.size()));
}

connection_type::EventResult feed_request(connection_type& owner, stream_buffer& inbound,
    fixture& fixture, stream_id id, std::string_view method, std::string_view path) {
    const auto wire = request_wire(fixture, method, path);
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!accepted(inbound.try_send(id, bytes)) ||
        !accepted(inbound.try_send_control(
            {stream_control::kind::stream_fin, id, static_cast<std::uint64_t>(wire.size())}))) {
        throw std::runtime_error("HTTP/3 scheduler input mailbox unexpectedly full");
    }
    stream_buffer::borrowed_block block;
    if (!inbound.try_receive(block)) {
        throw std::runtime_error("HTTP/3 scheduler input block was not published");
    }
    const auto result = owner.acceptData(block);
    block.release();
    stream_control fin;
    if (!inbound.try_receive_control(fin)) {
        throw std::runtime_error("HTTP/3 scheduler input FIN was not published");
    }
    const auto fin_result = owner.acceptControl(fin);
    if (result.status != connection_type::EventStatus::kAccepted) {
        return result;
    }
    return fin_result;
}

connection_type::EventResult feed_malformed_headers(connection_type& owner, stream_buffer& inbound,
    fixture& fixture, stream_id id) {
    const std::array<ruvia::Http3FieldSectionFieldView, 4> fields{{{":method", "GET"}, {":scheme", "https"}, {"x-before-path", "bad"}, {":path", "/"}}};
    const auto section = ruvia::encodeHttp3FieldSection(fields, fixture.worker.resource());
    if (!section) {
        throw std::runtime_error("HTTP/3 malformed scheduler request encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(section->data(), section->size()));
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!accepted(inbound.try_send(id, bytes))) {
        throw std::runtime_error("HTTP/3 malformed scheduler request mailbox full");
    }
    stream_buffer::borrowed_block block;
    if (!inbound.try_receive(block)) {
        throw std::runtime_error("HTTP/3 malformed scheduler block was not published");
    }
    const auto result = owner.acceptData(block);
    block.release();
    return result;
}

ruvia::Task<void> wait_for_ready(std::span<connection_type* const> connections,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stop_token) {
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        bool ready = true;
        for (const auto* connection : connections) {
            ready = ready && connection != nullptr && connection->readyRequestCount() != 0;
        }
        if (ready) {
            co_return;
        }
        if (co_await ruvia::sleepFor(worker, 1ms, stop_token) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    throw std::runtime_error("HTTP/3 scheduler requests did not become publishable");
}

ruvia::Task<void> wait_for_no_tasks(std::span<connection_type* const> connections,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stop_token) {
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        bool idle = true;
        for (const auto* connection : connections) {
            idle = idle && connection != nullptr && connection->activeTaskCount() == 0;
        }
        if (idle) {
            co_return;
        }
        if (co_await ruvia::sleepFor(worker, 1ms, stop_token) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    throw std::runtime_error("HTTP/3 scheduler request tasks did not retire");
}

ruvia::Task<void> stop_and_retire(scheduler_type& scheduler, scheduler_type::connection_token token,
    connection_type& connection, ruvia::testing::TestContext& ruvia_ctx);

void drain_buffer(stream_buffer& mailbox, unsigned& data_blocks, unsigned& controls) {
    stream_buffer::borrowed_block block;
    while (mailbox.try_receive(block)) {
        ++data_blocks;
        block.release();
    }
    stream_control control;
    while (mailbox.try_receive_control(control)) {
        ++controls;
    }
}

ruvia::Task<void> stop_and_retire(scheduler_type& scheduler, scheduler_type::connection_token token,
    connection_type& connection, ruvia::testing::TestContext& ruvia_ctx) {
    RUVIA_CHECK(connection.requestStop());
    RUVIA_CHECK(scheduler.begin_retirement(token));
    std::optional<scheduler_type::step_result> close;
    for (std::size_t turn = 0; turn < 1024; ++turn) {
        const auto step = scheduler.step();
        if (step.kind == scheduler_type::step_kind::idle) {
            break;
        }
        if (step.kind != scheduler_type::step_kind::transport_intent) {
            continue;
        }
        if (step.connection == token &&
            step.intent.token.kind == connection_type::TransportIntentKind::kConnectionClose) {
            close = step;
            break;
        }
        if (!scheduler.acknowledge_intent(step.connection, step.intent.token)) {
            throw std::runtime_error("scheduler intent handoff could not be settled");
        }
    }
    RUVIA_CHECK(close.has_value());
    if (!close) {
        throw std::runtime_error("HTTP/3 close intent was not offered during retirement");
    }
    co_await connection.join();
    RUVIA_CHECK(connection.takeOverTransportRetirement(
        {.epoch = token.epoch, .connectionGeneration = token.connection_generation}));
    RUVIA_CHECK(scheduler.acknowledge_intent(token, close->intent.token));
    while (connection.pendingTransportIntentCount() != 0) {
        const auto pending = scheduler.step();
        if (pending.kind != scheduler_type::step_kind::transport_intent ||
            !scheduler.acknowledge_intent(token, pending.intent.token)) {
            throw std::runtime_error("retirement intent debt could not be settled");
        }
    }
    RUVIA_CHECK(scheduler.retire(token));
}

ruvia::Task<void> exercise_retirement_waits_for_intent_acknowledgement(
    fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    scheduler_type scheduler(worker, 1, fixture.worker.resource());
    stream_buffer outbound(2, 2, 2, fixture.worker.resource());
    const auto registration = scheduler.reserve(base_epoch, base_generation);
    RUVIA_CHECK(registration.has_value());
    if (!registration) {
        throw std::runtime_error("scheduler retirement fixture slot unavailable");
    }
    connection_type owner(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, registration->activation,
        connection_config{.epoch = base_epoch,
            .connectionGeneration = base_generation,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(scheduler.attach(registration->token, owner));
    RUVIA_CHECK(owner.requestStop());
    const auto close = scheduler.step();
    RUVIA_CHECK(close.kind == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(close.intent.token.kind == connection_type::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(scheduler.begin_retirement(registration->token));
    co_await owner.join();
    RUVIA_CHECK(owner.confirmTransportRetired({.epoch = registration->token.epoch,
        .connectionGeneration = registration->token.connection_generation}));
    RUVIA_CHECK(!scheduler.retire(registration->token));
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token, close.intent.token));
    RUVIA_CHECK(scheduler.retire(registration->token));
    RUVIA_CHECK(outbound.stop());
    static_cast<void>(ruvia_ctx);
}

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

void run_worker_task(ruvia::EventLoopAttachment& attachment, ruvia::Task<void> operation) {
    auto root = attachment.loop().start(stop_after(attachment, std::move(operation)));
    attachment.run();
}

ruvia::Task<void> exercise_cross_connection_lane_rotation(fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    constexpr std::size_t head_connections = 6;
    constexpr std::size_t connections = head_connections + 1;
    scheduler_type scheduler(worker, connections, fixture.worker.resource());
    stream_buffer outbound(32, 32, 16, fixture.worker.resource());
    stream_buffer inbound(2, 2, 2, fixture.worker.resource());
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
        tokens[index] = registration->token;
        owners[index].emplace(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.services, fixture.options, outbound, registration->activation,
            connection_config{.epoch = epoch,
                .connectionGeneration = generation,
                .maxTrackedStreams = 8});
        RUVIA_CHECK(scheduler.attach(tokens[index], *owners[index]));
        owner_refs[index] = &*owners[index];
        const auto request = index < head_connections
                                 ? feed_request(*owners[index], inbound, fixture,
                                       {epoch, generation, 0}, "HEAD", "/head")
                                 : feed_request(*owners[index], inbound, fixture,
                                       {epoch, generation, 0}, "GET", "/large");
        RUVIA_CHECK(request.status == connection_type::EventStatus::kDispatched);
    }
    co_await wait_for_ready(owner_refs, worker, fixture.stop_token);

    unsigned data_blocks = 0;
    unsigned controls = 0;
    std::size_t consecutive_control_turns = 0;
    std::size_t max_consecutive_control_turns = 0;
    bool data_served_while_control_pending = false;
    bool large_served = false;
    std::size_t turns = 0;
    for (; turns < 256; ++turns) {
        const auto step = scheduler.step();
        if (step.kind == scheduler_type::step_kind::idle) {
            break;
        }
        if (step.kind == scheduler_type::step_kind::wrong_worker) {
            RUVIA_CHECK(false);
            break;
        }
        if (step.kind != scheduler_type::step_kind::publication) {
            continue;
        }
        if (step.connection == tokens.back() &&
            step.publication.publication.status ==
                connection_type::Dispatch::PublishStatus::kBytesPublished) {
            large_served = true;
        }
        if (step.publication.publication.status ==
            connection_type::Dispatch::PublishStatus::kFinPublished) {
            ++consecutive_control_turns;
            max_consecutive_control_turns =
                std::max(max_consecutive_control_turns, consecutive_control_turns);
        } else {
            consecutive_control_turns = 0;
            const auto lanes = scheduler.snapshot();
            data_served_while_control_pending = data_served_while_control_pending ||
                                                (step.publication.publication.status ==
                                                        connection_type::Dispatch::PublishStatus::kBytesPublished &&
                                                    lanes.runnable[0] != 0 && lanes.runnable[1] != 0);
        }
        drain_buffer(outbound, data_blocks, controls);
    }
    RUVIA_CHECK(turns < 256);
    RUVIA_CHECK(large_served);
    RUVIA_CHECK(max_consecutive_control_turns <= scheduler_type::default_control_burst_limit);
    RUVIA_CHECK(data_served_while_control_pending);
    RUVIA_CHECK(data_blocks > head_connections);
    RUVIA_CHECK(controls >= head_connections);
    co_await wait_for_no_tasks(owner_refs, worker, fixture.stop_token);

    for (std::size_t index = 0; index < connections; ++index) {
        co_await stop_and_retire(scheduler, tokens[index], *owners[index], ruvia_ctx);
        owners[index].reset();
    }
    RUVIA_CHECK(outbound.stop());
}

ruvia::Task<void> exercise_local_deadline_without_capacity_notification(
    fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    fixture.options.deadline = ruvia::DeadlineConfig{.handler = 60ms};
    scheduler_type scheduler(worker, 1, fixture.worker.resource());
    stream_buffer outbound(2, 1, 1, fixture.worker.resource());
    stream_buffer inbound(1, 1, 1, fixture.worker.resource());
    const auto registration = scheduler.reserve(base_epoch + 50, base_generation + 50);
    RUVIA_CHECK(registration.has_value());
    if (!registration) {
        throw std::runtime_error("scheduler local deadline slot unavailable");
    }
    std::optional<connection_type> owner;
    owner.emplace(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, registration->activation,
        connection_config{.epoch = base_epoch + 50,
            .connectionGeneration = base_generation + 50,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(scheduler.attach(registration->token, *owner));
    const std::array<std::byte, 1> filler{std::byte{0x23}};
    RUVIA_CHECK(accepted(outbound.try_send(
        {base_epoch + 80, base_generation + 80, 0}, filler)));
    const auto request = feed_request(*owner, inbound, fixture,
        {base_epoch + 50, base_generation + 50, 0}, "GET", "/deadline");
    RUVIA_CHECK(request.status == connection_type::EventStatus::kDispatched);
    std::array<connection_type*, 1> references{&*owner};
    co_await wait_for_ready(references, worker, fixture.stop_token);

    const auto backpressure = scheduler.step();
    RUVIA_CHECK(backpressure.kind == scheduler_type::step_kind::publication);
    RUVIA_CHECK(backpressure.publication.publication.status ==
                connection_type::Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(backpressure.publication.publication.blockReason ==
                connection_type::Dispatch::PublishBlockReason::kData);
    RUVIA_CHECK(scheduler.step().kind == scheduler_type::step_kind::idle);

    bool localReady = false;
    for (std::size_t attempt = 0; attempt < 1000; ++attempt) {
        const auto state = scheduler.snapshot();
        if (state.runnable[2] != 0) {
            localReady = true;
            break;
        }
        if (co_await ruvia::sleepFor(worker, 1ms, fixture.stop_token) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    RUVIA_CHECK(localReady);
    const auto cancelled = scheduler.step();
    RUVIA_CHECK(cancelled.kind == scheduler_type::step_kind::publication);
    RUVIA_CHECK(cancelled.publication.publication.status ==
                connection_type::Dispatch::PublishStatus::kCancelled);
    const auto reset = scheduler.step();
    RUVIA_CHECK(reset.kind == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(reset.intent.token.kind == connection_type::TransportIntentKind::kStreamReset);
    RUVIA_CHECK(reset.intent.streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kRequestCancelled);
    RUVIA_CHECK(owner->requestStop());
    RUVIA_CHECK(scheduler.step().kind == scheduler_type::step_kind::idle);
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token, reset.intent.token));
    const auto close = scheduler.step();
    RUVIA_CHECK(close.kind == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(close.intent.token.kind == connection_type::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(scheduler.begin_retirement(registration->token));
    co_await owner->join();
    RUVIA_CHECK(owner->takeOverTransportRetirement(
        {.epoch = registration->token.epoch,
            .connectionGeneration = registration->token.connection_generation}));
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token, close.intent.token));
    RUVIA_CHECK(scheduler.retire(registration->token));
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

ruvia::Task<void> exercise_stale_slot_activation_after_join(
    fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    scheduler_type scheduler(worker, 1, fixture.worker.resource());
    stream_buffer outbound(2, 2, 2, fixture.worker.resource());
    const auto old_registration = scheduler.reserve(base_epoch + 60, base_generation + 60);
    RUVIA_CHECK(old_registration.has_value());
    if (!old_registration) {
        throw std::runtime_error("scheduler ABA fixture slot unavailable");
    }
    std::optional<connection_type> old_owner;
    old_owner.emplace(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, old_registration->activation,
        connection_config{.epoch = base_epoch + 60,
            .connectionGeneration = base_generation + 60,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(scheduler.attach(old_registration->token, *old_owner));
    RUVIA_CHECK(!scheduler.reserve(base_epoch + 61, base_generation + 61).has_value());
    RUVIA_CHECK(old_owner->requestStop());
    const auto old_close = scheduler.step();
    RUVIA_CHECK(old_close.kind == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(scheduler.begin_retirement(old_registration->token));
    co_await old_owner->join();
    RUVIA_CHECK(old_owner->takeOverTransportRetirement(
        {.epoch = old_registration->token.epoch,
            .connectionGeneration = old_registration->token.connection_generation}));
    RUVIA_CHECK(scheduler.acknowledge_intent(old_registration->token, old_close.intent.token));
    RUVIA_CHECK(scheduler.retire(old_registration->token));
    old_owner.reset();

    const auto current_registration = scheduler.reserve(base_epoch + 62, base_generation + 62);
    RUVIA_CHECK(current_registration.has_value());
    if (!current_registration) {
        throw std::runtime_error("scheduler did not return a retired slot");
    }
    RUVIA_CHECK_EQ(current_registration->token.slot, old_registration->token.slot);
    RUVIA_CHECK(current_registration->token.slot_generation !=
                old_registration->token.slot_generation);
    std::optional<connection_type> current_owner;
    current_owner.emplace(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, current_registration->activation,
        connection_config{.epoch = base_epoch + 62,
            .connectionGeneration = base_generation + 62,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(scheduler.attach(current_registration->token, *current_owner));

    connection_type::WorkerActivation stale{
        .work = {.runnable = {.data = true}}};
    old_registration->activation.activate(old_registration->activation.context,
        old_registration->token.epoch, old_registration->token.connection_generation,
        old_registration->token.slot_generation, stale);
    const auto state = scheduler.snapshot();
    RUVIA_CHECK_EQ(state.runnable[0], std::size_t{0});
    RUVIA_CHECK_EQ(state.attached_connections, std::size_t{1});

    RUVIA_CHECK(current_owner->requestStop());
    const auto current_close = scheduler.step();
    RUVIA_CHECK(current_close.kind == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(scheduler.begin_retirement(current_registration->token));
    co_await current_owner->join();
    RUVIA_CHECK(current_owner->takeOverTransportRetirement(
        {.epoch = current_registration->token.epoch,
            .connectionGeneration = current_registration->token.connection_generation}));
    RUVIA_CHECK(scheduler.acknowledge_intent(
        current_registration->token, current_close.intent.token));
    RUVIA_CHECK(scheduler.retire(current_registration->token));
    current_owner.reset();
    RUVIA_CHECK(outbound.stop());
}

ruvia::Task<void> exercise_control_burst_one_preserves_intent_turn(fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    scheduler_type scheduler(worker, 4, fixture.worker.resource(), {}, 1);
    stream_buffer outbound(16, 16, 16, fixture.worker.resource());
    stream_buffer inbound(1, 1, 1, fixture.worker.resource());
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
        owners[i].emplace(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.services, fixture.options, outbound, registration->activation,
            connection_config{.epoch = epoch,
                .connectionGeneration = generation,
                .maxTrackedStreams = 8});
        RUVIA_CHECK(scheduler.attach(registration->token, *owners[i]));
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& token = registrations[i].token;
        const auto request = feed_request(*owners[i], inbound, fixture,
            {token.epoch, token.connection_generation, 0}, i == 2 ? "GET" : "HEAD",
            i == 2 ? "/large" : "/head");
        RUVIA_CHECK(request.status == connection_type::EventStatus::kDispatched);
    }
    std::array<connection_type*, 3> ready{&*owners[0], &*owners[1], &*owners[2]};
    co_await wait_for_ready(ready, worker, fixture.stop_token);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto head = owners[i]->publishOne({.data = true});
        RUVIA_CHECK(head.publication.status ==
                    connection_type::Dispatch::PublishStatus::kBytesPublished);
        const auto& registration = registrations[i];
        registration.activation.activate(registration.activation.context,
            registration.token.epoch, registration.token.connection_generation,
            registration.token.slot_generation, {.work = owners[i]->workState()});
    }
    const auto& error_token = registrations[3].token;
    const auto malformed = feed_malformed_headers(*owners[3], inbound, fixture,
        {error_token.epoch, error_token.connection_generation, 0});
    RUVIA_CHECK(malformed.status == connection_type::EventStatus::kProtocolError);
    const auto initially = scheduler.snapshot();
    RUVIA_CHECK(initially.runnable[0] != 0);
    RUVIA_CHECK(initially.runnable[1] >= 2);
    RUVIA_CHECK(initially.runnable[3] != 0);
    const auto data = scheduler.step();
    const auto control = scheduler.step();
    const auto forced_data = scheduler.step();
    RUVIA_CHECK(data.kind == scheduler_type::step_kind::publication);
    RUVIA_CHECK(control.kind == scheduler_type::step_kind::publication);
    RUVIA_CHECK(forced_data.kind == scheduler_type::step_kind::publication);
    RUVIA_CHECK(scheduler.snapshot().runnable[1] != 0);
    const auto intent = scheduler.step();
    RUVIA_CHECK(intent.kind == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(intent.intent.token.kind == connection_type::TransportIntentKind::kStreamReset);
    RUVIA_CHECK(intent.connection == error_token);
    RUVIA_CHECK(scheduler.acknowledge_intent(intent.connection, intent.intent.token));

    for (std::size_t i = 0; i < owners.size(); ++i) {
        co_await stop_and_retire(scheduler, registrations[i].token, *owners[i], ruvia_ctx);
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
    std::size_t signals{};
    static void ready(void* context) noexcept {
        ++static_cast<ready_probe*>(context)->signals;
    }
};

ruvia::Task<void> exercise_local_capacity_recovery(fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    ready_probe probe;
    scheduler_type scheduler(worker, 4, fixture.worker.resource(), {&probe, &ready_probe::ready});
    stream_buffer outbound(4, 1, 1, fixture.worker.resource());
    stream_buffer inbound(1, 1, 1, fixture.worker.resource());
    std::array<std::optional<connection_type>, 4> owners;
    std::array<scheduler_type::registration, 4> registrations{};
    for (std::size_t index = 0; index < owners.size(); ++index) {
        const auto registration = scheduler.reserve(base_epoch + 300 + index, base_generation + 300 + index);
        if (!registration) {
            throw std::runtime_error("local capacity recovery slot unavailable");
        }
        registrations[index] = *registration;
        owners[index].emplace(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.services, fixture.options, outbound, registration->activation,
            connection_config{.epoch = registration->token.epoch,
                .connectionGeneration = registration->token.connection_generation,
                .maxTrackedStreams = 8});
        RUVIA_CHECK(scheduler.attach(registration->token, *owners[index]));
    }
    const auto id = [&](std::size_t index) {
        const auto& token = registrations[index].token;
        return stream_id{token.epoch, token.connection_generation, 0};
    };
    const std::array<std::byte, 1> filler{std::byte{0x42}};
    const stream_id filler_id{base_epoch + 399, base_generation + 399, 0};
    RUVIA_CHECK(accepted(outbound.try_send_control({stream_control::kind::writable, filler_id, 0})));

    // Put a HEAD response on the CONTROL blocked list without occupying DATA.
    RUVIA_CHECK(feed_request(*owners[2], inbound, fixture, id(2), "HEAD", "/head").status ==
                connection_type::EventStatus::kDispatched);
    std::array<connection_type*, 1> head_ready{&*owners[2]};
    co_await wait_for_ready(head_ready, worker, fixture.stop_token);
    RUVIA_CHECK(scheduler.step().publication.publication.status ==
                connection_type::Dispatch::PublishStatus::kBytesPublished);
    stream_buffer::borrowed_block block;
    RUVIA_CHECK(outbound.try_receive(block));
    block.release();
    RUVIA_CHECK(scheduler.step().publication.publication.blockReason ==
                connection_type::Dispatch::PublishBlockReason::kControl);
    RUVIA_CHECK(scheduler.step().kind == scheduler_type::step_kind::idle);

    RUVIA_CHECK(accepted(outbound.try_send(filler_id, filler)));
    for (std::size_t index = 0; index < 2; ++index) {
        RUVIA_CHECK(feed_request(*owners[index], inbound, fixture, id(index), "GET", "/small").status ==
                    connection_type::EventStatus::kDispatched);
    }
    std::array<connection_type*, 2> data_ready{&*owners[0], &*owners[1]};
    co_await wait_for_ready(data_ready, worker, fixture.stop_token);
    RUVIA_CHECK(probe.signals != 0);
    RUVIA_CHECK(feed_malformed_headers(*owners[3], inbound, fixture, id(3)).status ==
                connection_type::EventStatus::kProtocolError);
    std::size_t data_attempts = 0;
    std::size_t reset_intents = 0;
    for (std::size_t turn = 0; turn < 8; ++turn) {
        const auto step = scheduler.step();
        if (step.kind == scheduler_type::step_kind::idle) {
            break;
        }
        if (step.kind == scheduler_type::step_kind::transport_intent) {
            RUVIA_CHECK(step.connection == registrations[3].token);
            RUVIA_CHECK(step.intent.token.kind == connection_type::TransportIntentKind::kStreamReset);
            RUVIA_CHECK(step.intent.streamResetErrorCode == ruvia::Http3ConnectionErrorCode::kMessageError);
            // Local execution is independent of response CONTROL capacity.
            RUVIA_CHECK(scheduler.acknowledge_intent(step.connection, step.intent.token));
            RUVIA_CHECK(!scheduler.acknowledge_intent(step.connection, step.intent.token));
            ++reset_intents;
        } else if (step.kind == scheduler_type::step_kind::publication) {
            RUVIA_CHECK(step.publication.publication.blockReason == connection_type::Dispatch::PublishBlockReason::kData);
            ++data_attempts;
        }
    }
    RUVIA_CHECK_EQ(data_attempts, std::size_t{2});
    RUVIA_CHECK_EQ(reset_intents, std::size_t{1});
    RUVIA_CHECK_EQ(scheduler.snapshot().blocked[0], std::size_t{2});
    RUVIA_CHECK_EQ(scheduler.snapshot().blocked[1], std::size_t{1});

    // A capacity event with no actual space is still finite and fair. Each
    // blocked DATA owner gets one attempt, never the blocked CONTROL owner.
    const auto signals_before_capacity = probe.signals;
    scheduler.receive_capacity(scheduler_type::capacity_data);
    RUVIA_CHECK(probe.signals > signals_before_capacity);
    std::array<std::size_t, 2> failed_attempts{};
    for (std::size_t turn = 0; turn < 8; ++turn) {
        const auto step = scheduler.step();
        if (step.kind == scheduler_type::step_kind::idle) {
            break;
        }
        RUVIA_CHECK(step.kind == scheduler_type::step_kind::publication);
        RUVIA_CHECK(step.publication.publication.blockReason == connection_type::Dispatch::PublishBlockReason::kData);
        for (std::size_t index = 0; index < failed_attempts.size(); ++index) {
            if (step.connection == registrations[index].token) {
                ++failed_attempts[index];
            }
        }
    }
    RUVIA_CHECK_EQ(failed_attempts[0], std::size_t{1});
    RUVIA_CHECK_EQ(failed_attempts[1], std::size_t{1});
    RUVIA_CHECK(scheduler.step().kind == scheduler_type::step_kind::idle);
    RUVIA_CHECK_EQ(scheduler.snapshot().capacity_pass_lanes, std::uint8_t{0});

    // Returning a borrow permits one connection to publish, not all blocked
    // owners. The second stays blocked until the next explicit capacity event.
    RUVIA_CHECK(outbound.try_receive(block));
    block.release();
    scheduler.receive_capacity(scheduler_type::capacity_data);
    bool first_owner_published = false;
    bool second_owner_blocked = false;
    for (std::size_t turn = 0; turn < 8; ++turn) {
        const auto step = scheduler.step();
        if (step.kind == scheduler_type::step_kind::idle) {
            break;
        }
        if (step.kind != scheduler_type::step_kind::publication) {
            continue;
        }
        if (step.publication.publication.status == connection_type::Dispatch::PublishStatus::kBytesPublished) {
            first_owner_published |= step.connection == registrations[0].token;
        }
        if (step.publication.publication.blockReason == connection_type::Dispatch::PublishBlockReason::kData) {
            second_owner_blocked |= step.connection == registrations[1].token;
        }
    }
    RUVIA_CHECK(first_owner_published);
    RUVIA_CHECK(second_owner_blocked);
    RUVIA_CHECK(scheduler.snapshot().blocked[1] != 0);

    unsigned data_blocks = 0;
    unsigned controls = 0;
    // Drain both independent lanes and drive each finite recovery to idle.
    for (std::size_t pass = 0; pass < 32; ++pass) {
        drain_buffer(outbound, data_blocks, controls);
        scheduler.receive_capacity(scheduler_type::capacity_all);
        for (std::size_t turn = 0; turn < 16; ++turn) {
            const auto step = scheduler.step();
            if (step.kind == scheduler_type::step_kind::idle) {
                break;
            }
            if (step.kind == scheduler_type::step_kind::transport_intent) {
                RUVIA_CHECK(scheduler.acknowledge_intent(step.connection, step.intent.token));
            }
        }
        const auto state = scheduler.snapshot();
        if (state.blocked[0] == 0 && state.blocked[1] == 0 && !outbound.has_pending()) {
            break;
        }
    }
    RUVIA_CHECK_EQ(scheduler.snapshot().blocked[0], std::size_t{0});
    RUVIA_CHECK_EQ(scheduler.snapshot().blocked[1], std::size_t{0});
    RUVIA_CHECK(controls >= 3);
    std::array<connection_type*, 4> all_owners{&*owners[0], &*owners[1], &*owners[2], &*owners[3]};
    co_await wait_for_no_tasks(all_owners, worker, fixture.stop_token);
    for (std::size_t index = 0; index < owners.size(); ++index) {
        co_await stop_and_retire(scheduler, registrations[index].token, *owners[index], ruvia_ctx);
        owners[index].reset();
    }
    drain_buffer(outbound, data_blocks, controls);
    RUVIA_CHECK(outbound.stop());
}

}  // namespace
RUVIA_TEST(http3_ready_scheduler_waits_for_intent_ack_before_retirement) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        fixture fixture(worker, upstream);
        run_worker_task(attachment,
            exercise_retirement_waits_for_intent_acknowledgement(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3_ready_scheduler_rotates_connections_and_publication_lanes) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        fixture fixture(worker, upstream);
        run_worker_task(attachment,
            exercise_cross_connection_lane_rotation(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
RUVIA_TEST(http3_ready_scheduler_control_burst_one_preserves_intent_turn) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        fixture fixture(worker, upstream);
        run_worker_task(attachment,
            exercise_control_burst_one_preserves_intent_turn(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3_ready_scheduler_keeps_local_deadline_activation_without_capacity_notification) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        fixture fixture(worker, upstream);
        run_worker_task(attachment,
            exercise_local_deadline_without_capacity_notification(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3_ready_scheduler_rejects_stale_activation_after_joined_slot_reuse) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        fixture fixture(worker, upstream);
        run_worker_task(attachment,
            exercise_stale_slot_activation_after_join(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3_ready_scheduler_recovers_blocked_publications_from_local_capacity) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        fixture fixture(worker, upstream);
        run_worker_task(attachment, exercise_local_capacity_recovery(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
