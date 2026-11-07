#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http3_request_head.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/web/detail/router/route_handlers.h"

#include "router/route_resolution.h"
#include "server/inbound_buffer_resource.h"

// Web-side state of one HTTP/3 request stream. The QUIC/HTTP/3 core owns
// framing, QPACK and flow control; this runtime owns what a route needs: the
// validated head, the request body as the route consumes it (buffered or
// streamed, with flow-control credit returned only as bytes are consumed), the
// trailers, and the wake/stop signal a dispatched handler waits on.

namespace ruvia::detail {

// Connection-owned terminal state of one HTTP/3 connection. Stream waits that
// must end on termination combine their own stop token with token().
class http3_termination final {
public:
    http3_termination() noexcept = default;
    http3_termination(const http3_termination&) = delete;
    http3_termination& operator=(const http3_termination&) = delete;

    [[nodiscard]] bool terminated() const noexcept {
        return static_cast<bool>(error_);
    }

    [[nodiscard]] std::error_code error() const noexcept {
        return error_;
    }

    [[nodiscard]] bool terminate(std::error_code error) noexcept {
        if (terminated()) {
            return false;
        }
        error_ = error ? error : std::make_error_code(std::errc::connection_aborted);
        if (stop_source_) {
            stop_source_->request_stop();
        }
        return true;
    }

    // Allocated on first use; already stopped once the connection terminated.
    [[nodiscard]] stop_token token() {
        if (!stop_source_) {
            stop_source_.emplace();
            if (terminated()) {
                stop_source_->request_stop();
            }
        }
        return stop_source_->token();
    }

private:
    std::error_code error_;
    std::optional<stop_source> stop_source_;
};

// The one wake primitive of a dispatched stream. Body readers and writers
// waiting for output budget share it and always re-check their own readiness
// after a wake.
class http3_stream_signal final {
public:
    http3_stream_signal(const worker_handle& worker_value, http3_termination& termination)
        : signal_(worker_value),
          termination_(termination) {}
    http3_stream_signal(worker_handle&&, http3_termination&) = delete;

    void wake() noexcept {
        signal_.notify();
    }

    // The stream ended abnormally (peer reset, local reset): every wait ends.
    void stop() noexcept {
        stopped_ = true;
        if (stop_source_) {
            stop_source_->request_stop();
        }
        wake();
    }

    [[nodiscard]] bool stopped() const noexcept {
        return stopped_ || termination_.terminated();
    }

    [[nodiscard]] stop_token terminal_stop_token() {
        if (!stop_source_) {
            stop_source_.emplace();
        }
        if (stopped()) {
            stop_source_->request_stop();
        }
        return stop_source_->token();
    }

    [[nodiscard]] bool terminated() const noexcept {
        return termination_.terminated();
    }

    [[nodiscard]] std::error_code terminal_error() const noexcept {
        return termination_.error();
    }

    [[nodiscard]] http3_termination& termination() & noexcept RUVIA_LIFETIMEBOUND {
        return termination_;
    }
    http3_termination& termination() && = delete;

    // Spans one whole wait for peer acknowledgements to free output budget;
    // the connection scanner times it against write_timeout_.
    void set_waiting_output(bool waiting) noexcept {
        waiting_output_ = waiting;
        output_wait_observed_ms_.reset();
        output_wait_expired_ = false;
    }

    [[nodiscard]] bool waiting_output() const noexcept {
        return waiting_output_;
    }

    // True once per wait, when it has been observed for `timeout` in the
    // scanner's clock. The first observation starts the clock.
    [[nodiscard]] bool output_wait_expired(std::int64_t now_ms, std::chrono::milliseconds timeout) noexcept {
        if (!waiting_output_ || output_wait_expired_) {
            return false;
        }
        if (!output_wait_observed_ms_.has_value()) {
            output_wait_observed_ms_ = now_ms;
        }
        output_wait_expired_ = now_ms - *output_wait_observed_ms_ >= timeout.count();
        return output_wait_expired_;
    }

    [[nodiscard]] worker_signal::wait_operation wait() {
        return signal_.wait();
    }

    [[nodiscard]] const worker_handle& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_.worker();
    }
    const worker_handle& worker() const&& = delete;

private:
    worker_signal signal_;
    http3_termination& termination_;
    std::optional<stop_source> stop_source_;
    bool stopped_{false};
    bool waiting_output_{false};
    bool output_wait_expired_{false};
    std::optional<std::int64_t> output_wait_observed_ms_;
};

enum class http3_body_store_status : std::uint8_t {
    stored,
    too_large,
    backlog_overflow,
};

class http3_stream final {
public:
    http3_stream(http3_request_head head, route_resolution resolution, request_body_mode body_mode,
        std::pmr::memory_resource* resource, std::pmr::memory_resource* body_resource)
        : head_(std::move(head)),
          resolution_(std::move(resolution)),
          body_mode_(body_mode),
          trailers_(resource),
          body_(body_resource),
          active_chunk_(body_resource) {}

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
                datagrams_.emplace(body_.get_allocator().resource());
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
    [[nodiscard]] std::optional<std::pmr::string> pop_datagram() {
        if (!datagrams_ || datagrams_->empty()) {
            return std::nullopt;
        }
        auto payload = std::move(datagrams_->front());
        datagrams_->pop_front();
        datagram_bytes_ -= payload.size();
        return payload;
    }
    [[nodiscard]] std::pmr::string take_tunnel_bytes() {
        std::pmr::string result(body_.get_allocator());
        result.swap(body_);
        return result;
    }

    // Buffered body: the whole content, borrowed by the request at dispatch.
    // Streaming body: the bytes queued for the reader.
    [[nodiscard]] http3_body_store_status store(
        std::string_view data, protocol_byte_limit total_limit, std::size_t backlog_limit) {
        if (http_request_body_addition_failure(received_bytes_, data.size(), total_limit).has_value()) {
            return http3_body_store_status::too_large;
        }
        if (body_mode_ == request_body_mode::stream &&
            (body_.size() > backlog_limit || data.size() > backlog_limit - body_.size())) {
            return http3_body_store_status::backlog_overflow;
        }
        try {
            body_.append(data.data(), data.size());
        } catch (const inbound_buffer_limit_error&) {
            return http3_body_store_status::backlog_overflow;
        }
        received_bytes_ += data.size();
        return http3_body_store_status::stored;
    }

    [[nodiscard]] std::string_view buffered_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    std::string_view buffered_body() const&& = delete;

    // Streaming reader: hands out every queued byte as one chunk, valid until
    // the next pop(). Returns how many bytes the previous chunk held, whose
    // flow-control credit the caller now returns.
    [[nodiscard]] std::pair<std::string_view, std::size_t> pop() & RUVIA_LIFETIMEBOUND {
        const auto consumed = active_chunk_.size();
        http_clear_pmr_string_retaining_small(active_chunk_);
        active_chunk_.swap(body_);
        return {active_chunk_, consumed};
    }
    std::pair<std::string_view, std::size_t> pop() && = delete;

    // Streamed content received and not yet handed back as credit, returned
    // when the handler ends so a stopped reader cannot strand
    // connection-level credit. Buffered content returns its credit as it is
    // stored.
    [[nodiscard]] std::size_t unconsumed_bytes() const noexcept {
        return body_mode_ == request_body_mode::stream ? body_.size() + active_chunk_.size() : 0;
    }

    // The handler finished: the content's storage goes back to the
    // connection's inbound budget. Its credit is returned first.
    void discard_body() noexcept {
        std::pmr::string(body_.get_allocator()).swap(body_);
        std::pmr::string(active_chunk_.get_allocator()).swap(active_chunk_);
        datagrams_.reset();
        datagram_bytes_ = 0;
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
        request_body_open_ = false;
    }
    [[nodiscard]] bool request_ended() const noexcept {
        return request_ended_;
    }

    void mark_request_body_open(std::int64_t now_ms) noexcept {
        request_body_open_ = true;
        request_body_opened_ms_ = now_ms;
        request_body_progress_ms_ = now_ms;
    }
    void note_request_body_progress(std::int64_t now_ms) noexcept {
        request_body_progress_ms_ = now_ms;
    }
    void close_request_body_timing() noexcept {
        request_body_open_ = false;
    }
    // The request body is still open and went `inactivity` without DATA, or
    // `completion` since it opened (request_body_timeout_ and
    // body_completion_timeout_).
    [[nodiscard]] bool request_body_overdue(std::int64_t now_ms,
        const std::optional<std::chrono::milliseconds>& inactivity,
        const std::optional<std::chrono::milliseconds>& completion) const noexcept {
        return request_body_open_ &&
               ((inactivity.has_value() && now_ms - request_body_progress_ms_ >= inactivity->count()) ||
                   (completion.has_value() && now_ms - request_body_opened_ms_ >= completion->count()));
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

    [[nodiscard]] http3_stream_signal* begin_dispatch(const worker_handle& worker_value, http3_termination& termination) {
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
    [[nodiscard]] http3_stream_signal* signal() & noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    http3_stream_signal* signal() && = delete;
    [[nodiscard]] const http3_stream_signal* signal() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    const http3_stream_signal* signal() const&& = delete;

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
    std::pmr::string body_;
    std::pmr::string active_chunk_;
    std::optional<std::pmr::deque<std::pmr::string>> datagrams_;
    std::size_t datagram_bytes_{0};
    std::size_t received_bytes_{0};
    std::optional<http_request_body_failure> body_failure_{};
    std::optional<http3_stream_signal> signal_;
    std::int64_t request_body_opened_ms_{0};
    std::int64_t request_body_progress_ms_{0};
    bool request_body_open_{false};
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
