#include <algorithm>
#include <exception>
#include <utility>

#include "http2/http2_connection.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_request_headers.h"
#include "util/geometric_reserve.h"

// Ruvia's stream records and their life: creating and finding one, pinning it
// while the owner still holds a reference, closing it, and the resets that end
// it. nghttp2 owns the protocol stream; a record outlives it while an owner
// lease or delivered DATA credit still names it.

namespace ruvia::detail {

void http2_connection::pin_stream(std::uint32_t stream_id) noexcept {
    if (auto* const stream = streams_.find(stream_id)) {
        stream->set_pinned(true);
    }
}

void http2_connection::unpin_stream(std::uint32_t stream_id) {
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr) {
        priorities_.erase(stream_id);
        return;  // never created, or already removed
    }
    if (stream->is_aborted()) {
        // A pinned stream may still have exact public DATA credits in flight.
        // Returning the owner lease settles any abandoned debt before removal.
        flush_window_debt(*stream);
        priorities_.erase(stream_id);
        release_local_request_stream(*stream);
        streams_.remove(stream_id);
        return;
    }
    if (http2_stream_is_closed(*stream)) {
        // Both halves are closed and the owner lease is gone: normal completion
        // releases storage and refills the rapid-reset budget.
        flush_window_debt(*stream);
        ++completed_responses_;
        release_local_request_stream_if_closed(*stream);
        priorities_.erase(stream_id);
        streams_.remove(stream_id);
        return;
    }
    // Releasing the last owner while either half is still open must not silently
    // erase the stream: abort it so the peer observes a legal terminal transition.
    const auto error = stream->local_send().end_stream_committed() != nullptr
                           ? http2_error_code::no_error
                           : http2_error_code::cancel;
    (void)submit_reset(stream_id, error);
    if (auto* retained_stream = streams_.find(stream_id)) {
        flush_window_debt(*retained_stream);
        release_local_request_stream(*retained_stream);
        streams_.remove(stream_id);
    }
}

bool http2_connection::close_stream_impl(std::uint32_t stream_id, http2_stream_close_source source_value,
    http2_error_code error, close_notification_type notification) {
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return false;
    }
    if (header_block_.has_value() && &header_block_->context_.stream_ == stream) {
        // nghttp2 still decodes the rest of the block for HPACK; its fields no
        // longer reach this stream.
        header_block_.reset();
    }
    const bool pinned = stream->pinned();
    release_local_request_stream(*stream);
    priorities_.erase(stream_id);
    (void)stream->abort(source_value);
    if (notification == close_notification_type::emit_event) {
        publish_event(http2_event(http2_stream_closed_event(stream_id, source_value, error)));
    }
    // A public DATA credit is exact: a pinned closed stream remains the ledger
    // until the credit is acknowledged or abandoned. Unpinned streams have no
    // external acknowledgement token, so their debt is settled immediately.
    if (!pinned) {
        flush_window_debt(*stream);
        streams_.remove(stream_id);
    }
    return true;
}

bool http2_connection::close_stream(
    std::uint32_t stream_id, http2_stream_close_source source_value, http2_error_code error) {
    return close_stream_impl(stream_id, source_value, error, close_notification_type::emit_event);
}

void http2_connection::submit_owner_reset(std::uint32_t stream_id, http2_error_code error) {
    if (nghttp2_session_find_stream(session_.get(), static_cast<std::int32_t>(stream_id)) == nullptr) {
        // nghttp2 keeps no closed-stream history; there is nothing to reset.
        return;
    }
    reserve_geometric(owner_resets_, owner_resets_.size() + 1);
    owner_resets_.push_back(stream_id);
    check_nghttp2(nghttp2_submit_rst_stream(
        session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(stream_id), static_cast<std::uint32_t>(error)));
}

void http2_connection::reset_stream_for_peer_error(std::uint32_t stream_id, http2_error_code error) {
    // The budgets for peer-induced resets are charged when nghttp2 sends the
    // frame (before_frame_send), which also ends a flood with
    // GOAWAY(ENHANCE_YOUR_CALM).
    check_nghttp2(nghttp2_submit_rst_stream(
        session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(stream_id), static_cast<std::uint32_t>(error)));
    (void)close_stream(stream_id, http2_stream_close_source::local, error);
}

http2_stream_state* http2_connection::find_outbound_stream(std::uint32_t stream_id) noexcept {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return nullptr;
    }
    auto* stream = streams_.find(stream_id);
    return stream != nullptr && !stream->is_aborted() ? stream : nullptr;
}

http2_stream_state* http2_connection::create_stream(std::uint32_t stream_id) {
    return streams_.create(stream_id);
}

std::optional<http2_request_head_submit_error> http2_connection::local_request_admission_error()
    const noexcept {
    if (role_ != http2_role::client) {
        return http2_request_head_submit_error::invalid_state;
    }
    // The facade begins the connection before any local stream.
    if (!started_) {
        std::terminate();
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_.has_value() ||
        nghttp2_session_get_next_stream_id(session_.get()) > http2_max_stream_id) {
        return http2_request_head_submit_error::connection_unavailable;
    }
    if (active_local_request_streams_ >= peer_setting(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)) {
        return http2_request_head_submit_error::peer_stream_limit_reached;
    }
    return std::nullopt;
}

std::size_t http2_connection::available_local_request_streams(std::size_t local_limit) const noexcept {
    if (draining() || local_request_admission_error().has_value()) {
        return 0;
    }
    auto limit = (std::min)(local_limit, static_cast<std::size_t>(http2_local_settings::max_concurrent_streams));
    limit = (std::min)(limit, static_cast<std::size_t>(peer_setting(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)));
    return limit > active_local_request_streams_ ? limit - active_local_request_streams_ : 0;
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

std::optional<http2_server_request_view> http2_connection::server_request_view(
    std::uint32_t stream_id) const noexcept {
    if (role_ != http2_role::server) {
        return std::nullopt;
    }
    const auto* stream_state = streams_.find(stream_id);
    if (stream_state == nullptr || stream_state->request_method().empty()) {
        return std::nullopt;
    }
    return http2_server_request_view{
        .method_ = stream_state->request_method(),
        .scheme_ = stream_state->request_scheme(),
        .path_ = stream_state->request_path(),
        .authority_ = http2_effective_request_authority(*stream_state),
        .protocol_ = stream_state->request_protocol(),
        .pseudo_header_order_ = stream_state->request_pseudo_header_order(),
    };
}

}  // namespace ruvia::detail
