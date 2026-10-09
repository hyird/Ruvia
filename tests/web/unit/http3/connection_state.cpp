#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <utility>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/web/context.h"

#include "http3/http3_connection_state.h"
#include "memory_resource_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using state_type = ruvia::detail::http3_connection_state;
using identity_type = ruvia::detail::http3_connection_identity;
using connection_type = ruvia::detail::http3_server_connection;
using scheduler_type = ruvia::detail::http3_ready_scheduler;
using status_type = state_type::status;
using outcome_type = state_type::execution_outcome;

struct fixture_type final {
    ruvia::detail::router router_;
    ruvia::detail::router_impl& routes_{ruvia::detail::router_impl::from(router_)};
    ruvia::test::counting_memory_resource& resource_;
    ruvia::worker_memory memory_;
    ruvia::stop_source stop_source_;
    ruvia::stop_token stop_token_;
    ruvia::detail::context_services services_;
    ruvia::detail::http_server_options options_;
    ruvia::detail::http3_stream_buffer outbound_;
    scheduler_type scheduler_;
    std::size_t changes_{};
    state_type state_;
    std::optional<connection_type> connection_;
    std::array<connection_type::transport_intent_type, 16> executed_{};
    std::size_t executions_{};
    state_type::intent_execution_result execution_result_{.outcome_ = outcome_type::executed};
    bool retire_during_execution_{};

    fixture_type(const ruvia::worker_handle& worker_value, ruvia::test::counting_memory_resource& resource)
        : resource_(resource),
          memory_(resource),
          stop_token_(stop_source_.token()),
          services_(worker_value, stop_token_),
          outbound_(8, 8, 8, memory_.resource()),
          scheduler_(worker_value, 1, memory_.resource()),
          state_({this, changed}, &resource) {
        routes_.finalize();
        state_.set_transport_executor({this, execute});
    }

    static void changed(void* context_value) noexcept {
        ++static_cast<fixture_type*>(context_value)->changes_;
    }

    static state_type::intent_execution_result execute(void* context_value, identity_type identity, const connection_type::transport_intent_type& intent) noexcept {
        auto& self = *static_cast<fixture_type*>(context_value);
        self.executed_[self.executions_++] = intent;
        if (self.retire_during_execution_) {
            (void)self.state_.mark_transport_retired(identity);
        }
        return self.execution_result_;
    }

    identity_type reserve(std::uint64_t epoch = 41, std::uint64_t generation = 9) {
        if (state_.reserve(scheduler_, epoch, generation) != status_type::changed) {
            std::terminate();
        }
        return *state_.identity();
    }

    void attach(identity_type identity) {
        if (state_.bind(identity, {.remote_address_ = "127.0.0.1", .client_certificate_subject_ = "peer", .remote_port_ = 443}, {.enable_connect_protocol_ = true}, 1200) != status_type::changed) {
            std::terminate();
        }
        const auto registration = *state_.registration();
        connection_.emplace(routes_.route_table(), memory_, services_, options_, outbound_, registration.activation_,
            ruvia::detail::http3_server_connection_config{.epoch_ = identity.epoch_, .connection_generation_ = identity.connection_generation_, .max_tracked_streams_ = 8});
        if (state_.attach_handler(identity, *connection_) != status_type::changed) {
            std::terminate();
        }
    }

    ruvia::task<void> finish(identity_type identity, bool transport_first = true) {
        if (!connection_->stopped()) {
            (void)connection_->request_stop();
        }
        if (state_.start_worker_draining(identity) != status_type::changed) {
            std::terminate();
        }
        if (transport_first && !state_.transport_retired() && state_.mark_transport_retired(identity) != status_type::changed) {
            std::terminate();
        }
        for (;;) {
            const auto step = scheduler_.step();
            if (step.kind_ == scheduler_type::step_kind::idle) {
                break;
            }
            if (step.kind_ == scheduler_type::step_kind::transport_intent) {
                const auto result_value = state_.execute_intent(identity, step.intent_);
                if (!result_value.completed() || !scheduler_.acknowledge_intent(step.connection_, step.intent_.token_, result_value.push_stream_)) {
                    std::terminate();
                }
            }
        }
        co_await connection_->join();
        if (state_.mark_worker_finalized(identity) != status_type::changed) {
            std::terminate();
        }
        if (!state_.transport_retired() && state_.mark_transport_retired(identity) != status_type::changed) {
            std::terminate();
        }
        if (state_.retire(identity) != status_type::changed) {
            std::terminate();
        }
        connection_.reset();
        if (!outbound_.stop()) {
            std::terminate();
        }
    }
};

connection_type::transport_intent_type reset_intent(identity_type identity, std::uint64_t stream_id, std::uint64_t sequence, ruvia::http3_connection_error_code code) {
    return {.token_ = {.kind_ = connection_type::transport_intent_kind_type::stream_reset,
                .id_ = {.epoch_ = identity.epoch_, .connection_generation_ = identity.connection_generation_, .stream_id_ = stream_id},
                .sequence_ = sequence},
        .stream_reset_error_code_ = code};
}

connection_type::transport_intent_type push_intent(identity_type identity, std::uint64_t push_id, std::uint64_t sequence) {
    return {.token_ = {.kind_ = connection_type::transport_intent_kind_type::open_push_stream,
                .id_ = {.epoch_ = identity.epoch_, .connection_generation_ = identity.connection_generation_, .stream_id_ = 0, .push_id_ = push_id},
                .sequence_ = sequence}};
}

ruvia::task<void> admission_and_generation(fixture_type& fixture_value, ruvia::testing::test_context& ruvia_ctx) {
    const auto identity = fixture_value.reserve();
    const auto token = fixture_value.state_.registration()->token_;
    RUVIA_CHECK(fixture_value.state_.available_identity() == identity);
    RUVIA_CHECK(fixture_value.state_.available_identity() == identity);
    RUVIA_CHECK(fixture_value.state_.admission() == state_type::admission_phase::reserved);
    RUVIA_CHECK(fixture_value.scheduler_.snapshot().free_connections_ == 0);
    RUVIA_CHECK(fixture_value.state_.reject(identity, state_type::reject_reason::capacity) == status_type::wrong_state);
    fixture_value.attach(identity);
    RUVIA_CHECK(!fixture_value.state_.available_identity());
    RUVIA_CHECK(fixture_value.state_.admission() == state_type::admission_phase::handler_attached);
    RUVIA_CHECK(fixture_value.scheduler_.snapshot().attached_connections_ == 1);
    const auto binding = *fixture_value.state_.binding();
    RUVIA_CHECK(binding.identity_ == identity);
    RUVIA_CHECK(binding.metadata_.remote_address_ == "127.0.0.1");
    RUVIA_CHECK(binding.metadata_.client_certificate_subject_ == "peer");
    RUVIA_CHECK(binding.metadata_.remote_port_ == 443);
    RUVIA_CHECK(binding.settings_.enable_connect_protocol_);
    RUVIA_CHECK(binding.max_quic_datagram_payload_bytes_ == 1200);
    RUVIA_CHECK(fixture_value.state_.attach_handler(identity, *fixture_value.connection_) == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.reject(identity, state_type::reject_reason::stopping) == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.reset() == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.mark_worker_finalized(identity) == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.seal_admission(identity, 0, 8) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.admission_seal()->identity_ == identity);
    RUVIA_CHECK(fixture_value.state_.admission_seal()->expected_admitted_requests_ == 0);
    RUVIA_CHECK(fixture_value.state_.admission_seal()->goaway_id_ == 8);
    RUVIA_CHECK(fixture_value.state_.seal_admission(identity, 1, 12) == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.mark_worker_drained(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.worker_drained());
    RUVIA_CHECK(!fixture_value.state_.slot_reusable());
    co_await fixture_value.finish(identity, false);
    RUVIA_CHECK(fixture_value.state_.worker_finalized());
    RUVIA_CHECK(fixture_value.state_.transport_retired());
    RUVIA_CHECK(fixture_value.state_.slot_reusable());
    RUVIA_CHECK(fixture_value.state_.ready_to_destroy());
    RUVIA_CHECK(fixture_value.state_.reset() == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.reserve(fixture_value.scheduler_, identity.epoch_, identity.connection_generation_) == status_type::stale);
    RUVIA_CHECK(fixture_value.state_.reserve(fixture_value.scheduler_, identity.epoch_ - 1, 100) == status_type::stale);
    const auto next_value = fixture_value.reserve(identity.epoch_ + 1, 1);
    RUVIA_CHECK(fixture_value.state_.registration()->token_.slot_generation_ > token.slot_generation_);
    RUVIA_CHECK(fixture_value.state_.bind(identity) == status_type::stale);
    RUVIA_CHECK(fixture_value.state_.mark_transport_retired(identity) == status_type::stale);
    RUVIA_CHECK(fixture_value.state_.execute_intent(identity, reset_intent(identity, 4, 1, ruvia::http3_connection_error_code::message_error)).outcome_ == outcome_type::stale);
    RUVIA_CHECK(!fixture_value.scheduler_.abandon(token));
    RUVIA_CHECK(fixture_value.state_.revoke(next_value) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.mark_worker_finalized(next_value) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.retire(next_value) == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.mark_transport_retired(next_value) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.retire(next_value) == status_type::changed);
}

ruvia::task<void> reject_revoke_and_stop(fixture_type& fixture_value, ruvia::testing::test_context& ruvia_ctx) {
    auto identity = fixture_value.reserve();
    RUVIA_CHECK(fixture_value.state_.bind(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.binding().has_value());
    RUVIA_CHECK(fixture_value.state_.reject(identity, state_type::reject_reason::construction_failed) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.admission() == state_type::admission_phase::rejected);
    RUVIA_CHECK(fixture_value.state_.rejection() == state_type::reject_reason::construction_failed);
    RUVIA_CHECK(fixture_value.scheduler_.snapshot().free_connections_ == 1);
    RUVIA_CHECK(!fixture_value.state_.slot_reusable());
    RUVIA_CHECK(fixture_value.state_.mark_transport_retired(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.mark_transport_retired(identity) == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.retire(identity) == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.mark_worker_finalized(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.retire(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.reset() == status_type::changed);
    identity = fixture_value.reserve(42, 1);
    const auto before_stop = fixture_value.changes_;
    fixture_value.state_.stop_admission();
    RUVIA_CHECK(fixture_value.changes_ == before_stop + 1);
    fixture_value.state_.stop_admission();
    RUVIA_CHECK(fixture_value.changes_ == before_stop + 1);
    RUVIA_CHECK(!fixture_value.state_.available_identity());
    RUVIA_CHECK(fixture_value.state_.bind(identity) == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.revoke(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.revoke(identity) == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.admission() == state_type::admission_phase::revoked);
    RUVIA_CHECK(fixture_value.state_.mark_worker_finalized(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.mark_transport_retired(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.retire(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.reset() == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.reserve(fixture_value.scheduler_, 43, 1) == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.ready_to_destroy());
    RUVIA_CHECK(fixture_value.outbound_.stop());
    co_return;
}

ruvia::task<void> direct_intents(fixture_type& fixture_value, ruvia::testing::test_context& ruvia_ctx) {
    const auto identity = fixture_value.reserve();
    fixture_value.attach(identity);
    const std::array codes{ruvia::http3_connection_error_code::message_error, ruvia::http3_connection_error_code::request_cancelled, ruvia::http3_connection_error_code::excessive_load};
    for (std::size_t i = 0; i < codes.size(); ++i) {
        const auto intent = reset_intent(identity, 4 * (i + 1), 101 * (i + 1), codes[i]);
        RUVIA_CHECK(fixture_value.state_.execute_intent(identity, intent).completed());
        RUVIA_CHECK(fixture_value.executed_[i].token_ == intent.token_);
        RUVIA_CHECK(fixture_value.executed_[i].stream_reset_error_code_ == codes[i]);
        RUVIA_CHECK(fixture_value.executed_[i].close_reason_ == intent.close_reason_);
        RUVIA_CHECK(fixture_value.executed_[i].connection_error_code_ == intent.connection_error_code_);
    }
    const std::array results{
        connection_type::push_stream_open_result_type{.status_ = connection_type::push_stream_open_result_type::status_type::opened, .stream_id_ = 31},
        connection_type::push_stream_open_result_type{},
        connection_type::push_stream_open_result_type{.status_ = connection_type::push_stream_open_result_type::status_type::stopped}};
    for (std::size_t i = 0; i < results.size(); ++i) {
        fixture_value.execution_result_.push_stream_ = results[i];
        const auto intent = push_intent(identity, i, i + 1);
        const auto result_value = fixture_value.state_.execute_intent(identity, intent);
        RUVIA_CHECK(result_value.completed());
        RUVIA_CHECK(result_value.push_stream_->status_ == results[i].status_);
        RUVIA_CHECK(result_value.push_stream_->stream_id_ == results[i].stream_id_);
        RUVIA_CHECK(fixture_value.executed_[3 + i].token_ == intent.token_);
    }
    fixture_value.execution_result_.push_stream_ = connection_type::push_stream_open_result_type{.status_ = connection_type::push_stream_open_result_type::status_type::opened, .stream_id_ = 0};
    RUVIA_CHECK(fixture_value.state_.execute_intent(identity, push_intent(identity, 3, 4)).outcome_ == outcome_type::invalid);
    fixture_value.execution_result_.push_stream_.reset();
    RUVIA_CHECK(fixture_value.state_.execute_intent(identity, push_intent(identity, 3, 5)).outcome_ == outcome_type::invalid);
    auto malformed = push_intent(identity, 0, 6);
    malformed.token_.id_.push_id_.reset();
    const auto count = fixture_value.executions_;
    RUVIA_CHECK(fixture_value.state_.execute_intent(identity, malformed).outcome_ == outcome_type::invalid);
    RUVIA_CHECK(fixture_value.executions_ == count);
    fixture_value.state_.set_transport_executor({});
    RUVIA_CHECK(fixture_value.state_.execute_intent(identity, reset_intent(identity, 4, 7, codes[0])).outcome_ == outcome_type::unavailable);
    fixture_value.state_.set_transport_executor({&fixture_value, fixture_type::execute});
    co_await fixture_value.finish(identity);
}

ruvia::task<void> offered_intent_survives_teardown(fixture_type& fixture_value, ruvia::testing::test_context& ruvia_ctx) {
    const auto identity = fixture_value.reserve();
    fixture_value.attach(identity);
    RUVIA_CHECK(fixture_value.state_.seal_admission(identity, 0, 0) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.mark_worker_drained(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.connection_->request_stop());
    const auto offered = fixture_value.scheduler_.step();
    RUVIA_CHECK(offered.kind_ == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(fixture_value.state_.start_worker_draining(identity) == status_type::changed);
    fixture_value.retire_during_execution_ = true;
    const auto result_value = fixture_value.state_.execute_intent(identity, offered.intent_);
    RUVIA_CHECK(result_value.outcome_ == outcome_type::transport_retired);
    RUVIA_CHECK(fixture_value.executions_ == 1);
    RUVIA_CHECK(fixture_value.executed_[0].token_ == offered.intent_.token_);
    RUVIA_CHECK(fixture_value.executed_[0].close_reason_ == offered.intent_.close_reason_);
    RUVIA_CHECK(fixture_value.state_.worker_drained());
    RUVIA_CHECK(fixture_value.state_.admission_seal()->goaway_id_ == 0);
    co_await fixture_value.connection_->join();
    RUVIA_CHECK(fixture_value.state_.mark_worker_finalized(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.retire(identity) == status_type::wrong_state);
    RUVIA_CHECK(fixture_value.state_.execute_intent(identity, offered.intent_).outcome_ == outcome_type::transport_retired);
    RUVIA_CHECK(fixture_value.executions_ == 1);
    auto forged = offered.intent_.token_;
    --forged.sequence_;
    RUVIA_CHECK(!fixture_value.scheduler_.acknowledge_intent(offered.connection_, forged));
    RUVIA_CHECK(fixture_value.scheduler_.acknowledge_intent(offered.connection_, offered.intent_.token_, result_value.push_stream_));
    RUVIA_CHECK(!fixture_value.scheduler_.acknowledge_intent(offered.connection_, offered.intent_.token_));
    RUVIA_CHECK(fixture_value.state_.retire(identity) == status_type::changed);
    fixture_value.connection_.reset();
    RUVIA_CHECK(fixture_value.outbound_.stop());
}

ruvia::task<void> retired_intents(fixture_type& fixture_value, ruvia::testing::test_context& ruvia_ctx) {
    const auto identity = fixture_value.reserve();
    fixture_value.attach(identity);
    RUVIA_CHECK(fixture_value.state_.mark_transport_retired(identity) == status_type::changed);
    const auto count = fixture_value.executions_;
    const auto reset = fixture_value.state_.execute_intent(identity, reset_intent(identity, 4, 10, ruvia::http3_connection_error_code::message_error));
    const auto push = fixture_value.state_.execute_intent(identity, push_intent(identity, 0, 11));
    RUVIA_CHECK(reset.outcome_ == outcome_type::transport_retired);
    RUVIA_CHECK(!reset.push_stream_);
    RUVIA_CHECK(push.outcome_ == outcome_type::transport_retired);
    RUVIA_CHECK(push.push_stream_->status_ == connection_type::push_stream_open_result_type::status_type::stopped);
    RUVIA_CHECK(fixture_value.executions_ == count);
    co_await fixture_value.finish(identity);
}

ruvia::task<void> datagram_boundaries_and_retirement(fixture_type& fixture_value, ruvia::testing::test_context& ruvia_ctx) {
    const auto identity = fixture_value.reserve();
    fixture_value.attach(identity);
    const std::array bytes_value{std::byte{1}, std::byte{2}, std::byte{3}};
    const auto allocations = fixture_value.resource_.allocation_count();
    for (std::size_t i = 0; i < state_type::datagram_capacity; ++i) {
        RUVIA_CHECK(fixture_value.state_.publish_request_datagram(identity, 4, i == 0 ? std::span<const std::byte>{} : std::span(bytes_value)) == status_type::changed);
        RUVIA_CHECK(fixture_value.state_.publish_response_datagram(identity, 8, bytes_value) == status_type::changed);
    }
    RUVIA_CHECK(fixture_value.state_.publish_request_datagram(identity, 4, bytes_value) == status_type::full);
    RUVIA_CHECK(fixture_value.state_.publish_response_datagram(identity, 8, bytes_value) == status_type::full);
    state_type::datagram request;
    state_type::datagram response;
    RUVIA_CHECK(fixture_value.state_.pop_request_datagram(request) == status_type::changed);
    RUVIA_CHECK(request.identity_ == identity && request.stream_id_ == 4 && request.bytes().empty());
    // Queue capacity alone cannot reuse a block retained by a linear borrow.
    RUVIA_CHECK(fixture_value.state_.publish_request_datagram(identity, 12, bytes_value) == status_type::full);
    request.storage_.reset();
    RUVIA_CHECK(fixture_value.state_.publish_request_datagram(identity, 12, std::span(bytes_value).first(1)) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.pop_response_datagram(response) == status_type::changed);
    RUVIA_CHECK(response.stream_id_ == 8 && std::equal(bytes_value.begin(), bytes_value.end(), response.bytes().begin()));
    response.storage_.reset();
    RUVIA_CHECK(fixture_value.state_.publish_response_datagram(identity, 16, {}) == status_type::changed);
    for (std::size_t i = 1; i < state_type::datagram_capacity; ++i) {
        RUVIA_CHECK(fixture_value.state_.pop_request_datagram(request) == status_type::changed);
        RUVIA_CHECK(request.stream_id_ == 4 && request.size_ == bytes_value.size());
        RUVIA_CHECK(std::equal(bytes_value.begin(), bytes_value.end(), request.bytes().begin()));
        request.storage_.reset();
    }
    RUVIA_CHECK(fixture_value.state_.pop_request_datagram(request) == status_type::changed);
    RUVIA_CHECK(request.stream_id_ == 12 && request.bytes().size() == 1 && request.bytes()[0] == bytes_value[0]);
    request.storage_.reset();
    RUVIA_CHECK(fixture_value.state_.pop_request_datagram(request) == status_type::empty);
    RUVIA_CHECK(fixture_value.state_.publish_request_datagram(identity, 20, bytes_value) == status_type::changed);
    RUVIA_CHECK_EQ(fixture_value.resource_.allocation_count(), allocations);
    RUVIA_CHECK(fixture_value.connection_->request_stop());
    RUVIA_CHECK(fixture_value.state_.start_worker_draining(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.pop_response_datagram(response) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.mark_transport_retired(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.pop_request_datagram(request) == status_type::empty);
    state_type::datagram discarded;
    RUVIA_CHECK(fixture_value.state_.pop_response_datagram(discarded) == status_type::empty);
    RUVIA_CHECK(fixture_value.state_.publish_response_datagram(identity, 8, bytes_value) == status_type::wrong_state);
    for (;;) {
        const auto step = fixture_value.scheduler_.step();
        if (step.kind_ == scheduler_type::step_kind::idle) {
            break;
        }
        if (step.kind_ == scheduler_type::step_kind::transport_intent) {
            const auto result_value = fixture_value.state_.execute_intent(identity, step.intent_);
            RUVIA_CHECK(result_value.completed());
            RUVIA_CHECK(fixture_value.scheduler_.acknowledge_intent(step.connection_, step.intent_.token_, result_value.push_stream_));
        }
    }
    co_await fixture_value.connection_->join();
    RUVIA_CHECK(fixture_value.state_.mark_worker_finalized(identity) == status_type::changed);
    RUVIA_CHECK(fixture_value.state_.retire(identity) == status_type::wrong_state);
    const auto changes = fixture_value.changes_;
    response.storage_.reset();
    RUVIA_CHECK(fixture_value.changes_ == changes + 1);
    RUVIA_CHECK(fixture_value.state_.retire(identity) == status_type::changed);
    fixture_value.connection_.reset();
    RUVIA_CHECK(fixture_value.state_.ready_to_destroy());
    RUVIA_CHECK(fixture_value.outbound_.stop());
}

ruvia::task<void> stop_after(ruvia::event_loop_attachment& attachment, ruvia::task<void> operation) {
    try {
        co_await std::move(operation);
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

using exercise_type = ruvia::task<void> (*)(fixture_type&, ruvia::testing::test_context&);

ruvia::task<void> exercise_on_worker(const ruvia::worker_handle& worker_value,
    ruvia::test::counting_memory_resource& upstream, exercise_type operation,
    ruvia::testing::test_context& ruvia_ctx) {
    // Construct and destroy all worker-affine state while the owner is running.
    fixture_type fixture_value(worker_value, upstream);
    co_await operation(fixture_value, ruvia_ctx);
}

void run(exercise_type operation, ruvia::testing::test_context& ruvia_ctx) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    auto root = attachment.loop().start(stop_after(attachment,
        exercise_on_worker(worker_value, upstream, operation, ruvia_ctx)));
    attachment.run();
    root.get();
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
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
