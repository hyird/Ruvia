#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <utility>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/Context.h"

#include "http3/http3_connection_state.h"
#include "memory_resource_fixture.h"
#include "router/Router.h"
#include "router/RouterImpl.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using state_type = ruvia::detail::http3_connection_state;
using identity_type = ruvia::detail::http3_connection_identity;
using connection_type = ruvia::detail::Http3ServerConnection;
using scheduler_type = ruvia::detail::http3_ready_scheduler;
using status = state_type::status;
using outcome = state_type::execution_outcome;

struct fixture final {
    ruvia::detail::Router router;
    ruvia::detail::RouterImpl& routes{ruvia::detail::RouterImpl::from(router)};
    ruvia::test::CountingMemoryResource& resource;
    ruvia::WorkerMemory memory;
    ruvia::StopSource stop_source;
    ruvia::StopToken stop_token;
    ruvia::detail::ContextServices services;
    ruvia::detail::HttpServerOptions options;
    ruvia::detail::http3_stream_buffer outbound;
    scheduler_type scheduler;
    std::size_t changes{};
    state_type state;
    std::optional<connection_type> connection;
    std::array<connection_type::TransportIntent, 16> executed{};
    std::size_t executions{};
    state_type::intent_execution_result execution_result{.outcome = outcome::executed};
    bool retire_during_execution{};

    fixture(const ruvia::WorkerHandle& worker, ruvia::test::CountingMemoryResource& resource)
        : resource(resource),
          memory(resource),
          stop_token(stop_source.token()),
          services(worker, stop_token),
          outbound(8, 8, 8, memory.resource()),
          scheduler(worker, 1, memory.resource()),
          state({this, changed}, &resource) {
        routes.finalize();
        state.set_transport_executor({this, execute});
    }

    static void changed(void* context) noexcept {
        ++static_cast<fixture*>(context)->changes;
    }

    static state_type::intent_execution_result execute(void* context, identity_type identity, const connection_type::TransportIntent& intent) noexcept {
        auto& self = *static_cast<fixture*>(context);
        self.executed[self.executions++] = intent;
        if (self.retire_during_execution) {
            (void)self.state.mark_transport_retired(identity);
        }
        return self.execution_result;
    }

    identity_type reserve(std::uint64_t epoch = 41, std::uint64_t generation = 9) {
        if (state.reserve(scheduler, epoch, generation) != status::changed) {
            std::terminate();
        }
        return *state.identity();
    }

    void attach(identity_type identity) {
        if (state.bind(identity, {.remote_address = "127.0.0.1", .client_certificate_subject = "peer", .remote_port = 443}, {.enableConnectProtocol = true}, 1200) != status::changed) {
            std::terminate();
        }
        const auto registration = *state.registration();
        connection.emplace(routes.routeTable(), memory, services, options, outbound, registration.activation,
            ruvia::detail::Http3ServerConnectionConfig{.epoch = identity.epoch, .connectionGeneration = identity.connection_generation, .maxTrackedStreams = 8});
        if (state.attach_handler(identity, *connection) != status::changed) {
            std::terminate();
        }
    }

    ruvia::Task<void> finish(identity_type identity, bool transport_first = true) {
        if (!connection->stopped()) {
            (void)connection->requestStop();
        }
        if (state.start_worker_draining(identity) != status::changed) {
            std::terminate();
        }
        if (transport_first && !state.transport_retired() && state.mark_transport_retired(identity) != status::changed) {
            std::terminate();
        }
        for (;;) {
            const auto step = scheduler.step();
            if (step.kind == scheduler_type::step_kind::idle) {
                break;
            }
            if (step.kind == scheduler_type::step_kind::transport_intent) {
                const auto result = state.execute_intent(identity, step.intent);
                if (!result.completed() || !scheduler.acknowledge_intent(step.connection, step.intent.token, result.push_stream)) {
                    std::terminate();
                }
            }
        }
        co_await connection->join();
        if (state.mark_worker_finalized(identity) != status::changed) {
            std::terminate();
        }
        if (!state.transport_retired() && state.mark_transport_retired(identity) != status::changed) {
            std::terminate();
        }
        if (state.retire(identity) != status::changed) {
            std::terminate();
        }
        connection.reset();
        if (!outbound.stop()) {
            std::terminate();
        }
    }
};

connection_type::TransportIntent reset_intent(identity_type identity, std::uint64_t stream_id, std::uint64_t sequence, ruvia::Http3ConnectionErrorCode code) {
    return {.token = {.kind = connection_type::TransportIntentKind::kStreamReset,
                .id = {.epoch = identity.epoch, .connection_generation = identity.connection_generation, .stream_id = stream_id},
                .sequence = sequence},
        .streamResetErrorCode = code};
}

connection_type::TransportIntent push_intent(identity_type identity, std::uint64_t push_id, std::uint64_t sequence) {
    return {.token = {.kind = connection_type::TransportIntentKind::kOpenPushStream,
                .id = {.epoch = identity.epoch, .connection_generation = identity.connection_generation, .stream_id = 0, .push_id = push_id},
                .sequence = sequence}};
}

ruvia::Task<void> admission_and_generation(fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    const auto identity = fixture.reserve();
    const auto token = fixture.state.registration()->token;
    RUVIA_CHECK(fixture.state.available_identity() == identity);
    RUVIA_CHECK(fixture.state.available_identity() == identity);
    RUVIA_CHECK(fixture.state.admission() == state_type::admission_phase::reserved);
    RUVIA_CHECK(fixture.scheduler.snapshot().free_connections == 0);
    RUVIA_CHECK(fixture.state.reject(identity, state_type::reject_reason::capacity) == status::wrong_state);
    fixture.attach(identity);
    RUVIA_CHECK(!fixture.state.available_identity());
    RUVIA_CHECK(fixture.state.admission() == state_type::admission_phase::handler_attached);
    RUVIA_CHECK(fixture.scheduler.snapshot().attached_connections == 1);
    const auto binding = *fixture.state.binding();
    RUVIA_CHECK(binding.identity == identity);
    RUVIA_CHECK(binding.metadata.remote_address == "127.0.0.1");
    RUVIA_CHECK(binding.metadata.client_certificate_subject == "peer");
    RUVIA_CHECK(binding.metadata.remote_port == 443);
    RUVIA_CHECK(binding.settings.enableConnectProtocol);
    RUVIA_CHECK(binding.max_quic_datagram_payload_bytes == 1200);
    RUVIA_CHECK(fixture.state.attach_handler(identity, *fixture.connection) == status::wrong_state);
    RUVIA_CHECK(fixture.state.reject(identity, state_type::reject_reason::stopping) == status::wrong_state);
    RUVIA_CHECK(fixture.state.reset() == status::wrong_state);
    RUVIA_CHECK(fixture.state.mark_worker_finalized(identity) == status::wrong_state);
    RUVIA_CHECK(fixture.state.seal_admission(identity, 0, 8) == status::changed);
    RUVIA_CHECK(fixture.state.admission_seal()->identity == identity);
    RUVIA_CHECK(fixture.state.admission_seal()->expected_admitted_requests == 0);
    RUVIA_CHECK(fixture.state.admission_seal()->goaway_id == 8);
    RUVIA_CHECK(fixture.state.seal_admission(identity, 1, 12) == status::wrong_state);
    RUVIA_CHECK(fixture.state.mark_worker_drained(identity) == status::changed);
    RUVIA_CHECK(fixture.state.worker_drained());
    RUVIA_CHECK(!fixture.state.slot_reusable());
    co_await fixture.finish(identity, false);
    RUVIA_CHECK(fixture.state.worker_finalized());
    RUVIA_CHECK(fixture.state.transport_retired());
    RUVIA_CHECK(fixture.state.slot_reusable());
    RUVIA_CHECK(fixture.state.ready_to_destroy());
    RUVIA_CHECK(fixture.state.reset() == status::changed);
    RUVIA_CHECK(fixture.state.reserve(fixture.scheduler, identity.epoch, identity.connection_generation) == status::stale);
    RUVIA_CHECK(fixture.state.reserve(fixture.scheduler, identity.epoch - 1, 100) == status::stale);
    const auto next = fixture.reserve(identity.epoch + 1, 1);
    RUVIA_CHECK(fixture.state.registration()->token.slot_generation > token.slot_generation);
    RUVIA_CHECK(fixture.state.bind(identity) == status::stale);
    RUVIA_CHECK(fixture.state.mark_transport_retired(identity) == status::stale);
    RUVIA_CHECK(fixture.state.execute_intent(identity, reset_intent(identity, 4, 1, ruvia::Http3ConnectionErrorCode::kMessageError)).outcome == outcome::stale);
    RUVIA_CHECK(!fixture.scheduler.abandon(token));
    RUVIA_CHECK(fixture.state.revoke(next) == status::changed);
    RUVIA_CHECK(fixture.state.mark_worker_finalized(next) == status::changed);
    RUVIA_CHECK(fixture.state.retire(next) == status::wrong_state);
    RUVIA_CHECK(fixture.state.mark_transport_retired(next) == status::changed);
    RUVIA_CHECK(fixture.state.retire(next) == status::changed);
}

ruvia::Task<void> reject_revoke_and_stop(fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    auto identity = fixture.reserve();
    RUVIA_CHECK(fixture.state.bind(identity) == status::changed);
    RUVIA_CHECK(fixture.state.binding().has_value());
    RUVIA_CHECK(fixture.state.reject(identity, state_type::reject_reason::construction_failed) == status::changed);
    RUVIA_CHECK(fixture.state.admission() == state_type::admission_phase::rejected);
    RUVIA_CHECK(fixture.state.rejection() == state_type::reject_reason::construction_failed);
    RUVIA_CHECK(fixture.scheduler.snapshot().free_connections == 1);
    RUVIA_CHECK(!fixture.state.slot_reusable());
    RUVIA_CHECK(fixture.state.mark_transport_retired(identity) == status::changed);
    RUVIA_CHECK(fixture.state.mark_transport_retired(identity) == status::wrong_state);
    RUVIA_CHECK(fixture.state.retire(identity) == status::wrong_state);
    RUVIA_CHECK(fixture.state.mark_worker_finalized(identity) == status::changed);
    RUVIA_CHECK(fixture.state.retire(identity) == status::changed);
    RUVIA_CHECK(fixture.state.reset() == status::changed);
    identity = fixture.reserve(42, 1);
    const auto before_stop = fixture.changes;
    fixture.state.stop_admission();
    RUVIA_CHECK(fixture.changes == before_stop + 1);
    fixture.state.stop_admission();
    RUVIA_CHECK(fixture.changes == before_stop + 1);
    RUVIA_CHECK(!fixture.state.available_identity());
    RUVIA_CHECK(fixture.state.bind(identity) == status::wrong_state);
    RUVIA_CHECK(fixture.state.revoke(identity) == status::changed);
    RUVIA_CHECK(fixture.state.revoke(identity) == status::wrong_state);
    RUVIA_CHECK(fixture.state.admission() == state_type::admission_phase::revoked);
    RUVIA_CHECK(fixture.state.mark_worker_finalized(identity) == status::changed);
    RUVIA_CHECK(fixture.state.mark_transport_retired(identity) == status::changed);
    RUVIA_CHECK(fixture.state.retire(identity) == status::changed);
    RUVIA_CHECK(fixture.state.reset() == status::changed);
    RUVIA_CHECK(fixture.state.reserve(fixture.scheduler, 43, 1) == status::wrong_state);
    RUVIA_CHECK(fixture.state.ready_to_destroy());
    RUVIA_CHECK(fixture.outbound.stop());
    co_return;
}

ruvia::Task<void> direct_intents(fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    const auto identity = fixture.reserve();
    fixture.attach(identity);
    const std::array codes{ruvia::Http3ConnectionErrorCode::kMessageError, ruvia::Http3ConnectionErrorCode::kRequestCancelled, ruvia::Http3ConnectionErrorCode::kExcessiveLoad};
    for (std::size_t i = 0; i < codes.size(); ++i) {
        const auto intent = reset_intent(identity, 4 * (i + 1), 101 * (i + 1), codes[i]);
        RUVIA_CHECK(fixture.state.execute_intent(identity, intent).completed());
        RUVIA_CHECK(fixture.executed[i].token == intent.token);
        RUVIA_CHECK(fixture.executed[i].streamResetErrorCode == codes[i]);
        RUVIA_CHECK(fixture.executed[i].closeReason == intent.closeReason);
        RUVIA_CHECK(fixture.executed[i].connectionErrorCode == intent.connectionErrorCode);
    }
    const std::array results{
        connection_type::PushStreamOpenResult{.status = connection_type::PushStreamOpenResult::Status::kOpened, .streamId = 31},
        connection_type::PushStreamOpenResult{},
        connection_type::PushStreamOpenResult{.status = connection_type::PushStreamOpenResult::Status::kStopped}};
    for (std::size_t i = 0; i < results.size(); ++i) {
        fixture.execution_result.push_stream = results[i];
        const auto intent = push_intent(identity, i, i + 1);
        const auto result = fixture.state.execute_intent(identity, intent);
        RUVIA_CHECK(result.completed());
        RUVIA_CHECK(result.push_stream->status == results[i].status);
        RUVIA_CHECK(result.push_stream->streamId == results[i].streamId);
        RUVIA_CHECK(fixture.executed[3 + i].token == intent.token);
    }
    fixture.execution_result.push_stream = connection_type::PushStreamOpenResult{.status = connection_type::PushStreamOpenResult::Status::kOpened, .streamId = 0};
    RUVIA_CHECK(fixture.state.execute_intent(identity, push_intent(identity, 3, 4)).outcome == outcome::invalid);
    fixture.execution_result.push_stream.reset();
    RUVIA_CHECK(fixture.state.execute_intent(identity, push_intent(identity, 3, 5)).outcome == outcome::invalid);
    auto malformed = push_intent(identity, 0, 6);
    malformed.token.id.push_id.reset();
    const auto count = fixture.executions;
    RUVIA_CHECK(fixture.state.execute_intent(identity, malformed).outcome == outcome::invalid);
    RUVIA_CHECK(fixture.executions == count);
    fixture.state.set_transport_executor({});
    RUVIA_CHECK(fixture.state.execute_intent(identity, reset_intent(identity, 4, 7, codes[0])).outcome == outcome::unavailable);
    fixture.state.set_transport_executor({&fixture, fixture::execute});
    co_await fixture.finish(identity);
}

ruvia::Task<void> offered_intent_survives_teardown(fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    const auto identity = fixture.reserve();
    fixture.attach(identity);
    RUVIA_CHECK(fixture.state.seal_admission(identity, 0, 0) == status::changed);
    RUVIA_CHECK(fixture.state.mark_worker_drained(identity) == status::changed);
    RUVIA_CHECK(fixture.connection->requestStop());
    const auto offered = fixture.scheduler.step();
    RUVIA_CHECK(offered.kind == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(fixture.state.start_worker_draining(identity) == status::changed);
    fixture.retire_during_execution = true;
    const auto result = fixture.state.execute_intent(identity, offered.intent);
    RUVIA_CHECK(result.outcome == outcome::transport_retired);
    RUVIA_CHECK(fixture.executions == 1);
    RUVIA_CHECK(fixture.executed[0].token == offered.intent.token);
    RUVIA_CHECK(fixture.executed[0].closeReason == offered.intent.closeReason);
    RUVIA_CHECK(fixture.state.worker_drained());
    RUVIA_CHECK(fixture.state.admission_seal()->goaway_id == 0);
    co_await fixture.connection->join();
    RUVIA_CHECK(fixture.state.mark_worker_finalized(identity) == status::changed);
    RUVIA_CHECK(fixture.state.retire(identity) == status::wrong_state);
    RUVIA_CHECK(fixture.state.execute_intent(identity, offered.intent).outcome == outcome::transport_retired);
    RUVIA_CHECK(fixture.executions == 1);
    auto forged = offered.intent.token;
    --forged.sequence;
    RUVIA_CHECK(!fixture.scheduler.acknowledge_intent(offered.connection, forged));
    RUVIA_CHECK(fixture.scheduler.acknowledge_intent(offered.connection, offered.intent.token, result.push_stream));
    RUVIA_CHECK(!fixture.scheduler.acknowledge_intent(offered.connection, offered.intent.token));
    RUVIA_CHECK(fixture.state.retire(identity) == status::changed);
    fixture.connection.reset();
    RUVIA_CHECK(fixture.outbound.stop());
}

ruvia::Task<void> retired_intents(fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    const auto identity = fixture.reserve();
    fixture.attach(identity);
    RUVIA_CHECK(fixture.state.mark_transport_retired(identity) == status::changed);
    const auto count = fixture.executions;
    const auto reset = fixture.state.execute_intent(identity, reset_intent(identity, 4, 10, ruvia::Http3ConnectionErrorCode::kMessageError));
    const auto push = fixture.state.execute_intent(identity, push_intent(identity, 0, 11));
    RUVIA_CHECK(reset.outcome == outcome::transport_retired);
    RUVIA_CHECK(!reset.push_stream);
    RUVIA_CHECK(push.outcome == outcome::transport_retired);
    RUVIA_CHECK(push.push_stream->status == connection_type::PushStreamOpenResult::Status::kStopped);
    RUVIA_CHECK(fixture.executions == count);
    co_await fixture.finish(identity);
}

ruvia::Task<void> datagram_boundaries_and_retirement(fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    const auto identity = fixture.reserve();
    fixture.attach(identity);
    const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}};
    const auto allocations = fixture.resource.allocationCount();
    for (std::size_t i = 0; i < state_type::datagram_capacity; ++i) {
        RUVIA_CHECK(fixture.state.publish_request_datagram(identity, 4, i == 0 ? std::span<const std::byte>{} : std::span(bytes)) == status::changed);
        RUVIA_CHECK(fixture.state.publish_response_datagram(identity, 8, bytes) == status::changed);
    }
    RUVIA_CHECK(fixture.state.publish_request_datagram(identity, 4, bytes) == status::full);
    RUVIA_CHECK(fixture.state.publish_response_datagram(identity, 8, bytes) == status::full);
    state_type::datagram request;
    state_type::datagram response;
    RUVIA_CHECK(fixture.state.pop_request_datagram(request) == status::changed);
    RUVIA_CHECK(request.identity == identity && request.stream_id == 4 && request.bytes().empty());
    // Queue capacity alone cannot reuse a block retained by a linear borrow.
    RUVIA_CHECK(fixture.state.publish_request_datagram(identity, 12, bytes) == status::full);
    request.storage.reset();
    RUVIA_CHECK(fixture.state.publish_request_datagram(identity, 12, std::span(bytes).first(1)) == status::changed);
    RUVIA_CHECK(fixture.state.pop_response_datagram(response) == status::changed);
    RUVIA_CHECK(response.stream_id == 8 && std::equal(bytes.begin(), bytes.end(), response.bytes().begin()));
    response.storage.reset();
    RUVIA_CHECK(fixture.state.publish_response_datagram(identity, 16, {}) == status::changed);
    for (std::size_t i = 1; i < state_type::datagram_capacity; ++i) {
        RUVIA_CHECK(fixture.state.pop_request_datagram(request) == status::changed);
        RUVIA_CHECK(request.stream_id == 4 && request.size == bytes.size());
        RUVIA_CHECK(std::equal(bytes.begin(), bytes.end(), request.bytes().begin()));
        request.storage.reset();
    }
    RUVIA_CHECK(fixture.state.pop_request_datagram(request) == status::changed);
    RUVIA_CHECK(request.stream_id == 12 && request.bytes().size() == 1 && request.bytes()[0] == bytes[0]);
    request.storage.reset();
    RUVIA_CHECK(fixture.state.pop_request_datagram(request) == status::empty);
    RUVIA_CHECK(fixture.state.publish_request_datagram(identity, 20, bytes) == status::changed);
    RUVIA_CHECK_EQ(fixture.resource.allocationCount(), allocations);
    RUVIA_CHECK(fixture.connection->requestStop());
    RUVIA_CHECK(fixture.state.start_worker_draining(identity) == status::changed);
    RUVIA_CHECK(fixture.state.pop_response_datagram(response) == status::changed);
    RUVIA_CHECK(fixture.state.mark_transport_retired(identity) == status::changed);
    RUVIA_CHECK(fixture.state.pop_request_datagram(request) == status::empty);
    state_type::datagram discarded;
    RUVIA_CHECK(fixture.state.pop_response_datagram(discarded) == status::empty);
    RUVIA_CHECK(fixture.state.publish_response_datagram(identity, 8, bytes) == status::wrong_state);
    for (;;) {
        const auto step = fixture.scheduler.step();
        if (step.kind == scheduler_type::step_kind::idle) {
            break;
        }
        if (step.kind == scheduler_type::step_kind::transport_intent) {
            const auto result = fixture.state.execute_intent(identity, step.intent);
            RUVIA_CHECK(result.completed());
            RUVIA_CHECK(fixture.scheduler.acknowledge_intent(step.connection, step.intent.token, result.push_stream));
        }
    }
    co_await fixture.connection->join();
    RUVIA_CHECK(fixture.state.mark_worker_finalized(identity) == status::changed);
    RUVIA_CHECK(fixture.state.retire(identity) == status::wrong_state);
    const auto changes = fixture.changes;
    response.storage.reset();
    RUVIA_CHECK(fixture.changes == changes + 1);
    RUVIA_CHECK(fixture.state.retire(identity) == status::changed);
    fixture.connection.reset();
    RUVIA_CHECK(fixture.state.ready_to_destroy());
    RUVIA_CHECK(fixture.outbound.stop());
}

ruvia::Task<void> stop_after(ruvia::EventLoopAttachment& attachment, ruvia::Task<void> operation) {
    try {
        co_await std::move(operation);
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

using exercise = ruvia::Task<void> (*)(fixture&, ruvia::testing::TestContext&);

ruvia::Task<void> exercise_on_worker(const ruvia::WorkerHandle& worker,
    ruvia::test::CountingMemoryResource& upstream, exercise operation,
    ruvia::testing::TestContext& ruvia_ctx) {
    // Construct and destroy all worker-affine state while the owner is running.
    fixture fixture(worker, upstream);
    co_await operation(fixture, ruvia_ctx);
}

void run(exercise operation, ruvia::testing::TestContext& ruvia_ctx) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    auto root = attachment.loop().start(stop_after(attachment,
        exercise_on_worker(worker, upstream, operation, ruvia_ctx)));
    attachment.run();
    root.get();
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

}  // namespace

RUVIA_TEST(http3_connection_state_reserves_binds_attaches_and_fences_generation_reuse) {
    run(admission_and_generation, ruvia_ctx);
}

RUVIA_TEST(http3_connection_state_rejects_revokes_and_stops_admission_locally) {
    run(reject_revoke_and_stop, ruvia_ctx);
}

RUVIA_TEST(http3_connection_state_executes_exact_reset_and_push_results_synchronously) {
    run(direct_intents, ruvia_ctx);
}

RUVIA_TEST(http3_connection_state_offered_close_settles_after_synchronous_transport_teardown) {
    run(offered_intent_survives_teardown, ruvia_ctx);
}

RUVIA_TEST(http3_connection_state_retired_transport_supersedes_reset_and_push_without_execution) {
    run(retired_intents, ruvia_ctx);
}

RUVIA_TEST(http3_connection_state_datagrams_preserve_boundaries_capacity_and_linear_retirement) {
    run(datagram_boundaries_and_retirement, ruvia_ctx);
}
