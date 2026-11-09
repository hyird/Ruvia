#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/web/detail/router/route_handlers.h"

#include "server/inbound_buffer_resource.h"

// Where a dispatched HTTP/2 request body lives on the Web side. The protocol
// core emits ordered DATA events without knowing whether the route buffers or
// streams them; everything here applies product limits after route resolution,
// and keeps an HTTP content failure distinguishable from local backlog
// exhaustion because the two need different runtime policy.

namespace ruvia::detail {

// Web-owned storage for body/tunnel bytes that have already crossed the
// http2_connection event boundary. The HTTP/2 core owns framing and flow-control
// debt; this queue owns only runtime buffering for a suspended route handler.
// Pending DATA is concatenated so each frame is copied once, not into a new
// string per frame. pop() swaps the pending buffer aside so enqueue cannot
// invalidate the borrowed view.
enum class http2_request_body_store_status : std::uint8_t {
    stored,
    too_large,
    backlog_overflow,
};

class http2_sans_io_body_queue final {
public:
    explicit http2_sans_io_body_queue(std::pmr::memory_resource* resource = nullptr)
        : queued_chunk_(pmr_resource_or_default(resource)),
          active_chunk_(pmr_resource_or_default(resource)) {}

    void enqueue(std::string_view data, http2_received_data_credit&& credit) {
        if (data.empty()) {
            return;
        }
        const auto before = queued_chunk_.size();
        // Commit byte accounting only after append succeeds. The incoming
        // credit is still untouched if allocation throws, so its owner can
        // safely return it during unwinding.
        queued_chunk_.append(data.data(), data.size());
        queued_bytes_ += queued_chunk_.size() - before;
        if (!credit.valid()) {
            return;
        }
        if (!queued_credit_.has_value()) {
            queued_credit_.emplace(std::move(credit));
            return;
        }
        queued_credit_->merge(std::move(credit));
    }

    [[nodiscard]] bool enqueue_bounded(std::string_view data,
        http2_received_data_credit&& credit, std::size_t backlog_limit) {
        if (queued_bytes_ > backlog_limit || data.size() > backlog_limit - queued_bytes_) {
            return false;
        }
        try {
            enqueue(data, std::move(credit));
        } catch (const inbound_buffer_limit_error&) {
            return false;
        }
        return true;
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
        http_clear_pmr_string_retaining_small(active_chunk_);
        release_active_credits();
        if (queued_chunk_.empty()) {
            return {};
        }
        active_chunk_.swap(queued_chunk_);
        if (queued_credit_.has_value()) {
            active_credit_.emplace(std::move(*queued_credit_));
            queued_credit_.reset();
        }
        queued_bytes_ = 0;
        http_clear_pmr_string_retaining_small(queued_chunk_);
        return std::string_view(active_chunk_);
    }
    std::string_view pop() && = delete;

    void discard() noexcept {
        std::pmr::string(queued_chunk_.get_allocator()).swap(queued_chunk_);
        std::pmr::string(active_chunk_.get_allocator()).swap(active_chunk_);
        queued_bytes_ = 0;
        queued_credit_.reset();
        active_credit_.reset();
    }

private:
    // The byte storage is destroyed before its credits, so return-window side
    // effects cannot outlive the copied data they account for.
    std::optional<http2_received_data_credit> queued_credit_;
    std::optional<http2_received_data_credit> active_credit_;
    std::pmr::string queued_chunk_;
    std::pmr::string active_chunk_;
    std::size_t queued_bytes_{0};
    bool ended_{false};
};

}  // namespace ruvia::detail
