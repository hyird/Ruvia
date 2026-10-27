#include <algorithm>
#include <utility>

#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request_target.h"

#include "client/http_client_connection.h"
#include "client/http_client_pool.h"
#include "client/http_client_response_decoding.h"
#include "client/http_client_response_state.h"
#include "client/http_client_transport_error.h"

namespace ruvia::detail {

http_client_connection::http2_stream* http_client_connection::find_http2_stream(std::uint32_t stream_id) noexcept {
    const auto match = std::ranges::find_if(
        http2_streams_, [stream_id](const http2_stream* stream) { return stream->stream_id_ == stream_id; });
    return match == http2_streams_.end() ? nullptr : *match;
}

std::uint64_t http_client_connection::next_http2_request_id() noexcept {
    next_http2_request_id_ = next_http2_request_id_ + 1 == 0 ? 1 : next_http2_request_id_ + 1;
    return next_http2_request_id_;
}

void http_client_connection::register_http2_stream(http2_stream& stream) {
    http2_streams_.push_back(&stream);
}

bool http_client_connection::http2_exchange_complete(const http2_stream& stream) noexcept {
    if (!stream.complete_) {
        return false;
    }
    if (const auto* output = stream.state_->output()) {
        return output->ended_ || output->stopped_;
    }
    return !http2_.has_connection() || !http2_.connection().has_queued_data(stream.stream_id_);
}

void http_client_connection::fail_http2_stream(
    http2_stream& stream, std::exception_ptr failure, bool reset_stream) noexcept {
    if (http2_exchange_complete(stream) || stream.failed()) {
        return;
    }
    stream.failure_ = std::move(failure);
    if (reset_stream) {
        http2_.submit_reset(stream.stream_id_);
    }
    stream.signal_.notify();
    http2_.state_signal().notify();
}

void http_client_connection::release_response_data(http_client_response_state& state_value) noexcept {
    if (!state_value.http2_data_credit_) {
        return;
    }
    // The token returns credit to its original connection even if that session
    // has already retired. Its destructor also defers allocation failures.
    state_value.http2_data_credit_.reset();
    http2_.wake_writer();
}

void http_client_connection::publish_http2_head(http2_stream& stream, http2_response_head_event& event) {
    auto& state_value = *stream.state_;
    if (state_value.upload_ && !state_value.upload_->output_.ended_ &&
        event.request_content_signal() == http_client_request_content_signal::exchange_complete) {
        state_value.upload_->output_.stop();
    }
    auto head = std::move(event).take_head();
    state_value.status_ = head.received_status();
    state_value.protocol_version_ = head.protocol_version();
    state_value.response_body_plan_ = plan_http_response_body(state_value.request_method_, state_value.status_.status());
    const auto headers = head.headers();
    state_value.headers_.clear();
    state_value.headers_.reserve(headers.size());
    for (const auto& header : headers) {
        state_value.headers_.emplace_back(header.name(), header.value(), state_value.resource_);
    }
    // Publish cookies with the final head, not again when its body completes.
    pool_.policy().retain_response_cookies(stream.request_target_, state_value.headers_);
    if (state_value.tunnel_) {
        state_value.tunnel_->accepted_ =
            state_value.response_body_plan_->content_semantics() == http_response_content_semantics::connect_tunnel;
        if (state_value.tunnel_->accepted_ && state_value.tunnel_->udp_) {
            if (validate_http_connect_udp_response(
                    state_value.protocol_version_, state_value.status_.status().value(), state_value.headers_)
                    .has_value()) {
                throw http_client_error(http_client_error::code_type::protocol_error, "invalid CONNECT-UDP response head");
            }
        }
        if (!state_value.tunnel_->accepted_) {
            state_value.tunnel_->output_.stop();
        }
        stream.signal_.notify();
    }
    if (!state_value.tunnel_ || !state_value.tunnel_->accepted_) {
        configure_http_client_response_decoding(state_value);
    }
    state_value.head_ready_ = true;
    state_value.head_signal_.notify();
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
    const auto limit = pool_.config().max_response_bytes_;
    const auto retained = state_value.buffered_.size() - state_value.offset_ + state_value.pending_.size();
    if (state_value.collect_all_ && bytes_value.size() > limit - std::min(retained, limit)) {
        // Complete collection failed before any encoded bytes were exposed.
        // Retaining the now-unusable prefix only holds response-budget memory.
        state_value.discard_response_body();
        stream->error_ = http_client_error::code_type::response_too_large;
        http2_.submit_reset(stream_id);
        stream->signal_.notify();
        return;
    }
    try {
        state_value.pending_.append(bytes_value);
        state_value.data_signal_.notify();
    } catch (...) {
        fail_http2_stream(*stream, std::current_exception(), true);
        return;
    }
    if (state_value.collect_all_ || !credit.valid()) {
        return;
    }
    auto& retained_credit = state_value.http2_data_credit_;
    if (!retained_credit) {
        retained_credit.emplace(std::move(credit));
    } else {
        retained_credit->merge(std::move(credit));
    }
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
            state_value.trailers_.emplace_back(trailer.name(), trailer.value(), state_value.resource_);
        }
        if (stream->timeout_ == nullptr) {
            std::terminate();
        }
        if (stream->timeout_->expired()) {
            throw http_client_error(http_client_error::code_type::timeout, "HTTP/2 response body decoding timed out");
        }
        decode_http_client_response_body(
            state_value, content_semantics_present, pool_.config().max_response_bytes_);
        if (stream->timeout_->expired()) {
            throw http_client_error(http_client_error::code_type::timeout, "HTTP/2 response body decoding timed out");
        }
        stream->complete_ = true;
        // Response EOF is independent of the request send half (RFC 9113 8.1).
        // A consumer can finish reading now and still write/end its upload.
        state_value.complete_ = true;
        state_value.data_signal_.notify();
        stream->signal_.notify();
    } catch (...) {
        fail_http2_stream(*stream, std::current_exception(), false);
    }
}

void http_client_connection::on_http2_event(http2_event& event) {
    if (const auto* origins = event.origin_advertisement()) {
        (void)pool_.advertisements().retain(index_, http_protocol_version::http2, *origins);
    } else if (const auto* service = event.alternative_service_advertisement()) {
        (void)pool_.advertisements().retain(index_, *service);
    }
    if (const auto* push = event.push_promise()) {
        pool_.accept_http2_push(*this, *push);
    } else if (const auto* interim = event.informational_head()) {
        auto* stream = find_http2_stream(interim->stream_id());
        if (stream == nullptr || stream->complete_ || stream->failed()) {
            return;
        }
        try {
            auto& state_value = *stream->state_;
            state_value.retain_informational(interim->head().status(), interim->head().headers());
            if (state_value.upload_ &&
                interim->request_content_signal() == http_client_request_content_signal::continue_value) {
                state_value.upload_->content_released_ = true;
                stream->signal_.notify();
            }
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
        // Unretained credits returned on destruction; flush their WINDOW_UPDATE.
        http2_.wake_writer();
    } else if (auto* data = event.tunnel_data()) {
        consume_http2_data(data->stream_id(), data->bytes(), data->take_credit());
        http2_.wake_writer();
    } else if (const auto* tunnel_end = event.tunnel_end()) {
        if (auto* stream = find_http2_stream(tunnel_end->stream_id()); stream != nullptr && !stream->failed()) {
            auto& state_value = *stream->state_;
            if (!state_value.tunnel_ || !state_value.tunnel_->accepted_) {
                stream->error_ = http_client_error::code_type::protocol_error;
            } else {
                state_value.tunnel_->receive_ended_ = true;
                state_value.data_signal_.notify();
            }
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
            } else if (!state_value.tunnel_ || !state_value.tunnel_->accepted_ || !state_value.tunnel_->receive_ended_ ||
                       !state_value.tunnel_->output_.end_requested_) {
                stream->error_ = http_client_error::code_type::protocol_error;
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
    } else if (event.goaway() != nullptr) {
        http2_.mark_draining();
    }
}

void http_client_connection::on_http2_data_drained(std::uint32_t stream_id) noexcept {
    if (auto* stream = find_http2_stream(stream_id)) {
        stream->signal_.notify();
    }
}

void http_client_connection::on_http2_failure(
    const client_transport_result& transport, std::exception_ptr failure) noexcept {
    connected_ = false;
    if (pool_.config().protocol_ == http_client_protocol::negotiate) {
        protocol_ = protocol_type::unknown;
    }
    auto stream_error = http_client_error::code_type::io_error;
    if (const auto reason = transport_.abort_reason(); reason != client_abort_reason::none) {
        stream_error = http_client_abort_error(reason);
    } else if (transport.status_ == client_transport_status::aborted) {
        stream_error = http_client_abort_error(transport.abort_);
    } else if (transport.status_ != client_transport_status::ok) {
        stream_error = http_client_transport_error(transport);
    }
    for (auto* stream : http2_streams_) {
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
    const auto match = std::ranges::find_if(
        http2_streams_, [request_id](const http2_stream* stream) { return stream->request_id_ == request_id; });
    if (match == http2_streams_.end()) {
        return;
    }
    auto& stream = **match;
    if (http2_exchange_complete(stream) || stream.failed() || stream.retryable_) {
        return;
    }
    stream.error_ = http_client_abort_error(reason);
    http2_.submit_reset(stream.stream_id_, error);
    stream.signal_.notify();
    http2_.state_signal().notify();
}

void http_client_connection::remove_http2_stream(http2_stream& stream) noexcept {
    // The core decides whether a half is still open. In particular a successful
    // early response does not retire an abandoned or Continue-cancelled upload.
    if (!stream.retryable_) {
        http2_.submit_reset(stream.stream_id_);
    }
    release_response_data(*stream.state_);
    const auto match = std::ranges::find(http2_streams_, &stream);
    if (match == http2_streams_.end()) {
        std::terminate();
    }
    http2_streams_.erase(match);
    http2_.state_signal().notify();
    if (http2_.draining() && http2_streams_.empty() && http2_.running()) {
        // A drained session admits no new streams; retire it so the next
        // request opens a fresh connection.
        http2_.fail({.status_ = client_transport_status::io_failed,
            .error_ = std::make_error_code(std::errc::operation_canceled)});
    }
}

void http_client_connection::reprioritize(http_client_response_state& state_value, http_priority priority) {
    if (generation_ != state_value.connection_generation_ || !http2_.has_connection() || !http2_.running() ||
        http2_.failed()) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP/2 response connection is retired");
    }
    const auto status = http2_.connection().submit_priority_update(static_cast<std::uint32_t>(state_value.stream_id_),
        {.urgency_ = priority.urgency_, .incremental_ = priority.incremental_});
    if (status != http2_submit_status::accepted) {
        throw http_client_error(http_client_error::code_type::protocol_error, "HTTP/2 priority update rejected");
    }
    http2_.wake_writer();
}

task<void> http_client_connection::wait_for_http2_session_stop(
    const operation_timeout& timeout, stop_token stop_token_value) {
    worker_timer_registration deadline_timer;
    if (const auto remaining = timeout.remaining()) {
        if (remaining->count() == 0) {
            throw http_client_error(http_client_error::code_type::timeout, "HTTP/2 session shutdown wait timed out");
        }
        (void)pool_.worker().schedule_timer(deadline_timer, worker_timer_deadline_after(*remaining),
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
    worker_cancellation_registration cancellation(pool_.cancellation_target(), waiter.cancellation_id_);
    cancellation.arm(stop_token_value);
    while (http2_.running() || !http2_streams_.empty()) {
        if (stop_token_value.stop_requested()) {
            throw http_client_error(http_client_error::code_type::cancelled, "HTTP/2 session shutdown wait cancelled");
        }
        if (timeout.expired()) {
            throw http_client_error(http_client_error::code_type::timeout, "HTTP/2 session shutdown wait timed out");
        }
        co_await http2_.state_signal().wait();
    }
}

task<bool> http_client_connection::execute_http2(const http_client_request_storage& request,
    const operation_timeout& timeout, stop_token stop_token_value, http_client_response_state& state_value) {
    const auto& config = pool_.config();
    auto* const resource = pool_.resource();
    std::pmr::vector<http_header_view> headers(resource);
    std::pmr::string cookie_header(resource);
    auto source_value = pool_.policy().prepare_request(request, headers, cookie_header);
    const std::string_view scheme = config.scheme_ == http_scheme::https ? "https" : "http";
    const auto* body = source_value.content_.borrowed_bytes();
    const auto content = request.upload() != nullptr
                             ? ::ruvia::http2_request_content::streaming(request.upload()->config_.content_length_)
                         : body != nullptr ? ::ruvia::http2_request_content::known_length(body->value().size())
                                           : ::ruvia::http2_request_content::none();

    for (int attempt_value = 0; attempt_value < 2; ++attempt_value) {
        if (!http2_.usable()) {
            if (http2_.draining() && http2_streams_.empty() && http2_.running()) {
                http2_.fail({.status_ = client_transport_status::io_failed,
                    .error_ = std::make_error_code(std::errc::operation_canceled)});
            }
            co_await wait_for_http2_session_stop(timeout, stop_token_value);
            (void)co_await ensure_connected(timeout, timeout, stop_token_value, state_value);
            if (protocol_ != protocol_type::http2) {
                co_return false;
            }
        }

        http2_stream stream(pool_.worker(), state_value);
        stream.timeout_ = &timeout;
        stream.request_target_ = request.target();
        stream.request_id_ = next_http2_request_id();
        state_value.transport_ = http_client_response_transport::http2;
        state_value.request_method_ = classify_http_method(request.method());
        state_value.connection_index_ = index_;
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
        worker_timer_registration deadline_timer;
        if (const auto remaining = timeout.remaining()) {
            (void)pool_.worker().schedule_timer(deadline_timer, worker_timer_deadline_after(*remaining),
                [this, request_id = stream.request_id_](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        cancel_http2_stream(request_id, client_abort_reason::timeout);
                    }
                });
        }
        worker_cancellation_registration cancellation(pool_.cancellation_target(), stream.cancellation_id_);
        state_value.cancellation_id_ = cancellation.id();
        cancellation.arm(stop_token_value);

        auto& connection = http2_.connection();
        for (;;) {
            if (stream.failed()) {
                break;
            }
            if (timeout.expired()) {
                stream.error_ = http_client_error::code_type::timeout;
                break;
            }
            const auto expectation = request.upload() != nullptr ? request.upload()->config_.expectation_
                                                                 : http_client_request_expectation::none;
            const auto submitted =
                !request.is_tunnel()
                    ? connection.submit_request_head(::ruvia::http2_regular_request_head_view{
                          .method_ = source_value.method_,
                          .scheme_ = scheme,
                          .authority_ = borrowed_text(std::string_view(config.authority_)),
                          .target_ = source_value.target_,
                          .headers_ = headers,
                          .content_ = content,
                          .expectation_ = expectation})
                : request.tunnel_protocol().empty()
                    ? connection.submit_request_head(http2_connect_request_head_view{
                          .authority_ = borrowed_text(request.tunnel_authority()), .headers_ = headers})
                    : connection.submit_request_head(http2_extended_connect_request_head_view{
                          .protocol_ = borrowed_text(request.tunnel_protocol()),
                          .scheme_ = scheme,
                          .authority_ = borrowed_text(request.tunnel_authority()),
                          .target_ = source_value.target_,
                          .headers_ = headers});
            if (const auto* accepted = std::get_if<std::uint32_t>(&submitted)) {
                stream.stream_id_ = *accepted;
                state_value.stream_id_ = stream.stream_id_;
                if (body != nullptr && !body->value().empty()) {
                    co_await send_http2_buffered_request_body(stream, body->value());
                } else {
                    http2_.wake_writer();
                }
                break;
            }
            const auto error = std::get<http2_request_head_submit_error>(submitted);
            if (error == http2_request_head_submit_error::peer_stream_limit_reached ||
                error == http2_request_head_submit_error::local_stream_capacity_reached) {
                co_await http2_.state_signal().wait();
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
                stream.error_ = http_client_error::code_type::invalid_request;
            }
            break;
        }

        if (auto* upload = request.output();
            upload != nullptr && stream.stream_id_ != 0 && !stream.failed() && !stream.retryable_) {
            co_await stream_http2_upload(request, stream, *upload);
        }
        while ((!stream.complete_ || (request.output() == nullptr && connection.has_queued_data(stream.stream_id_))) &&
               !stream.failed() && !stream.retryable_) {
            if (request.tunnel() != nullptr && request.tunnel()->accepted_ && request.tunnel()->receive_ended_ &&
                request.tunnel()->output_.ended_) {
                stream.complete_ = true;
                break;
            }
            co_await stream.signal_.wait();
        }
        deadline_timer.cancel();
        const bool retryable = stream.retryable_;
        const auto error = stream.error_;
        const auto failure = stream.failure_;
        registration.reset();
        if (retryable && request.output() == nullptr && attempt_value == 0 && !timeout.expired()) {
            continue;
        }
        if (retryable) {
            throw http_client_error(http_client_error::code_type::protocol_error,
                "HTTP/2 request was not processed by the server (GOAWAY or REFUSED_STREAM)");
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        if (error) {
            switch (*error) {
                case http_client_error::code_type::timeout:
                    throw http_client_error(*error, "HTTP/2 request timed out");
                case http_client_error::code_type::cancelled:
                    throw http_client_error(*error, "HTTP/2 request cancelled");
                case http_client_error::code_type::response_too_large:
                    throw http_client_error(*error, "HTTP/2 response exceeds configured byte limit");
                case http_client_error::code_type::closing:
                    throw http_client_error(*error, "HTTP client pool is closing");
                case http_client_error::code_type::tls_failed:
                    throw http_client_error(*error, "HTTP/2 TLS connection failed");
                case http_client_error::code_type::io_error:
                    throw http_client_error(*error, "HTTP/2 connection failed");
                case http_client_error::code_type::protocol_error:
                    throw http_client_error(*error, "HTTP/2 protocol failed");
                default:
                    throw http_client_error(*error, "HTTP/2 stream failed");
            }
        }
        co_return true;
    }
    throw http_client_error(http_client_error::code_type::protocol_error, "HTTP/2 request retry exhausted");
}

task<void> http_client_connection::send_http2_buffered_request_body(
    http2_stream& stream, std::string_view body) {
    // Admit at most one frame-sized slice per active request stream until the
    // session has written that slice, bounding both queued and serialized output.
    constexpr std::size_t submit_bytes = http2_default_max_frame_size;
    auto& connection = http2_.connection();
    std::size_t offset = 0;
    while (offset < body.size() && !stream.failed() && !stream.retryable_) {
        const auto count = std::min(submit_bytes, body.size() - offset);
        const auto end_stream =
            offset + count == body.size() ? http2_end_stream::end_stream : http2_end_stream::keep_open;
        http2_data_submit_status submitted{};
        try {
            submitted = connection.submit_data(stream.stream_id_, body.substr(offset, count), end_stream);
        } catch (...) {
            stream.error_ = http_client_error::code_type::protocol_error;
            http2_.submit_reset(stream.stream_id_);
            co_return;
        }
        if (submitted == http2_data_submit_status::backpressured) {
            co_await wait_for_http2_output(stream);
            continue;
        }
        if (submitted != http2_data_submit_status::accepted &&
            submitted != http2_data_submit_status::queued) {
            if (!stream.complete_) {
                stream.error_ = http_client_error::code_type::protocol_error;
                http2_.submit_reset(stream.stream_id_);
            }
            co_return;
        }
        offset += count;
        http2_.wake_writer();
        co_await wait_for_http2_output(stream);
    }
}

task<void> http_client_connection::wait_for_http2_output(
    http2_stream& stream, const http_client_output_queue* upload) {
    auto& connection = http2_.connection();
    while (http2_.has_connection() &&
           (connection.wants_write() || http2_.writing() ||
               connection.data_queue_state(stream.stream_id_) == http2_data_queue_state::queued) &&
           !stream.failed() && !stream.retryable_ && (upload == nullptr || !upload->stopped_)) {
        co_await http2_.state_signal().wait();
    }
}

task<bool> http_client_connection::send_http2_output_chunk(
    http2_stream& stream, http_client_output_queue& upload) {
    // Keep one frame-sized copy in the sans-I/O/session output while the queue
    // retains the caller's whole chunk until every slice reaches the transport.
    constexpr std::size_t submit_bytes = http2_default_max_frame_size;
    auto& connection = http2_.connection();
    std::size_t offset = 0;
    while (offset < upload.chunk_.size() && !stream.failed() && !stream.retryable_ && !upload.stopped_) {
        const auto count = std::min(submit_bytes, upload.chunk_.size() - offset);
        http2_data_submit_status submitted{};
        try {
            submitted = connection.submit_data(
                stream.stream_id_, std::string_view(upload.chunk_).substr(offset, count), http2_end_stream::keep_open);
        } catch (const std::invalid_argument&) {
            stream.error_ = http_client_error::code_type::invalid_request;
            http2_.submit_reset(stream.stream_id_);
            co_return false;
        } catch (const std::logic_error&) {
            stream.error_ = http_client_error::code_type::invalid_request;
            http2_.submit_reset(stream.stream_id_);
            co_return false;
        }
        if (submitted == http2_data_submit_status::expectation_pending) {
            co_await stream.signal_.wait();
            continue;
        }
        if (submitted == http2_data_submit_status::backpressured) {
            co_await wait_for_http2_output(stream, &upload);
            continue;
        }
        if (submitted != http2_data_submit_status::accepted &&
            submitted != http2_data_submit_status::queued) {
            stream.error_ = http_client_error::code_type::invalid_request;
            http2_.submit_reset(stream.stream_id_);
            co_return false;
        }
        offset += count;
        http2_.wake_writer();
        co_await wait_for_http2_output(stream, &upload);
    }
    co_return offset == upload.chunk_.size() && !stream.failed() && !stream.retryable_ && !upload.stopped_;
}

task<void> http_client_connection::stream_http2_upload(
    const http_client_request_storage& request, http2_stream& stream, http_client_output_queue& upload) {
    auto& connection = http2_.connection();
    auto& state_value = *stream.state_;
    struct upload_wake_target final {
        worker_signal& stream_signal_;
        worker_signal& state_signal_;
    } wake_target{stream.signal_, http2_.state_signal()};
    upload.wake_target_ = &wake_target;
    upload.wake_ = [](void* target) noexcept {
        auto& wake_target_value = *static_cast<upload_wake_target*>(target);
        wake_target_value.stream_signal_.notify();
        wake_target_value.state_signal_.notify();
    };
    struct upload_wake_guard final {
        http_client_output_queue& upload_;
        ~upload_wake_guard() {
            upload_.wake_ = nullptr;
            upload_.wake_target_ = nullptr;
        }
    } wake_guard{upload};
    worker_timer_registration continue_timer;
    if (request.upload() != nullptr && !request.upload()->content_released_) {
        (void)pool_.worker().schedule_timer(continue_timer,
            worker_timer_deadline_after(request.upload()->config_.continue_timeout_),
            [&upload, policy = request.upload()](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired && !upload.stopped_) {
                    policy->content_released_ = true;
                    upload.notify_data();
                }
            });
    }
    while (!upload.ended_ && !upload.stopped_ && !stream.failed() && !stream.retryable_) {
        if (request.is_tunnel() ? !request.tunnel()->accepted_ : !request.upload()->content_released_) {
            co_await stream.signal_.wait();
            continue;
        }
        continue_timer.cancel();
        if (!request.is_tunnel() &&
            connection.release_request_content(stream.stream_id_) == http2_request_content_release_status::closed) {
            break;
        }
        if (upload.chunk_ready_) {
            if (co_await send_http2_output_chunk(stream, upload)) {
                upload.acknowledge_chunk();
            }
        } else if (upload.end_requested_) {
            bool finish_accepted{};
            try {
                if (request.is_tunnel()) {
                    const auto finished = connection.submit_data(stream.stream_id_, {}, http2_end_stream::end_stream);
                    finish_accepted = finished == http2_data_submit_status::accepted ||
                                      finished == http2_data_submit_status::queued;
                } else {
                    std::pmr::vector<http_header_view> trailers(state_value.resource_);
                    trailers.reserve(request.upload()->trailers_.size());
                    for (const auto& field : request.upload()->trailers_) {
                        trailers.emplace_back(field.name(), field.value());
                    }
                    const auto finished = connection.finish_request(stream.stream_id_, trailers);
                    finish_accepted =
                        finished == http2_finish_status::accepted || finished == http2_finish_status::queued;
                }
            } catch (const std::invalid_argument&) {
                finish_accepted = false;
            } catch (const std::logic_error&) {
                finish_accepted = false;
            }
            if (!finish_accepted) {
                stream.error_ = http_client_error::code_type::invalid_request;
                http2_.submit_reset(stream.stream_id_);
                break;
            }
            http2_.wake_writer();
            co_await wait_for_http2_output(stream, &upload);
            if (!stream.failed() && !stream.retryable_ && !upload.stopped_) {
                upload.finish();
            }
        } else {
            co_await stream.signal_.wait();
        }
    }
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
    operation_timeout timeout_;
    http_client_connection::http2_stream stream_;
    bool registered_{false};
};

void http_client_pool::accept_http2_push(http_client_connection& connection, const http2_push_promise_event& promise) {
    const bool https = config_.scheme_ == http_scheme::https;
    if (!config_.push_.enabled_ || pushes_.size() >= config_.push_.max_queued_pushes_ ||
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
        state_value.connection_generation_ = connection.generation_;
        state_value.stream_id_ = promise.promised_stream_id_;
        state_value.request_id_ = connection.next_http2_request_id();
        state_value.promised_request_.emplace(state_value.resource_);
        auto& request = *state_value.promised_request_;
        request.method_ = promise.request_.method_;
        request.scheme_ = promise.request_.scheme_;
        request.authority_ = promise.request_.authority_;
        request.path_ = promise.request_.path_;
        request.headers_.reserve(promise.request_.headers_.size());
        for (const auto& field : promise.request_.headers_) {
            request.headers_.emplace_back(field.name(), field.value(), state_value.resource_);
        }
        auto driver = make_pmr_object<http2_push_driver>(
            resource_, *this, connection, http_client_response(response.state_), state_value);
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
    std::unique_ptr<http2_push_driver, pmr_object_deleter<http2_push_driver>> driver) {
    auto& stream = driver->stream_;
    auto& state_value = driver->state_;
    worker_timer_registration timer;
    try {
        if (const auto remaining = driver->timeout_.remaining()) {
            (void)worker_.schedule_timer(timer, worker_timer_deadline_after(*remaining),
                [raw = driver.get()](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        raw->connection_.cancel_http2_stream(raw->stream_.request_id_, client_abort_reason::timeout);
                    }
                });
        }
        while (!stream.complete_ && !stream.failed()) {
            co_await stream.signal_.wait();
        }
        timer.cancel();
        state_value.failure_ = stream.failure_;
        if (stream.error_) {
            state_value.failure_ = std::make_exception_ptr(http_client_error(*stream.error_, "HTTP request failed"));
        }
    } catch (...) {
        state_value.failure_ = std::current_exception();
        driver->connection_.http2_.submit_reset(stream.stream_id_);
    }
    state_value.complete_ = true;
    state_value.head_signal_.notify();
    state_value.data_signal_.notify();
    state_value.space_signal_.notify();
}

}  // namespace ruvia::detail
