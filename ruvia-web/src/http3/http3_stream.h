#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_request_head.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_request_body_error.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/web/detail/router/route_handlers.h"

#include "router/route_resolution.h"
#include "server/inbound_buffer_budget.h"
#include "server/multiplexed_stream_signal.h"

// Web-side state of one HTTP/3 request stream. The QUIC/HTTP/3 core owns
// framing, QPACK and flow control; this runtime owns what a route needs: the
// validated head, the request body as the route consumes it (buffered or
// streamed, with flow-control credit returned only as bytes are consumed), the
// trailers, and the wake/stop signal a dispatched handler waits on.

namespace ruvia::detail {

enum class http3_body_store_status : std::uint8_t {
    stored,
    too_large,
    backlog_overflow,
    buffer_limit,
};

// Tunnel bytes taken together with the receive credit they hold; the reader
// drops the credit once it consumed the bytes.
struct http3_tunnel_chunk final {
    std::string bytes_;
    std::optional<http3_received_data_credit> credit_;
};

class http3_stream final {
public:
    http3_stream(http3_request_head head, route_resolution resolution, request_body_mode body_mode,
        inbound_buffer_account* inbound = nullptr) noexcept
        : head_(std::move(head)),
          resolution_(std::move(resolution)),
          body_mode_(body_mode),
          body_reservation_(inbound),
          active_reservation_(inbound) {}

    http3_stream(const http3_stream&) = delete;
    http3_stream& operator=(const http3_stream&) = delete;

    [[nodiscard]] std::int64_t id() const noexcept {
        return head_.stream_id();
    }
    [[nodiscard]] const http3_request_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    const http3_request_head& head() const&& = delete;
    [[nodiscard]] const route_resolution& resolution() const& noexcept RUVIA_LIFETIMEBOUND {
        return resolution_;
    }
    const route_resolution& resolution() const&& = delete;
    [[nodiscard]] request_body_mode body_mode() const noexcept {
        return body_mode_;
    }
    [[nodiscard]] http_request_trailers& trailers() & noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    http_request_trailers& trailers() && = delete;
    [[nodiscard]] const std::optional<http_priority>& priority() const& noexcept RUVIA_LIFETIMEBOUND {
        return priority_;
    }
    const std::optional<http_priority>& priority() const&& = delete;
    void set_priority(std::optional<http_priority> priority) noexcept {
        priority_ = priority;
    }
    void defer_dispatch() noexcept {
        dispatch_pending_ = true;
    }
    [[nodiscard]] bool dispatch_pending() const noexcept {
        return dispatch_pending_ && !dispatched();
    }
    void defer_continue() noexcept {
        continue_pending_ = true;
    }
    [[nodiscard]] bool take_continue_pending() noexcept {
        return std::exchange(continue_pending_, false);
    }

    void store_datagram(std::string_view payload, std::size_t byte_limit) {
        if ((datagrams_ && datagrams_->size() >= 64) || datagram_bytes_ > byte_limit ||
            payload.size() > byte_limit - datagram_bytes_) {
            return;  // RFC 9221 permits loss; never backpressure reliable DATA.
        }
        try {
            if (!datagrams_) {
                datagrams_.emplace();
            }
            datagrams_->emplace_back(payload);
            datagram_bytes_ += payload.size();
        } catch (const inbound_buffer_limit_error&) {
            return;
        }
        if (signal_) {
            signal_->wake();
        }
    }
    [[nodiscard]] std::optional<std::string> pop_datagram() {
        if (!datagrams_ || datagrams_->empty()) {
            return std::nullopt;
        }
        auto payload = std::move(datagrams_->front());
        datagrams_->pop_front();
        datagram_bytes_ -= payload.size();
        return payload;
    }
    [[nodiscard]] http3_tunnel_chunk take_tunnel_bytes() {
        http3_tunnel_chunk result{std::string{}, std::exchange(body_credit_, std::nullopt)};
        result.bytes_.swap(body_);
        body_reservation_.clear();
        return result;
    }

    // Buffered body: the whole content, borrowed by the request at dispatch;
    // its credit returns as it is stored. Streaming body and tunnel: the bytes
    // queued for the reader, holding their credit until it takes them.
    [[nodiscard]] http3_body_store_status store(std::string_view data, http3_received_data_credit&& credit,
        protocol_byte_limit total_limit, std::size_t backlog_limit) {
        if (http_request_body_addition_failure(received_bytes_, data.size(), total_limit).has_value()) {
            return http3_body_store_status::too_large;
        }
        if (body_mode_ == request_body_mode::stream &&
            (body_.size() > backlog_limit || data.size() > backlog_limit - body_.size())) {
            return http3_body_store_status::backlog_overflow;
        }
        try {
            inbound_append(body_, body_reservation_, data);
        } catch (const inbound_buffer_limit_error&) {
            return http3_body_store_status::buffer_limit;
        }
        received_bytes_ += data.size();
        if (body_mode_ == request_body_mode::stream && credit.valid()) {
            if (body_credit_) {
                body_credit_->merge(std::move(credit));
            } else {
                body_credit_.emplace(std::move(credit));
            }
        }
        return http3_body_store_status::stored;
    }

    [[nodiscard]] std::string_view buffered_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    std::string_view buffered_body() const&& = delete;

    // Streaming reader: hands out every queued byte as one chunk, valid until
    // the next pop(). The previous chunk's flow-control credit returns now;
    // true when it held any, so the caller can let the writer send it.
    [[nodiscard]] std::pair<std::string_view, bool> pop() & RUVIA_LIFETIMEBOUND {
        const bool returned = active_credit_.has_value();
        active_credit_.reset();
        if (body_credit_) {
            active_credit_.emplace(std::move(*body_credit_));
            body_credit_.reset();
        }
        inbound_clear_retaining_small(active_chunk_, active_reservation_);
        active_chunk_.swap(body_);
        active_reservation_.swap(body_reservation_);
        return {active_chunk_, returned};
    }
    std::pair<std::string_view, bool> pop() && = delete;

    // The handler finished: the content's storage goes back to the
    // connection's inbound budget, and credit for content received but never
    // read returns so a stopped reader cannot strand connection-level credit.
    // True when any credit returned.
    bool discard_body() noexcept {
        const bool returned = body_credit_.has_value() || active_credit_.has_value();
        body_credit_.reset();
        active_credit_.reset();
        inbound_discard(body_, body_reservation_);
        inbound_discard(active_chunk_, active_reservation_);
        datagrams_.reset();
        datagram_bytes_ = 0;
        return returned;
    }

    void mark_body_failure(http_request_body_failure failure) noexcept {
        if (!body_failure_.has_value()) {
            body_failure_ = failure;
        }
    }
    [[nodiscard]] const std::optional<http_request_body_failure>& body_failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_failure_;
    }
    const std::optional<http_request_body_failure>& body_failure() const&& = delete;

    // The request content ended (the peer's FIN, or no content at all).
    void mark_request_ended() noexcept {
        request_ended_ = true;
        request_body_deadline_.close();
    }
    [[nodiscard]] bool request_ended() const noexcept {
        return request_ended_;
    }

    void mark_request_body_open(std::int64_t now_ms) noexcept {
        request_body_deadline_.open(now_ms);
    }
    void note_request_body_progress(std::int64_t now_ms) noexcept {
        request_body_deadline_.progress(now_ms);
    }
    void mark_request_body_closed() noexcept {
        request_body_deadline_.close();
    }
    // The request body is still open and went `inactivity` without DATA, or
    // `completion` since it opened (request_body_timeout_ and
    // body_completion_timeout_).
    [[nodiscard]] bool request_body_overdue(std::int64_t now_ms,
        const std::optional<timeout_duration>& inactivity,
        const std::optional<timeout_duration>& completion) const noexcept {
        return request_body_deadline_.overdue(now_ms, inactivity, completion);
    }

    void mark_tunnel() noexcept {
        tunnel_ = true;
    }
    [[nodiscard]] bool tunnel() const noexcept {
        return tunnel_;
    }

    void mark_response_committed() noexcept {
        response_committed_ = true;
    }
    [[nodiscard]] bool response_committed() const noexcept {
        return response_committed_;
    }

    // The peer reset the stream, or this side did: no response byte may follow.
    void mark_aborted() noexcept {
        aborted_ = true;
        if (signal_) {
            signal_->stop();
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }

    // The QUIC stream closed in both directions.
    void mark_closed() noexcept {
        closed_ = true;
    }
    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }

    [[nodiscard]] multiplexed_stream_signal* begin_dispatch(
        const worker_ref& worker_value, multiplexed_session_termination& termination) {
        if (signal_ || handler_done_) {
            return nullptr;
        }
        signal_.emplace(worker_value, termination);
        if (aborted_) {
            signal_->stop();
        }
        return &*signal_;
    }
    [[nodiscard]] bool dispatched() const noexcept {
        return signal_.has_value();
    }
    [[nodiscard]] multiplexed_stream_signal* signal() & noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    multiplexed_stream_signal* signal() && = delete;
    [[nodiscard]] const multiplexed_stream_signal* signal() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    const multiplexed_stream_signal* signal() const&& = delete;

    void mark_handler_done() noexcept {
        handler_done_ = true;
    }
    [[nodiscard]] bool handler_done() const noexcept {
        return handler_done_;
    }

private:
    http3_request_head head_;
    route_resolution resolution_;
    request_body_mode body_mode_;
    http_request_trailers trailers_;
    std::optional<http_priority> priority_{};
    // Received request content: the whole body (buffered), or the queue the
    // reader drains (streaming). active_chunk_ backs the last popped view.
    std::string body_;
    inbound_buffer_reservation body_reservation_;
    std::string active_chunk_;
    inbound_buffer_reservation active_reservation_;
    // Receive credit of body_ and active_chunk_ (streaming body or tunnel).
    std::optional<http3_received_data_credit> body_credit_;
    std::optional<http3_received_data_credit> active_credit_;
    std::optional<std::deque<std::string>> datagrams_;
    std::size_t datagram_bytes_{0};
    std::size_t received_bytes_{0};
    std::optional<http_request_body_failure> body_failure_{};
    std::optional<multiplexed_stream_signal> signal_;
    multiplexed_request_body_deadline request_body_deadline_;
    bool request_ended_{false};
    bool tunnel_{false};
    bool response_committed_{false};
    bool aborted_{false};
    bool closed_{false};
    bool handler_done_{false};
    bool dispatch_pending_{false};
    bool continue_pending_{false};
};

}  // namespace ruvia::detail
