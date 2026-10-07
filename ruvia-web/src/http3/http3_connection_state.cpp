#include "ruvia/web/detail/http3/http3_connection_state.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "ruvia/http/Http3PeerStreams.h"

namespace ruvia::detail {

http3_connection_state::datagram_storage::datagram_storage(std::pmr::memory_resource* resource)
    : pool(2 * datagram_capacity, max_datagram_bytes, resource),
      requests(datagram_capacity, resource),
      responses(datagram_capacity, resource) {}

http3_connection_state::http3_connection_state(local_change_callback changed, std::pmr::memory_resource* datagram_resource)
    : changed_(changed),
      datagrams_(datagram_resource ? makePmrObject<datagram_storage>(datagram_resource, datagram_resource) : std::unique_ptr<datagram_storage, PmrObjectDeleter<datagram_storage>>{}) {}

http3_connection_state::~http3_connection_state() {
    if (!ready_to_destroy()) {
        std::terminate();
    }
}

http3_connection_state::status http3_connection_state::reserve(http3_ready_scheduler& scheduler, std::uint64_t epoch, std::uint64_t connection_generation) noexcept {
    if (admission_stopped_ || admission_ != admission_phase::vacant) {
        return status::wrong_state;
    }
    if (last_identity_ && (epoch < last_identity_->epoch ||
                              (epoch == last_identity_->epoch && connection_generation <= last_identity_->connection_generation))) {
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

http3_connection_state::status http3_connection_state::bind(http3_connection_identity identity, connection_metadata_view metadata, Http3Settings settings, std::size_t max_quic_datagram_payload_bytes) noexcept {
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

http3_connection_state::status http3_connection_state::attach_handler(http3_connection_identity identity, Http3ServerConnection& connection) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (admission_ != admission_phase::bound || admission_stopped_) {
        return status::wrong_state;
    }
    if (!scheduler_->attach(registration_->token, connection)) {
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
    if (admission_ != admission_phase::bound || !scheduler_->abandon(registration_->token)) {
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
    if (admission_ != admission_phase::reserved || !scheduler_->abandon(registration_->token)) {
        return status::wrong_state;
    }
    admission_ = admission_phase::revoked;
    worker_draining_ = true;
    signal_change();
    return status::changed;
}

std::optional<Http3ServerConnection::EventResult> http3_connection_state::accept_peer_stream_data(
    http3_stream_id id, std::span<const std::byte> bytes) {
    if (!matches({id.epoch, id.connection_generation}) ||
        admission_ != admission_phase::handler_attached || !connection_ ||
        transport_retired_ || worker_finalized_ || slot_reusable_) {
        return std::nullopt;
    }
    return connection_->accept_peer_stream_data(id, bytes);
}

std::optional<Http3ServerConnection::EventResult> http3_connection_state::accept_peer_stream_control(
    const http3_stream_control& control) {
    if (!matches({control.id.epoch, control.id.connection_generation}) ||
        admission_ != admission_phase::handler_attached || !connection_ ||
        transport_retired_ || worker_finalized_ || slot_reusable_ ||
        !isHttp3ClientUnidirectionalStreamId(control.id.stream_id) ||
        (control.kind != http3_stream_control::kind::stream_fin &&
            control.kind != http3_stream_control::kind::stream_reset)) {
        return std::nullopt;
    }
    return connection_->acceptControl(control);
}

void http3_connection_state::set_transport_executor(transport_executor executor) noexcept {
    executor_ = executor;
}

http3_connection_state::intent_execution_result http3_connection_state::retired_intent(const Http3ServerConnection::TransportIntent& intent) noexcept {
    intent_execution_result result{.outcome = execution_outcome::transport_retired};
    if (intent.token.kind == Http3ServerConnection::TransportIntentKind::kOpenPushStream) {
        result.push_stream = Http3ServerConnection::PushStreamOpenResult{.status = Http3ServerConnection::PushStreamOpenResult::Status::kStopped};
    }
    return result;
}

http3_connection_state::intent_execution_result http3_connection_state::execute_intent(http3_connection_identity identity, const Http3ServerConnection::TransportIntent& intent) noexcept {
    if (!matches(identity) || intent.token.id.epoch != identity.epoch || intent.token.id.connection_generation != identity.connection_generation) {
        return {.outcome = execution_outcome::stale};
    }
    if (admission_ != admission_phase::handler_attached || slot_reusable_) {
        return {.outcome = execution_outcome::invalid};
    }
    const auto kind = intent.token.kind;
    if (kind != Http3ServerConnection::TransportIntentKind::kConnectionClose &&
        kind != Http3ServerConnection::TransportIntentKind::kStreamReset &&
        kind != Http3ServerConnection::TransportIntentKind::kOpenPushStream) {
        return {.outcome = execution_outcome::invalid};
    }
    if (kind == Http3ServerConnection::TransportIntentKind::kOpenPushStream &&
        (!intent.token.id.push_id || !isHttp3RequestStreamId(intent.token.id.stream_id) || intent.token.sequence == 0)) {
        return {.outcome = execution_outcome::invalid};
    }
    if (transport_retired_) {
        return retired_intent(intent);
    }
    if (!executor_.execute) {
        return {.outcome = execution_outcome::unavailable};
    }
    auto result = executor_.execute(executor_.context, identity, intent);
    // A close/reset callback can physically tear down this generation. The
    // offered token must still settle, without executing against a replacement.
    if (!matches(identity)) {
        return {.outcome = execution_outcome::stale};
    }
    if (transport_retired_) {
        return retired_intent(intent);
    }
    if (result.outcome == execution_outcome::transport_retired ||
        (result.completed() && ((kind == Http3ServerConnection::TransportIntentKind::kOpenPushStream) != result.push_stream.has_value())) ||
        (result.push_stream && result.push_stream->status == Http3ServerConnection::PushStreamOpenResult::Status::kOpened &&
            http3StreamIdType(result.push_stream->streamId) != Http3StreamIdType::kServerUnidirectional)) {
        return {.outcome = execution_outcome::invalid};
    }
    return result;
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
        !connection_->drainReady(admission_seal_->expected_admitted_requests)) {
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
        !scheduler_->begin_retirement(registration_->token)) {
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
    if (connection_ && !connection_->confirmTransportRetired({.epoch = identity.epoch, .connectionGeneration = identity.connection_generation})) {
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
    if (!transport_retired_ || !worker_finalized_ || slot_reusable_ || (datagrams_ && datagrams_->pool.outstanding() != 0)) {
        return status::wrong_state;
    }
    if (connection_ && !scheduler_->retire(registration_->token)) {
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
    return (admission_ == admission_phase::vacant || slot_reusable_) && (!datagrams_ || datagrams_->pool.outstanding() == 0);
}

bool http3_connection_state::matches(http3_connection_identity identity) const noexcept {
    return identity_ && *identity_ == identity;
}

void http3_connection_state::signal_change() noexcept {
    if (changed_.changed) {
        changed_.changed(changed_.context);
    }
}

void http3_connection_state::return_datagram(void* context, buffer_credit credit) noexcept {
    auto& state = *static_cast<http3_connection_state*>(context);
    state.datagrams_->pool.reclaim(std::move(credit));
    state.signal_change();
}

http3_connection_state::status http3_connection_state::publish_datagram(bool request, http3_connection_identity identity, std::uint64_t stream_id, std::span<const std::byte> bytes) noexcept {
    if (!matches(identity)) {
        return status::stale;
    }
    if (admission_ != admission_phase::handler_attached || transport_retired_ || worker_finalized_) {
        return status::wrong_state;
    }
    if (!datagrams_ || !isHttp3RequestStreamId(stream_id) || bytes.size() > max_datagram_bytes) {
        return status::unavailable;
    }
    auto& queue = request ? datagrams_->requests : datagrams_->responses;
    auto* slot = queue.prepare_push();
    if (!slot) {
        return status::full;
    }
    auto lease = datagrams_->pool.try_acquire({this, return_datagram});
    if (!lease) {
        queue.cancel_push();
        return status::full;
    }
    std::copy(bytes.begin(), bytes.end(), lease->bytes().begin());
    *slot = datagram{identity, stream_id, std::move(*lease), bytes.size()};
    queue.commit_push();
    signal_change();
    return status::changed;
}

http3_connection_state::status http3_connection_state::pop_datagram(bool request, datagram& value) noexcept {
    if (!datagrams_) {
        return status::empty;
    }
    auto& queue = request ? datagrams_->requests : datagrams_->responses;
    auto* slot = queue.front();
    if (!slot) {
        return status::empty;
    }
    value = std::move(*slot);
    queue.pop();
    return matches(value.identity) ? status::changed : status::stale;
}

void http3_connection_state::discard_datagrams() noexcept {
    if (!datagrams_) {
        return;
    }
    for (auto* queue : {&datagrams_->requests, &datagrams_->responses}) {
        while (auto* value = queue->front()) {
            value->storage.reset();
            queue->pop();
        }
    }
}

http3_connection_state::status http3_connection_state::publish_request_datagram(http3_connection_identity identity, std::uint64_t stream_id, std::span<const std::byte> bytes) noexcept {
    return publish_datagram(true, identity, stream_id, bytes);
}

http3_connection_state::status http3_connection_state::publish_response_datagram(http3_connection_identity identity, std::uint64_t stream_id, std::span<const std::byte> bytes) noexcept {
    return publish_datagram(false, identity, stream_id, bytes);
}

http3_connection_state::status http3_connection_state::pop_request_datagram(datagram& value) noexcept {
    return pop_datagram(true, value);
}

http3_connection_state::status http3_connection_state::pop_response_datagram(datagram& value) noexcept {
    return pop_datagram(false, value);
}

}  // namespace ruvia::detail
