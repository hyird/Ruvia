#include "http2/websocket_http2_transport.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/http/http_pmr_string.h"

#include "client/websocket_client_state.h"

namespace ruvia::detail {

namespace {

constexpr std::size_t websocket_http2_data_chunk_bytes = 16384;

}  // namespace

websocket_http2_transport::websocket_http2_transport(websocket_client_state& owner_value,
    const worker_handle& worker_value, std::pmr::memory_resource* resource)
    : owner_(owner_value),
      drivers_(worker_value, {.resource_ = resource}),
      session_(*this, owner_value.transport_, drivers_, worker_value, owner_value.config_.write_timeout_, resource),
      received_(resource) {}

task<void> websocket_http2_transport::connect() {
    // The owner's connect deadline bounds the preface and SETTINGS exchange.
    const operation_timeout unbounded(std::nullopt);
    const auto started = co_await session_.start({}, unbounded);
    check_failure();
    switch (started.status_) {
        case http2_client_session_start_status::started:
            break;
        case http2_client_session_start_status::transport_failed:
            if (started.transport_.status_ == client_transport_status::eof) {
                throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                    "upstream closed before HTTP/2 SETTINGS");
            }
            owner_.throw_transport_failure(started.transport_);
        case http2_client_session_start_status::protocol_failed:
            throw websocket_client_error(
                websocket_client_error::code_type::protocol_error, "invalid HTTP/2 connection input");
    }
    const auto& config = owner_.config_;
    const auto submitted = [&]() {
        static constexpr std::size_t inline_header_capacity = 16;
        const std::size_t total_headers =
            config.headers_.size() + (config.user_agent_.empty() ? 0U : 1U);
        std::array<http_header_view, inline_header_capacity> inline_headers{};
        std::pmr::vector<http_header_view> heap_headers(received_.get_allocator().resource());
        std::span<const http_header_view> extended_headers{};
        if (total_headers <= inline_headers.size()) {
            std::size_t index = 0;
            for (const auto& header : config.headers_) {
                inline_headers[index++] = http_header_view{header.name(), header.value()};
            }
            if (!config.user_agent_.empty()) {
                inline_headers[index++] = http_header_view{"user-agent", config.user_agent_};
            }
            extended_headers = std::span<const http_header_view>(inline_headers.data(), total_headers);
        } else {
            heap_headers.reserve(total_headers);
            for (const auto& header : config.headers_) {
                heap_headers.emplace_back(header.name(), header.value());
            }
            if (!config.user_agent_.empty()) {
                heap_headers.emplace_back("user-agent", config.user_agent_);
            }
            extended_headers = heap_headers;
        }
        return config.negotiation_.submit_http2_request(session_.connection(),
            config.scheme_ == websocket_scheme::wss ? "https" : "http", config.authority_,
            config.target_, extended_headers);
    }();
    if (submitted.index() != 0) {
        throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
            "could not open HTTP/2 WebSocket Extended CONNECT");
    }
    stream_id_ = std::get<0>(submitted);
    session_.wake_writer();
    while (!response_) {
        check_failure();
        if (eof_) {
            throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                "upstream closed before WebSocket handshake response");
        }
        co_await session_.state_signal().wait();
    }
    check_failure();
    // RFC 8441 section 5: END_STREAM after the 2xx is the server's orderly
    // close of the tunnel (possibly with its Close frame in the same read);
    // only END_STREAM on the response HEADERS leaves no WebSocket stream.
    const auto negotiated = config.negotiation_.validate_response(*response_, !response_stream_ended_);
    if ((negotiated.index() != 0)) {
        throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
            "invalid HTTP/2 WebSocket handshake response");
    }
    owner_.selected_subprotocol_.assign(std::get<0>(negotiated).selected_subprotocol_);
    owner_.negotiated_compression_ = std::get<0>(negotiated).compression_;
    response_.reset();
    co_await flush();
}

void websocket_http2_transport::check_failure() const {
    if (failure_) {
        std::rethrow_exception(failure_);
    }
    owner_.throw_if_aborted();
}

void websocket_http2_transport::require_live() const {
    check_failure();
    if (session_.failed()) {
        // Only the peer's end of the connection fails a session silently.
        throw websocket_client_error(
            websocket_client_error::code_type::io_error, "HTTP/2 WebSocket connection ended");
    }
}

bool websocket_http2_transport::stop() noexcept {
    if (stopped_) {
        return goaway_pending_;
    }
    stopped_ = true;
    if (session_.has_connection() && session_.running() && !session_.failed()) {
        try {
            // A client drain queues its final GOAWAY(NO_ERROR) at once.
            session_.connection().begin_drain();
            goaway_pending_ = true;
            session_.wake_writer();
            // Wakes every waiter: operations observe the owner's abort reason.
            session_.mark_draining();
            return true;
        } catch (...) {
            // Without the frame there is nothing to flush; close now.
        }
    }
    session_.fail({.status_ = client_transport_status::aborted, .abort_ = owner_.abort_reason_});
    return false;
}

task<void> websocket_http2_transport::flush_goaway() {
    // The bound covers a peer that does not read and an unbounded
    // write_timeout_; a stopping worker cannot arm it and closes at once.
    const bool armed = owner_.worker_.schedule_timer(goaway_timer_,
                           worker_timer_deadline_after(websocket_http2_goaway_flush_time),
                           [this](worker_timer_outcome outcome) noexcept {
                               if (outcome == worker_timer_outcome::expired) {
                                   goaway_expired_ = true;
                                   session_.state_signal().notify();
                               }
                           }) == worker_timer_schedule_status::scheduled;
    while (armed && !goaway_expired_ && !session_.failed() &&
           (session_.writing() || session_.connection().wants_write())) {
        co_await session_.state_signal().wait();
    }
    goaway_timer_.cancel();
    goaway_pending_ = false;
    session_.fail({.status_ = client_transport_status::aborted, .abort_ = owner_.abort_reason_});
    owner_.transport_.abort(owner_.abort_reason_);
    owner_.transport_.close();
}

task<void> websocket_http2_transport::join() {
    if (goaway_pending_) {
        co_await flush_goaway();
    }
    co_await drivers_.join();
    received_credit_.reset();
    std::pmr::string(received_.get_allocator()).swap(received_);
}

void websocket_http2_transport::on_http2_event(http2_event& event) {
    if (auto* head = event.response_head()) {
        if (head->stream_id() != stream_id_ || response_) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "unexpected HTTP/2 WebSocket response head");
        }
        response_stream_ended_ = head->stream_ended();
        response_.emplace(std::move(*head).take_head());
    } else if (auto* data = event.tunnel_data()) {
        if (data->stream_id() != stream_id_) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "unexpected HTTP/2 WebSocket tunnel stream");
        }
        received_.append(data->bytes());
        auto credit = data->take_credit();
        if (received_.empty()) {
            (void)session_.connection().acknowledge(std::move(credit));
            return;
        }
        if (credit.valid()) {
            if (received_credit_) {
                received_credit_->merge(std::move(credit));
            } else {
                received_credit_.emplace(std::move(credit));
            }
        }
    } else if (auto* end = event.tunnel_end()) {
        if (end->stream_id() == stream_id_) {
            eof_ = true;
        }
    } else if (auto* message_end = event.message_end()) {
        if (message_end->stream_id() == stream_id_) {
            eof_ = true;
        }
    } else if (auto* reset = event.stream_closed()) {
        if (reset->stream_id() == stream_id_) {
            if (reset->source() == http2_stream_close_source::peer &&
                reset->error() == http2_error_code::no_error) {
                // NO_ERROR can retire a half-open Extended CONNECT after
                // the peer's DATA. Deliver those bytes before EOF: only the
                // websocket parser can prove that they contain peer Close.
                clean_reset_ = true;
                eof_ = true;
                return;
            }
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "upstream reset HTTP/2 WebSocket stream");
        }
    } else if (auto* unprocessed = event.request_unprocessed()) {
        if (unprocessed->stream_id() == stream_id_) {
            throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                "upstream did not process HTTP/2 WebSocket stream");
        }
    } else if (event.push_promise()) {
        throw websocket_client_error(websocket_client_error::code_type::protocol_error,
            "unexpected HTTP/2 server push");
    }
}

void websocket_http2_transport::on_http2_data_drained(std::uint32_t) noexcept {
    // flush() re-checks the stream's DATA queue on the session state signal
    // that every drain notifies.
}

void websocket_http2_transport::on_http2_failure(
    const client_transport_result& transport, std::exception_ptr failure) noexcept {
    if (stopped_) {
        return;
    }
    if (failure == nullptr && transport.status_ == client_transport_status::eof) {
        // The peer ended the connection: buffered DATA is still delivered,
        // then read() reports the end of the WebSocket transport. Output that
        // was still pending or in flight can no longer reach the peer.
        eof_ = true;
        output_lost_ = session_.writing() || session_.connection().wants_write() ||
                       (stream_id_ != 0 && session_.connection().data_queue_state(stream_id_) == http2_data_queue_state::queued);
        return;
    }
    try {
        if (failure != nullptr) {
            failure_ = std::move(failure);
        } else if (transport.status_ == client_transport_status::io_failed &&
                   transport.error_ == http2_client_protocol_failure().error_) {
            failure_ = std::make_exception_ptr(websocket_client_error(
                websocket_client_error::code_type::protocol_error, "invalid HTTP/2 connection input"));
        } else {
            failure_ = std::make_exception_ptr(owner_.transport_error(transport));
        }
    } catch (...) {
        failure_ = std::current_exception();
    }
    owner_.retire(transport.status_ == client_transport_status::aborted ? transport.abort_
                                                                        : client_abort_reason::closing);
}

task<void> websocket_http2_transport::flush() {
    session_.wake_writer();
    for (;;) {
        check_failure();
        if (session_.failed()) {
            // A peer that ends the connection right after receiving the final
            // bytes (e.g. after our END_STREAM) has still received them.
            if (output_lost_) {
                throw websocket_client_error(
                    websocket_client_error::code_type::io_error, "HTTP/2 WebSocket connection ended");
            }
            co_return;
        }
        auto& connection = session_.connection();
        if (!connection.wants_write() && !session_.writing() &&
            connection.data_queue_state(stream_id_) != http2_data_queue_state::queued) {
            co_return;
        }
        co_await session_.state_signal().wait();
    }
}

task<std::size_t> websocket_http2_transport::read(std::span<char> output) {
    for (;;) {
        check_failure();
        const auto available = received_.size() - read_offset_;
        if (available != 0) {
            const auto count = std::min(output.size(), available);
            std::memcpy(output.data(), received_.data() + read_offset_, count);
            read_offset_ += count;
            if (read_offset_ == received_.size()) {
                http_clear_pmr_string_retaining_small(received_, 2 * websocket_http2_data_chunk_bytes);
                read_offset_ = 0;
                received_credit_.reset();
                session_.wake_writer();
            }
            co_return count;
        }
        if (eof_) {
            co_return 0;
        }
        co_await session_.state_signal().wait();
    }
}

task<void> websocket_http2_transport::write(std::string_view bytes_value) {
    while (!bytes_value.empty()) {
        require_live();
        auto& connection = session_.connection();
        if (connection.stream_aborted(stream_id_)) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "HTTP/2 WebSocket stream is closed");
        }
        const auto count = std::min(bytes_value.size(), websocket_http2_data_chunk_bytes);
        http2_data_submit_status submitted{};
        try {
            submitted = connection.submit_data(stream_id_, bytes_value.substr(0, count), http2_end_stream::keep_open);
        } catch (...) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "could not submit HTTP/2 WebSocket DATA");
        }
        if (submitted == http2_data_submit_status::backpressured) {
            session_.wake_writer();
            co_await session_.state_signal().wait();
            continue;
        }
        if (submitted != http2_data_submit_status::accepted && submitted != http2_data_submit_status::queued) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "could not submit HTTP/2 WebSocket DATA");
        }
        bytes_value.remove_prefix(count);
        co_await flush();
    }
}

task<void> websocket_http2_transport::finish() {
    check_failure();
    if (clean_reset_ || session_.failed()) {
        // The peer already retired the stream or the whole connection. The
        // websocket caller reaches finish only after parsing its protocol's
        // transport-end plan.
        co_return;
    }
    http2_data_submit_status submitted{};
    try {
        submitted = session_.connection().submit_data(stream_id_, {}, http2_end_stream::end_stream);
    } catch (...) {
        throw websocket_client_error(websocket_client_error::code_type::protocol_error,
            "could not conclude HTTP/2 WebSocket stream");
    }
    if (submitted != http2_data_submit_status::accepted && submitted != http2_data_submit_status::queued) {
        throw websocket_client_error(websocket_client_error::code_type::protocol_error,
            "could not conclude HTTP/2 WebSocket stream");
    }
    co_await flush();
}

}  // namespace ruvia::detail
