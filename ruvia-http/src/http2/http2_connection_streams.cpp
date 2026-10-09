#include <utility>

#include "http2/http2_connection.h"
#include "http2/http2_frame_codec.h"
#include "http2/http2_frame_payload.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_response_headers.h"
#include "http2/http2_window_update.h"

// The stream table and a stream's life: creating and finding one, pinning it
// while the owner still holds a reference, closing it, and the RST_STREAM
// handling that includes the rapid-reset budget.

namespace ruvia::detail {

namespace {

// Rapid-reset (CVE-2023-44487): a peer can open a stream, let HEADERS complete so the
// owner spawns a handler, then RST it to free the concurrency slot -- repeating forever
// without ever tripping the 128-stream cap. We allow the peer to reset up to this many
// MORE streams than it has let run to completion; a flood that never lets a response
// finish climbs to the cap and trips, while legitimate cancels interleaved with
// completed responses keep refilling the budget and never trip.
constexpr std::uint32_t http2_max_unprocessed_resets = 1000;
constexpr char http2_rapid_reset_goaway_debug[] = "excessive stream resets";
constexpr std::size_t http2_rapid_reset_goaway_bytes =
    http2_frame_header_bytes + 8 + sizeof(http2_rapid_reset_goaway_debug) - 1;

}  // namespace

bool http2_connection::is_pinned(std::uint32_t stream_id) const noexcept {
    return std::ranges::find(pinned_streams_, stream_id) != pinned_streams_.end();
}

void http2_connection::pin_stream(std::uint32_t stream_id) {
    if (!is_pinned(stream_id)) {
        pinned_streams_.push_back(stream_id);
    }
}

void http2_connection::detach_active_header_block(http2_stream_state& stream) {
    if (!header_continuation_.matches(stream.id())) {
        return;
    }
    if (header_continuation_.kind() == http2_header_block_kind::discarded) {
        // An owner-side terminal transition won the race while an already-invalid
        // block was still awaiting CONTINUATION. The RST just emitted is now the final
        // local frame; finish decoding the peer block later without emitting another.
        discarded_header_action_ = discarded_header_action_type::ignore;
        return;
    }
    if (discarded_header_stream_) {
        append_goaway(http2_error_code::protocol_error, "overlapping detached HEADERS state");
        return;
    }
    discarded_header_stream_.emplace(stream.id(), resource_);
    // Both strings use the connection resource, so the partial compressed block moves
    // without allocation. Future CONTINUATION fragments complete it in detached state.
    discarded_header_stream_->remote_header_block().swap(stream.remote_header_block());
    discarded_header_action_ = discarded_header_action_type::ignore;
    header_continuation_.start(stream.id(), http2_header_block_kind::discarded);
}

void http2_connection::unpin_stream(std::uint32_t stream_id) {
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr) {
        std::erase(pinned_streams_, stream_id);
        return;  // never created, or already removed
    }
    if (stream->is_aborted()) {
        // A pinned stream may still have exact public DATA credits in flight.
        // Returning the owner lease is the fallback that settles any abandoned
        // debt before storage is finally removed.
        flush_window_debt(*stream);
        std::erase(pinned_streams_, stream_id);
        release_local_request_stream(*stream);
        streams_.remove(stream_id);
        return;
    }

    if (http2_stream_is_closed(*stream)) {
        // Both protocol halves are closed and the owner lease is gone: normal
        // completion can finally release storage and refill the rapid-reset budget.
        flush_window_debt(*stream);
        detach_active_header_block(*stream);
        discard_deferred_stream_state(stream_id);
        ready_queue_.remove(stream_id);
        closed_streams_.remember(stream_id, http2_stream_close_source::local);
        ++completed_responses_;
        release_local_request_stream_if_closed(*stream);
        std::erase(pinned_streams_, stream_id);
        streams_.remove(stream_id);
        return;
    }

    // Releasing the last owner while either protocol half is still open must not
    // silently erase the stream. Abort it explicitly so the peer observes a legal
    // terminal transition and the table cannot leak an ownerless half-open stream.
    // Keep the pin until submit_reset() finishes: if appending the RST_STREAM throws,
    // the caller still owns request-view storage and can retry unpinning.
    const auto error = stream->local_send().end_stream_committed() != nullptr
                           ? http2_error_code::no_error
                           : http2_error_code::cancel;
    (void)submit_reset(stream_id, error);
    if (auto* retained_stream = streams_.find(stream_id);
        retained_stream != nullptr && retained_stream->is_aborted()) {
        flush_window_debt(*retained_stream);
        release_local_request_stream(*retained_stream);
        std::erase(pinned_streams_, stream_id);
        streams_.remove(stream_id);
        return;
    }
    std::erase(pinned_streams_, stream_id);
}

void http2_connection::discard_deferred_stream_state(std::uint32_t stream_id) {
    std::erase_if(pending_sends_,
        [stream_id](const http2_pending_send& pending) { return pending.stream_id_ == stream_id; });
    std::erase(drained_data_streams_, stream_id);
    if (auto* stream = streams_.find(stream_id); stream != nullptr) {
        http2_release_local_header_block(*stream);
    }
}

bool http2_connection::close_stream_impl(std::uint32_t stream_id, http2_stream_close_source source_value,
    http2_error_code error, close_notification_type notification) {
    auto* stream = streams_.find(stream_id);
    if (stream != nullptr && !stream->is_aborted()) {
        // The terminal transition below is allocation-free only after its two
        // externally visible side effects have been reserved. In particular,
        // do not abort the stream and then discover that publishing its close
        // event or returning its connection-level receive credit needs storage.
        if (notification == close_notification_type::emit_event) {
            reserve_event_slots(1);
        }
        if (!is_pinned(stream_id)) {
            const auto debt = stream->window_debt();
            if (debt != 0 && connection_receive_credit_.ready_after(debt)) {
                output_.reserve_additional(http2_window_update_frame_bytes);
            }
        }
    }
    if (stream != nullptr && !stream->is_aborted()) {
        detach_active_header_block(*stream);
    }
    ready_queue_.remove(stream_id);
    discard_deferred_stream_state(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return false;
    }

    release_local_request_stream(*stream);
    priorities_.erase(stream_id);
    (void)stream->abort(source_value);
    if (notification == close_notification_type::emit_event) {
        events_.push_back(http2_event::stream_closed(stream_id, source_value, error));
    }
    // A public DATA credit is exact: a pinned closed stream remains the ledger
    // until the credit is acknowledged or its token is abandoned. Unpinned
    // internal consumers have no external acknowledgement token, so their
    // window debt is settled immediately.
    if (!is_pinned(stream_id)) {
        flush_window_debt(*stream);
    }
    closed_streams_.remember(stream_id, source_value);
    if (!is_pinned(stream_id)) {
        streams_.remove(stream_id);
    }
    return true;
}

bool http2_connection::close_stream(
    std::uint32_t stream_id, http2_stream_close_source source_value, http2_error_code error) {
    return close_stream_impl(stream_id, source_value, error, close_notification_type::emit_event);
}

bool http2_connection::close_stream_by_owner(std::uint32_t stream_id) {
    return close_stream_impl(stream_id, http2_stream_close_source::local, http2_error_code::no_error,
        close_notification_type::owner_already_knows);
}

bool http2_connection::was_closed_by_peer_reset(
    std::uint32_t stream_id, const http2_stream_state* retained_stream) const noexcept {
    if (retained_stream != nullptr) {
        return retained_stream->is_aborted() &&
               retained_stream->local_send().aborted()->source() == http2_stream_close_source::peer;
    }
    return closed_streams_.source(stream_id) == http2_stream_close_source::peer;
}

bool http2_connection::process_rst_stream(const http2_frame_header& header_value, std::string_view payload_value) {
    if (payload_value.size() != 4) {
        append_goaway(http2_error_code::frame_size_error, "invalid RST_STREAM");
        return false;
    }
    if (header_value.stream_id_ == 0) {
        append_goaway(http2_error_code::protocol_error, "RST_STREAM stream id must be nonzero");
        return false;
    }
    auto* const stream = streams_.find(header_value.stream_id_);
    if (stream == nullptr && is_idle_stream_id(header_value.stream_id_)) {
        append_goaway(http2_error_code::protocol_error, "RST_STREAM on idle stream");
        return false;
    }
    if (was_closed_by_peer_reset(header_value.stream_id_, stream)) {
        // This peer's two resets are ordered on the same connection, so the
        // second cannot have been sent before it knew that it had terminated
        // the stream. Enforce the same peer-reset finality as DATA/HEADERS and
        // avoid counting a duplicate as another rapid-reset lifecycle.
        append_goaway(http2_error_code::stream_closed, "RST_STREAM after peer RST_STREAM");
        return false;
    }
    if (stream != nullptr && http2_stream_is_closed(*stream)) {
        // RST_STREAM can race with END_STREAM or a reset sent by this endpoint.
        // The stream was already terminal, so minimally process without charging
        // another peer-reset lifecycle.
        return true;
    }
    if (closed_streams_.source(header_value.stream_id_) == http2_stream_close_source::peer_goaway) {
        // The peer already declared this request unprocessed. A trailing reset has no
        // stream lifecycle left to mutate and must not consume the rapid-reset budget.
        return true;
    }
    const auto error = static_cast<http2_error_code>(
        http2_read32(reinterpret_cast<const unsigned char*>(payload_value.data())));
    // Rapid-reset budget (CVE-2023-44487): count peer resets and trip if they run too far
    // ahead of the responses this connection has actually let complete.
    const auto reset_count_after_this_frame = static_cast<std::uint64_t>(peer_reset_streams_) + 1U;
    const auto reset_budget =
        static_cast<std::uint64_t>(completed_responses_) + http2_max_unprocessed_resets;
    const bool rapid_reset_budget_exceeded = reset_count_after_this_frame > reset_budget;
    if (rapid_reset_budget_exceeded) {
        std::size_t close_output_bytes = 0;
        if (stream != nullptr) {
            const auto debt = stream->window_debt();
            if (debt != 0 && connection_receive_credit_.ready_after(debt)) {
                close_output_bytes = http2_window_update_frame_bytes;
            }
        }
        output_.reserve_additional(close_output_bytes + http2_rapid_reset_goaway_bytes);
    }
    if (!close_stream(header_value.stream_id_, http2_stream_close_source::peer, error)) {
        // A reset can race with one sent by this endpoint. RFC 9113 requires
        // minimal processing in that state, but the no-op neither opened a
        // handler slot nor freed one and therefore must not spend the
        // rapid-reset lifecycle budget.
        return true;
    }
    ++peer_reset_streams_;
    if (rapid_reset_budget_exceeded) {
        append_goaway(http2_error_code::enhance_your_calm, http2_rapid_reset_goaway_debug);
        return false;
    }
    return true;
}

http2_stream_state* http2_connection::find_stream(std::uint32_t stream_id) noexcept {
    return streams_.find(stream_id);
}

http2_stream_state* http2_connection::create_stream(std::uint32_t stream_id) {
    return streams_.create(stream_id, peer_settings_.initial_window_size());
}

std::optional<http2_request_head_submit_error> http2_connection::local_request_admission_error()
    const noexcept {
    if (role_ != http2_role::client) {
        return http2_request_head_submit_error::invalid_state;
    }
    if (preface_phase_ == preface_phase_type::not_started) {
        return http2_request_head_submit_error::connection_not_started;
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_.has_value() ||
        next_local_stream_id_ > 0x7fffffffU) {
        return http2_request_head_submit_error::connection_unavailable;
    }
    if (active_local_request_streams_ >= peer_settings_.max_concurrent_streams()) {
        return http2_request_head_submit_error::peer_stream_limit_reached;
    }
    return std::nullopt;
}

http2_stream_state* http2_connection::admit_local_request_stream() {
    auto* stream = create_stream(next_local_stream_id_);
    if (stream != nullptr) {
        next_local_stream_id_ += 2;
    }
    return stream;
}

http2_request_head_submit_result http2_connection::publish_local_request_head(
    http2_stream_state& stream) noexcept {
    if (stream.hold_peer_concurrency_slot()) {
        ++active_local_request_streams_;
    }
    http2_release_local_header_block(stream);
    return http2_request_head_submit_result::make_submitted(stream.id());
}

void http2_connection::release_local_request_stream(http2_stream_state& stream) noexcept {
    if (stream.release_peer_concurrency_slot() && active_local_request_streams_ != 0) {
        --active_local_request_streams_;
    }
}

void http2_connection::release_local_request_stream_if_closed(http2_stream_state& stream) noexcept {
    if (http2_stream_is_closed(stream)) {
        release_local_request_stream(stream);
    }
}

void http2_connection::retire_completed_local_push(std::uint32_t stream_id) {
    if (role_ != http2_role::server || (stream_id & 1U) != 0 || is_pinned(stream_id) || has_pending_events(stream_id)) {
        return;
    }
    const auto* stream = find_stream(stream_id);
    if (stream != nullptr && http2_stream_is_closed(*stream)) {
        unpin_stream(stream_id);
    }
}

bool http2_connection::is_idle_stream_id(std::uint32_t stream_id) const noexcept {
    if (role_ == http2_role::client) {
        return (stream_id & 1U) == 0 ? stream_id > last_peer_push_stream_id_ : stream_id >= next_local_stream_id_;
    }
    return (stream_id & 1U) == 0 ? stream_id >= next_push_stream_id_ : http2_is_idle_stream(stream_id, last_stream_id_);
}

http2_stream_state* http2_connection::stream(std::uint32_t stream_id) & noexcept {
    return streams_.find(stream_id);
}

http2_stream_receive_status http2_connection::stream_receive_status(
    std::uint32_t stream_id) const noexcept {
    const auto* current = streams_.find(stream_id);
    if (current == nullptr || current->is_aborted()) {
        return http2_stream_receive_status::closed;
    }
    return current->remote_receive().end_stream() != nullptr
               ? http2_stream_receive_status::ended
               : http2_stream_receive_status::open;
}

}  // namespace ruvia::detail
