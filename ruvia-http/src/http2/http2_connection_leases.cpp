#include <exception>

#include "http2/http2_connection.h"
#include "http2/http2_remote_receive_semantics.h"

namespace ruvia::detail {

void http2_connection::lease_submitted_request(const http2_request_head_submit_result& result) {
    const auto* submitted = result.submitted();
    if (submitted == nullptr) {
        if (result.failure()->error() == http2_request_head_submit_error::connection_not_started) {
            std::terminate();
        }
        return;
    }
    try {
        pin_stream(submitted->stream_id());
    } catch (...) {
        const auto original = std::current_exception();
        try {
            (void)submit_reset(submitted->stream_id(), http2_error_code::cancel);
            // Preserve the original pinning failure; this reset is rollback only.
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (...) {
        }
        std::rethrow_exception(original);
    }
}

void http2_connection::release_stream_lease(std::uint32_t stream_id) {
    const auto* stream = find_stream(stream_id);
    if ((stream != nullptr && stream->window_debt() != 0) || has_pending_events(stream_id)) {
        leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::after_credits);
        return;
    }
    unpin_stream(stream_id);
    leases_.erase_release(stream_id);
}

void http2_connection::release_stream_lease_after_tunnel_end(std::uint32_t stream_id) noexcept {
    leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::after_tunnel_end);
}

void http2_connection::abandon_stream_lease_checked(std::uint32_t stream_id) {
    const auto* stream = find_stream(stream_id);
    if (stream == nullptr) {
        leases_.erase_release(stream_id);
        return;
    }
    if (!stream->is_aborted()) {
        (void)submit_reset(stream_id, http2_error_code::cancel);
    }
    release_stream_lease(stream_id);
}

void http2_connection::abandon_stream_lease(std::uint32_t stream_id) noexcept {
    try {
        abandon_stream_lease_checked(stream_id);
    } catch (...) {
        leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::abandon);
    }
}

void http2_connection::abandon_received_data(std::uint32_t stream_id, std::uint32_t bytes) noexcept {
    try {
        (void)release_received_data(stream_id, bytes);
        retry_deferred_stream_lease(stream_id);
    } catch (...) {
        leases_.defer_credit(stream_id, bytes);
    }
}

bool http2_connection::acknowledge_received_data(std::uint32_t stream_id, std::uint32_t bytes) {
    if (!release_received_data(stream_id, bytes)) {
        return false;
    }
    retry_deferred_stream_lease(stream_id);
    return true;
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
    const auto* stream = find_stream(stream_id);
    if ((stream != nullptr && (stream->window_debt() != 0 ||
                                  (pending->kind_ == kind_type::after_tunnel_end &&
                                      !connection_error() && !http2_stream_is_closed(*stream)))) ||
        has_pending_events(stream_id)) {
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
        const auto stream_id = leases_.release_stream_at(index);
        retry_deferred_stream_lease(stream_id);
        if (index < leases_.release_count() && leases_.release_stream_at(index) == stream_id) {
            ++index;
        }
    }
}

}  // namespace ruvia::detail
