#include "http2/http2_connection.h"
#include "http2/http2_remote_receive_semantics.h"

namespace ruvia::detail {

void http2_connection::lease_submitted_request(
    const std::variant<std::uint32_t, http2_request_head_submit_error>& result) noexcept {
    const auto* submitted = std::get_if<std::uint32_t>(&result);
    if (submitted == nullptr) {
        return;
    }
    // Pinning only marks the stream, so a submitted request is always leased.
    pin_stream(*submitted);
}

void http2_connection::release_stream_lease(std::uint32_t stream_id) {
    const auto* stream = streams_.find(stream_id);
    if (stream != nullptr && (stream->window_debt() != 0 || stream->has_pending_events())) {
        leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::after_credits);
        return;
    }
    unpin_stream(stream_id);
    leases_.erase_release(stream_id);
}

void http2_connection::release_stream_lease_after_remote_end(std::uint32_t stream_id) noexcept {
    leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::after_remote_end);
}

void http2_connection::abandon_stream_lease_checked(std::uint32_t stream_id) {
    const auto* stream = streams_.find(stream_id);
    if (stream == nullptr) {
        leases_.erase_release(stream_id);
        return;
    }
    if (!stream->is_aborted() && !http2_stream_is_closed(*stream)) {
        const auto error = stream->local_send().end_stream_committed() != nullptr
                               ? http2_error_code::no_error
                               : http2_error_code::cancel;
        (void)submit_reset(stream_id, error);
    }
    release_stream_lease(stream_id);
}

void http2_connection::abandon_received_data(std::uint32_t stream_id, std::uint32_t bytes) noexcept {
    try {
        (void)release_received_data(stream_id, bytes);
        retry_deferred_stream_lease(stream_id);
    } catch (...) {
        leases_.defer_credit(stream_id, bytes);
    }
    flush_output();
}

bool http2_connection::acknowledge_received_data(std::uint32_t stream_id, std::uint32_t bytes) {
    bool released = false;
    try {
        released = release_received_data(stream_id, bytes);
    } catch (...) {
        leases_.defer_credit(stream_id, bytes);
        throw;
    }
    if (!released) {
        return false;
    }
    retry_deferred_stream_lease(stream_id);
    flush_session();
    return true;
}

void http2_connection::abandon_stream_lease(std::uint32_t stream_id) noexcept {
    try {
        abandon_stream_lease_checked(stream_id);
    } catch (...) {
        leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::abandon);
    }
    flush_output();
}

void http2_connection::flush_output() noexcept {
    try {
        flush_session();
        // A failed flush has already failed the connection; the next feed() or
        // output drain observes that terminal state.
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
}

void http2_connection::retry_deferred_stream_lease(std::uint32_t stream_id) noexcept {
    const auto* pending = leases_.release(stream_id);
    if (pending == nullptr) {
        return;
    }
    using kind_type = http2_stream_lease_ledger::release_kind_type;
    if (pending->kind_ == kind_type::abandon) {
        try {
            abandon_stream_lease_checked(stream_id);
            // The deferred entry remains queued for the next noexcept retry.
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (...) {
        }
        return;
    }
    const auto* stream = streams_.find(stream_id);
    if (stream != nullptr &&
        (stream->window_debt() != 0 || stream->has_pending_events() ||
            (pending->kind_ == kind_type::after_remote_end && !connection_error() &&
                !http2_stream_is_closed(*stream)))) {
        return;
    }
    try {
        unpin_stream(stream_id);
        leases_.erase_release(stream_id);
        // The deferred entry remains queued for the next noexcept retry.
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
}

void http2_connection::retry_deferred_stream_leases() noexcept {
    std::size_t index = 0;
    while (index < leases_.credit_count()) {
        const auto pending = leases_.credit_at(index);
        try {
            (void)release_received_data(pending.stream_id_, pending.bytes_);
            leases_.erase_credit(pending.stream_id_);
            retry_deferred_stream_lease(pending.stream_id_);
        } catch (...) {
            ++index;
        }
    }
    index = 0;
    while (index < leases_.release_count()) {
        const auto release = leases_.release_at(index);
        retry_deferred_stream_lease(release.stream_id_);
        if (index < leases_.release_count() && leases_.release_at(index).stream_id_ == release.stream_id_) {
            ++index;
        }
    }
    flush_output();
}

}  // namespace ruvia::detail
