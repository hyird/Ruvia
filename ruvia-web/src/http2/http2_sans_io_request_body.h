#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http2_connection.h"

#include "server/inbound_buffer_budget.h"

namespace ruvia::detail {

enum class http2_body_queue_store_status : std::uint8_t {
    stored,
    backlog_overflow,
    buffer_limit,
};

// Web-owned storage for body/tunnel bytes that have already crossed the
// http2_connection event boundary. The HTTP/2 core owns framing and flow-control
// debt; this queue owns only runtime buffering for a suspended route handler.
// Pending DATA is concatenated so each frame is copied once, not into a new
// string per frame. pop() swaps the pending buffer aside so enqueue cannot
// invalidate the borrowed view.
class http2_sans_io_body_queue final {
public:
    explicit http2_sans_io_body_queue(inbound_buffer_account* inbound = nullptr) noexcept
        : queued_reservation_(inbound),
          active_reservation_(inbound) {}

    void enqueue(std::string_view data, http2_received_data_credit&& credit) {
        if (data.empty()) {
            return;
        }
        // Transfer credit only after append succeeds; allocation failure leaves
        // the incoming token with its owner for unwinding.
        inbound_append(queued_chunk_, queued_reservation_, data);
        if (!credit.valid()) {
            return;
        }
        if (!queued_credit_.has_value()) {
            queued_credit_.emplace(std::move(credit));
            return;
        }
        queued_credit_->merge(std::move(credit));
    }

    [[nodiscard]] http2_body_queue_store_status enqueue_bounded(std::string_view data,
        http2_received_data_credit&& credit, std::size_t backlog_limit) {
        const auto queued_bytes = queued_chunk_.size();
        if (queued_bytes > backlog_limit || data.size() > backlog_limit - queued_bytes) {
            return http2_body_queue_store_status::backlog_overflow;
        }
        try {
            enqueue(data, std::move(credit));
        } catch (const inbound_buffer_limit_error&) {
            return http2_body_queue_store_status::buffer_limit;
        }
        return http2_body_queue_store_status::stored;
    }

    // Release credits only after the consumer has copied/finished the active view.
    void release_active_credits() noexcept {
        active_credit_.reset();
    }

    [[nodiscard]] bool empty() const noexcept {
        return queued_chunk_.empty();
    }

    // The message-end event for this body has been delivered. The core's own
    // receive state can run ahead of event delivery within one input batch, so
    // readers end on this flag rather than on the stream's receive status.
    void mark_ended() noexcept {
        ended_ = true;
    }

    [[nodiscard]] bool ended() const noexcept {
        return ended_;
    }

    // The returned view remains valid until the next pop().
    [[nodiscard]] std::string_view pop() & {
        inbound_clear_retaining_small(active_chunk_, active_reservation_);
        release_active_credits();
        if (queued_chunk_.empty()) {
            return {};
        }
        active_chunk_.swap(queued_chunk_);
        active_reservation_.swap(queued_reservation_);
        if (queued_credit_.has_value()) {
            active_credit_.emplace(std::move(*queued_credit_));
            queued_credit_.reset();
        }
        inbound_clear_retaining_small(queued_chunk_, queued_reservation_);
        return std::string_view(active_chunk_);
    }
    std::string_view pop() && = delete;

    void discard() noexcept {
        inbound_discard(queued_chunk_, queued_reservation_);
        inbound_discard(active_chunk_, active_reservation_);
        queued_credit_.reset();
        active_credit_.reset();
    }

private:
    // The byte storage is destroyed before its credits, so return-window side
    // effects cannot outlive the copied data they account for.
    std::optional<http2_received_data_credit> queued_credit_;
    std::optional<http2_received_data_credit> active_credit_;
    std::string queued_chunk_;
    inbound_buffer_reservation queued_reservation_;
    std::string active_chunk_;
    inbound_buffer_reservation active_reservation_;
    bool ended_{false};
};

}  // namespace ruvia::detail
