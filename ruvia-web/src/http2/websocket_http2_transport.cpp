#include "http2/websocket_http2_transport.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/websocket_client_negotiation.h"

#include "client/websocket_client_state.h"

namespace ruvia::detail {

websocket_http2_transport::websocket_http2_transport(websocket_client_state& owner_value,
    const worker_handle& worker_value, std::pmr::memory_resource* resource)
    : owner_(owner_value),
      connection_(ruvia::http2_connection::client({.resource_ = resource})),
      drivers_(worker_value, {.resource_ = resource}),
      progress_(worker_value),
      writer_wake_(worker_value),
      received_(resource) {}

task<void> websocket_http2_transport::connect() {
    drivers_.spawn(run_reader());
    drivers_.spawn(run_writer());
    writer_wake_.notify();
    while (!connection_.received_peer_settings()) {
        check_failure();
        if (eof_) {
            throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                "upstream closed before HTTP/2 SETTINGS");
        }
        co_await progress_.wait();
    }
    check_failure();
    auto* resource = received_.get_allocator().resource();
    std::pmr::vector<http_header_view> headers(resource);
    headers.reserve(owner_.config_.headers_.size() + 1);
    for (const auto& header : owner_.config_.headers_) {
        headers.emplace_back(header.name_, header.value_);
    }
    if (!owner_.config_.user_agent_.empty()) {
        headers.emplace_back("user-agent", owner_.config_.user_agent_);
    }
    std::pmr::vector<std::string_view> subprotocols(resource);
    subprotocols.reserve(owner_.config_.subprotocols_.size());
    for (const auto& value : owner_.config_.subprotocols_) {
        subprotocols.emplace_back(value);
    }
    websocket_client_negotiation negotiation({.headers_ = headers,
                                                 .subprotocols_ = subprotocols,
                                                 .deflate_ = owner_.config_.deflate_},
        resource);
    auto authority = client_uri_host(owner_.config_.host_, resource);
    client_port_text_buffer_type port_buffer{};
    authority.append(":");
    authority.append(format_client_port(owner_.port(), port_buffer));
    const auto submitted = negotiation.submit_http2_request(connection_,
        owner_.config_.scheme_ == websocket_scheme::wss ? "https" : "http",
        authority, owner_.config_.target_);
    if (!submitted.submitted()) {
        throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
            "could not open HTTP/2 WebSocket Extended CONNECT");
    }
    stream_id_ = submitted.submitted()->stream_id();
    writer_wake_.notify();
    while (!response_) {
        check_failure();
        if (eof_) {
            throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                "upstream closed before WebSocket handshake response");
        }
        co_await progress_.wait();
    }
    check_failure();
    const auto negotiated = negotiation.validate_response(*response_, !eof_);
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
    owner_.throw_abort();
}

void websocket_http2_transport::fail(std::exception_ptr failure) noexcept {
    if (!stopped_ && !failure_) {
        failure_ = std::move(failure);
    }
    owner_.close_on_worker(websocket_client_state::abort_reason_type::closing);
}

void websocket_http2_transport::stop() noexcept {
    stopped_ = true;
    drivers_.request_stop();
    progress_.notify();
    writer_wake_.notify();
}

task<void> websocket_http2_transport::join() {
    co_await drivers_.join();
    received_credit_.reset();
    std::pmr::string(received_.get_allocator()).swap(received_);
}

void websocket_http2_transport::drain_events() {
    while (auto event = connection_.next_event()) {
        if (auto* head = event->response_head()) {
            if (head->stream_id() != stream_id_ || response_) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "unexpected HTTP/2 WebSocket response head");
            }
            response_.emplace(std::move(*head).take_head());
        } else if (auto* data = event->tunnel_data()) {
            if (data->stream_id() != stream_id_) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "unexpected HTTP/2 WebSocket tunnel stream");
            }
            received_.append(data->bytes());
            auto credit = data->take_credit();
            if (received_.empty()) {
                (void)connection_.acknowledge(std::move(credit));
                continue;
            }
            if (credit.valid()) {
                if (received_credit_) {
                    if (received_credit_->merge(std::move(credit)) != http2_received_data_credit_merge_status::merged) {
                        std::terminate();
                    }
                } else {
                    received_credit_.emplace(std::move(credit));
                }
            }
        } else if (auto* end = event->tunnel_end()) {
            if (end->stream_id() == stream_id_) {
                eof_ = true;
            }
        } else if (auto* message_end = event->message_end()) {
            if (message_end->stream_id() == stream_id_) {
                eof_ = true;
            }
        } else if (auto* reset = event->stream_closed()) {
            if (reset->stream_id() == stream_id_) {
                if (reset->source() == http2_stream_close_source::peer &&
                    reset->error() == http2_error_code::no_error) {
                    // NO_ERROR can retire a half-open Extended CONNECT after
                    // the peer's DATA. Deliver those bytes before EOF: only the
                    // websocket parser can prove that they contain peer Close.
                    clean_reset_ = true;
                    eof_ = true;
                    continue;
                }
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "upstream reset HTTP/2 WebSocket stream");
            }
        } else if (auto* unprocessed = event->request_unprocessed()) {
            if (unprocessed->stream_id() == stream_id_) {
                throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                    "upstream did not process HTTP/2 WebSocket stream");
            }
        } else if (event->push_promise()) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "unexpected HTTP/2 server push");
        }
    }
    writer_wake_.notify();
    progress_.notify();
}

task<void> websocket_http2_transport::run_reader() {
    std::array<char, 16384> bytes_value{};
    try {
        while (!stopped_) {
            const auto count = co_await owner_.read_socket(bytes_value);
            if (count == 0) {
                eof_ = true;
                progress_.notify();
                co_return;
            }
            const auto input = std::string_view(bytes_value.data(), count);
            for (;;) {
                const auto result_value = connection_.feed(input);
                drain_events();
                if (result_value == http2_feed_result::protocol_failure || connection_.connection_error()) {
                    throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                        "invalid HTTP/2 connection input");
                }
                if (result_value != http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    } catch (...) {
        fail(std::current_exception());
    }
}

task<void> websocket_http2_transport::run_writer() {
    std::pmr::string bytes_value(received_.get_allocator());
    worker_timer_registration timer;
    try {
        while (!stopped_) {
            if (!connection_.wants_write()) {
                co_await writer_wake_.wait();
                continue;
            }
            bytes_value.clear();
            (void)connection_.take_output_batch(16384, bytes_value);
            writer_active_ = true;
            owner_.arm(timer, owner_.config_.write_timeout_, websocket_client_state::abort_reason_type::timeout);
            co_await owner_.write_socket(bytes_value);
            owner_.disarm(timer);
            writer_active_ = false;
            progress_.notify();
        }
    } catch (...) {
        owner_.disarm(timer);
        writer_active_ = false;
        fail(std::current_exception());
    }
}

task<void> websocket_http2_transport::flush() {
    writer_wake_.notify();
    for (;;) {
        check_failure();
        if (!connection_.wants_write() && !writer_active_ &&
            connection_.data_queue_state(stream_id_) != http2_data_queue_state::queued) {
            co_return;
        }
        co_await progress_.wait();
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
                received_.clear();
                read_offset_ = 0;
                received_credit_.reset();
                writer_wake_.notify();
            }
            co_return count;
        }
        if (eof_) {
            co_return 0;
        }
        co_await progress_.wait();
    }
}

task<void> websocket_http2_transport::write(std::string_view bytes_value) {
    while (!bytes_value.empty()) {
        check_failure();
        if (connection_.stream_aborted(stream_id_)) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "HTTP/2 WebSocket stream is closed");
        }
        const auto count = std::min<std::size_t>(bytes_value.size(), 16384);
        const auto submitted = connection_.submit_data(stream_id_, bytes_value.substr(0, count), http2_end_stream::keep_open);
        if (submitted == http2_data_submit_status::backpressured) {
            writer_wake_.notify();
            co_await progress_.wait();
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
    if (clean_reset_) {
        // The peer already retired both HTTP/2 halves. The websocket caller
        // reaches finish only after parsing its protocol's transport-end plan.
        co_return;
    }
    const auto submitted = connection_.submit_data(stream_id_, {}, http2_end_stream::end_stream);
    if (submitted != http2_data_submit_status::accepted && submitted != http2_data_submit_status::queued) {
        throw websocket_client_error(websocket_client_error::code_type::protocol_error,
            "could not conclude HTTP/2 WebSocket stream");
    }
    co_await flush();
}

}  // namespace ruvia::detail
