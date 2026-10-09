#include "http3/http3_connection_state.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "ruvia/http/http3_peer_streams.h"

namespace ruvia::detail {

http3_connection_state::datagram_storage::datagram_storage(std::pmr::memory_resource* resource)
    : pool_(2 * datagram_capacity, max_datagram_bytes, resource),
      requests_(datagram_capacity, resource),
      responses_(datagram_capacity, resource) {}

http3_connection_state::http3_connection_state(local_change_callback changed, std::pmr::memory_resource* datagram_resource)
    : changed_(changed),
      datagrams_(datagram_resource ? make_pmr_object<datagram_storage>(datagram_resource, datagram_resource) : std::unique_ptr<datagram_storage, pmr_object_deleter<datagram_storage>>{}) {}

http3_connection_state::~http3_connection_state() {
    if (!ready_to_destroy()) {
        std::terminate();
    }
}

http3_connection_state::status http3_connection_state::reserve(http3_ready_scheduler& scheduler, std::uint64_t epoch, std::uint64_t connection_generation) noexcept {
    if (admission_stopped_ || admission_ != admission_phase::vacant) {
        return status::wrong_state;
    }
    if (last_identity_ && (epoch < last_identity_->epoch_ ||
                              (epoch == last_identity_->epoch_ && connection_generation <= last_identity_->connection_generation_))) {
        return status::stale;
    }
    auto reserved = scheduler.reserve(epoch, connection_generation);
    if (!reserved) {
        return status::unavailable;
    }
    scheduler_ = &scheduler;
    registration_ = *reserved;
    identity_ = http3_connection_identity{epoch, connection_generation};
    last_identity_ = identity_;
    admission_ = admission_phase::reserved;
    signal_change();
    return status::changed;
}

std::optional<http3_connection_identity> http3_connection_state::available_identity() const noexcept {
    return !admission_stopped_ && admission_ == admission_phase::reserved ? identity_ : std::nullopt;
}

std::optional<http3_connection_identity> http3_connection_state::identity() const noexcept {
    return identity_;
}

std::optional<http3_ready_scheduler::registration> http3_connection_state::registration() const noexcept {
    return registration_;
}

http3_connection_state::admission_phase http3_connection_state::admission() const noexcept {
    return admission_;
}

void http3_connection_state::stop_admission() noexcept {
    if (!std::exchange(admission_stopped_, true)) {
        signal_change();
    }
}

http3_connection_state::status http3_connection_state::bind(http3_connection_identity identity, connection_metadata_view metadata, http3_settings settings, std::size_t max_quic_datagram_payload_bytes) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (admission_stopped_ || admission_ != admission_phase::reserved) {
        return status::wrong_state;
    }
    binding_ = binding_snapshot{identity, metadata, settings, max_quic_datagram_payload_bytes};
    admission_ = admission_phase::bound;
    signal_change();
    return status::changed;
}

std::optional<http3_connection_state::binding_snapshot> http3_connection_state::binding() const noexcept {
    return binding_;
}

http3_connection_state::status http3_connection_state::attach_handler(http3_connection_identity identity, http3_server_connection& connection) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (admission_ != admission_phase::bound || admission_stopped_) {
        return status::wrong_state;
    }
    if (!scheduler_->attach(registration_->token_, connection)) {
        return status::unavailable;
    }
    connection_ = &connection;
    admission_ = admission_phase::handler_attached;
    signal_change();
    return status::changed;
}

http3_connection_state::status http3_connection_state::reject(http3_connection_identity identity, reject_reason reason) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (admission_ != admission_phase::bound || !scheduler_->abandon(registration_->token_)) {
        return status::wrong_state;
    }
    rejection_ = reason;
    admission_ = admission_phase::rejected;
    worker_draining_ = true;
    signal_change();
    return status::changed;
}

std::optional<http3_connection_state::reject_reason> http3_connection_state::rejection() const noexcept {
    return rejection_;
}

http3_connection_state::status http3_connection_state::revoke(http3_connection_identity identity) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (admission_ != admission_phase::reserved || !scheduler_->abandon(registration_->token_)) {
        return status::wrong_state;
    }
    admission_ = admission_phase::revoked;
    worker_draining_ = true;
    signal_change();
    return status::changed;
}

std::optional<http3_server_connection::event_result_type> http3_connection_state::accept_peer_stream_data(
    http3_stream_id id, std::span<const std::byte> bytes_value) {
    if (!matches({id.epoch_, id.connection_generation_}) ||
        admission_ != admission_phase::handler_attached || !connection_ ||
        transport_retired_ || worker_finalized_ || slot_reusable_) {
        return std::nullopt;
    }
    return connection_->accept_peer_stream_data(id, bytes_value);
}

std::optional<http3_server_connection::event_result_type> http3_connection_state::accept_peer_stream_control(
    const http3_stream_control& control) {
    if (!matches({control.id_.epoch_, control.id_.connection_generation_}) ||
        admission_ != admission_phase::handler_attached || !connection_ ||
        transport_retired_ || worker_finalized_ || slot_reusable_ ||
        !is_http3_client_unidirectional_stream_id(control.id_.stream_id_) ||
        (control.kind_ != http3_stream_control::kind::stream_fin &&
            control.kind_ != http3_stream_control::kind::stream_reset)) {
        return std::nullopt;
    }
    return connection_->accept_control(control);
}

void http3_connection_state::set_transport_executor(transport_executor executor) noexcept {
    executor_ = executor;
}

http3_connection_state::intent_execution_result http3_connection_state::retired_intent(const http3_server_connection::transport_intent_type& intent) noexcept {
    intent_execution_result result_value{.outcome_ = execution_outcome::transport_retired};
    if (intent.token_.kind_ == http3_server_connection::transport_intent_kind_type::open_push_stream) {
        result_value.push_stream_ = http3_server_connection::push_stream_open_result_type{.status_ = http3_server_connection::push_stream_open_result_type::status_type::stopped};
    }
    return result_value;
}

http3_connection_state::intent_execution_result http3_connection_state::execute_intent(http3_connection_identity identity, const http3_server_connection::transport_intent_type& intent) noexcept {
    if (!matches(identity) || intent.token_.id_.epoch_ != identity.epoch_ || intent.token_.id_.connection_generation_ != identity.connection_generation_) {
        return {.outcome_ = execution_outcome::stale};
    }
    if (admission_ != admission_phase::handler_attached || slot_reusable_) {
        return {.outcome_ = execution_outcome::invalid};
    }
    const auto kind = intent.token_.kind_;
    if (kind != http3_server_connection::transport_intent_kind_type::connection_close &&
        kind != http3_server_connection::transport_intent_kind_type::stream_reset &&
        kind != http3_server_connection::transport_intent_kind_type::open_push_stream) {
        return {.outcome_ = execution_outcome::invalid};
    }
    if (kind == http3_server_connection::transport_intent_kind_type::open_push_stream &&
        (!intent.token_.id_.push_id_ || !is_http3_request_stream_id(intent.token_.id_.stream_id_) || intent.token_.sequence_ == 0)) {
        return {.outcome_ = execution_outcome::invalid};
    }
    if (transport_retired_) {
        return retired_intent(intent);
    }
    if (!executor_.execute_) {
        return {.outcome_ = execution_outcome::unavailable};
    }
    auto result_value = executor_.execute_(executor_.context_, identity, intent);
    // A close/reset callback can physically tear down this generation. The
    // offered token must still settle, without executing against a replacement.
    if (!matches(identity)) {
        return {.outcome_ = execution_outcome::stale};
    }
    if (transport_retired_) {
        return retired_intent(intent);
    }
    if (result_value.outcome_ == execution_outcome::transport_retired ||
        (result_value.completed() && ((kind == http3_server_connection::transport_intent_kind_type::open_push_stream) != result_value.push_stream_.has_value())) ||
        (result_value.push_stream_ && result_value.push_stream_->status_ == http3_server_connection::push_stream_open_result_type::status_type::opened &&
            http3_stream_id_type(result_value.push_stream_->stream_id_) != http3_stream_id_type::server_unidirectional)) {
        return {.outcome_ = execution_outcome::invalid};
    }
    return result_value;
}

http3_connection_state::status http3_connection_state::seal_admission(http3_connection_identity identity, std::size_t expected_admitted_requests, std::uint64_t goaway_id) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (admission_ != admission_phase::handler_attached || admission_seal_ || transport_retired_ || worker_finalized_) {
        return status::wrong_state;
    }
    admission_seal_ = admission_seal_snapshot{identity, expected_admitted_requests, goaway_id};
    signal_change();
    return status::changed;
}

std::optional<http3_connection_state::admission_seal_snapshot> http3_connection_state::admission_seal() const noexcept {
    return admission_seal_;
}

http3_connection_state::status http3_connection_state::mark_worker_drained(http3_connection_identity identity) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (!admission_seal_ || worker_drained_ || !connection_ ||
        !connection_->drain_ready(admission_seal_->expected_admitted_requests_)) {
        return status::wrong_state;
    }
    worker_drained_ = true;
    signal_change();
    return status::changed;
}

bool http3_connection_state::worker_drained() const noexcept {
    return worker_drained_;
}

http3_connection_state::status http3_connection_state::start_worker_draining(http3_connection_identity identity) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (admission_ != admission_phase::handler_attached || worker_draining_ ||
        !scheduler_->begin_retirement(registration_->token_)) {
        return status::wrong_state;
    }
    worker_draining_ = true;
    signal_change();
    return status::changed;
}

http3_connection_state::status http3_connection_state::mark_transport_retired(http3_connection_identity identity) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if ((admission_ != admission_phase::handler_attached && admission_ != admission_phase::rejected && admission_ != admission_phase::revoked) || transport_retired_) {
        return status::wrong_state;
    }
    if (connection_ && !connection_->confirm_transport_retired({.epoch_ = identity.epoch_, .connection_generation_ = identity.connection_generation_})) {
        return status::wrong_state;
    }
    transport_retired_ = true;
    discard_datagrams();
    signal_change();
    return status::changed;
}

bool http3_connection_state::transport_retired() const noexcept {
    return transport_retired_;
}

http3_connection_state::status http3_connection_state::mark_worker_finalized(http3_connection_identity identity) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (!worker_draining_ || worker_finalized_) {
        return status::wrong_state;
    }
    worker_finalized_ = true;
    worker_drained_ = true;
    signal_change();
    return status::changed;
}

bool http3_connection_state::worker_finalized() const noexcept {
    return worker_finalized_;
}

http3_connection_state::status http3_connection_state::retire(http3_connection_identity identity) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (!transport_retired_ || !worker_finalized_ || slot_reusable_ || (datagrams_ && datagrams_->pool_.outstanding() != 0)) {
        return status::wrong_state;
    }
    if (connection_ && !scheduler_->retire(registration_->token_)) {
        return status::wrong_state;
    }
    connection_ = nullptr;
    slot_reusable_ = true;
    signal_change();
    return status::changed;
}

bool http3_connection_state::slot_reusable() const noexcept {
    return slot_reusable_;
}

http3_connection_state::status http3_connection_state::reset() noexcept {
    if (!slot_reusable_) {
        return status::wrong_state;
    }
    scheduler_ = nullptr;
    registration_.reset();
    identity_.reset();
    binding_.reset();
    rejection_.reset();
    admission_seal_.reset();
    admission_ = admission_phase::vacant;
    worker_draining_ = false;
    worker_drained_ = false;
    transport_retired_ = false;
    worker_finalized_ = false;
    slot_reusable_ = false;
    signal_change();
    return status::changed;
}

bool http3_connection_state::ready_to_destroy() const noexcept {
    return (admission_ == admission_phase::vacant || slot_reusable_) && (!datagrams_ || datagrams_->pool_.outstanding() == 0);
}

bool http3_connection_state::matches(http3_connection_identity identity) const noexcept {
    return identity_ && *identity_ == identity;
}

void http3_connection_state::signal_change() noexcept {
    if (changed_.changed_) {
        changed_.changed_(changed_.context_);
    }
}

void http3_connection_state::return_datagram(void* context_value, buffer_credit credit) noexcept {
    auto& state_value = *static_cast<http3_connection_state*>(context_value);
    state_value.datagrams_->pool_.reclaim(std::move(credit));
    state_value.signal_change();
}

http3_connection_state::status http3_connection_state::publish_datagram(bool request, http3_connection_identity identity, std::uint64_t stream_id, std::span<const std::byte> bytes_value) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (admission_ != admission_phase::handler_attached || transport_retired_ || worker_finalized_) {
        return status::wrong_state;
    }
    if (!datagrams_ || !is_http3_request_stream_id(stream_id) || bytes_value.size() > max_datagram_bytes) {
        return status::unavailable;
    }
    auto& queue = request ? datagrams_->requests_ : datagrams_->responses_;
    auto* slot = queue.prepare_push();
    if (!slot) {
        return status::full;
    }
    auto lease_value = datagrams_->pool_.try_acquire({this, return_datagram});
    if (!lease_value) {
        queue.cancel_push();
        return status::full;
    }
    std::copy(bytes_value.begin(), bytes_value.end(), lease_value->bytes().begin());
    *slot = datagram{identity, stream_id, std::move(*lease_value), bytes_value.size()};
    queue.commit_push();
    signal_change();
    return status::changed;
}

http3_connection_state::status http3_connection_state::pop_datagram(bool request, datagram& value) noexcept {
    if (!datagrams_) {
        return status::empty;
    }
    auto& queue = request ? datagrams_->requests_ : datagrams_->responses_;
    auto* slot = queue.front();
    if (!slot) {
        return status::empty;
    }
    value = std::move(*slot);
    queue.pop();
    return matches(value.identity_) ? status::changed : status::stale;
}

void http3_connection_state::discard_datagrams() noexcept {
    if (!datagrams_) {
        return;
    }
    for (auto* queue : {&datagrams_->requests_, &datagrams_->responses_}) {
        while (auto* value = queue->front()) {
            value->storage_.reset();
            queue->pop();
        }
    }
}

http3_connection_state::status http3_connection_state::publish_request_datagram(http3_connection_identity identity, std::uint64_t stream_id, std::span<const std::byte> bytes_value) noexcept {
    return publish_datagram(true, identity, stream_id, bytes_value);
}

http3_connection_state::status http3_connection_state::publish_response_datagram(http3_connection_identity identity, std::uint64_t stream_id, std::span<const std::byte> bytes_value) noexcept {
    return publish_datagram(false, identity, stream_id, bytes_value);
}

http3_connection_state::status http3_connection_state::pop_request_datagram(datagram& value) noexcept {
    return pop_datagram(true, value);
}

http3_connection_state::status http3_connection_state::pop_response_datagram(datagram& value) noexcept {
    return pop_datagram(false, value);
}

}  // namespace ruvia::detail
