#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"

#include "client/client_tcp_transport.h"
#include "client/http_client_pool.h"
#include "http/inline_http_header_views.h"

namespace ruvia::detail {

namespace {

// A protocol failure observed by the reader is reported to the handler as an
// io_failed transport result carrying this error.
[[nodiscard]] client_transport_result http2_client_protocol_failure() noexcept {
    return {.status_ = client_transport_status::io_failed, .error_ = std::make_error_code(std::errc::protocol_error)};
}

// Product limit: the longest a graceful close waits for its GOAWAY to reach the
// transport before it closes anyway (an unread peer, or write_timeout nullopt).
constexpr std::chrono::milliseconds http2_client_goaway_flush_time{1000};

}  // namespace

http2_client_session::http2_client_session(http_client_connection& handler, client_tcp_transport& transport, task_scope& tasks,
    const worker_ref& worker_value, std::optional<timeout_duration> ping_interval)
    : handler_(handler),
      transport_(transport),
      tasks_(tasks),
      worker_(worker_value),
      ping_interval_(ping_interval),
      read_buffer_(),
      write_buffer_(),
      write_signal_(worker_value),
      state_signal_(worker_value),
      capacity_signal_(worker_value) {}

task<http2_client_session_start_result> http2_client_session::start(
    http2_connection_options options, const operation_deadline& timeout) {
    if (session_tasks_ != 0) {
        std::terminate();
    }
    connection_.emplace(::ruvia::http2_connection::client(options));
    next_nonzero_id(generation_);
    failed_ = false;
    draining_ = false;
    closing_ = false;
    request_capacity_ = 0;
    read_buffer_.clear();
    // Only the prefix reported by read_some reaches the parser.
    const auto input = read_buffer_.prepare(read_buffer_bytes);
    for (;;) {
        while (connection_->wants_write()) {
            take_output_batch(write_buffer_);
            const auto written = co_await transport_.write(write_buffer_, timeout);
            http_clear_string_retaining_small(write_buffer_, write_batch_bytes);
            if (!written.ok()) {
                reset();
                co_return http2_client_session_start_result{
                    .status_ = http2_client_session_start_status::transport_failed, .transport_ = written};
            }
        }
        if (connection_->received_peer_settings()) {
            break;
        }
        const auto read = co_await transport_.read_some(input, timeout);
        if (!read.ok()) {
            reset();
            co_return http2_client_session_start_result{
                .status_ = http2_client_session_start_status::transport_failed, .transport_ = read};
        }
        if (!feed(std::string_view(input.data(), read.bytes_))) {
            reset();
            co_return http2_client_session_start_result{.status_ = http2_client_session_start_status::protocol_failed};
        }
    }
    const auto generation = generation_;
    try {
        ++session_tasks_;
        try {
            tasks_.spawn(run_reader(generation));
        } catch (...) {
            --session_tasks_;
            throw;
        }
        ++session_tasks_;
        try {
            tasks_.spawn(run_writer(generation));
        } catch (...) {
            --session_tasks_;
            throw;
        }
        if (ping_interval_) {
            last_input_ = std::chrono::steady_clock::now();
            ping_pending_ = false;
            schedule_ping(generation,
                saturating_deadline(last_input_, ping_interval_->value()));
        }
    } catch (...) {
        const auto failure = std::current_exception();
        fail(generation, {}, failure);
        std::rethrow_exception(failure);
    }
    co_return http2_client_session_start_result{};
}

::ruvia::http2_connection& http2_client_session::connection() noexcept {
    return *connection_;
}

bool http2_client_session::has_connection() const noexcept {
    return connection_.has_value();
}

bool http2_client_session::usable() const noexcept {
    return connection_.has_value() && session_tasks_ != 0 && !failed_ && !draining_;
}

std::size_t http2_client_session::available_request_streams(std::size_t local_limit) const noexcept {
    return usable() ? connection_->available_request_streams(local_limit) : 0;
}

bool http2_client_session::running() const noexcept {
    return session_tasks_ != 0;
}

bool http2_client_session::failed() const noexcept {
    return failed_;
}

bool http2_client_session::draining() const noexcept {
    return draining_;
}

bool http2_client_session::writing() const noexcept {
    return writing_;
}

bool http2_client_session::closing() const noexcept {
    return closing_ && !failed_;
}

std::uint64_t http2_client_session::generation() const noexcept {
    return generation_;
}

void http2_client_session::mark_draining() noexcept {
    draining_ = true;
    state_signal_.notify();
    capacity_signal_.notify();
}

void http2_client_session::wake_writer() noexcept {
    if (connection_.has_value() && !failed_ && connection_->wants_write()) {
        write_signal_.notify();
    }
}

worker_signal& http2_client_session::state_signal() noexcept {
    return state_signal_;
}

worker_signal& http2_client_session::capacity_signal() noexcept {
    return capacity_signal_;
}

void http2_client_session::fail(const client_transport_result& transport, std::exception_ptr failure) noexcept {
    fail(generation_, transport, std::move(failure));
}

bool http2_client_session::begin_close(const client_transport_result& transport) noexcept {
    if (closing()) {
        return true;
    }
    if (!connection_.has_value() || session_tasks_ == 0 || failed_) {
        fail(transport);
        return false;
    }
    const auto generation = generation_;
    close_result_ = transport;
    try {
        // A client drain queues its final GOAWAY at once; idempotent.
        connection_->begin_drain();
        if (worker_.schedule_timer(close_timer_, deadline_after(http2_client_goaway_flush_time),
                [this, generation](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        fail(generation, close_result_, {});
                    }
                }) != worker_timer_schedule_status::scheduled) {
            // A stopping worker cannot bound the flush.
            fail(generation, transport, {});
            return false;
        }
    } catch (...) {
        fail(generation, transport, {});
        return false;
    }
    closing_ = true;
    mark_draining();
    // Also wakes an idle writer when the GOAWAY was already sent.
    write_signal_.notify();
    return true;
}

void http2_client_session::submit_reset(std::uint32_t stream_id, http2_error_code error) noexcept {
    if (stream_id == 0 || !connection_.has_value() || failed_) {
        return;
    }
    try {
        if (connection_->submit_reset(stream_id, error) == http2_submit_status::accepted &&
            connection_->wants_write()) {
            write_signal_.notify();
        }
    } catch (...) {
        fail({}, std::current_exception());
    }
}

bool http2_client_session::output_flushed() const noexcept {
    return !writing_ && connection_.has_value() && !connection_->wants_write();
}

void http2_client_session::reset() noexcept {
    if (session_tasks_ == 0) {
        connection_.reset();
        read_buffer_.clear();
        std::string().swap(write_buffer_);
    }
}

bool http2_client_session::ended(std::uint64_t generation) const noexcept {
    return generation != generation_ || failed_ || !connection_.has_value();
}

void http2_client_session::schedule_ping(std::uint64_t generation, std::chrono::steady_clock::time_point deadline) {
    (void)worker_.schedule_timer(ping_timer_, deadline, [this, generation](worker_timer_outcome outcome) noexcept {
        if (outcome == worker_timer_outcome::expired) {
            check_liveness(generation);
        }
    });
}

void http2_client_session::check_liveness(std::uint64_t generation) noexcept {
    if (ended(generation) || session_tasks_ == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto interval = ping_interval_->value();
    try {
        if (ping_pending_ && last_input_ > ping_sent_at_) {
            ping_pending_ = false;
        }
        if (ping_pending_) {
            if (now >= saturating_deadline(ping_sent_at_, interval)) {
                fail(generation,
                    {.status_ = client_transport_status::aborted, .abort_ = client_abort_reason::timeout}, {});
            } else {
                schedule_ping(generation, saturating_deadline(ping_sent_at_, interval));
            }
            return;
        }
        if (const auto idle_deadline = saturating_deadline(last_input_, interval); idle_deadline > now) {
            schedule_ping(generation, idle_deadline);
            return;
        }
        if (connection_->submit_ping() != http2_submit_status::accepted) {
            // The connection already failed; the reader reports it.
            return;
        }
        ping_pending_ = true;
        ping_sent_at_ = now;
        wake_writer();
        schedule_ping(generation, saturating_deadline(now, interval));
    } catch (...) {
        fail(generation, {}, std::current_exception());
    }
}

void http2_client_session::take_output_batch(std::string& output) {
    output.clear();
    (void)connection_->take_output_batch(write_batch_bytes, output);
}

bool http2_client_session::feed(std::string_view bytes_value) {
    for (;;) {
        const auto status = connection_->feed(bytes_value);
        drain();
        if (status == http2_feed_result::protocol_failure || connection_->connection_error()) {
            return false;
        }
        if (status != http2_feed_result::events_pending) {
            return true;
        }
    }
}

void http2_client_session::publish_state() noexcept {
    state_signal_.notify();
    const auto capacity =
        usable() ? connection_->available_request_streams(std::numeric_limits<std::size_t>::max()) : 0;
    if (capacity > request_capacity_) {
        capacity_signal_.notify();
        handler_.on_http2_capacity();
    }
    request_capacity_ = capacity;
}

void http2_client_session::finish_task(std::uint64_t generation) noexcept {
    if (generation != generation_ || session_tasks_ == 0) {
        std::terminate();
    }
    --session_tasks_;
    state_signal_.notify();
    if (session_tasks_ == 0) {
        handler_.on_http2_stopped();
    }
}

http2_client_session::task_guard::~task_guard() {
    session_.finish_task(generation_);
}

task<void> http2_client_session::run_reader(std::uint64_t generation) {
    const task_guard guard{*this, generation};
    const auto input = read_buffer_.prepare(read_buffer_bytes);
    const operation_deadline unbounded(std::nullopt);
    try {
        while (generation_ == generation && !failed_) {
            const auto read = co_await transport_.read_some(input, unbounded);
            if (!read.ok()) {
                // A peer can close immediately after receiving our final
                // END_STREAM, before the socket write callback is dispatched.
                // Let that callback establish success or failure first.
                if (read.status_ == client_transport_status::eof) {
                    while (generation_ == generation && writing_ && !failed_) {
                        co_await state_signal_.wait();
                    }
                    if (generation_ != generation || failed_) {
                        co_return;
                    }
                }
                fail(generation, read, {});
                co_return;
            }
            if (generation_ != generation || failed_) {
                co_return;
            }
            if (ping_interval_) {
                last_input_ = std::chrono::steady_clock::now();
            }
            if (!feed(std::string_view(input.data(), read.bytes_))) {
                fail(generation, http2_client_protocol_failure(), {});
                co_return;
            }
        }
    } catch (...) {
        fail(generation, {}, std::current_exception());
    }
}

task<void> http2_client_session::run_writer(std::uint64_t generation) {
    const task_guard guard{*this, generation};
    try {
        for (;;) {
            while (generation_ == generation && !failed_ && connection_->wants_write()) {
                take_output_batch(write_buffer_);
                writing_ = true;
                const auto written =
                    co_await transport_.write(write_buffer_, operation_deadline(handler_.http2_write_timeout()));
                writing_ = false;
                // Retain ordinary heads and frame-sized DATA batches, but
                // return an unexpectedly larger indivisible frame before
                // the connection idles.
                http_clear_string_retaining_small(write_buffer_, write_batch_bytes);
                if (!written.ok()) {
                    fail(generation, written, {});
                    co_return;
                }
                publish_state();
            }
            if (generation_ != generation || failed_) {
                co_return;
            }
            if (closing_) {
                // Every frame queued up to and after the GOAWAY was written.
                fail(generation, close_result_, {});
                co_return;
            }
            co_await write_signal_.wait();
        }
    } catch (...) {
        writing_ = false;
        fail(generation, {}, std::current_exception());
    }
}

void http2_client_session::drain() {
    while (auto event = connection_->next_event()) {
        handler_.on_http2_event(*event);
    }
    if (connection_->wants_write()) {
        write_signal_.notify();
    }
    publish_state();
}

void http2_client_session::fail(
    std::uint64_t generation, const client_transport_result& transport, std::exception_ptr failure) noexcept {
    if (generation != generation_ || failed_) {
        return;
    }
    failed_ = true;
    draining_ = true;
    ping_timer_.cancel();
    close_timer_.cancel();
    transport_.close();
    handler_.on_http2_failure(transport, std::move(failure));
    write_signal_.notify();
    state_signal_.notify();
    capacity_signal_.notify();
}

http_client_connection::http2_stream* http_client_connection::find_http2_stream(std::uint32_t stream_id) noexcept {
    if (stream_id != 0 && last_http2_stream_ != nullptr && last_http2_stream_->stream_id_ == stream_id) {
        return last_http2_stream_;
    }
    const auto match = std::ranges::find_if(
        http2_streams_, [stream_id](const http2_stream* stream) { return stream->stream_id_ == stream_id; });
    if (match == http2_streams_.end()) {
        return nullptr;
    }
    last_http2_stream_ = *match;
    return *match;
}

http_client_connection::http2_stream* http_client_connection::find_http2_request(std::uint64_t request_id) noexcept {
    const auto match = std::ranges::find_if(
        http2_streams_, [request_id](const http2_stream* stream) { return stream->request_id_ == request_id; });
    return match == http2_streams_.end() ? nullptr : *match;
}

std::optional<timeout_duration> http_client_connection::http2_write_timeout() const noexcept {
    if (http2_streams_.empty()) {
        return pool_.config().write_timeout_;
    }
    std::optional<timeout_duration> maximum;
    for (const auto* stream : http2_streams_) {
        const auto bound = stream->state_->effective_write_timeout_;
        if (!bound) {
            return std::nullopt;
        }
        if (!maximum || maximum->value() < bound->value()) {
            maximum = bound;
        }
    }
    return maximum;
}

void http_client_connection::restart_http2_stream_read(http2_stream& stream) {
    if (stream.websocket_ && stream.state_->head_ready_) {
        stream.timer_.read_deadline_.reset();
        schedule_http2_stream_timer(stream);
        return;
    }
    // An accepted tunnel is bounded by its own idle timeout, not read_timeout_.
    const auto& read_timeout = stream.state_->tunnel_ && stream.state_->tunnel_->accepted_
                                   ? stream.state_->tunnel_->config_.idle_timeout_
                                   : stream.state_->effective_read_timeout_;
    if (!read_timeout && stream.state_->tunnel_ && stream.state_->tunnel_->accepted_) {
        stream.timer_.read_deadline_.reset();
        schedule_http2_stream_timer(stream);
        return;
    }
    if (!read_timeout || stream.complete_ || stream.state_->informational_active_ ||
        (stream.state_->tunnel_ && stream.state_->tunnel_->receive_ended_)) {
        return;
    }
    stream.timer_.read_deadline_ = deadline_after(read_timeout->value());
    schedule_http2_stream_timer(stream);
}

void http_client_connection::schedule_http2_stream_timer(http2_stream& stream) {
    stream.timer_.schedule_with_retry(
        pool_.worker(), stream.timeout_,
        [this, request_id = stream.request_id_]() noexcept {
            cancel_http2_stream(request_id, client_abort_reason::timeout);
        },
        [this, &stream](std::exception_ptr failure) noexcept {
            fail_http2_stream(stream, std::move(failure), true);
        });
}

void http_client_connection::arm_http2_write_timer(
    worker_timer_registration& write_timer, const http2_stream& stream) {
    if (const auto deadline = stream.timeout_->constrained_by(stream.state_->effective_write_timeout_).deadline()) {
        (void)pool_.worker().schedule_timer(write_timer, *deadline,
            [this, request_id = stream.request_id_](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    cancel_http2_stream(request_id, client_abort_reason::timeout);
                }
            });
    }
}

std::uint64_t http_client_connection::next_http2_request_id() noexcept {
    return next_nonzero_id(next_http2_request_id_);
}

void http_client_connection::register_http2_stream(http2_stream& stream) {
    // A push stream is open on registration; a request reads once its head
    // was submitted.
    if (stream.stream_id_ != 0) {
        restart_http2_stream_read(stream);
    }
    schedule_http2_stream_timer(stream);
    end_idle();
    stream.registry_index_ = http2_streams_.size();
    http2_streams_.push_back(&stream);
    last_http2_stream_ = &stream;
}

bool http_client_connection::http2_exchange_complete(const http2_stream& stream) noexcept {
    if (stream.state_->tunnel_ && stream.state_->tunnel_->accepted_ &&
        stream.state_->tunnel_->receive_ended_ && stream.state_->tunnel_->output_.ended_) {
        return true;
    }
    if (!stream.complete_) {
        return false;
    }
    if (const auto* output = stream.state_->output()) {
        return output->ended_ || output->stopped_;
    }
    return !stream.request_body_pending_;
}

void http_client_connection::fail_http2_stream(
    http2_stream& stream, std::exception_ptr failure, bool reset_stream) noexcept {
    if (http2_exchange_complete(stream) || stream.failed()) {
        return;
    }
    stream.failure_ = std::move(failure);
    stream.state_->request_informational_stop();
    if (reset_stream) {
        http2_.submit_reset(stream.stream_id_);
    }
    wake_http2_stream(stream);
}

void http_client_connection::wake_http2_stream(http2_stream& stream) noexcept {
    stream.signal_.notify();
    http2_.state_signal().notify();
    if (stream.stream_id_ == 0) {
        // Its driver may be parked for request-stream capacity.
        http2_.capacity_signal().notify();
    }
}

void http_client_connection::release_response_data(http_client_response_state& state_value) noexcept {
    if (!state_value.http2_data_credit_) {
        return;
    }
    // The token returns credit to its original connection even if that session
    // has already retired. Its destructor also defers allocation failures.
    state_value.http2_data_credit_.reset();
    http2_.wake_writer();
    // The returned credit lets the peer send again: the read is active.
    if (auto* stream = find_http2_request(state_value.request_id_); stream != nullptr && stream->state_ == &state_value) {
        try {
            restart_http2_stream_read(*stream);
        } catch (...) {
            fail_http2_stream(*stream, std::current_exception(), true);
        }
    }
}

void http_client_connection::publish_http2_head(http2_stream& stream, http2_response_head_event& event) {
    auto& state_value = *stream.state_;
    const auto request_content_signal = event.request_content_signal();
    const bool stream_ended = event.stream_ended();
    const auto received_status = event.head().received_status();
    const auto protocol_version = event.head().protocol_version();
    retain_http_client_binary_response_head(state_value, std::move(event).take_head());
    if (auto* websocket = stream.websocket_) {
        accept_extended_connect_websocket_response(*websocket, received_status.status(), state_value.headers_,
            stream_ended, "invalid HTTP/2 WebSocket handshake response");
        // HTTP request lifetime ends at the accepted handshake. WebSocket
        // operations own their deadlines; the session itself has no total cap.
        static const operation_deadline websocket_lifetime(std::nullopt);
        stream.timeout_ = &websocket_lifetime;
        // WebSocket receive operations own their active-read inactivity timer.
        stream.timer_.read_deadline_.reset();
        schedule_http2_stream_timer(stream);
    }
    publish_http_client_final_head(state_value, pool_.policy(),
        {.status_ = received_status,
            .protocol_version_ = protocol_version,
            .request_content_signal_ = request_content_signal,
            .origin_target_ = stream.addresses_origin_ ? std::optional(stream.request_target_) : std::nullopt,
            // RFC 9110 section 9.3.6 and RFC 8441 section 5: a 2xx to CONNECT,
            // plain or extended, opens the tunnel.
            .opens_tunnel_ = state_value.tunnel_.has_value() && received_status.status().is_successful()});
    // The upload loop waits for tunnel acceptance.
    stream.signal_.notify();
    restart_http2_stream_read(stream);
}

void http_client_connection::consume_http2_data(
    std::uint32_t stream_id, std::string_view bytes_value, http2_received_data_credit credit) {
    auto* stream = find_http2_stream(stream_id);
    if (stream == nullptr || stream->complete_ || stream->failed()) {
        // Unretained credits return on destruction, including failed or
        // cancelled streams; the session wakes the writer for WINDOW_UPDATE.
        return;
    }
    auto& state_value = *stream->state_;
    try {
        if (!state_value.append_received_body(bytes_value, pool_.config().max_response_bytes_)) {
            stream->error_ = make_error_code(http_client_errc::response_too_large);
            http2_.submit_reset(stream_id);
            stream->signal_.notify();
            return;
        }
        if (state_value.collect_all_ || !credit.valid()) {
            restart_http2_stream_read(*stream);
            return;
        }
    } catch (...) {
        fail_http2_stream(*stream, std::current_exception(), true);
        return;
    }
    auto& retained_credit = state_value.http2_data_credit_;
    if (!retained_credit) {
        retained_credit.emplace(std::move(credit));
    } else {
        retained_credit->merge(std::move(credit));
    }
    // The peer may now wait for this client's consumer to return the credit;
    // release_response_data() restarts the read.
    stream->timer_.read_deadline_.reset();
}

void http_client_connection::finish_http2_message(std::uint32_t stream_id, http2_message_end_event& end) {
    auto* stream = find_http2_stream(stream_id);
    if (stream == nullptr || stream->failed()) {
        return;
    }
    try {
        const bool content_semantics_present = end.content_semantics() == http2_message_content_semantics::content;
        const auto trailers = end.trailers();
        auto& state_value = *stream->state_;
        state_value.trailers_.clear();
        state_value.trailers_.reserve(trailers.size());
        for (const auto& trailer : trailers) {
            state_value.trailers_.emplace_back(trailer.name(), trailer.value());
        }
        if (stream->timeout_ == nullptr) {
            std::terminate();
        }
        finish_http_client_response_body(
            state_value, *stream->timeout_, content_semantics_present, pool_.config().max_response_bytes_);
        stream->complete_ = true;
        stream->timer_.read_deadline_.reset();
        // Response EOF is independent of the request send half (RFC 9113 8.1).
        // A consumer can finish reading now and still write/end its upload.
        state_value.complete_ = true;
        state_value.data_signal_.notify();
        stream->signal_.notify();
    } catch (...) {
        fail_http2_stream(*stream, std::current_exception(), false);
    }
}

task<void> http_client_connection::deliver_http2_informational(std::uint32_t stream_id) {
    auto* stream = find_http2_stream(stream_id);
    if (stream == nullptr || stream->failed()) {
        co_return;
    }
    auto* state = stream->state_;
    const auto timeout = *stream->timeout_;
    const auto request_id = stream->request_id_;
    state->retain_reference();
    struct response_pin final {
        http_client_response_state* state_;
        ~response_pin() {
            state_->release_reference();
        }
    } pin{state};
    stream->timer_.read_deadline_.reset();
    std::exception_ptr failure;
    try {
        co_await state->dispatch_informational(timeout);
    } catch (...) {
        failure = std::current_exception();
    }
    state->informational_active_ = false;
    if (failure) {
        state->failure_ = failure;
    }
    state->head_signal_.notify();
    if (auto* current = find_http2_request(request_id)) {
        if (failure) {
            current->failure_ = std::move(failure);
            http2_.submit_reset(current->stream_id_);
        } else if (!current->failed() && !state->http2_data_credit_) {
            restart_http2_stream_read(*current);
        }
        wake_http2_stream(*current);
    }
}

void http_client_connection::on_http2_event(http2_event& event) {
    if (const auto* origins = event.origin_advertisement()) {
        pool_.advertisements().retain(index_, http_protocol_version::http2, *origins);
    } else if (const auto* service = event.alternative_service_advertisement()) {
        pool_.advertisements().retain(index_, *service);
        if (service->stream_id_ != 0) {
            if (auto* stream = find_http2_stream(service->stream_id_);
                stream != nullptr && stream->addresses_origin_ && stream->state_ != nullptr && stream->state_->route_ != nullptr) {
                const http_header field("alt-svc", service->field_value_);
                pool_.observe_alt_svc(*stream->state_->route_, std::span<const http_header>(&field, 1));
            }
        } else if (alias_route() && service->origin_.size() > 8 &&
                   http_ascii_equals_ignore_case(std::string_view(service->origin_).substr(0, 8), "https://") &&
                   http_authorities_equal(borrowed_text(std::string_view(service->origin_).substr(8)),
                       borrowed_text(std::string_view(pool_.config().authority_)), 443)) {
            const http_header field("alt-svc", service->field_value_);
            pool_.observe_alt_svc(http_client_route{.origin_ = pool_.config().origin(), .authority_ = pool_.config().authority_, .dial_host_ = pool_.config().dial_host_, .dial_port_ = pool_.config().dial_port_, .server_name_ = pool_.config().host_, .alias_origin_ = true, .versions_ = pool_.config().versions_}, std::span<const http_header>(&field, 1));
        }
    }
    if (const auto* push = event.push_promise()) {
        pool_.accept_http2_push(*this, *push);
    } else if (const auto* interim = event.informational_head()) {
        auto* stream = find_http2_stream(interim->stream_id());
        if (stream == nullptr || stream->complete_ || stream->failed()) {
            return;
        }
        try {
            stream->state_->retain_informational(
                interim->head().status(), interim->head().headers(), interim->request_content_signal());
            if (stream->state_->informational_callback_ && !stream->state_->informational_active_) {
                stream->state_->informational_active_ = true;
                stream->timer_.read_deadline_.reset();
                try {
                    pool_.tasks().spawn(deliver_http2_informational(stream->stream_id_));
                } catch (...) {
                    stream->state_->informational_active_ = false;
                    throw;
                }
            }
            restart_http2_stream_read(*stream);
        } catch (...) {
            fail_http2_stream(*stream, std::current_exception(), true);
        }
    } else if (auto* head = event.response_head()) {
        auto* stream = find_http2_stream(head->stream_id());
        if (stream == nullptr || stream->complete_ || stream->failed()) {
            return;
        }
        try {
            publish_http2_head(*stream, *head);
        } catch (...) {
            fail_http2_stream(*stream, std::current_exception(), true);
        }
    } else if (auto* chunk = event.message_body_chunk()) {
        consume_http2_data(chunk->stream_id(), chunk->bytes(), chunk->take_credit());
    } else if (auto* data = event.tunnel_data()) {
        consume_http2_data(data->stream_id(), data->bytes(), data->take_credit());
    } else if (const auto* tunnel_end = event.tunnel_end()) {
        if (auto* stream = find_http2_stream(tunnel_end->stream_id()); stream != nullptr && !stream->failed()) {
            auto& state_value = *stream->state_;
            if (!state_value.tunnel_ || !state_value.tunnel_->accepted_) {
                stream->error_ = make_error_code(http_client_errc::protocol_error);
            } else {
                state_value.tunnel_->receive_ended_ = true;
                state_value.data_signal_.notify();
            }
            stream->timer_.read_deadline_.reset();
            stream->signal_.notify();
        }
    } else if (auto* end = event.message_end()) {
        finish_http2_message(end->stream_id(), *end);
    } else if (const auto* closed = event.stream_closed()) {
        if (auto* stream = find_http2_stream(closed->stream_id());
            stream != nullptr && !stream->failed() && !stream->retryable_) {
            auto& state_value = *stream->state_;
            if (stream->complete_) {
                // A peer can stop the remaining upload after a complete response,
                // including RST_STREAM(NO_ERROR); preserve that response.
                if (auto* output = state_value.output()) {
                    output->stop();
                }
                stream->signal_.notify();
            } else if (closed->source() == http2_stream_close_source::peer &&
                       closed->error() == http2_error_code::http11_required && !state_value.head_ready_ &&
                       !state_value.promised_request_) {
                // RFC 9113 section 7: the origin refuses this request over
                // HTTP/2. Like Pingora, a negotiate pool prefers HTTP/1.1 from
                // now on and this session admits no further request.
                stream->retryable_ = true;
                stream->http11_required_ = true;
                if (alpn_ == client_alpn::negotiate) {
                    pool_.require_http11();
                    http2_.mark_draining();
                }
                stream->signal_.notify();
            } else if (stream->websocket_ && state_value.tunnel_->accepted_ && state_value.tunnel_->receive_ended_ &&
                       closed->source() == http2_stream_close_source::peer &&
                       closed->error() == http2_error_code::no_error) {
                // A server can retire its half-open request lease immediately
                // after Close + END_STREAM. Keep the buffered Close for the
                // WebSocket parser; NO_ERROR alone is never a close handshake.
                // The reset closes the send half too. Do not free a chunk still
                // borrowed by the upload coroutine before it resumes.
                stream->complete_ = true;
                state_value.complete_ = true;
                state_value.tunnel_->output_.ended_ = true;
                state_value.tunnel_->output_.notify_data();
                state_value.tunnel_->output_.space_.notify();
                state_value.data_signal_.notify();
                stream->signal_.notify();
            } else if (!state_value.tunnel_ || !state_value.tunnel_->accepted_ || !state_value.tunnel_->receive_ended_ ||
                       !state_value.tunnel_->output_.end_requested_ ||
                       closed->error() != http2_error_code::no_error) {
                stream->error_ = make_error_code(http_client_errc::protocol_error);
                stream->signal_.notify();
            }
        }
    } else if (const auto* unprocessed = event.request_unprocessed()) {
        // Published after a peer GOAWAY (whose own event marks the session
        // draining) or for RST_STREAM(REFUSED_STREAM), which keeps it usable.
        if (auto* stream = find_http2_stream(unprocessed->stream_id());
            stream != nullptr && !stream->complete_ && !stream->failed()) {
            stream->retryable_ = true;
            stream->signal_.notify();
        }
    } else if (const auto* goaway = event.goaway()) {
        if (goaway->error() == http2_error_code::http11_required) {
            // Its unprocessed requests retry over a connection offering only
            // http/1.1.
            if (alpn_ == client_alpn::negotiate) {
                pool_.require_http11();
            }
        }
        http2_.mark_draining();
    }
}

void http_client_connection::on_http2_failure(
    const client_transport_result& transport, std::exception_ptr failure) noexcept {
    connected_ = false;
    // An h2-only offer reconnects as HTTP/2 again.
    if (alpn_ != client_alpn::http2) {
        protocol_ = protocol_type::unknown;
    }
    auto stream_error = make_error_code(http_client_errc::io_error);
    if (const auto reason = transport_.abort_reason(); reason != client_abort_reason::none) {
        stream_error = http_client_abort_error(reason);
    } else if (transport.status_ == client_transport_status::aborted) {
        stream_error = http_client_abort_error(transport.abort_);
    } else if (transport.status_ != client_transport_status::ok) {
        stream_error = http_client_transport_error(transport);
    }
    for (auto* stream : http2_streams_) {
        stream->state_->request_informational_stop();
        if (transport.status_ == client_transport_status::eof && failure == nullptr && !stream->failed() && !stream->retryable_ &&
            stream->tunnel_end_submitted_ && http2_.output_flushed() &&
            stream->state_->tunnel_->receive_ended_) {
            // Both wire halves finished, even if the upload driver is still
            // queued behind this read callback. EOF cannot invalidate them.
            stream->state_->tunnel_->output_.finish();
            stream->complete_ = true;
        }
        if (!http2_exchange_complete(*stream) && !stream->failed() && !stream->retryable_) {
            if (failure != nullptr) {
                stream->failure_ = failure;
            } else {
                stream->error_ = stream_error;
            }
        }
        // A driver can still await its final drain notification after the wire
        // halves closed. Failure must wake it even when its response stays valid.
        stream->signal_.notify();
    }
}

void http_client_connection::cancel_http2_stream(
    std::uint64_t request_id, client_abort_reason reason, http2_error_code error) noexcept {
    auto* const match = find_http2_request(request_id);
    if (match == nullptr) {
        return;
    }
    auto& stream = *match;
    if (http2_exchange_complete(stream) || stream.failed() || stream.retryable_) {
        return;
    }
    stream.error_ = http_client_abort_error(reason);
    stream.state_->request_informational_stop();
    http2_.submit_reset(stream.stream_id_, error);
    wake_http2_stream(stream);
}

void http_client_connection::remove_http2_stream(http2_stream& stream) noexcept {
    // The core decides whether a half is still open. In particular a successful
    // early response does not retire an abandoned or Continue-cancelled upload.
    const bool both_halves_closed =
        stream.complete_ && !stream.failed() &&
        (stream.state_->output() != nullptr ? stream.state_->output()->ended_ : !stream.request_body_pending_);
    // Once the complete response arrived, only the unfinished sending half
    // ends, without error (RFC 9113 section 8.1), whether the caller abandoned
    // its upload or the response made it unnecessary.
    const bool response_kept = stream.complete_ && !stream.failed() && !stream.state_->abandoned_;
    if (!stream.retryable_ && stream.stream_id_ != 0 && !both_halves_closed) {
        http2_.submit_reset(stream.stream_id_, response_kept ? http2_error_code::no_error : http2_error_code::cancel);
    }
    stream.timer_.cancel();
    if (stream.state_->http2_data_credit_) {
        stream.state_->http2_data_credit_.reset();
        http2_.wake_writer();
    }
    const auto index = stream.registry_index_;
    if (index >= http2_streams_.size() || http2_streams_[index] != &stream) {
        std::terminate();
    }
    if (last_http2_stream_ == &stream) {
        last_http2_stream_ = nullptr;
    }
    auto* const back = http2_streams_.back();
    http2_streams_[index] = back;
    back->registry_index_ = index;
    http2_streams_.pop_back();
    http2_.state_signal().notify();
    if (http2_.draining() && http2_streams_.empty() && http2_.running()) {
        // A drained session admits no new streams; retire it so the next
        // request opens a fresh connection.
        close_http2_session();
    } else if (http2_streams_.empty() && http2_.usable()) {
        begin_idle();
    }
}

void http_client_connection::reprioritize(http_client_response_state& state_value, http_priority priority) {
    if (generation_ != state_value.connection_generation_ || !http2_.has_connection() || !http2_.running() ||
        http2_.failed()) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP/2 response connection is retired");
    }
    const auto status = http2_.connection().submit_priority_update(static_cast<std::uint32_t>(state_value.stream_id_),
        {.urgency_ = priority.urgency_, .incremental_ = priority.incremental_});
    if (status != http2_submit_status::accepted) {
        throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP/2 priority update rejected");
    }
    http2_.wake_writer();
}

task<void> http_client_connection::wait_for_http2_session_stop(
    const operation_deadline& timeout, stop_token stop_token_value, http_client_response_state& response) {
    worker_timer_registration deadline_timer;
    if (const auto deadline = timeout.deadline()) {
        if (*deadline <= std::chrono::steady_clock::now()) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP/2 session shutdown wait timed out");
        }
        (void)pool_.worker().schedule_timer(deadline_timer, *deadline,
            [this](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    http2_.state_signal().notify();
                }
            });
    }
    http2_state_waiter waiter{.next_ = state_waiters_};
    state_waiters_ = &waiter;
    struct linked_wait final {
        http2_state_waiter*& head_;
        http2_state_waiter& waiter_;
        ~linked_wait() {
            auto** link = &head_;
            while (*link != &waiter_) {
                if (!*link) {
                    std::terminate();
                }
                link = &(*link)->next_;
            }
            *link = waiter_.next_;
        }
    } wait_guard{state_waiters_, waiter};
    http_client_response_cancellation cancellation(pool_.cancellation_target(), waiter.cancellation_id_, response);
    cancellation.arm(stop_token_value);
    while (http2_.running() || !http2_streams_.empty()) {
        if (stop_token_value.stop_requested() || response.abandoned_) {
            throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP/2 session shutdown wait cancelled");
        }
        if (timeout.expired()) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP/2 session shutdown wait timed out");
        }
        co_await http2_.state_signal().wait();
    }
}

task<bool> http_client_connection::execute_http2(const http_client_request_storage& request,
    const operation_deadline& timeout, const operation_deadline& acquire_timeout, stop_token stop_token_value,
    http_client_response_state& state_value) {
    if (request.is_upgrade()) {
        throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "HTTP Upgrade requires HTTP/1.1");
    }
    const auto& config = pool_.config();
    const std::string_view scheme = config.scheme_ == http_scheme::https ? "https" : "http";
    const auto content = request.upload() != nullptr
                             ? ::ruvia::http_request_content_plan::streaming(request.upload()->config_.content_length_)
                         : request.has_body() ? ::ruvia::http_request_content_plan::known_length(request.body().size())
                                              : ::ruvia::http_request_content_plan::none();

    for (int attempt_value = 0; attempt_value < 2; ++attempt_value) {
        // Each attempt acquires its stream like a connection slot: the slot's
        // reconnect and the wait for request-stream capacity both end at
        // acquire_timeout_. A retry is a new acquisition.
        const auto attempt_acquire =
            attempt_value == 0 ? acquire_timeout : timeout.constrained_by(state_value.effective_acquire_timeout_);
        if (!http2_.usable()) {
            (void)co_await ensure_connected(timeout, attempt_acquire, stop_token_value, state_value);
            if (protocol_ != protocol_type::http2) {
                co_return false;
            }
        }

        http2_stream stream(pool_.worker(), state_value);
        // Until the head is accepted; the stream timer then also bounds the
        // exchange by the request timeout.
        stream.timeout_ = &attempt_acquire;
        stream.request_target_ = request.target();
        stream.websocket_ = request.websocket();
        stream.addresses_origin_ = pool_.policy().addresses_origin(request);
        stream.request_id_ = next_http2_request_id();
        state_value.transport_ = http_client_response_transport::http2;
        state_value.request_method_ = classify_http_method(request.method());
        state_value.connection_index_ = index_;
        state_value.connection_serial_ = serial_;
        state_value.connection_generation_ = generation_;
        state_value.request_id_ = stream.request_id_;
        register_http2_stream(stream);
        struct stream_registration final {
            http_client_connection& connection_;
            http2_stream& stream_;
            bool active_{true};
            void reset() noexcept {
                if (std::exchange(active_, false)) {
                    connection_.remove_http2_stream(stream_);
                }
            }
            ~stream_registration() {
                reset();
            }
        } registration{*this, stream};
        http_client_response_cancellation cancellation(pool_.cancellation_target(), stream.cancellation_id_, state_value);
        cancellation.arm(stop_token_value);

        auto& connection = http2_.connection();
        const auto expectation = request.upload() != nullptr ? request.upload()->config_.expectation_
                                                             : http_client_request_expectation::none;
        // Prepared once per attempt: a wait for stream capacity resubmits the
        // same head instead of rescanning the jar on every session wake.
        client_prepared_request_scratch scratch;
        const auto source_value = pool_.policy().prepare_request(request, scratch);
        const auto headers = static_cast<std::span<const http_header_view>>(source_value.headers_);
        // Whether another request already used this session when this head
        // was accepted.
        bool reused_session = false;
        for (;;) {
            if (stream.failed()) {
                break;
            }
            if (attempt_acquire.expired()) {
                stream.error_ = std::make_error_code(std::errc::timed_out);
                break;
            }
            const auto submitted = request.websocket() != nullptr
                                       ? submit_http2_websocket_client_request(connection,
                                             request.websocket()->negotiation_, scheme, request.tunnel_authority(),
                                             source_value.target_.view(), headers, request.options_.field_source_)
                                   : !request.is_tunnel()
                                       ? connection.submit_request_head(::ruvia::http2_regular_request_head_view{
                                             .method_ = source_value.method_,
                                             .scheme_ = scheme,
                                             .authority_ = borrowed_text(request.route().authority_),
                                             .target_ = source_value.target_,
                                             .headers_ = headers,
                                             .content_ = content,
                                             .expectation_ = expectation,
                                             .field_source_ = request.options_.field_source_})
                                   : request.tunnel_protocol().empty()
                                       ? connection.submit_request_head(http2_connect_request_head_view{
                                             .authority_ = borrowed_text(request.tunnel_authority()), .headers_ = headers, .field_source_ = request.options_.field_source_})
                                       : connection.submit_request_head(http2_extended_connect_request_head_view{
                                             .protocol_ = borrowed_text(request.tunnel_protocol()),
                                             .scheme_ = scheme,
                                             .authority_ = borrowed_text(request.tunnel_authority()),
                                             .target_ = source_value.target_,
                                             .headers_ = headers,
                                             .field_source_ = request.options_.field_source_});
            if (const auto* accepted = std::get_if<std::uint32_t>(&submitted)) {
                state_value.request_written_ = true;
                stream.stream_id_ = *accepted;
                state_value.stream_id_ = stream.stream_id_;
                reused_session = http2_session_requests_ != 0;
                capture_connection_info(state_value, reused_session);
                ++http2_session_requests_;
                stream.timeout_ = &timeout;
                schedule_http2_stream_timer(stream);
                worker_timer_registration head_write_timer;
                arm_http2_write_timer(head_write_timer, stream);
                if (request.has_body() && !request.body().empty()) {
                    stream.request_body_pending_ = true;
                    (void)co_await send_http2_request_data(stream, request.body(), http2_end_stream::end_stream);
                    stream.request_body_pending_ = false;
                } else {
                    http2_.wake_writer();
                    co_await http2_.wait_output_flushed([&stream]() noexcept {
                        return stream.failed() || stream.retryable_;
                    });
                }
                // The response read starts once the buffered request is sent.
                restart_http2_stream_read(stream);
                break;
            }
            const auto error = std::get<http2_request_head_submit_error>(submitted);
            if (error == http2_request_head_submit_error::peer_stream_limit_reached ||
                error == http2_request_head_submit_error::local_stream_capacity_reached) {
                // Outer admission counts the local stream limit; a lower peer
                // limit parks here until a stream closes or the peer raises
                // it, within the attempt's acquisition bound.
                co_await http2_.capacity_signal().wait();
                continue;
            }
            if (error == http2_request_head_submit_error::connection_unavailable) {
                // GOAWAY may have been consumed during the preface, or local
                // stream IDs may be exhausted (no GOAWAY at all). Either way this
                // session admits no new streams: drain it so the retry opens a
                // new connection (RFC 9113 §5.1.1, §6.8).
                stream.retryable_ = true;
                http2_.mark_draining();
            } else {
                stream.error_ = make_error_code(http_client_errc::invalid_request);
            }
            break;
        }

        if (auto* upload = request.output();
            upload != nullptr && stream.stream_id_ != 0 && !stream.failed() && !stream.retryable_) {
            co_await stream_http2_upload(request, stream, *upload);
        }
        while ((!stream.complete_ || state_value.informational_active_) &&
               !stream.failed() && !stream.retryable_) {
            if (request.tunnel() != nullptr && request.tunnel()->accepted_ && request.tunnel()->receive_ended_ &&
                request.tunnel()->output_.ended_ && !state_value.informational_active_) {
                stream.complete_ = true;
                break;
            }
            co_await stream.signal_.wait();
        }
        const bool retryable = stream.retryable_;
        const bool http11_required = stream.http11_required_;
        const auto error = stream.error_;
        const auto failure = stream.failure_;
        registration.reset();
        const bool may_retry = attempt_value == 0 && !timeout.expired() && !stop_token_value.stop_requested();
        if (http11_required) {
            // RFC 9113 section 7 (Pingora downgrades and retries): the
            // retry reconnects this slot offering only http/1.1, once the
            // drained session retired.
            if (alpn_ == client_alpn::negotiate && pool_.prefers_http11() && request.output() == nullptr && may_retry) {
                continue;
            }
            throw http_client_error(
                make_error_code(http_client_errc::protocol_unavailable), "upstream requires HTTP/1.1 for this request");
        }
        // No stream ID means no request head or body was submitted to this
        // connection. A transport failure at the stream-capacity wait is safe
        // to retry even for a streaming upload; protocol and cancellation
        // failures must still be reported.
        if (stream.stream_id_ == 0 && failure == nullptr && error == make_error_code(http_client_errc::io_error) &&
            may_retry) {
            continue;
        }
        // The same rule as an HTTP/1 exchange on a reused connection.
        if (stream.stream_id_ != 0 && reused_session && may_retry &&
            can_retry_reused_connection_request(
                failure != nullptr ? http_client_failure_code(failure) : error, request, state_value)) {
            continue;
        }
        if (retryable && (request.output() == nullptr || stream.stream_id_ == 0) &&
            attempt_value == 0 && !timeout.expired()) {
            state_value.informational_.clear();
            state_value.informational_budget_ = {};
            continue;
        }
        if (retryable) {
            throw http_client_error(make_error_code(http_client_errc::protocol_error),
                "HTTP/2 request was not processed by the server (GOAWAY or REFUSED_STREAM)");
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        if (error) {
            throw http_client_error(*error, http_client_error_message(*error));
        }
        co_return true;
    }
    throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP/2 request retry exhausted");
}

task<bool> http_client_connection::send_http2_request_data(http2_stream& stream,
    std::string_view bytes, http2_end_stream end_stream, const http_client_output_queue* upload) {
    // Flow-control waits are part of the write, even when no socket write
    // exists yet or the response receive half has already ended.
    worker_timer_registration write_timer;
    arm_http2_write_timer(write_timer, stream);
    const auto stopped = [&stream, upload] {
        return stream.failed() || stream.retryable_ || (upload != nullptr && upload->stopped_);
    };
    // Keep one frame-sized copy in the sans-I/O/session output while the caller
    // or upload queue retains the unsent suffix until it reaches the transport.
    const auto status = co_await http2_.send_data(
        stream.stream_id_, bytes, end_stream, http2_default_max_frame_size, stopped);
    // Until the response head, and through an accepted tunnel, sending
    // progress also proves the exchange alive: an origin may read the whole
    // request before it answers.
    if (status == http2_client_data_status::submitted && stream.timer_.read_deadline_ &&
        (!stream.state_->head_ready_ || (stream.state_->tunnel_ && stream.state_->tunnel_->accepted_))) {
        restart_http2_stream_read(stream);
    }
    // A complete response may close the stream before its buffered request
    // content was sent; that response stands.
    if (status == http2_client_data_status::rejected && (upload != nullptr || !stream.complete_)) {
        stream.error_ = upload != nullptr ? make_error_code(http_client_errc::invalid_request)
                                          : make_error_code(http_client_errc::protocol_error);
        http2_.submit_reset(stream.stream_id_);
    }
    co_return status == http2_client_data_status::submitted && !stopped();
}

task<void> http_client_connection::stream_http2_upload(
    const http_client_request_storage& request, http2_stream& stream, http_client_output_queue& upload) {
    auto& connection = http2_.connection();
    const auto stopped = [&stream, &upload] { return stream.failed() || stream.retryable_ || upload.stopped_; };
    co_await run_multiplexed_client_upload(
        pool_.worker(), request, upload, stream.signal_,
        [&stream, &state_signal = http2_.state_signal()]() noexcept {
            stream.signal_.notify();
            state_signal.notify();
        },
        [&stream]() noexcept { return stream.failed() || stream.retryable_; },
        [&]() noexcept {
            return request.is_tunnel() ||
                   connection.release_request_content(stream.stream_id_) != http2_request_content_release_status::closed;
        },
        [&](std::string_view chunk) {
            return send_http2_request_data(stream, chunk, http2_end_stream::keep_open, &upload);
        },
        [&]() -> task<bool> {
            worker_timer_registration write_timer;
            arm_http2_write_timer(write_timer, stream);
            http2_client_data_status finished{};
            if (request.is_tunnel()) {
                finished = co_await http2_.send_data(stream.stream_id_, {}, http2_end_stream::end_stream, 0, stopped,
                    &stream.tunnel_end_submitted_);
            } else {
                const inline_http_header_views trailers(request.upload()->trailers_);
                finished = co_await http2_.finish_request(stream.stream_id_, trailers.view(), stopped);
            }
            if (finished == http2_client_data_status::rejected) {
                stream.error_ = make_error_code(http_client_errc::invalid_request);
                http2_.submit_reset(stream.stream_id_);
                co_return false;
            }
            co_return finished == http2_client_data_status::submitted && !stopped();
        });
}

// --- server push: pool policy over a connection's stream registry ---------------

struct http_client_pool::http2_push_driver final {
    http2_push_driver(http_client_pool& pool, http_client_connection& connection, http_client_response response,
        http_client_response_state& state_value)
        : pool_(pool),
          connection_(connection),
          response_(std::move(response)),
          state_(state_value),
          timeout_(pool.config_.push_.timeout_),
          stream_(pool.worker_, state_value) {}
    ~http2_push_driver() {
        if (registered_) {
            connection_.remove_http2_stream(stream_);
            --pool_.active_pushes_;
        }
    }
    http2_push_driver(const http2_push_driver&) = delete;
    http2_push_driver& operator=(const http2_push_driver&) = delete;

    http_client_pool& pool_;
    http_client_connection& connection_;
    http_client_response response_;
    http_client_response_state& state_;
    operation_deadline timeout_;
    http_client_connection::http2_stream stream_;
    bool registered_{false};
};

void http_client_pool::accept_http2_push(http_client_connection& connection, const http2_push_promise_event& promise) {
    const bool https = config_.scheme_ == http_scheme::https;
    // Only a connection authenticated for the alias route speaks for its
    // origin (RFC 9113 section 8.4).
    if (!config_.push_.enabled_ || !connection.alias_route() || pushes_.size() >= config_.push_.max_queued_pushes_ ||
        active_pushes_ >= config_.push_.max_concurrent_pushes_ ||
        !http_ascii_equals_ignore_case(promise.request_.scheme_, https ? "https" : "http") ||
        !http_authorities_equal(borrowed_text(promise.request_.authority_),
            borrowed_text(std::string_view(config_.authority_)), https ? 443 : 80)) {
        ++rejected_pushes_;
        connection.http2_.submit_reset(promise.promised_stream_id_);
        return;
    }
    try {
        http_client_response response(*this);
        auto& state_value = *response.state_;
        state_value.buffered_limit_ = config_.max_response_bytes_;
        state_value.transport_ = http_client_response_transport::http2;
        state_value.request_method_ = classify_http_method(promise.request_.method_);
        state_value.connection_index_ = connection.index();
        state_value.connection_serial_ = connection.serial();
        state_value.connection_generation_ = connection.generation_;
        state_value.stream_id_ = promise.promised_stream_id_;
        state_value.request_id_ = connection.next_http2_request_id();
        state_value.promised_request_.emplace();
        auto& request = *state_value.promised_request_;
        request.method_ = promise.request_.method_;
        request.scheme_ = promise.request_.scheme_;
        request.authority_ = promise.request_.authority_;
        request.path_ = promise.request_.path_;
        request.headers_.reserve(promise.request_.headers_.size());
        for (const auto& field : promise.request_.headers_) {
            request.headers_.emplace_back(field.name(), field.value());
        }
        auto driver = std::make_unique<http2_push_driver>(*this, connection, http_client_response(response.state_), state_value);
        driver->stream_.timeout_ = &driver->timeout_;
        driver->stream_.request_target_ = request.path_;
        driver->stream_.request_id_ = state_value.request_id_;
        driver->stream_.stream_id_ = promise.promised_stream_id_;
        connection.register_http2_stream(driver->stream_);
        driver->registered_ = true;
        ++active_pushes_;
        // Register before the reader drains another event. The driver is address
        // stable and its destructor unregisters on cold spawn failure as well.
        background_tasks_.spawn(run_http2_push(std::move(driver)));
        pushes_.push_back(http_client_push(std::move(response)));
        ++received_pushes_;
    } catch (...) {
        ++rejected_pushes_;
        connection.http2_.submit_reset(promise.promised_stream_id_);
        // A resource admission failure is local to this push, not its parent.
    }
}

task<void> http_client_pool::run_http2_push(
    std::unique_ptr<http2_push_driver> driver) {
    auto& stream = driver->stream_;
    auto& state_value = driver->state_;
    try {
        // The registered stream's timer bounds the push (push_.timeout_ and
        // read_timeout_).
        while ((!stream.complete_ || state_value.informational_active_) && !stream.failed()) {
            co_await stream.signal_.wait();
        }
        state_value.failure_ = stream.failure_;
        if (stream.error_) {
            state_value.failure_ =
                std::make_exception_ptr(http_client_error(*stream.error_, http_client_error_message(*stream.error_)));
        }
    } catch (...) {
        state_value.failure_ = std::current_exception();
        driver->connection_.http2_.submit_reset(stream.stream_id_);
    }
    if (state_value.failure_ != nullptr) {
        state_value.discard_failed_encoded_response();
    }
    state_value.complete_ = true;
    state_value.head_signal_.notify();
    state_value.data_signal_.notify();
    state_value.space_signal_.notify();
}

}  // namespace ruvia::detail
