#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_allocator.hpp>
#include <asio/bind_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/ip/udp.hpp>
#include <asio/socket_base.hpp>

#include "connection_lifecycle.h"
#include "context_access.h"
#include "crypto.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http3_quic_connection.h"
#include "http_content_negotiation.h"
#include "http_header.h"
#include "http_response.h"
#include "response_file.h"
#include "server_session.h"
#include "stop_token.h"
#include "streaming.h"
#include "tls.h"
#include "websocket_handshake.h"
#include "worker_ref.h"
#if !defined(_WIN32)
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <linux/filter.h>
#endif

#include <concepts>

#include <openssl/ssl.h>

#include "http_client_pool.h"
#include "worker_wait.h"

namespace ruvia::detail {
namespace {

// Product limit on a drain's wait between the shutdown-notice GOAWAY and the
// final one (a request cap or a connection timeout; a stop closes at once),
// also capped at the connection's closing period, three probe timeouts, which
// a round trip never exceeds.
constexpr std::chrono::milliseconds http3_drain_round_trip_bound{std::chrono::seconds(1)};
// Datagrams one flush writes before yielding the worker to other connections.
constexpr std::size_t http3_flush_burst = 32;
// Upper bound of a request stream's receive window. A streamed request body
// holds at most this much unconsumed content (credit returns only as the
// handler reads), so it is also capped at max_buffered_body_bytes_.
constexpr std::uint64_t http3_max_stream_receive_window = 1024 * 1024;
constexpr std::uint64_t http3_max_connection_receive_window = 16 * 1024 * 1024;
constexpr std::uint64_t http3_max_concurrent_requests = 100;

[[nodiscard]] bool reject_early_request(const http3_request_head& head) noexcept {
    return head.early_data() && !is_http_method_safe(head.method());
}

struct interim_target final {
    http3_session& session_;
    std::int64_t stream_id_;
};

// Streaming request-body reader of one HTTP/3 request stream; the body_reader
// facade wraps it for the handler. Each read hands out every byte queued
// since the previous read and returns the previous chunk's receive
// flow-control credit (RFC 9000 §4.1): the peer can send more only once the
// handler has consumed what it sent.
class http3_request_body_reader final : public multiplexed_request_body_reader_base<http3_request_body_reader> {
public:
    static constexpr const char* terminated_error_message = "HTTP/3 connection ended during body read";
    static constexpr const char* reset_error_message = "HTTP/3 request stream reset during body read";

    // continue_output is non-null when the request carries a pending
    // 100-continue expectation: the first read sends it, as the HTTP/1 reader
    // does, so content the handler never reads is never solicited.
    http3_request_body_reader(http3_session& session, http3_stream& stream, multiplexed_stream_signal& signal, http_interim_response_output* continue_output) noexcept :
        multiplexed_request_body_reader_base(signal, continue_output),
        session_(session),
        stream_(stream) {}

private:
    friend class multiplexed_request_body_reader_base<http3_request_body_reader>;

    [[nodiscard]] const std::optional<http_request_body_failure>& body_failure() const noexcept {
        return stream_.body_failure();
    }

    [[nodiscard]] bool stream_aborted() const noexcept {
        return stream_.aborted();
    }

    [[nodiscard]] std::string_view pop_chunk() noexcept {
        const auto [chunk, returned] = stream_.pop();
        if (returned) {
            session_.wake_writer();
        }
        return chunk;
    }

    [[nodiscard]] bool body_ended() const noexcept {
        return stream_.request_ended();
    }

    http3_session& session_;
    http3_stream& stream_;
};

// Streaming response sink of one HTTP/3 request stream, the method set
// dispatch_response_stream_with drives: commit() submits the streaming final
// head (an application Content-Length becomes the exact content total),
// write() submits DATA within the stream's output budget, waiting for
// acknowledgements when it is spent, and end() finishes the content or sends
// the trailer section. The HTTP/3 core owns the field mapping and framing.
class http3_response_stream_sink final : public multiplexed_response_stream_sink_base<http3_response_stream_sink, http3_stream> {
public:
    http3_response_stream_sink(http3_session& session, http3_stream& stream, multiplexed_stream_signal& signal, http_known_method request_method, worker_codecs& codecs, http_response_coding_selection response_coding, const compression_config* compression, const http_cors_policy* cors) noexcept :
        multiplexed_response_stream_sink_base(stream, signal, request_method, codecs, response_coding, compression, cors),
        session_(session) {}

private:
    friend class multiplexed_response_stream_sink_base<http3_response_stream_sink, http3_stream>;

    static constexpr const char* terminated_error_message = "HTTP/3 connection ended during response stream";
    static constexpr const char* sleep_terminated_error_message = "HTTP/3 connection ended during response stream";
    static constexpr const char* reset_error_message = "HTTP/3 response stream reset";
    static constexpr const char* sleep_reset_error_message = "HTTP/3 response stream reset during sleep";

    [[nodiscard]] bool stream_aborted() const noexcept {
        return stream_.aborted();
    }

    void wake_writer() noexcept {
        session_.wake_writer();
    }

    [[nodiscard]] http_response_stream_commit_plan submit_streaming_head(http_response&& response, http_response_trailer_intent trailer_intent) {
        const auto head_result = session_.codec().submit_streaming_response_head(stream_.id(), std::move(response), trailer_intent);
        const auto* submitted_head = std::get_if<http_response_stream_commit_plan>(&head_result);
        if (submitted_head != nullptr) {
            return *submitted_head;
        }
        if (std::get<http3_response_head_error>(head_result) == http3_response_head_error::closed) {
            throw inbound_transport_error(std::make_error_code(std::errc::connection_reset), reset_error_message);
        }
        throw std::logic_error("HTTP/3 streaming response head submission failed");
    }

    task<void> write_encoded_data(std::string_view chunk) {
        const auto status = co_await write_http3_stream_data(session_, stream_.id(), signal_, chunk);
        if (status == http3_data_write_status::completed) {
            co_return;
        }
        if (status == http3_data_write_status::terminated) {
            throw inbound_transport_error(signal_.terminal_error(), terminated_error_message);
        }
        throw inbound_transport_error(std::make_error_code(std::errc::connection_reset), reset_error_message);
    }

    void finish_stream_response(const http_response_trailer_section& trailer_section) {
        http3_finish_status finished{};
        try {
            finished = session_.codec().finish_response(stream_.id(), trailer_section);
        } catch (...) {
            session_.wake_writer();
            throw;
        }
        session_.wake_writer();
        if (finished == http3_finish_status::invalid_message) {
            throw std::invalid_argument("HTTP/3 response ended before its declared Content-Length");
        }
        if (finished != http3_finish_status::accepted) {
            throw inbound_transport_error(std::make_error_code(std::errc::connection_reset), reset_error_message);
        }
    }

    http3_session& session_;
};

// One duplex HTTP/3 request stream. The session remains its sole QUIC reader
// and writer; this adapter returns flow credit only after consuming DATA.
class http3_tunnel_transport final : public multiplexed_tunnel_transport_base<http3_tunnel_transport> {
public:
    http3_tunnel_transport(http3_session& session, http3_stream& stream, multiplexed_stream_signal& signal) noexcept :
        multiplexed_tunnel_transport_base(signal),
        session_(session),
        stream_(stream) {}

    // Like read_more(): a cancellation emitted on `slot` wakes the wait and
    // `check` runs before every wait; unread input stays queued.
    template <typename check_type>
    [[nodiscard]] task<std::optional<http_tunnel_input>> read_datagram(check_type check, asio::cancellation_slot slot) {
        const wake_assignment assignment(signal_, slot);
        for (;;) {
            require_live();
            // Reliable input is not starved by a native datagram flood.
            auto chunk = stream_.take_tunnel_bytes();
            if (!chunk.bytes_.empty()) {
                chunk.credit_.reset();
                session_.wake_writer();
                co_return http_tunnel_input{std::move(chunk.bytes_), false};
            }
            if (auto payload = stream_.pop_datagram()) {
                co_return http_tunnel_input{std::move(*payload), true};
            }
            if (stream_.request_ended()) {
                co_return std::nullopt;
            }
            check();
            co_await signal_.wait();
        }
    }

    [[nodiscard]] task<bool> write_datagram(std::string_view payload) {
        // The QUIC owner checks the negotiated frame size and current path MTU;
        // invalid_state selects reliable capsule fallback without consuming it.
        multiplexed_stream_signal::output_wait_guard deadline(signal_);
        for (;;) {
            require_live();
            const auto result = session_.submit_datagram(stream_.id(), std::as_bytes(std::span(payload.data(), payload.size())));
            if (result == http3_quic_status::accepted) {
                co_return true;
            }
            if (result == http3_quic_status::invalid_state) {
                co_return false;
            }
            if (result != http3_quic_status::would_block) {
                throw inbound_transport_error(std::make_error_code(std::errc::connection_reset), "HTTP/3 datagram write");
            }
            co_await signal_.wait();
        }
    }

    [[nodiscard]] task<std::error_code> write_bytes(std::string_view bytes, http_stream_end disposition) {
        const auto result = co_await write_http3_stream_data(session_, stream_.id(), signal_, bytes);
        if (result != http3_data_write_status::completed || signal_.stopped() || stream_.aborted()) {
            co_return signal_.terminated() ? signal_.terminal_error() : std::make_error_code(std::errc::connection_reset);
        }
        if (disposition == http_stream_end::end && session_.codec().finish_stream(stream_.id()) != http3_finish_status::accepted) {
            co_return std::make_error_code(std::errc::protocol_error);
        }
        session_.wake_writer();
        co_return std::error_code{};
    }

    void abort(bool malformed = false) noexcept {
        session_.reset_stream(stream_.id(), malformed ? http3_error_code::message_error : http3_error_code::request_cancelled);
    }

private:
    friend class multiplexed_tunnel_transport_base<http3_tunnel_transport>;

    void require_live() const {
        if (signal_.stopped() || stream_.aborted()) {
            throw inbound_transport_error(signal_.terminated() ? signal_.terminal_error() : std::make_error_code(std::errc::connection_reset), "HTTP/3 tunnel read");
        }
    }

    template <typename buffer_type>
    [[nodiscard]] bool consume_tunnel_chunk(buffer_type& buffer) {
        auto chunk = stream_.take_tunnel_bytes();
        if (chunk.bytes_.empty()) {
            return false;
        }
        buffer.append(chunk.bytes_);
        std::string(chunk.bytes_.get_allocator()).swap(chunk.bytes_);
        chunk.credit_.reset();
        session_.wake_writer();
        return true;
    }

    [[nodiscard]] bool receive_ended() const noexcept {
        return stream_.request_ended();
    }

    http3_session& session_;
    http3_stream& stream_;
};
}  // namespace
ngtcp2_conn* http3_session::native_connection(ngtcp2_crypto_conn_ref* reference) noexcept {
    return static_cast<http3_session*>(reference->user_data)->connection_->native_handle();
}

http3_session::http3_session(http3_endpoint& endpoint, connection_slot slot, const ngtcp2_pkt_hd& initial, const asio::ip::udp::endpoint& remote, const ngtcp2_cid& original_dcid, std::optional<ngtcp2_cid> retry_scid, std::span<const std::uint8_t> retry_token, connection_lifecycle lifecycle) :
    endpoint_(endpoint),
    services_(endpoint.services()),
    slot_(std::move(slot)),
    connection_lifecycle_(std::move(lifecycle)),
    connection_id_(connection_lifecycle_.connection_id()),
    local_(to_asio_address(connection_lifecycle_.local_endpoint().address()), connection_lifecycle_.local_endpoint().port()),
    tls_local_address_(local_.address()),
    remote_(remote),
    path_remote_(remote),
    path_local_(local_),
    path_interface_index_(connection_lifecycle_.interface_index()),
    remote_address_(),
    local_address_(),
    connection_ids_(),
    datagram_(),
    client_certificate_(),
    inbound_buffers_(*services_.options_.inbound_budget_, services_.options_.server_.max_inbound_buffer_bytes_per_connection_),
    admission_policy_(http_request_admission_policy::server(services_.options_)),
    write_signal_(services_.worker_),
    handler_finished_(services_.worker_),
    streams_(),
    buffered_response_writer_(*this, response_file_io{.worker_ = services_.worker_, .stop_ = services_.stop_token_, .timeout_ = services_.options_.server_.write_timeout_}) {
    assign_peer_address(remote_address_, remote.address());
    auto& quic_context = endpoint.session_config().quic_tls_context_;
    if (!quic_context.has_value()) {
        throw std::logic_error("HTTP/3 listener has no QUIC TLS context");
    }
    tls_.reset(SSL_new(quic_context->native_handle()));
    if (!tls_ || ngtcp2_crypto_ossl_configure_server_session(tls_.get()) != 0) {
        throw std::runtime_error("failed to create the QUIC TLS session");
    }
    bind_tls_local_address(*tls_, tls_local_address_);
    if (const auto callback = services_.options_.tls_client_hello_; callback || tls_certificate_selection_enabled(endpoint.session_config())) {
        assign_peer_address(local_address_, local_.address());
        client_hello_.emplace(tls_client_hello_observation{.callback_ = callback, .fields_ = {.connection_id_ = connection_id_, .local_address_ = local_address_, .local_port_ = local_.port(), .remote_address_ = remote_address_, .remote_port_ = remote_.port(), .quic_ = true, .message_ = {}}});
        observe_tls_client_hello(*tls_, *client_hello_);
    }
    connection_reference_.get_conn = native_connection;
    connection_reference_.user_data = this;
    SSL_set_app_data(tls_.get(), &connection_reference_);
    SSL_set_accept_state(tls_.get());
    ngtcp2_crypto_ossl_ctx* crypto = nullptr;
    if (ngtcp2_crypto_ossl_ctx_new(&crypto, tls_.get()) != 0) {
        throw std::runtime_error("failed to create the QUIC crypto context");
    }
    crypto_.reset(crypto);

    ngtcp2_cid source{};
    http3_quic_stateless_reset_token reset_token{};
    if (!make_http3_connection_id(endpoint.secret(), services_.placement_.worker_index_, source) || !make_http3_stateless_reset_token(endpoint.secret(), source, reset_token)) {
        throw std::runtime_error("failed to issue a QUIC connection ID");
    }
    const auto& server = services_.options_.server_;
    auto crypto_callbacks = make_http3_quic_crypto_callbacks();
    crypto_callbacks.recv_client_initial_ = ngtcp2_crypto_recv_client_initial_cb;
    const auto handshake_timeout = http3_quic_duration(server.header_completion_timeout_.has_value() ? server.header_completion_timeout_ : server.request_header_timeout_);
    const http3_connection_options http3_options{.max_concurrent_streams_ = static_cast<std::size_t>(http3_max_concurrent_requests), .max_stream_output_bytes_ = http3_stream_output_bytes, .enable_connect_protocol_ = true, .enable_h3_datagram_ = true, .frame_observer_ = services_.options_.http3_frame_ ? +[](void* target, const http3_frame_metadata& metadata) noexcept {
        const auto& self = *static_cast<http3_session*>(target);
        self.services_.options_.http3_frame_(server_observation_access::http3_frame(self.connection_id_, metadata));
    } : nullptr, .field_observer_ = services_.options_.http3_field_ ? +[](void* target, const http3_field_metadata& metadata) noexcept {
        const auto& self = *static_cast<http3_session*>(target);
        self.services_.options_.http3_field_(server_observation_access::http3_field(self.connection_id_, metadata));
    } : nullptr, .observer_context_ = this};
    connection_.emplace(http3_quic_connection::server({
        .crypto_callbacks_ = crypto_callbacks,
        .owner_callbacks_ = {.get_new_connection_id_ = new_connection_id, .remove_connection_id_ = remove_connection_id},
        .source_connection_id_ = source,
        .destination_connection_id_ = initial.scid,
        .original_destination_connection_id_ = original_dcid,
        .retry_source_connection_id_ = retry_scid,
        .retry_token_ = retry_token,
        .stateless_reset_token_ = reset_token,
        .initial_path_ = path(remote_),
        .version_ = initial.version,
        .initial_timestamp_ = now(),
        .handshake_timeout_ = handshake_timeout == 0 ? UINT64_MAX : handshake_timeout,
        // Zero disables the QUIC idle timeout, as an absent idle_timeout_
        // disables the HTTP one.
        .idle_timeout_ = http3_quic_duration(server.idle_timeout_),
        .initial_max_data_ = (std::min)(http3_max_connection_receive_window, static_cast<std::uint64_t>(server.max_inbound_buffer_bytes_per_connection_)),
        .initial_max_stream_data_ = (std::min)(http3_max_stream_receive_window, static_cast<std::uint64_t>((std::max)(server.max_buffered_body_bytes_, std::size_t{1}))),
        .initial_max_streams_bidi_ = http3_max_concurrent_requests,
        .max_udp_payload_size_ = http3_max_datagram_bytes,
        .callback_context_ = this,
        .http3_ = http3_options,
    }));
    connection_->set_tls_native_handle(crypto_.get());
    configure_http3_server_tls_session(*tls_, *connection_, initial.version, http3_options, endpoint.session_config().http3_->early_data_, tls_resumption_context_);

    const connection_transport transport(this, [](void* target) noexcept {
        static_cast<http3_session*>(target)->close_now();
    }, [](void* target) noexcept {
        static_cast<http3_session*>(target)->request_drain();
    });
    connection_lifecycle_.bind_transport(transport);
    scanner_entry_.bind_lifecycle(&connection_lifecycle_);
    scanner_guard_.emplace(services_.scanner_, scanner_entry_, transport);
    scanner_entry_.register_periodic_check(scanner_checks_, this, [](void* target, std::int64_t now_ms) noexcept {
        static_cast<http3_session*>(target)->expire_stream_deadlines(now_ms);
    }, [](void* target) noexcept {
        static_cast<http3_session*>(target)->request_drain();
    });
    set_inactivity_phase();

    // Registered last: a constructor that throws leaves no table entry behind.
    connection_ids_.reserve(2);
    register_connection_id(source);
    try {
        // Retransmitted client Initials still carry the Initial packet's ID.
        if (ngtcp2_cid_eq(&source, &initial.dcid) == 0) {
            register_connection_id(initial.dcid);
        }
    } catch (...) {
        unregister_connection_ids();
        throw;
    }
}

http3_session::~http3_session() {
    unregister_connection_ids();
    connection_.reset();
    if (tls_) {
        SSL_set_app_data(tls_.get(), nullptr);
    }
}

ngtcp2_tstamp http3_session::now() noexcept {
    return http3_quic_now();
}

ngtcp2_path http3_session::path(asio::ip::udp::endpoint& remote) noexcept {
    auto result = http3_quic_path(path_local_, remote);
    result.user_data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(path_interface_index_));
    return result;
}

void http3_session::register_connection_id(const ngtcp2_cid& cid) {
    const auto key = http3_cid_key::from(cid);
    connection_ids_.push_back(key);
    if (!endpoint_.register_connection_id(key, *this)) {
        connection_ids_.pop_back();
        throw std::runtime_error("QUIC connection ID collision");
    }
}

void http3_session::unregister_connection_ids() noexcept {
    for (const auto& key : connection_ids_) {
        endpoint_.unregister_connection_id(key);
    }
    connection_ids_.clear();
}

int http3_session::new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t size, void* user_data) noexcept {
    auto& self = *static_cast<http3_session*>(user_data);
    // ngtcp2 asks for IDs of the length the connection started with, which
    // is always this server's worker-tagged length.
    if (size != http3_connection_id_length) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    http3_quic_stateless_reset_token reset_token{};
    try {
        // A fresh random ID colliding with a live one is retried, not reused.
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (!make_http3_connection_id(self.endpoint_.secret(), self.services_.placement_.worker_index_, *cid) || !make_http3_stateless_reset_token(self.endpoint_.secret(), *cid, reset_token)) {
                return NGTCP2_ERR_CALLBACK_FAILURE;
            }
            const auto key = http3_cid_key::from(*cid);
            self.connection_ids_.push_back(key);
            if (self.endpoint_.register_connection_id(key, self)) {
                std::memcpy(token->data, reset_token.data(), reset_token.size());
                return 0;
            }
            self.connection_ids_.pop_back();
        }
    } catch (...) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return NGTCP2_ERR_CALLBACK_FAILURE;
}

int http3_session::remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data) noexcept {
    auto& self = *static_cast<http3_session*>(user_data);
    const auto key = http3_cid_key::from(*cid);
    const auto found = std::ranges::find(self.connection_ids_, key);
    if (found != self.connection_ids_.end()) {
        self.endpoint_.unregister_connection_id(key);
        self.connection_ids_.erase(found);
    }
    return 0;
}

const worker_ref& http3_session::worker() const noexcept {
    return services_.worker_;
}

http3_stream* http3_session::find_stream(std::int64_t stream_id) noexcept {
    const auto found = streams_.find(stream_id);
    return found == streams_.end() ? nullptr : found->second.get();
}

void http3_session::wake_writer() noexcept {
    write_signal_.notify();
}

void http3_session::reset_stream(std::int64_t stream_id, http3_error_code error) noexcept {
    if (lifecycle_ == lifecycle_type::open) {
        connection_->reset_stream(stream_id, error);
    }
    if (auto* stream = find_stream(stream_id)) {
        stream->mark_aborted();
        if (!stream->dispatched() || stream->handler_done()) {
            stream->mark_request_body_closed();
            (void)stream->discard_body();
            remove_stream_if_done(stream_id);
        }
    }
    wake_writer();
}

http3_quic_status http3_session::submit_datagram(std::int64_t stream_id, std::span<const std::byte> payload) {
    const auto status = connection_->submit_datagram(stream_id, payload);
    if (status == http3_quic_status::accepted || status == http3_quic_status::would_block) {
        wake_writer();
    }
    return status;
}

void http3_session::on_datagram(const http3_datagram_event& event) {
    auto* stream = find_stream(event.stream_id());
    if (stream == nullptr || stream->aborted() || stream->handler_done() || !stream->head().connect()) {
        return;
    }
    stream->note_request_body_progress(scanner_entry_.last_active_ms());
    stream->store_datagram(event.payload(), services_.options_.server_.max_buffered_body_bytes_);
}

void http3_session::receive(std::span<const std::byte> datagram, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    connection_lifecycle_.note_read(datagram.size());
    if (lifecycle_ != lifecycle_type::open || connection_->closed()) {
        // The closing period absorbs late packets (RFC 9000 §10.2).
        return;
    }
    scanner_entry_.touch();
    path_remote_ = remote;
    path_local_ = local;
    path_interface_index_ = interface_index;
    try {
        const auto result = connection_->receive_datagram(path(path_remote_), NGTCP2_ECN_NOT_ECT, datagram, now());
        if (result.status_ == http3_quic_status::transport_error || result.status_ == http3_quic_status::application_error || (result.transport_error_code_.has_value() && *result.transport_error_code_ != 0) || (result.application_error_code_.has_value() && *result.application_error_code_ != http3_error_code::no_error)) {
            connection_lifecycle_.set_reason(connection_close_reason::protocol_error);
        } else if (result.status_ == http3_quic_status::closed) {
            connection_lifecycle_.set_reason(connection_->stateless_reset_received() ? connection_close_reason::transport_error : connection_close_reason::peer_eof);
        }
    } catch (...) {
        services_.options_.connection_failure_.invoke(remote_address_, std::current_exception());
        connection_->close(http3_error_code::internal_error);
    }
    process_events();
    // Acknowledgements may have freed stream output budget.
    wake_output_waiters();
    wake_writer();
}

void http3_session::process_events() noexcept {
    try {
        drain_events();
    } catch (...) {
        // Only allocation fails here; the connection cannot keep its streams
        // consistent without the event it lost.
        services_.options_.connection_failure_.invoke(remote_address_, std::current_exception());
        connection_->close(http3_error_code::internal_error);
        terminate(std::make_error_code(std::errc::not_enough_memory));
    }
}

void http3_session::drain_events() {
    start_application();
    while (auto event = connection_->next_event()) {
        if (auto* head = event->request_head()) {
            on_request_head(*head);
        } else if (auto* data = event->data()) {
            on_data(*data);
        } else if (const auto* datagram = event->datagram()) {
            on_datagram(*datagram);
        } else if (const auto* end = event->message_end()) {
            on_message_end(*end);
        } else if (const auto* action = event->transport_action()) {
            // Either origin abandons the request: a peer RESET_STREAM or
            // STOP_SENDING, or the connection's own stream error (a malformed
            // trailer section, RFC 9114 §4.1.2), already sent.
            on_stream_abort(action->stream_id());
        } else if (const auto* closed = event->stream_close()) {
            on_stream_close(*closed);
        }
        // A client GOAWAY (RFC 9114 §5.2) bounds only server push, which this
        // server never sends.
    }
    for (auto& [stream_id, stream] : streams_) {
        if (!stream->aborted() && !stream->handler_done()) {
            if (const auto priority = codec().stream_priority(stream_id)) {
                stream->set_priority(priority);
            }
        }
    }
    set_inactivity_phase();
}

void http3_session::start_application() {
    if (base_services_.has_value() || !connection_->handshake_completed() || connection_->closed()) {
        return;
    }
    // RFC 9114 §6.2: a client must allow the three server unidirectional
    // streams; one that does not cannot be served.
    if (connection_->start_http3() != http3_quic_status::accepted) {
        connection_->close(http3_error_code::internal_error);
        return;
    }
    std::optional<std::string_view> client_subject;
    if (endpoint_.session_config().tls()->client_certificates_.has_value()) {
        client_subject = extract_tls_client_certificate(tls_.get(), client_certificate_);
    }
    served_certificate_.emplace(SSL_get_certificate(tls_.get()));
    base_services_.emplace(services_.capabilities_.make_context_services(services_.stop_token_).with_tls_transport(connection_id_, remote_address_, client_subject, tls_selected_alpn(*tls_), tls_server_name(*tls_), remote_.port(), *served_certificate_).with_connection_lifecycle(connection_lifecycle_).with_inbound_buffers(inbound_buffers_).with_request_stream(endpoint_.session_config().request_stream_ ? services_.options_.request_stream_ : request_stream_callback_ref{}));
    base_services_->bind_routes(services_.routes_);
    set_inactivity_phase();
    if (drain_begun_) {
        if (codec().begin_drain() != http3_submit_status::accepted) {
            connection_->close(http3_error_code::internal_error);
            return;
        }
        arm_drain_timer();
        wake_writer();
    }
    // Authenticate the connection before releasing its bounded 0-RTT requests.
    for (auto it = streams_.begin(); it != streams_.end();) {
        const auto stream_id = it->first;
        auto& stream = *it->second;
        ++it;
        if (stream.aborted()) {
            continue;
        }
        if (stream.take_continue_pending() && !stream.request_ended() && !stream.body_failure().has_value()) {
            if (codec().submit_interim_response_head(stream_id, http_interim_response_head(ruvia::http_status::continue_value)) != http3_submit_status::accepted) {
                reset_stream(stream_id, http3_error_code::internal_error);
                continue;
            }
            wake_writer();
        }
        if (stream.dispatch_pending() && !admit_stream(stream_id)) {
            reset_stream(stream_id, http3_error_code::internal_error);
        }
    }
}

void http3_session::on_request_head(http3_request_head_event& event) {
    const auto stream_id = event.stream_id();
    if (find_stream(stream_id) != nullptr) {
        reset_stream(stream_id, http3_error_code::internal_error);
        return;
    }
    const auto& server = services_.options_.server_;
    ++accepted_request_heads_;
    connection_lifecycle_.note_request();
    if (!drain_begun_ && server.max_requests_per_connection_.has_value() && accepted_request_heads_ >= *server.max_requests_per_connection_) {
        begin_drain();
    }
    // The connection validated the head; a malformed request never reaches
    // here (RFC 9114 §4.1.2).
    auto head = std::move(event).take_head();
    const auto connect = head.connect();
    auto [resolution, body_mode] = resolve_multiplexed_stream_route(services_.routes_, endpoint_.session_config().request_stream_, head.method(), head.path(), head.authority(), head.protocol());
    const auto* resolved = resolution.resolved();
    if (connect) {
        body_mode = request_body_mode::stream;
    }
    const bool resolved_route = resolved != nullptr && !connect;
    const auto expectation_plan = head.expectation_plan(http_unsupported_expectation_policy::reject);
    const auto declared_length = head.declared_content_length();
    const bool ended = head.stream_ended();
    auto stream_value = std::make_unique<http3_stream>(std::move(head), std::move(resolution), body_mode, &inbound_buffers_);
    auto& stream = *stream_value;
    stream.set_priority(codec().stream_priority(stream_id));
    streams_.emplace(stream_id, std::move(stream_value));
    // An accepted stream ends the stream-less idle wait (awaiting_stream).
    set_inactivity_phase();
    if (ended) {
        stream.mark_request_ended();
    } else if (!connect) {
        stream.mark_request_body_open(scanner_entry_.last_active_ms());
    }
    bool body_limit_exceeded = false;
    if (resolved_route && declared_length.has_value()) {
        const auto failure = http_request_body_size_failure(*declared_length, http_request_body_limit(stream.resolution(), body_mode, admission_policy_));
        if (failure.has_value()) {
            stream.mark_body_failure(*failure);
            body_limit_exceeded = true;
        }
    }
    // Same policy as HTTP/1 and HTTP/2: 100 Continue is sent when the server
    // starts consuming the content -- now for a buffered route, at the first
    // read for a streaming one -- and never for a request answered from its
    // head alone (RFC 9110 §10.1.1).
    if (expectation_plan == http_server_expectation_plan::send_continue && resolved_route && !ended && !body_limit_exceeded && !reject_early_request(stream.head()) && body_mode == request_body_mode::buffered) {
        if (!base_services_.has_value()) {
            stream.defer_continue();
        } else {
            if (codec().submit_interim_response_head(stream_id, http_interim_response_head(ruvia::http_status::continue_value)) != http3_submit_status::accepted) {
                reset_stream(stream_id, http3_error_code::internal_error);
                return;
            }
            wake_writer();
        }
    }
    // A buffered body is dispatched once complete; everything else -- a
    // streamed body, CONNECT, an unresolved or oversized
    // request, a rejected expectation -- is answered from its head.
    if (ended || connect || body_mode == request_body_mode::stream || !resolved_route || body_limit_exceeded || expectation_plan == http_server_expectation_plan::reject_unsupported || reject_early_request(stream.head())) {
        if (!admit_stream(stream_id)) {
            reset_stream(stream_id, http3_error_code::internal_error);
        }
    }
}

void http3_session::on_data(http3_data_event& event) {
    const auto stream_id = event.stream_id();
    // Credit the stream does not keep returns when this scope ends.
    auto credit = event.take_credit();
    if (credit.valid()) {
        wake_writer();
    }
    auto* stream = find_stream(stream_id);
    // Content nobody will read -- of a stream already answered or abandoned,
    // a failed body, or a buffered body dispatched early -- returns
    // its credit at once. The request borrows a dispatched buffered body, so
    // appending to it would reallocate under that view.
    if (stream == nullptr || stream->aborted() || stream->handler_done() || reject_early_request(stream->head()) || stream->body_failure().has_value() || (stream->body_mode() == request_body_mode::buffered && stream->dispatched())) {
        return;
    }
    stream->note_request_body_progress(scanner_entry_.last_active_ms());
    const auto total_limit = stream->head().connect() ? protocol_byte_limit::unlimited() : http_request_body_limit(stream->resolution(), stream->body_mode(), admission_policy_);
    const auto status = stream->store(event.bytes(), std::move(credit), total_limit, services_.options_.server_.max_buffered_body_bytes_);
    if (status == http3_body_store_status::backlog_overflow) {
        reset_stream(stream_id, http3_error_code::request_cancelled);
        return;
    }
    if (status == http3_body_store_status::too_large) {
        stream->mark_body_failure(http_request_body_failure::too_large);
        if (stream->response_committed()) {
            reset_stream(stream_id, http3_error_code::request_cancelled);
            return;
        }
        if (auto* signal = stream->signal()) {
            signal->wake();
        } else if (!admit_stream(stream_id)) {
            reset_stream(stream_id, http3_error_code::internal_error);
        }
        return;
    }
    if (stream->body_mode() == request_body_mode::stream) {
        if (auto* signal = stream->signal()) {
            signal->wake();
        }
    }
}

void http3_session::on_message_end(const http3_message_end_event& event) {
    const auto stream_id = event.stream_id();
    auto* stream = find_stream(stream_id);
    if (stream != nullptr && !stream->aborted() && !stream->handler_done() && !event.trailers().empty() && stream->trailers().append_received(event.trailers()).has_value()) {
        // RFC 9114 §4.1.2: a malformed trailer section makes the request
        // malformed.
        reset_stream(stream_id, http3_error_code::message_error);
    }
    on_stream_end(stream_id);
}

void http3_session::on_stream_end(std::int64_t stream_id) {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr || stream->request_ended()) {
        return;
    }
    stream->mark_request_ended();
    if (auto* signal = stream->signal()) {
        signal->wake();
    } else if (!stream->aborted() && !admit_stream(stream_id)) {
        reset_stream(stream_id, http3_error_code::internal_error);
    }
}

void http3_session::on_stream_abort(std::int64_t stream_id) noexcept {
    if (auto* stream = find_stream(stream_id)) {
        stream->mark_aborted();
        if (!stream->dispatched() || stream->handler_done()) {
            reset_stream(stream_id, http3_error_code::request_cancelled);
        }
    }
}

void http3_session::on_stream_close(const http3_stream_close_event& event) noexcept {
    auto* stream = find_stream(event.stream_id());
    if (stream == nullptr) {
        finish_drain_if_ready();
        return;
    }
    stream->mark_closed();
    if (event.error().has_value()) {
        stream->mark_aborted();
    }
    if (auto* signal = stream->signal()) {
        signal->wake();
    }
    remove_stream_if_done(event.stream_id());
}

void http3_session::wake_output_waiters() noexcept {
    for (auto& [stream_id, stream] : streams_) {
        if (auto* signal = stream->signal(); signal != nullptr && signal->waiting_output()) {
            signal->wake();
        }
    }
}

void http3_session::remove_stream_if_done(std::int64_t stream_id) noexcept {
    const auto found = streams_.find(stream_id);
    if (found == streams_.end()) {
        return;
    }
    auto& stream = *found->second;
    // A dispatched handler still borrows the stream until it finishes.
    if (stream.dispatched() ? !stream.handler_done() : (!stream.closed() && !stream.aborted())) {
        return;
    }
    if (stream.discard_body()) {
        wake_writer();
    }
    if (stream.tunnel()) {
        --tunnel_streams_;
    }
    streams_.erase(found);
    set_inactivity_phase();
    finish_drain_if_ready();
}

void http3_session::finish_stream_handler(std::int64_t stream_id) noexcept {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr) {
        return;
    }
    stream->mark_handler_done();
    if (stream->aborted() && lifecycle_ == lifecycle_type::open) {
        connection_->reset_stream(stream_id, http3_error_code::request_cancelled);
    } else if (!stream->request_ended() && lifecycle_ == lifecycle_type::open) {
        // RFC 9114 §4.1.2: once the response is complete the server may stop
        // reading the request with H3_NO_ERROR.
        connection_->stop_receiving(stream_id, http3_error_code::no_error);
    }
    // Content that arrived but was never read holds connection credit.
    stream->mark_request_body_closed();
    (void)stream->discard_body();
    remove_stream_if_done(stream_id);
    wake_writer();
}

void http3_session::set_inactivity_phase() noexcept {
    if (lifecycle_ != lifecycle_type::open) {
        return;
    }
    scanner_entry_.set_phase(multiplexed_inactivity_phase(base_services_.has_value() && codec().peer_settings().has_value(), base_services_.has_value() && codec().header_block_in_progress(), !streams_.empty() || active_handler_tasks_ != 0, tunnel_streams_ != 0));
}

void http3_session::expire_stream_deadlines(std::int64_t now_ms) noexcept {
    const auto& server = services_.options_.server_;
    for (auto it = streams_.begin(); it != streams_.end();) {
        const auto stream_id = it->first;
        auto& stream = *it->second;
        ++it;
        if (stream.aborted()) {
            continue;
        }
        if (stream.request_body_overdue(now_ms, server.request_body_timeout_, server.body_completion_timeout_)) {
            stream.mark_request_body_closed();
            reset_stream(stream_id, http3_error_code::request_cancelled);
            continue;
        }
        auto* signal = stream.signal();
        if (signal != nullptr && server.write_timeout_.has_value() && signal->output_wait_expired(now_ms, *server.write_timeout_)) {
            reset_stream(stream_id, http3_error_code::request_cancelled);
        }
    }
}

bool http3_session::admit_stream(std::int64_t stream_id) {
    auto* stream = find_stream(stream_id);
    if (stream != nullptr && !base_services_) {
        stream->defer_dispatch();
        return true;
    }
    if (stream == nullptr) {
        return false;
    }
    bool counted = false;
    try {
        if (stream->begin_dispatch(services_.worker_, termination_) == nullptr) {
            return false;
        }
        ++active_handler_tasks_;
        counted = true;
        ruvia::async_start(dispatch_one(stream_id), asio::bind_executor(services_.io_.get_executor(), [this, stream_id](std::exception_ptr exception) noexcept {
            if (exception != nullptr) {
                services_.options_.connection_failure_.invoke(remote_address_, std::move(exception));
            }
            finish_stream_handler(stream_id);
            --active_handler_tasks_;
            set_inactivity_phase();
            finish_drain_if_ready();
            if (active_handler_tasks_ == 0) {
                handler_finished_.notify();
            }
            wake_writer();
        }));
    } catch (...) {
        if (counted) {
            --active_handler_tasks_;
        }
        stream->mark_handler_done();
        services_.options_.connection_failure_.invoke(remote_address_, std::current_exception());
        return false;
    }
    return true;
}

task<void> http3_session::dispatch_one(std::int64_t stream_id) {
    const auto& routes = services_.routes_;
    const auto& options = services_.options_;
    try {
        const auto request_start = options.access_log_.callback_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto* stream = find_stream(stream_id);
        auto* signal = stream != nullptr ? stream->signal() : nullptr;
        if (signal == nullptr) {
            throw std::logic_error("HTTP/3 request dispatched without its stream");
        }
        const auto& head = stream->head();
        http_request request = head.build_request(stream->body_mode() == request_body_mode::buffered ? stream->buffered_body() : std::string_view{});
        http_response response;
        interim_target target{*this, stream_id};
        http_interim_response_output interim_output(&target, [](void* raw, const http_interim_response_head& interim) -> task<void> {
            auto& output = *static_cast<interim_target*>(raw);
            // RFC 9114 §4.1: any number of 1xx heads may precede the final one.
            if (output.session_.codec().submit_interim_response_head(output.stream_id_, interim) != http3_submit_status::accepted) {
                throw std::invalid_argument("HTTP/3 interim response rejected");
            }
            output.session_.wake_writer();
            co_return;
        });
        http_connection_advertisement_output advertisements(this, [](void* raw, std::span<const std::string_view> origins) {
            auto& session = *static_cast<http3_session*>(raw);
            if (session.codec().submit_origin_advertisement(origins) != http3_submit_status::accepted) {
                throw std::invalid_argument("HTTP/3 ORIGIN advertisement rejected");
            }
            session.wake_writer();
        }, nullptr);
        const auto expectation_plan = head.expectation_plan(http_unsupported_expectation_policy::reject);
        auto admission_services = *base_services_;
        admission_services.bind_body_state({
            .framing_ = head.connect() || head.stream_ended() ? request_body_framing::none : request_body_framing::multiplexed,
            .content_length_ = head.declared_content_length(),
            .initial_end_stream_ = head.stream_ended(),
        });
        auto reset_request = [this, stream_id](request_reset_code error) {
            const auto* code = std::get_if<http3_error_code>(&error);
            reset_stream(stream_id, code != nullptr ? *code : http3_error_code::request_cancelled);
        };
        request_stream_control stream_control{callback_access::bind<void(request_reset_code)>(reset_request)};
        admission_services.bind_stream_control(stream_control);
        admission_services.bind_early_data(head.early_data());
        admission_services.bind_request_channels({
            .interim_output_ = &interim_output,
            .connection_advertisements_ = &advertisements,
            .priority_update_ = &stream->priority(),
        });
        admission_services.bind_request_trailers(stream->trailers());
        auto admission = admit_http_request(
            {
                .request_ = request,
                .resolution_ = stream->resolution(),
                .coding_ = http_response_coding_selection_for(request),
                .expectation_ = &expectation_plan,
                .start_ = request_start,
            },
            admission_policy_, admission_services);
        const auto& request_services = admission.services();
        const auto* resolved = admission.resolved();

        std::optional<body_reader_binding<http3_request_body_reader>> body_reader_storage;
        if (stream->body_mode() == request_body_mode::stream && !head.connect() && !stream->body_failure().has_value()) {
            const bool continue_on_read = expectation_plan == http_server_expectation_plan::send_continue && resolved != nullptr;
            body_reader_storage.emplace(*this, *stream, *signal, continue_on_read ? &interim_output : nullptr);
        }
        auto dispatch_services = request_services;
        if (body_reader_storage) {
            dispatch_services.bind_streaming_request_body(body_reader_storage->facade());
        }
        const stop_group request_stop(dispatch_services.get_stop_token(), signal->terminal_stop_token());
        const auto request_stop_token = request_stop.token();
        dispatch_services.bind_stop_token(request_stop_token);

        const auto reset_dispatch_stream = [this, stream_id](bool cancelled) noexcept {
            reset_stream(stream_id, cancelled ? http3_error_code::request_cancelled : http3_error_code::internal_error);
        };
        bool body_failure_handled = false;
        if (reject_early_request(head)) {
            body_failure_handled = true;
            admission.tail().disable_response_coding();
            // Replay rejection bypasses application and error handlers alike.
            response.status(ruvia::http_status::too_early);
            response.static_body("Replay-unsafe request requires a completed handshake");
        } else if (const auto& body_failure = stream->body_failure(); body_failure.has_value() && admission.endpoint() != http_request_endpoint::rejection) {
            body_failure_handled = true;
            response = co_await respond_http_request_body_failure(routes, request, *body_failure, request_services, admission.tail());
        } else {
            switch (admission.endpoint()) {
                case http_request_endpoint::rejection:
                    admission.tail().disable_response_coding();
                    response = co_await respond_http_request_rejection(routes, request, *admission.rejection(), request_services);
                    break;
                case http_request_endpoint::tunnel: {
                    auto commit = [this, stream, signal, &admission, &options](context& context_value, const http_response& head_response) {
                        if (codec().submit_connect_response_head(stream->id(), head_response) != http3_submit_status::accepted) {
                            throw std::invalid_argument("HTTP/3 CONNECT response head rejected");
                        }
                        if (!stream->tunnel()) {
                            stream->mark_tunnel();
                            ++tunnel_streams_;
                        }
                        stream->mark_response_committed();
                        set_inactivity_phase();
                        context_access::commit_upgrade(context_value);
                        wake_writer();
                        admission.tail().record_access(options.access_log_, head_response.status());
                        return http3_tunnel_transport(*this, *stream, *signal);
                    };
                    auto commit_websocket = [this, stream, signal, &request, &head, &admission, &options](context& context_value, const websocket_route_endpoint& endpoint, std::span<const http_header_view> headers) -> task<websocket_upgrade_attempt<http3_tunnel_transport>> {
                        const auto validation = validate_http3_websocket_handshake(head, request, endpoint.handshake_validation());
                        if (const auto* failure = validation.failure()) {
                            co_return reject_request_stream_websocket(context_value, *failure);
                        }
                        const auto result = codec().submit_websocket_handshake(stream->id(), request, validation, {.supported_subprotocols_ = endpoint.subprotocols(), .response_headers_ = headers, .deflate_ = endpoint.deflate()});
                        const auto* handshake = std::get_if<websocket_extended_connect_negotiation>(&result);
                        if (handshake == nullptr) {
                            throw std::invalid_argument("HTTP/3 WebSocket handshake rejected");
                        }
                        if (!stream->tunnel()) {
                            stream->mark_tunnel();
                            ++tunnel_streams_;
                        }
                        stream->mark_response_committed();
                        set_inactivity_phase();
                        context_access::commit_upgrade(context_value);
                        wake_writer();
                        admission.tail().record_access(options.access_log_, http_status::ok);
                        co_return websocket_upgrade_commit<http3_tunnel_transport>(http3_tunnel_transport(*this, *stream, *signal), handshake->compression(), handshake->subprotocol());
                    };
                    http3_response_stream_sink refusal_sink(*this, *stream, *signal, request.known_method(), *options.codecs_, *admission.tail().coding_policy().selection(), options.compression_ ? &*options.compression_ : nullptr, admission.tail().cors(options));
                    auto refusal_writer = make_response_stream_writer(refusal_sink);
                    auto upgrade_services = dispatch_services;
                    if (is_request_stream_tunnel_route(resolved->route())) {
                        upgrade_services.bind_response_stream(refusal_writer);
                    }
                    auto buffered = co_await execute_tunnel_upgrade_session<http3_tunnel_transport>(routes, request, *resolved, upgrade_services, options, scanner_entry_, inbound_buffers_, admission.udp_tunnel(), std::string_view{}, commit, commit_websocket);
                    if (refusal_sink.committed()) {
                        admission.tail().record_access(options.access_log_, committed_response_stream_status(refusal_sink));
                        co_return;
                    }
                    if (!buffered) {
                        co_return;
                    }
                    response = std::move(*buffered);
                    break;
                }
                case http_request_endpoint::websocket: {
                    const auto& endpoint = *resolved->route().endpoint().websocket();
                    const auto validation = validate_http3_websocket_handshake(head, request, endpoint.handshake_validation());
                    if (const auto* failure = validation.failure()) {
                        admission.tail().disable_response_coding();
                        response = co_await routes.handle_error(request, http_error_info({.status_ = failure->protocol_error().status(), .code_ = failure->protocol_error().code(), .message_ = std::string_view(failure->protocol_error().what())}), request_services);
                        failure->apply_required_response_headers(response);
                        break;
                    }
                    auto commit = [this, stream, signal, &request, &validation, &endpoint, &admission, &options](context& context_value, std::span<const http_header_view> headers) {
                        const auto result = codec().submit_websocket_handshake(stream->id(), request, validation, {.supported_subprotocols_ = endpoint.subprotocols(), .response_headers_ = headers, .deflate_ = endpoint.deflate()});
                        const auto* handshake = std::get_if<websocket_extended_connect_negotiation>(&result);
                        if (handshake == nullptr) {
                            throw std::invalid_argument("HTTP/3 WebSocket handshake rejected");
                        }
                        if (!stream->tunnel()) {
                            stream->mark_tunnel();
                            ++tunnel_streams_;
                        }
                        stream->mark_response_committed();
                        set_inactivity_phase();
                        context_access::commit_upgrade(context_value);
                        wake_writer();
                        admission.tail().record_access(options.access_log_, http_status::ok);
                        return websocket_upgrade_commit<http3_tunnel_transport>(http3_tunnel_transport(*this, *stream, *signal), handshake->compression(), handshake->subprotocol());
                    };
                    auto buffered = co_await execute_websocket_upgrade_session<http3_tunnel_transport>(routes, request, *resolved, dispatch_services, options, scanner_entry_, inbound_buffers_, std::string_view{}, commit);
                    if (!buffered) {
                        co_return;
                    }
                    response = std::move(*buffered);
                    break;
                }
                case http_request_endpoint::response_stream: {
                    // A stream reset or the connection ending stops the
                    // request: context::stop_token() combines it with the
                    // worker's stop.
                    http3_response_stream_sink sink_value(*this, *stream, *signal, request.known_method(), *options.codecs_, *admission.tail().coding_policy().selection(), options.compression_ ? &*options.compression_ : nullptr, admission.tail().cors(options));
                    auto result_value = co_await dispatch_response_stream_with(sink_value, routes, request, *resolved, dispatch_services, [stream]() noexcept {
                        return stream->aborted();
                    });
                    if (complete_multiplexed_response_stream_dispatch(result_value, stream->body_failure().has_value(), options, remote_address_, admission.tail(), response, body_failure_handled, reset_dispatch_stream)) {
                        co_return;
                    }
                    break;
                }
                case http_request_endpoint::buffered:
                    if (resolved == nullptr) {
                        co_await routes.dispatch_buffered_response(request, stream->resolution(), options.document_root_.binding(), dispatch_services, admission.static_file_mode(), response);
                        break;
                    }
                    [[fallthrough]];
                case http_request_endpoint::stream_body:
                    body_failure_handled = co_await routes.dispatch_resolved_route(request, *resolved, dispatch_services, &stream->body_failure(), response);
                    break;
            }
        }

        co_await finish_multiplexed_buffered_response(stream_id, stream->body_failure(), body_failure_handled, response, request, routes, request_services, options, remote_address_, admission.tail(), buffered_response_writer_, reset_dispatch_stream);
    } catch (...) {
        const auto failure = std::current_exception();
        auto* stream = find_stream(stream_id);
        if (stream != nullptr && !stream->aborted()) {
            reset_stream(stream_id, http3_error_code::internal_error);
        }
        options.connection_failure_.invoke(remote_address_, failure);
    }
}

task<void> http3_session::run() {
    std::exception_ptr failure;
    try {
        while (lifecycle_ != lifecycle_type::ended) {
            co_await flush();
            if (lifecycle_ == lifecycle_type::ended) {
                break;
            }
            co_await write_signal_.wait();
        }
    } catch (...) {
        failure = std::current_exception();
        connection_lifecycle_.note_exception(failure);
    }
    lifecycle_ = lifecycle_type::ended;
    terminate(std::make_error_code(std::errc::connection_aborted));
    drain_timer_.cancel();
    timer_.cancel();
    // Handlers borrow this session until they finish.
    while (active_handler_tasks_ != 0) {
        co_await handler_finished_.wait();
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

task<void> http3_session::flush() {
    std::size_t burst = 0;
    while (lifecycle_ == lifecycle_type::open) {
        const auto result = connection_->next_datagram(http3_max_datagram_bytes, now(), datagram_);
        // Writing can close streams (an acknowledged final frame).
        process_events();
        if (result.status_ == http3_quic_status::accepted && !datagram_.bytes_.empty()) {
            co_await send_datagram();
            wake_output_waiters();
            if (++burst == http3_flush_burst) {
                burst = 0;
                co_await worker_turn{services_.worker_};
            }
            continue;
        }
        if (result.status_ == http3_quic_status::transport_error || result.status_ == http3_quic_status::application_error) {
            connection_lifecycle_.set_reason(connection_close_reason::protocol_error);
            // The next call writes the CONNECTION_CLOSE.
            continue;
        }
        if (result.status_ == http3_quic_status::closed || connection_->closed()) {
            enter_closing();
        }
        break;
    }
    if (lifecycle_ == lifecycle_type::open) {
        finish_drain_if_ready();
        arm_timer();
    }
}

task<void> http3_session::send_datagram() {
    // ngtcp2 names the path of every packet it writes.
    const auto destination = http3_udp_endpoint(datagram_.path_.path.remote);
    const auto source = http3_udp_endpoint(datagram_.path_.path.local);
    const auto interface_index = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(datagram_.path_.path.user_data));
    if (!destination || !source) {
        throw std::logic_error("QUIC output has no valid remote path");
    }
#if defined(_WIN32)
    // The acceptor sends it from the listener's socket; a datagram past its
    // bounded queue is lost, as UDP may lose any, and QUIC recovery resends.
    if (!endpoint_.send(*destination, datagram_.bytes_, *source, interface_index)) {
        connection_lifecycle_.note_write(datagram_.bytes_.size());
    }
    co_return;
#else
    auto error = endpoint_.send(*destination, datagram_.bytes_, *source, interface_index);
    while (error == asio::error::would_block || error == asio::error::try_again) {
        // Only a full socket send buffer stalls the writer; the scanner's
        // write clock bounds it like a blocked TCP write.
        const connection_scanner::entry::write_scope write_stall_clock(scanner_entry_);
        if (co_await endpoint_.wait_writable()) {
            // The socket closed: the datagram is lost, as UDP may lose any.
            co_return;
        }
        error = endpoint_.send(*destination, datagram_.bytes_, *source, interface_index);
        if (!error) {
            scanner_entry_.note_write_progress();
        }
    }
    if (!error) {
        connection_lifecycle_.note_write(datagram_.bytes_.size());
    }
    // Other send errors (an ICMP-reported unreachable peer) lose the datagram
    // too; QUIC recovery and the idle timeout handle the peer.
#endif
}

void http3_session::arm_timer() noexcept {
    const auto expiry = connection_->expiry();
    if (timer_.registered() && expiry == armed_expiry_) {
        return;
    }
    timer_.cancel();
    armed_expiry_ = expiry;
    if (expiry == UINT64_MAX) {
        return;
    }
    try {
        const auto status = services_.worker_.schedule_timer(timer_, http3_quic_steady_time(expiry), [this](worker_timer_outcome outcome) {
            timer_fired(outcome);
        });
        if (status == worker_timer_schedule_status::worker_stopping) {
            close_now();
        }
    } catch (...) {
        services_.options_.connection_failure_.invoke(remote_address_, std::current_exception());
        close_now();
    }
}

void http3_session::timer_fired(worker_timer_outcome outcome) noexcept {
    armed_expiry_ = UINT64_MAX;
    if (outcome != worker_timer_outcome::expired) {
        // Only a stopping worker cancels with notification.
        connection_lifecycle_.set_reason(connection_close_reason::cancelled);
        close_now();
        return;
    }
    if (lifecycle_ == lifecycle_type::closing) {
        lifecycle_ = lifecycle_type::ended;
        wake_writer();
        return;
    }
    if (lifecycle_ != lifecycle_type::open) {
        return;
    }
    try {
        const auto result = connection_->handle_expiry(now());
        if (result.status_ == http3_quic_status::closed) {
            connection_lifecycle_.set_reason(connection_close_reason::timeout);
        } else if (result.status_ == http3_quic_status::transport_error || result.status_ == http3_quic_status::application_error) {
            connection_lifecycle_.set_reason(connection_close_reason::protocol_error);
        }
    } catch (...) {
        services_.options_.connection_failure_.invoke(remote_address_, std::current_exception());
        connection_->close(http3_error_code::internal_error);
    }
    process_events();
    wake_output_waiters();
    wake_writer();
}

void http3_session::enter_closing() noexcept {
    if (lifecycle_ != lifecycle_type::open) {
        return;
    }
    lifecycle_ = lifecycle_type::closing;
    terminate(std::make_error_code(std::errc::connection_aborted));
    drain_timer_.cancel();
    timer_.cancel();
    armed_expiry_ = UINT64_MAX;
    try {
        const auto status = services_.worker_.schedule_timer(timer_, deadline_after(http3_quic_nanoseconds(connection_->close_period())), [this](worker_timer_outcome outcome) {
            timer_fired(outcome);
        });
        if (status == worker_timer_schedule_status::worker_stopping) {
            lifecycle_ = lifecycle_type::ended;
        }
    } catch (...) {
        lifecycle_ = lifecycle_type::ended;
    }
    wake_writer();
}

void http3_session::close_now() noexcept {
    if (lifecycle_ == lifecycle_type::ended) {
        return;
    }
    if (lifecycle_ == lifecycle_type::open && !connection_->closed()) {
        connection_->close(http3_error_code::no_error);
        // Written at once: a stopping worker closes the socket before the
        // writer would run again.
        try {
            http3_quic_datagram close_packet;
            const auto result = connection_->next_datagram(http3_max_datagram_bytes, now(), close_packet);
            if (result.status_ == http3_quic_status::accepted && !close_packet.bytes_.empty()) {
                const auto destination = http3_udp_endpoint(close_packet.path_.path.remote);
                const auto source = http3_udp_endpoint(close_packet.path_.path.local);
                if (destination && source) {
                    const auto interface_index = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(close_packet.path_.path.user_data));
                    if (!endpoint_.send(*destination, close_packet.bytes_, *source, interface_index)) {
                        connection_lifecycle_.note_write(close_packet.bytes_.size());
                    }
                }
            }
        } catch (...) {
            // The peer then learns of the close from its idle timeout.
        }
    }
    lifecycle_ = lifecycle_type::ended;
    terminate(std::make_error_code(std::errc::connection_aborted));
    drain_timer_.cancel();
    timer_.cancel();
    wake_writer();
}

void http3_session::terminate(std::error_code error) noexcept {
    if (!termination_.terminate(error)) {
        return;
    }
    for (auto& [stream_id, stream] : streams_) {
        if (auto* signal = stream->signal()) {
            signal->stop();
        }
    }
    wake_writer();
}

void http3_session::request_drain() noexcept {
    // The scanner asks for a drain only when a connection timeout fired; it
    // recorded the close request before invoking this callback.
    drain_timer_.request_timeout();
    begin_drain();
    finish_drain_if_ready();
}

void http3_session::begin_drain() noexcept {
    if (lifecycle_ != lifecycle_type::open) {
        return;
    }
    if (!connection_->http3_started()) {
        if (streams_.empty() && accepted_request_heads_ == 0) {
            // No request can be in flight before the handshake completed.
            connection_->close(http3_error_code::no_error);
            wake_writer();
            return;
        }
        drain_begun_ = true;
        return;
    }
    if (!drain_begun_) {
        drain_begun_ = true;
        // RFC 9114 §5.2: the shutdown notice still admits requests the client
        // already sent; the final GOAWAY follows a round trip later.
        if (codec().begin_drain() != http3_submit_status::accepted) {
            connection_->close(http3_error_code::internal_error);
        }
        wake_writer();
    }
    arm_drain_timer();
}

void http3_session::arm_drain_timer() noexcept {
    // A final boundary needs no time bound, unless a timeout's drain must
    // still end the connection.
    if (!drain_timer_.needed(termination_.terminated(), codec().drain_final())) {
        return;
    }
    const auto round_trip = std::chrono::ceil<std::chrono::milliseconds>(http3_quic_nanoseconds(connection_->close_period()));
    const auto bound = (std::min)(http3_drain_round_trip_bound, (std::max)(round_trip, std::chrono::milliseconds(1)));
    drain_timer_.arm(services_.worker_, bound, [this]() noexcept {
        drain_timer_expired();
    }, [this]() noexcept {
        close_now();
    });
}

void http3_session::drain_timer_expired() noexcept {
    if (lifecycle_ != lifecycle_type::open || termination_.terminated()) {
        return;
    }
    if (codec().finish_drain() != http3_submit_status::accepted) {
        connection_->close(http3_error_code::internal_error);
    }
    drain_timer_.mark_expired();
    wake_writer();
    finish_drain_if_ready();
    arm_drain_timer();
}

void http3_session::finish_drain_if_ready() noexcept {
    if (lifecycle_ != lifecycle_type::open || !drain_begun_ || connection_->closed() || !codec().drain_final()) {
        return;
    }
    // Every admitted stream closed -- its response acknowledged -- ends the
    // connection with H3_NO_ERROR (RFC 9114 §5.2). A connection timeout's
    // drain at its bound cancels what is left.
    if ((streams_.empty() && active_handler_tasks_ == 0 && codec().drained()) || drain_timer_.expired()) {
        if (drain_timer_.expired()) {
            terminate(std::make_error_code(std::errc::timed_out));
        }
        connection_->close(http3_error_code::no_error);
        wake_writer();
    }
}

namespace {

[[nodiscard]] std::span<const std::byte> byte_view(std::string_view bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

[[nodiscard]] bool stream_aborted(http3_session& session, std::int64_t stream_id) noexcept {
    const auto* stream = session.find_stream(stream_id);
    return stream == nullptr || stream->aborted();
}

// Waits until acknowledgements free output budget; false when the stream or
// connection ended first.
[[nodiscard]] task<bool> await_output_budget(http3_session& session, std::int64_t stream_id, multiplexed_stream_signal& signal) {
    std::optional<multiplexed_stream_signal::output_wait_guard> waiting;
    while (session.codec().available_output_bytes(stream_id) == 0) {
        if (!waiting) {
            waiting.emplace(signal);
        }
        if (signal.stopped() || stream_aborted(session, stream_id)) {
            co_return false;
        }
        co_await signal.wait();
    }
    co_return !signal.stopped() && !stream_aborted(session, stream_id);
}

}  // namespace

task<http3_data_write_status> write_http3_stream_data(http3_session& session, std::int64_t stream_id, multiplexed_stream_signal& signal, std::string_view bytes) {
    while (!bytes.empty()) {
        if (signal.terminated()) {
            co_return http3_data_write_status::terminated;
        }
        if (!(co_await await_output_budget(session, stream_id, signal))) {
            co_return signal.terminated() ? http3_data_write_status::terminated : http3_data_write_status::stream_aborted;
        }
        const auto submitted = session.codec().submit_data(stream_id, byte_view(bytes));
        if (submitted.status_ == http3_data_submit_status::invalid_message) {
            throw std::length_error("HTTP/3 response exceeds Content-Length");
        }
        if (submitted.status_ == http3_data_submit_status::closed) {
            // The stream no longer accepts content: it was reset, or the
            // connection ended.
            co_return signal.terminated() || session.codec().connection_error().has_value() ? http3_data_write_status::terminated : http3_data_write_status::stream_aborted;
        }
        // backpressured keeps the unconsumed suffix for the next budget wait.
        bytes.remove_prefix(submitted.consumed_bytes_);
        session.wake_writer();
    }
    co_return http3_data_write_status::completed;
}

http3_buffered_response_writer::http3_buffered_response_writer(http3_session& session, response_file_io file_io) noexcept :
    session_(session),
    file_io_(std::move(file_io)),
    file_reads_(file_io_.worker_, multiplexed_file_read_slots) {}
bool http3_buffered_response_writer::stream_ended(std::int64_t stream_id) noexcept {
    auto* stream = session_.find_stream(stream_id);
    if (stream == nullptr || stream->aborted()) {
        return true;
    }
    auto* signal = stream->signal();
    return signal != nullptr && signal->stopped();
}

task<multiplexed_buffered_response_write_result> http3_buffered_response_writer::write_file_body(std::int64_t stream_id, multiplexed_stream_signal& signal, response_file_source& source, http_status_code committed_status) {
    for (;;) {
        std::optional<multiplexed_file_read_limiter::slot> slot;
        std::size_t max_bytes = response_file_read_ahead_bytes;
        if (source.file_bytes_pending()) {
            // Read only what the stream may submit: a stream whose peer stopped
            // acknowledging holds neither a read buffer nor a pool task.
            if (!(co_await await_output_budget(session_, stream_id, signal))) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            auto acquired = co_await file_reads_.acquire([this, stream_id]() noexcept {
                return stream_ended(stream_id);
            });
            if (!acquired) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            slot.emplace(std::move(*acquired));
            if (stream_ended(stream_id)) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            const auto budget = session_.codec().available_output_bytes(stream_id);
            if (budget == 0) {
                continue;
            }
            max_bytes = budget;
        }
        const auto chunk = co_await source.next(max_bytes);
        if (chunk.kind() == response_body_chunk::kind_type::failed) {
            // The committed Content-Length can no longer be honoured.
            session_.reset_stream(stream_id, http3_error_code::internal_error);
            co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status, .file_failure_ = *source.failure()};
        }
        if (chunk.kind() == response_body_chunk::kind_type::end) {
            if (stream_ended(stream_id)) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            if (session_.codec().finish_stream(stream_id) != http3_finish_status::accepted) {
                session_.reset_stream(stream_id, http3_error_code::internal_error);
            }
            session_.wake_writer();
            co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
        }
        const auto status = co_await write_http3_stream_data(session_, stream_id, signal, chunk.bytes());
        source.release_buffer();
        if (status != http3_data_write_status::completed) {
            co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
        }
    }
}

task<multiplexed_buffered_response_write_result> http3_buffered_response_writer::write(std::int64_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    if (stream_ended(stream_id)) {
        co_return multiplexed_buffered_response_write_result{};
    }
    std::optional<response_file_source> source;
    // Open and validate the file before the head, without reading it: a file
    // that cannot be served still gets an error response. HEAD and
    // status-suppressed bodies do not transfer a representation.
    if (!write_plan.body_plan().body_suppressed() && (response.file_body() || response.has_multipart_file_body())) {
        if (const auto open_failure = co_await file_reads_.open_file_body(source, response, file_io_, [this, stream_id]() noexcept {
            return stream_ended(stream_id);
        })) {
            co_return *open_failure;
        }
    }

    const auto head_result = session_.codec().submit_response_head(stream_id, response, std::move(write_plan));
    const auto* submitted_head = std::get_if<http_buffered_response_write_plan>(&head_result);
    if (submitted_head == nullptr) {
        if (std::get<http3_response_head_error>(head_result) != http3_response_head_error::closed) {
            // Invalid final metadata cannot leave the peer's request waiting
            // for a response the head submission rejected.
            session_.reset_stream(stream_id, http3_error_code::internal_error);
        }
        co_return multiplexed_buffered_response_write_result{};
    }
    auto* stream = session_.find_stream(stream_id);
    auto* signal = stream != nullptr ? stream->signal() : nullptr;
    if (stream == nullptr || signal == nullptr) {
        throw std::logic_error("HTTP/3 response written for an undispatched stream");
    }
    stream->mark_response_committed();
    session_.wake_writer();
    const auto committed_status = submitted_head->body_plan().response_status();
    if (!submitted_head->send_body()) {
        co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
    }
    if (source) {
        co_return co_await write_file_body(stream_id, *signal, *source, committed_status);
    }
    const auto status = co_await write_http3_stream_data(session_, stream_id, *signal, response.body_bytes());
    if (status == http3_data_write_status::completed) {
        if (session_.codec().finish_stream(stream_id) != http3_finish_status::accepted) {
            session_.reset_stream(stream_id, http3_error_code::internal_error);
        }
        session_.wake_writer();
    }
    co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
}

}  // namespace ruvia::detail

#if !defined(_WIN32)

namespace ruvia::detail {

inline void enable_http3_packet_info(asio::ip::udp::socket& socket) {
    const int enabled = 1;
    const bool v6 = socket.local_endpoint().protocol() == asio::ip::udp::v6();
    int result;
    if (v6) {
#if defined(IPV6_RECVPKTINFO)
        result = ::setsockopt(socket.native_handle(), IPPROTO_IPV6, IPV6_RECVPKTINFO, &enabled, sizeof(enabled));
#else
        result = ::setsockopt(socket.native_handle(), IPPROTO_IPV6, IPV6_PKTINFO, &enabled, sizeof(enabled));
#endif
    } else {
#if defined(IP_PKTINFO)
        result = ::setsockopt(socket.native_handle(), IPPROTO_IP, IP_PKTINFO, &enabled, sizeof(enabled));
#else
        result = ::setsockopt(socket.native_handle(), IPPROTO_IP, IP_RECVDSTADDR, &enabled, sizeof(enabled));
#endif
    }
    if (result != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable QUIC packet information");
    }
}

// Receives actual destination and ingress interface even for wildcard sockets.
// Socket is nonblocking; callers wait on the existing Asio reactor between bursts.
inline std::size_t receive_http3_packet(asio::ip::udp::socket& socket, std::span<std::byte> bytes, asio::ip::udp::endpoint& remote, asio::ip::udp::endpoint& local, std::uint32_t& interface_index, std::error_code& error) noexcept {
    alignas(cmsghdr) std::array<std::byte, 256> control{};
    iovec buffer{bytes.data(), bytes.size()};
    msghdr message{};
    message.msg_name = remote.data();
    message.msg_namelen = static_cast<socklen_t>(remote.capacity());
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    ssize_t received;
    do {
        received = ::recvmsg(socket.native_handle(), &message, 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        error = {errno, asio::system_category()};
        return 0;
    }
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        error = asio::error::message_size;
        return 0;
    }
    if (message.msg_controllen > control.size()) {
        error = asio::error::fault;
        return 0;
    }
    remote.resize(message.msg_namelen);
    bool found = false;
    interface_index = 0;
    for (std::size_t offset = 0; message.msg_controllen - offset >= CMSG_LEN(0);) {
        auto* cmsg = reinterpret_cast<cmsghdr*>(control.data() + offset);
        const auto remaining = message.msg_controllen - offset;
        if (cmsg->cmsg_len < CMSG_LEN(0) || cmsg->cmsg_len > remaining) {
            error = asio::error::fault;
            return 0;
        }
#if defined(IP_PKTINFO)
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO && cmsg->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
            in_pktinfo info{};
            std::memcpy(&info, CMSG_DATA(cmsg), sizeof(info));
            asio::ip::address_v4::bytes_type address{};
            std::memcpy(address.data(), &info.ipi_addr, address.size());
            local.address(asio::ip::address_v4(address));
            if (info.ipi_ifindex < 0) {
                error = asio::error::fault;
                return 0;
            }
            interface_index = static_cast<std::uint32_t>(info.ipi_ifindex);
            found = true;
        }
#else
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_RECVDSTADDR && cmsg->cmsg_len >= CMSG_LEN(sizeof(in_addr))) {
            asio::ip::address_v4::bytes_type address{};
            std::memcpy(address.data(), CMSG_DATA(cmsg), address.size());
            local.address(asio::ip::address_v4(address));
            found = true;
        }
#endif
        if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO && cmsg->cmsg_len >= CMSG_LEN(sizeof(in6_pktinfo))) {
            in6_pktinfo info{};
            std::memcpy(&info, CMSG_DATA(cmsg), sizeof(info));
            asio::ip::address_v6::bytes_type address{};
            std::memcpy(address.data(), &info.ipi6_addr, address.size());
            auto v6 = asio::ip::address_v6(address);
            interface_index = info.ipi6_ifindex;
            if (v6.is_link_local() || v6.is_multicast_link_local()) {
                v6.scope_id(interface_index);
            }
            local.address(v6);
            found = true;
        }
        // CMSG_SPACE keeps the native alignment without CMSG_NXTHDR's
        // signed pointer-difference comparison on musl.
        const auto advance = CMSG_SPACE(cmsg->cmsg_len - CMSG_LEN(0));
        if (advance > remaining) {
            break;
        }
        offset += advance;
    }
    if (!found) {
        error = asio::error::fault;
        return 0;
    }
    error.clear();
    return static_cast<std::size_t>(received);
}

inline std::error_code send_http3_packet(asio::ip::udp::socket& socket, const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    alignas(cmsghdr) std::array<std::byte, 256> control{};
    iovec buffer{const_cast<std::byte*>(bytes.data()), bytes.size()};
    msghdr message{};
    message.msg_name = const_cast<asio::ip::udp::endpoint::data_type*>(remote.data());
    message.msg_namelen = static_cast<socklen_t>(remote.size());
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    auto* cmsg = reinterpret_cast<cmsghdr*>(control.data());
    if (local.address().is_v4()) {
        if (!std::in_range<int>(interface_index)) {
            return asio::error::invalid_argument;
        }
        const auto address = local.address().to_v4().to_bytes();
        cmsg->cmsg_level = IPPROTO_IP;
#if defined(IP_PKTINFO)
        cmsg->cmsg_type = IP_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
        message.msg_controllen = CMSG_SPACE(sizeof(in_pktinfo));
        in_pktinfo info{};
        info.ipi_ifindex = static_cast<int>(interface_index);
        std::memcpy(&info.ipi_spec_dst, address.data(), address.size());
        std::memcpy(CMSG_DATA(cmsg), &info, sizeof(info));
#else
        cmsg->cmsg_type = IP_SENDSRCADDR;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in_addr));
        message.msg_controllen = CMSG_SPACE(sizeof(in_addr));
        std::memcpy(CMSG_DATA(cmsg), address.data(), address.size());
#endif
    } else {
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
        message.msg_controllen = CMSG_SPACE(sizeof(in6_pktinfo));
        in6_pktinfo info{};
        const auto address = local.address().to_v6().to_bytes();
        info.ipi6_ifindex = interface_index;
        std::memcpy(&info.ipi6_addr, address.data(), address.size());
        std::memcpy(CMSG_DATA(cmsg), &info, sizeof(info));
    }
    ssize_t sent;
    do {
        sent = ::sendmsg(socket.native_handle(), &message, 0);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        return {errno, asio::system_category()};
    }
    if (static_cast<std::size_t>(sent) != bytes.size()) {
        return asio::error::message_size;
    }
    return {};
}

}  // namespace ruvia::detail
#endif

namespace ruvia::detail {
namespace {

#if !defined(_WIN32)
// Datagrams read after one completed wait before waiting again, so a busy
// socket neither costs a wait per datagram nor monopolizes its worker.
constexpr std::size_t receive_burst = 32;
#endif
// RFC 9000 §14.1: a client Initial is padded to 1200 bytes; smaller
// datagrams never get a Version Negotiation reply (§6.1), which keeps it no
// larger than what triggered it.
constexpr std::size_t min_initial_datagram_bytes = 1200;

void initialize_server_quic_crypto() {
    if (!try_initialize_quic_crypto()) {
        throw std::runtime_error("failed to initialize the QUIC TLS backend");
    }
}

}  // namespace

http3_endpoint::http3_endpoint(const http3_worker_services& services, http_server_session_config& session, asio::ip::udp::endpoint endpoint, std::size_t listener_index, task_scope& tasks, connection_count& listener_connections)
#if !defined(_WIN32)
    :
    activated_(services.worker_),
    services_(services),
#else
    :
    services_(services),
#endif
    session_(session),
    configured_endpoint_(endpoint),
    listener_index_(listener_index),
    tasks_(tasks),
    listener_connections_(listener_connections),
#if !defined(_WIN32)
    socket_(services.io_),
    receive_buffer_(),
#endif
    connections_() {
    if (!session_.http3_.has_value() || session_.http3_->secret_ == nullptr) {
        throw std::logic_error("HTTP/3 endpoint requires a validated HTTP/3 service");
    }
}

http3_endpoint::~http3_endpoint() {
    close();
    if (registered_) {
        services_.placement_.directory_->unregister_endpoint(services_.placement_.worker_index_, *this);
    }
}

void http3_endpoint::open([[maybe_unused]] inherited_listener_sockets* inherited) {
    initialize_server_quic_crypto();
#if defined(_WIN32)
    // The acceptor binds exactly the configured endpoint (its port is never 0).
    local_endpoint_ = configured_endpoint_;
#else
    receive_buffer_.resize(http3_max_udp_payload_bytes);
    services_.placement_.directory_->open_listener_socket(services_.placement_.worker_index_, listener_index_, configured_endpoint_, secret(), inherited, socket_, session_.ipv6_only_);
    local_endpoint_ = socket_.local_endpoint();
    enable_http3_packet_info(socket_);
#endif
    services_.placement_.directory_->register_endpoint(services_.placement_.worker_index_, *this, local_endpoint_, session_.http3_->secret_, listener_index_);
    registered_ = true;
    open_ = true;
#if !defined(_WIN32)
    tasks_.spawn(run_receive_loop());
#endif
}

void http3_endpoint::start() noexcept {
    admitting_ = true;
#if !defined(_WIN32)
    activated_.notify();
#endif
}

void http3_endpoint::retire() noexcept {
    admitting_ = false;
    retiring_ = true;
    if (sessions_ == 0) {
        release_after_last_connection();
    }
}

void http3_endpoint::release_after_last_connection() noexcept {
#if defined(_WIN32)
    // The other workers' connections may still need the acceptor's socket.
    if (std::exchange(open_, false)) {
        services_.placement_.acceptor_->release_http3_listener(listener_index_);
    }
#else
    close();
#endif
}

void http3_endpoint::close() noexcept {
    admitting_ = false;
    open_ = false;
#if !defined(_WIN32)
    activated_.notify();
    std::error_code ignored;
    socket_.close(ignored);
#endif
}

#if !defined(_WIN32)
native_socket http3_endpoint::duplicate_socket() {
    if (!socket_.is_open()) {
        throw std::logic_error("the HTTP/3 listener socket is closed");
    }
    return duplicate_native_socket(socket_.native_handle());
}
#endif

std::error_code http3_endpoint::send(const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
#if defined(_WIN32)
    const auto& placement = services_.placement_;
    if (!placement.acceptor_->transmit(placement.worker_index_, listener_index_, remote, bytes, local, interface_index)) {
        return asio::error::no_buffer_space;
    }
    return {};
#else
    return send_http3_packet(socket_, remote, bytes, local, interface_index);
#endif
}

#if !defined(_WIN32)
task<std::error_code> http3_endpoint::wait_writable() {
    const auto completion = co_await ruvia::async_asio([this](auto handler) {
        socket_.async_wait(asio::socket_base::wait_write, std::move(handler));
    });
    co_return completion.error_code();
}
#endif

bool http3_endpoint::register_connection_id(const http3_cid_key& key, http3_session& session) {
    return connections_.try_emplace(key, &session).second;
}

void http3_endpoint::unregister_connection_id(const http3_cid_key& key) noexcept {
    connections_.erase(key);
}

#if !defined(_WIN32)
task<void> http3_endpoint::run_receive_loop() {
    co_await activated_.wait();
    try {
        while (socket_.is_open()) {
            const auto completion = co_await ruvia::async_asio([this](auto handler) {
                socket_.async_wait(asio::socket_base::wait_read, std::move(handler));
            });
            if (!socket_.is_open()) {
                co_return;
            }
            if (const auto error = completion.error_code()) {
                if (http3_ignorable_receive_error(error)) {
                    continue;
                }
                throw std::system_error(error, "HTTP/3 UDP receive failed");
            }
            for (std::size_t burst = 0; burst < receive_burst && socket_.is_open(); ++burst) {
                asio::ip::udp::endpoint remote;
                auto local = local_endpoint_;
                std::uint32_t interface_index{};
                std::error_code error;
                const auto size = receive_http3_packet(socket_, receive_buffer_, remote, local, interface_index, error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    break;
                }
                if (error) {
                    if (http3_ignorable_receive_error(error)) {
                        continue;
                    }
                    throw std::system_error(error, "HTTP/3 UDP receive failed");
                }
                receive(std::span<const std::byte>(receive_buffer_.data(), size), remote, local, interface_index);
            }
        }
    } catch (...) {
        // The listener cannot receive any more: its connections time out and
        // the failure is reported once.
        admitting_ = false;
        ruvia::report_unhandled_failure("HTTP/3 receive", std::current_exception());
    }
}
#endif

void http3_endpoint::receive_handoff(http3_handoff_datagram& datagram) noexcept {
    receive(std::span<const std::byte>(datagram.bytes_.data(), datagram.bytes_.size()), datagram.remote_, datagram.local_, datagram.interface_index_);
}

void http3_endpoint::receive(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    ngtcp2_version_cid header{};
    const auto decoded = ngtcp2_pkt_decode_version_cid(&header, data, bytes.size(), http3_connection_id_length);
    if ((decoded != 0 && decoded != NGTCP2_ERR_VERSION_NEGOTIATION) || header.dcidlen > NGTCP2_MAX_CIDLEN) {
        return;
    }
    const auto destination = std::span<const std::uint8_t>(header.dcid, header.dcidlen);
    const auto key = http3_cid_key::from(destination);
    const auto& placement = services_.placement_;
    const http3_endpoint_registration* authenticated = nullptr;
    http3_endpoint* admitting = nullptr;
    std::optional<std::size_t> owner;
    // SO_REUSEPORT may choose any socket, including a removed listener's.
    // Search the one worker-local directory before interpreting an unknown
    // CID with the receiving socket's (possibly obsolete) secret.
    for (const auto& registration : placement.directory_->endpoints(placement.worker_index_)) {
        if (registration.local_ != local_endpoint_) {
            continue;
        }
        auto* endpoint = registration.endpoint_;
        if (endpoint != nullptr && endpoint->open_) {
            if (const auto found = endpoint->connections_.find(key); found != endpoint->connections_.end()) {
                found->second->receive(bytes, remote, local, interface_index);
                return;
            }
            if (endpoint->admitting_) {
                admitting = endpoint;
            }
        }
    }
    for (const auto& registration : placement.directory_->endpoints(placement.worker_index_)) {
        if (registration.local_ != local_endpoint_) {
            continue;
        }
        if (const auto candidate = http3_connection_id_owner(*registration.secret_, destination, placement.directory_->worker_count())) {
            authenticated = &registration;
            owner = candidate;
        }
    }
    if (owner.has_value() && *owner != placement.worker_index_) {
        (void)placement.directory_->hand_off(*owner, authenticated->listener_index_, bytes, remote, local, interface_index);
        return;
    }
    auto* endpoint = authenticated != nullptr ? authenticated->endpoint_ : admitting;
    if (endpoint == nullptr || !endpoint->open_) {
        return;
    }
    if (!owner.has_value() && header.version != 0) {
        owner = http3_initial_worker(endpoint->secret(), destination, placement.directory_->worker_count());
        if (*owner != placement.worker_index_) {
            (void)placement.directory_->hand_off(*owner, endpoint->listener_index_, bytes, remote, local, interface_index);
            return;
        }
    }
    if (decoded == NGTCP2_ERR_VERSION_NEGOTIATION) {
        if (bytes.size() >= min_initial_datagram_bytes && endpoint->admitting_) {
            if (const auto admission = services_.options_.connection_admission_) {
                const connection_event event(services_.connection_ids_.next(), {to_ip_address(local.address()), local.port()}, {to_ip_address(remote.address()), remote.port()}, true);
                if (!admission(event)) {
                    services_.refused_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            endpoint->send_version_negotiation(header, remote, local, interface_index);
        }
    } else if (header.version != 0) {
        endpoint->accept(bytes, remote, local, interface_index);
    } else if (endpoint->admitting_ && owner == placement.worker_index_) {
        endpoint->send_stateless_reset(destination, bytes.size(), remote, local, interface_index);
    }
}

void http3_endpoint::accept(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    if (!admitting_ || !http_server_worker_running(services_.state_)) {
        return;
    }
    ngtcp2_pkt_hd header{};
    if (ngtcp2_accept(&header, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()) != 0) {
        return;
    }
    const auto connection_id = services_.connection_ids_.next();
    connection_event event(connection_id, {to_ip_address(local.address()), local.port()}, {to_ip_address(remote.address()), remote.port()}, true);
    if (services_.options_.connection_admission_ && !services_.options_.connection_admission_(event)) {
        services_.refused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ngtcp2_cid original_dcid = header.dcid;
    std::optional<ngtcp2_cid> retry_scid;
    std::span<const std::uint8_t> retry_token;
    const bool require_retry = session_.http3_->retry_;
    if (require_retry || header.tokenlen != 0) {
        if (header.tokenlen == 0) {
            const auto max_bytes = bytes.size() > (std::numeric_limits<std::size_t>::max)() / 3 ? (std::numeric_limits<std::size_t>::max)() : bytes.size() * 3;
            send_retry(header, remote, max_bytes, local, interface_index);
            return;
        }
        if (header.token[0] != NGTCP2_CRYPTO_TOKEN_MAGIC_RETRY2) {
            if (require_retry || header.dcid.datalen < NGTCP2_MIN_INITIAL_DCIDLEN) {
                send_stateless_connection_close(header, remote, NGTCP2_INVALID_TOKEN, local, interface_index);
                return;
            }
        } else {
            const auto& server = services_.options_.server_;
            const auto timeout_ms = server.header_completion_timeout_.has_value() ? server.header_completion_timeout_ : server.request_header_timeout_;
            constexpr auto max_milliseconds = static_cast<std::int64_t>(UINT64_MAX / NGTCP2_MILLISECONDS);
            const auto milliseconds = timeout_ms.has_value() ? (std::max)(timeout_ms->value().count(), std::int64_t{1}) : std::int64_t{0};
            const ngtcp2_duration token_lifetime = timeout_ms.has_value() ? (milliseconds >= max_milliseconds ? UINT64_MAX : static_cast<ngtcp2_duration>(milliseconds) * NGTCP2_MILLISECONDS) : 10 * NGTCP2_SECONDS;
            const auto timestamp = static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            const int verified = ngtcp2_crypto_verify_retry_token2(&original_dcid, header.token, header.tokenlen, secret().data(), secret().size(), header.version, reinterpret_cast<const ngtcp2_sockaddr*>(remote.data()), static_cast<ngtcp2_socklen>(remote.size()), &header.dcid, token_lifetime, timestamp);
            if (verified != 0) {
                if (verified == NGTCP2_CRYPTO_ERR_VERIFY_TOKEN || require_retry || header.dcid.datalen < NGTCP2_MIN_INITIAL_DCIDLEN) {
                    send_stateless_connection_close(header, remote, NGTCP2_INVALID_TOKEN, local, interface_index);
                    return;
                }
            } else {
                retry_scid = header.dcid;
                retry_token = std::span<const std::uint8_t>(header.token, header.tokenlen);
            }
        }
    }
    const auto& max_connections = services_.options_.server_.max_connections_per_worker_;
    // At the cap the connection idle longest gives up its slot, as for TCP.
    if (max_connections.has_value() && services_.connection_count_.active() >= *max_connections && !services_.scanner_.evict_oldest_reusable()) {
        services_.refused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try {
        connection_lifecycle lifecycle(services_.registry_, &session_, std::move(event), services_.options_.connection_open_, services_.options_.connection_close_, interface_index);
        auto session = std::make_unique<http3_session>(*this, connection_slot(listener_connections_), header, remote, original_dcid, retry_scid, retry_token, std::move(lifecycle));
        session->receive(bytes, remote, local, interface_index);
        tasks_.spawn(serve(std::move(session)));
    } catch (...) {
        auto failure = std::current_exception();
        std::string address;
        try {
            assign_peer_address(address, remote.address());
        } catch (...) {
            address.clear();
        }
        services_.options_.connection_failure_.invoke(address, std::move(failure));
    }
}

task<void> http3_endpoint::serve(session_pointer session) {
    // Spawning runs this task until its first suspension, so the count
    // includes every session spawned.
    ++sessions_;
    try {
        co_await session->run();
    } catch (...) {
        // run() already ended the connection; its reason goes to the
        // connection-failure sink rather than the worker's task scope.
        services_.options_.connection_failure_.invoke(session->remote_address(), std::current_exception());
    }
    // The session object itself is destroyed with this frame, which the
    // listener's scope joins before the endpoint goes away.
    if (--sessions_ == 0 && retiring_) {
        release_after_last_connection();
    }
}

void http3_endpoint::send_version_negotiation(const ngtcp2_version_cid& header, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    // RFC 9000 §17.2.1: the client's IDs echoed back, swapped, with the
    // versions this server accepts.
    static constexpr std::array<std::uint32_t, 2> versions{NGTCP2_PROTO_VER_V1, NGTCP2_PROTO_VER_V2};
    std::array<std::uint8_t, 1 + 4 + 1 + 255 + 1 + 255 + sizeof(versions)> packet{};
    unsigned char unused = 0;
    if (!fill_random_bytes(std::span<unsigned char>(&unused, 1))) {
        return;
    }
    const auto written = ngtcp2_pkt_write_version_negotiation(packet.data(), packet.size(), unused, header.scid, header.scidlen, header.dcid, header.dcidlen, versions.data(), versions.size());
    if (written > 0) {
        (void)send(remote, std::as_bytes(std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(written))), local, interface_index);
    }
}

void http3_endpoint::send_retry(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote, std::size_t max_packet_bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    ngtcp2_cid retry_scid{};
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (!make_http3_connection_id(secret(), services_.placement_.worker_index_, retry_scid)) {
            return;
        }
        if (ngtcp2_cid_eq(&retry_scid, &header.dcid) == 0) {
            break;
        }
    }
    if (ngtcp2_cid_eq(&retry_scid, &header.dcid) != 0) {
        return;
    }
    std::array<std::uint8_t, NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN2> token{};
    const auto timestamp = static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto token_len = ngtcp2_crypto_generate_retry_token2(token.data(), secret().data(), secret().size(), header.version, reinterpret_cast<const ngtcp2_sockaddr*>(remote.data()), static_cast<ngtcp2_socklen>(remote.size()), &retry_scid, &header.dcid, timestamp);
    if (token_len < 0) {
        return;
    }
    std::array<std::uint8_t, http3_max_datagram_bytes> packet{};
    const auto capacity = (std::min)(packet.size(), max_packet_bytes);
    const auto written = ngtcp2_crypto_write_retry(packet.data(), capacity, header.version, &header.scid, &retry_scid, &header.dcid, token.data(), static_cast<std::size_t>(token_len));
    if (written > 0) {
        (void)send(remote, std::as_bytes(std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(written))), local, interface_index);
    }
}

void http3_endpoint::send_stateless_connection_close(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote, std::uint64_t error_code, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    std::array<std::uint8_t, http3_max_datagram_bytes> packet{};
    const auto written = ngtcp2_crypto_write_connection_close(packet.data(), packet.size(), header.version, &header.scid, &header.dcid, error_code, nullptr, 0);
    if (written > 0) {
        (void)send(remote, std::as_bytes(std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(written))), local, interface_index);
    }
}

bool http3_endpoint::consume_stateless_reset_budget() noexcept {
    constexpr std::size_t max_burst = 32;
    constexpr auto refill_interval = std::chrono::milliseconds(25);
    const auto current = std::chrono::steady_clock::now();
    if (stateless_reset_refill_at_ == std::chrono::steady_clock::time_point{}) {
        stateless_reset_refill_at_ = current;
    } else if (current > stateless_reset_refill_at_) {
        const auto steps = static_cast<std::size_t>((current - stateless_reset_refill_at_) / refill_interval);
        if (steps != 0) {
            stateless_reset_tokens_ = steps >= max_burst - stateless_reset_tokens_ ? max_burst : stateless_reset_tokens_ + steps;
            stateless_reset_refill_at_ = current - ((current - stateless_reset_refill_at_) % refill_interval);
        }
    }
    if (stateless_reset_tokens_ == 0) {
        return false;
    }
    --stateless_reset_tokens_;
    return true;
}

void http3_endpoint::send_stateless_reset(std::span<const std::uint8_t> destination_cid, std::size_t trigger_bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    constexpr std::size_t min_stateless_reset_bytes = 1 + NGTCP2_MIN_STATELESS_RESET_RANDLEN + NGTCP2_STATELESS_RESET_TOKENLEN;
    if (trigger_bytes <= min_stateless_reset_bytes || !consume_stateless_reset_budget()) {
        return;
    }
    ngtcp2_cid cid{};
    ngtcp2_cid_init(&cid, destination_cid.data(), destination_cid.size());
    http3_quic_stateless_reset_token token{};
    std::array<unsigned char, http3_connection_id_length + 22 - NGTCP2_STATELESS_RESET_TOKENLEN> random_bytes{};
    if (!make_http3_stateless_reset_token(secret(), cid, token) || !fill_random_bytes(random_bytes)) {
        return;
    }
    std::array<std::byte, http3_connection_id_length + 23> packet{};
    const auto written = write_http3_quic_stateless_reset(token, random_bytes, packet, trigger_bytes - 1);
    if (written != 0) {
        (void)send(remote, std::span<const std::byte>(packet.data(), written), local, interface_index);
    }
}

namespace {

// Computes the 32-bit keyed digest of the 16 random bytes `cid[4..19]`. The
// high 16 bits mask the owner worker index in `cid[0..1]`; the full digest
// and the unmasked worker index feed `connection_id_tag`. Executed identically
// by the Linux SO_ATTACH_REUSEPORT_CBPF program.
[[nodiscard]] std::uint32_t connection_id_digest(const http3_secret& secret, const std::uint8_t* cid) noexcept {
    const auto k0 = read_http3_be32(secret.data());
    const auto k1 = read_http3_be32(secret.data() + 4);
    const auto k2 = read_http3_be32(secret.data() + 8);
    const auto k3 = read_http3_be32(secret.data() + 12);
    const auto w0 = read_http3_be32(cid + 4);
    const auto w1 = read_http3_be32(cid + 8);
    const auto w2 = read_http3_be32(cid + 12);
    const auto w3 = read_http3_be32(cid + 16);
    std::uint32_t acc = (w0 ^ k0) * http3_cid_mix_prime_1;
    acc = (acc ^ w1 ^ k1) * http3_cid_mix_prime_2;
    acc = (acc ^ w2 ^ k2) * http3_cid_mix_prime_1;
    acc = (acc ^ w3 ^ k3) * http3_cid_mix_prime_2;
    return acc ^ (acc >> 16);
}

[[nodiscard]] std::uint16_t connection_id_tag(const http3_secret& secret, std::uint32_t digest, std::uint16_t worker_index) noexcept {
    const auto k4 = read_http3_be32(secret.data() + 16);
    const std::uint32_t mixed = (digest ^ k4 ^ static_cast<std::uint32_t>(worker_index)) * http3_cid_mix_prime_1;
    return static_cast<std::uint16_t>(mixed ^ (mixed >> 16));
}

}  // namespace

std::size_t http3_cid_key_hash::operator()(const http3_cid_key& key) const noexcept {
    return static_cast<std::size_t>(fnv1a_accumulate(fnv1a_offset_basis, std::string_view(reinterpret_cast<const char*>(key.bytes_.data()), key.size_)));
}

bool make_http3_connection_id(const http3_secret& secret, std::size_t worker_index, ngtcp2_cid& cid) noexcept {
    if (!std::in_range<std::uint16_t>(worker_index)) {
        return false;
    }
    std::array<std::uint8_t, http3_connection_id_length> bytes{};
    if (!fill_random_bytes(std::span<unsigned char>(bytes.data() + 4, bytes.size() - 4))) {
        return false;
    }
    const auto worker = static_cast<std::uint16_t>(worker_index);
    const auto digest = connection_id_digest(secret, bytes.data());
    const auto masked_worker = static_cast<std::uint16_t>(worker ^ static_cast<std::uint16_t>(digest >> 16));
    const auto tag = connection_id_tag(secret, digest, worker);
    bytes[0] = static_cast<std::uint8_t>(masked_worker >> 8);
    bytes[1] = static_cast<std::uint8_t>(masked_worker);
    bytes[2] = static_cast<std::uint8_t>(tag >> 8);
    bytes[3] = static_cast<std::uint8_t>(tag);
    ngtcp2_cid_init(&cid, bytes.data(), bytes.size());
    return true;
}

std::optional<std::size_t> http3_connection_id_owner(const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept {
    if (cid.size() != http3_connection_id_length) {
        return std::nullopt;
    }
    const auto digest = connection_id_digest(secret, cid.data());
    const auto worker = static_cast<std::uint16_t>(read_http3_be16(cid.data()) ^ static_cast<std::uint16_t>(digest >> 16));
    if (static_cast<std::size_t>(worker) >= worker_count) {
        return std::nullopt;
    }
    if (read_http3_be16(cid.data() + 2) != connection_id_tag(secret, digest, worker)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(worker);
}

std::size_t http3_initial_worker(const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept {
    // Match the optional Linux cBPF selector, so software dispatch keeps the
    // same owner when SO_REUSEPORT members change or no selector is available.
    if (cid.size() == http3_connection_id_length) {
        return connection_id_digest(secret, cid.data()) % worker_count;
    }
    if (cid.size() >= NGTCP2_MIN_INITIAL_DCIDLEN) {
        const auto first = read_http3_be32(cid.data()) ^ read_http3_be32(secret.data());
        const auto second = read_http3_be32(cid.data() + 4) ^ read_http3_be32(secret.data() + 4);
        const auto mixed = ((first * http3_cid_mix_prime_1) ^ second) * http3_cid_mix_prime_2;
        return (mixed >> 16) % worker_count;
    }
    auto hash = fnv1a_accumulate(fnv1a_offset_basis, std::string_view(reinterpret_cast<const char*>(secret.data()), secret.size()));
    hash = fnv1a_accumulate(hash, std::string_view(reinterpret_cast<const char*>(cid.data()), cid.size()));
    return static_cast<std::size_t>(hash % worker_count);
}

bool make_http3_stateless_reset_token(const http3_secret& secret, const ngtcp2_cid& cid, http3_quic_stateless_reset_token& token) noexcept {
    return ngtcp2_crypto_generate_stateless_reset_token(token.data(), secret.data(), secret.size(), &cid) == 0;
}

#if !defined(_WIN32)
namespace {

class unique_udp_fd final {
public:
    unique_udp_fd() noexcept = default;
    explicit unique_udp_fd(int fd) noexcept :
        fd_(fd) {}
    unique_udp_fd(unique_udp_fd&& other) noexcept :
        fd_(std::exchange(other.fd_, -1)) {}
    unique_udp_fd& operator=(unique_udp_fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    unique_udp_fd(const unique_udp_fd&) = delete;
    unique_udp_fd& operator=(const unique_udp_fd&) = delete;
    ~unique_udp_fd() {
        reset();
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return fd_ >= 0;
    }
    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }
    void reset() noexcept {
        if (fd_ >= 0) {
            (void)::close(std::exchange(fd_, -1));
        }
    }

private:
    int fd_{-1};
};

unique_udp_fd open_reuseport_udp_fd(const asio::ip::udp& protocol, const sockaddr* address, socklen_t address_length, [[maybe_unused]] std::optional<bool> ipv6_only) {
#if !defined(SO_REUSEPORT)
    (void)protocol;
    (void)address;
    (void)address_length;
    throw std::system_error(asio::error::operation_not_supported, "HTTP/3 worker listeners require SO_REUSEPORT");
#else
    int type = SOCK_DGRAM;
#if defined(SOCK_CLOEXEC)
    type |= SOCK_CLOEXEC;
#endif
    unique_udp_fd fd(::socket(protocol.family(), type, protocol.protocol()));
    if (!fd) {
        throw std::system_error(errno, asio::system_category(), "failed to open HTTP/3 UDP listener socket");
    }
#if !defined(SOCK_CLOEXEC) && defined(FD_CLOEXEC)
    const int flags = ::fcntl(fd.get(), F_GETFD);
    if (flags == -1 || ::fcntl(fd.get(), F_SETFD, flags | FD_CLOEXEC) == -1) {
        throw std::system_error(errno, asio::system_category(), "failed to configure HTTP/3 UDP listener FD_CLOEXEC");
    }
#endif
    const int enabled = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable HTTP/3 listener SO_REUSEADDR");
    }
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable HTTP/3 listener SO_REUSEPORT");
    }
    if (ipv6_only) {
        const int value = *ipv6_only ? 1 : 0;
        if (::setsockopt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &value, sizeof(value)) != 0) {
            throw std::system_error(errno, asio::system_category(), "failed to configure HTTP/3 IPV6_V6ONLY");
        }
    }
    if (::bind(fd.get(), address, address_length) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to bind HTTP/3 UDP listener socket");
    }
    return fd;
#endif
}

#if defined(__linux__) && defined(SO_ATTACH_REUSEPORT_CBPF)
// Attaches an unprivileged cBPF socket selector that verifies server-issued
// 20-byte QUIC connection IDs (both Short Header 1-RTT packets, including
// post-migration / NAT-rebound paths, and Long Header packets carrying a
// 20-byte server DCID) using the listener secret inlined as immediate words,
// returning the unmasked owner worker index directly in the kernel. Client
// Initial packets and forged IDs fall back to a keyed hash of the packet's
// connection ID bytes so retransmissions stick to one worker while unkeyed
// floods spread across all workers.
void attach_reuseport_cbpf(int fd, const http3_secret& secret, std::size_t worker_count) {
    if (worker_count <= 1 || !std::in_range<std::uint16_t>(worker_count)) {
        return;
    }
    const auto count = static_cast<std::uint32_t>(worker_count);
    const auto k0 = read_http3_be32(secret.data());
    const auto k1 = read_http3_be32(secret.data() + 4);
    const auto k2 = read_http3_be32(secret.data() + 8);
    const auto k3 = read_http3_be32(secret.data() + 12);
    const auto k4 = read_http3_be32(secret.data() + 16);
    constexpr auto p1 = http3_cid_mix_prime_1;
    constexpr auto p2 = http3_cid_mix_prime_2;
    constexpr auto random_offset = static_cast<std::uint32_t>(SKF_AD_OFF + SKF_AD_RANDOM);

    std::array<sock_filter, 79> filter{{
        // 0..10: locate 20-byte server DCID at offset X = 1 (Short Header, len >= 21)
        // or X = 6 (Long Header, len >= 26 and DCID length == 20).
        BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 21, 0, 60),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x80, 2, 0),
        BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, 1),
        BPF_STMT(BPF_JMP | BPF_JA, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 26, 0, 54),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 5),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, http3_connection_id_length, 0, 52),
        BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, 6),

        // 11..21: load masked worker, tag, and random words w0..w3 from DCID[0..19].
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 0),
        BPF_STMT(BPF_ST, 0),
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 2),
        BPF_STMT(BPF_ST, 1),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 8),
        BPF_STMT(BPF_ST, 2),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 12),
        BPF_STMT(BPF_ST, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 16),
        BPF_STMT(BPF_ST, 4),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 4),

        // 22..40: compute connection_id_digest(secret, dcid) into M[2].
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k0),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_LDX | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k1),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p2),
        BPF_STMT(BPF_LDX | BPF_MEM, 3),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k2),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_LDX | BPF_MEM, 4),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k3),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p2),
        BPF_STMT(BPF_ST, 2),
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_LDX | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ST, 2),

        // 41..45: unmask worker_index = (digest >> 16) ^ masked_worker and bounds-check.
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_LDX | BPF_MEM, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, count, 14, 0),
        BPF_STMT(BPF_ST, 0),

        // 46..58: verify connection_id_tag(secret, digest, worker_index).
        BPF_STMT(BPF_LDX | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k4),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_ST, 3),
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_LDX | BPF_MEM, 3),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xffffu),
        BPF_STMT(BPF_LDX | BPF_MEM, 1),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_X, 0, 0, 2),
        BPF_STMT(BPF_LD | BPF_MEM, 0),
        BPF_STMT(BPF_RET | BPF_A, 0),

        // 59..61: unauthenticated 20-byte CID: route by keyed digest % worker_count.
        BPF_STMT(BPF_LD | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, count),
        BPF_STMT(BPF_RET | BPF_A, 0),

        // 62..75: client Initial with 8..19-byte DCID: keyed hash of DCID[0..7].
        BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 14, 0, 12),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 10),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k1),
        BPF_STMT(BPF_ST, 0),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 6),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k0),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_LDX | BPF_MEM, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p2),
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, count),
        BPF_STMT(BPF_RET | BPF_A, 0),

        // 76..78: sub-minimum datagram fallback.
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, random_offset),
        BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, count),
        BPF_STMT(BPF_RET | BPF_A, 0),
    }};
    const sock_fprog program{
        .len = static_cast<unsigned short>(filter.size()),
        .filter = filter.data(),
    };
    if (::setsockopt(fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF, &program, sizeof(program)) != 0) {
        if (errno != ENOPROTOOPT && errno != EOPNOTSUPP && errno != EPERM && errno != EACCES) {
            throw std::system_error(errno, asio::system_category(), "failed to attach HTTP/3 SO_ATTACH_REUSEPORT_CBPF");
        }
    }
}
#endif

}  // namespace
#endif

http3_worker_directory::http3_worker_directory(std::size_t worker_count) :
    slots_(),
    listener_sockets_() {
    slots_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        slots_.push_back(std::make_unique<slot>());
    }
}

http3_worker_directory::~http3_worker_directory() {
#if !defined(_WIN32)
    for (auto& group : listener_sockets_) {
        for (int& fd : group) {
            if (fd >= 0) {
                (void)::close(std::exchange(fd, -1));
            }
        }
    }
#endif
}

void http3_worker_directory::bind_worker(std::size_t index, const worker_ref& worker_value) {
    if (index >= slots_.size() || !worker_value.valid()) {
        throw std::invalid_argument("invalid HTTP/3 worker directory slot");
    }
    slots_[index]->worker_ = worker_value;
}

void http3_worker_directory::attach(std::size_t index, void* target, receive_type receive) noexcept {
    auto& value = *slots_[index];
    value.target_ = target;
    value.receive_ = receive;
}

void http3_worker_directory::detach(std::size_t index) noexcept {
    auto& value = *slots_[index];
    value.target_ = nullptr;
    value.receive_ = nullptr;
}

void http3_worker_directory::register_endpoint(std::size_t worker_index, http3_endpoint& endpoint, const asio::ip::udp::endpoint& local, std::shared_ptr<const http3_secret> secret, std::size_t listener_index) {
    slots_[worker_index]->endpoints_.push_back({&endpoint, local, std::move(secret), listener_index});
}

void http3_worker_directory::unregister_endpoint(std::size_t worker_index, http3_endpoint& endpoint) noexcept {
    for (auto& registration : slots_[worker_index]->endpoints_) {
        if (registration.endpoint_ == &endpoint) {
            registration.endpoint_ = nullptr;
            return;
        }
    }
}

#if !defined(_WIN32)
void http3_worker_directory::open_listener_socket(std::size_t worker_index, std::size_t listener_index, const asio::ip::udp::endpoint& configured_endpoint, const http3_secret& secret, inherited_listener_sockets* inherited, asio::ip::udp::socket& socket, std::optional<bool> ipv6_only) {
    if (worker_index >= slots_.size()) {
        throw std::invalid_argument("invalid HTTP/3 worker index");
    }
    unique_udp_fd assigned;
    {
        std::lock_guard<std::mutex> lock(bind_mutex_);
        while (listener_sockets_.size() <= listener_index) {
            listener_sockets_.emplace_back();
        }
        auto& group = listener_sockets_[listener_index];
        if (group.empty()) {
            std::vector<unique_udp_fd> opened;
            opened.reserve(slots_.size());
            if (inherited != nullptr && !inherited->udp().empty()) {
                // Validated as bound to this endpoint, one per worker in
                // worker order: the group's members as the kernel numbers them.
                for (const auto fd : inherited->claim_udp()) {
                    opened.emplace_back(fd);
                }
            } else {
                asio::ip::udp::endpoint bound_endpoint = configured_endpoint;
                for (std::size_t index = 0; index < slots_.size(); ++index) {
                    auto fd = open_reuseport_udp_fd(configured_endpoint.protocol(), bound_endpoint.data(), static_cast<socklen_t>(bound_endpoint.size()), ipv6_only);
                    if (index == 0) {
                        auto length = static_cast<socklen_t>(bound_endpoint.capacity());
                        if (::getsockname(fd.get(), bound_endpoint.data(), &length) != 0) {
                            throw std::system_error(errno, asio::system_category(), "failed to query HTTP/3 UDP listener endpoint");
                        }
                        bound_endpoint.resize(length);
                    }
                    opened.push_back(std::move(fd));
                }
            }
            if (opened.size() != slots_.size()) {
                throw std::invalid_argument("HTTP/3 listener sockets must number one per worker");
            }
#if defined(__linux__) && defined(SO_ATTACH_REUSEPORT_CBPF)
            attach_reuseport_cbpf(opened.front().get(), secret, slots_.size());
#else
            (void)secret;
#endif
            group.resize(slots_.size(), -1);
            for (std::size_t index = 0; index < opened.size(); ++index) {
                group[index] = opened[index].release();
            }
        }
        if (group[worker_index] < 0) {
            throw std::logic_error("HTTP/3 listener socket already claimed by worker");
        }
        assigned = unique_udp_fd(std::exchange(group[worker_index], -1));
    }
    socket.assign(configured_endpoint.protocol(), assigned.get());
    (void)assigned.release();
    socket.non_blocking(true);
}

void http3_worker_directory::close_unclaimed_listener_sockets(std::size_t listener_index) noexcept {
    const std::lock_guard<std::mutex> lock(bind_mutex_);
    if (listener_index < listener_sockets_.size()) {
        for (int& fd : listener_sockets_[listener_index]) {
            if (fd >= 0) {
                (void)::close(std::exchange(fd, -1));
            }
        }
    }
}
#endif

bool http3_worker_directory::hand_off(std::size_t owner, std::size_t listener_index, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index, std::uint32_t ecn) noexcept {
    if (owner >= slots_.size()) {
        return false;
    }
    auto& target = *slots_[owner];
    auto token = http3_in_flight_token::reserve(target.in_flight_);
    if (!token) {
        return false;
    }
    try {
        const auto status = target.worker_.post_factory([&]() -> move_only_function<void()> {
            std::vector<std::byte> copied(bytes.begin(), bytes.end());
            http3_handoff_datagram datagram{
                .bytes_ = std::move(copied),
                .remote_ = remote,
                .ecn_ = ecn,
                .local_ = local,
                .interface_index_ = interface_index,
            };
            return [&target, listener_index, owned = std::move(datagram), token = std::move(token)]() mutable noexcept {
                // The target pointer is confined to this, the owner's, thread.
                if (target.receive_ == nullptr) {
                    return;
                }
                for (const auto& registration : target.endpoints_) {
                    if (registration.listener_index_ != listener_index) {
                        continue;
                    }
                    if (registration.endpoint_ != nullptr) {
                        target.receive_(target.target_, listener_index, owned);
                        return;
                    }
                    // This worker may already have released the old endpoint
                    // while another worker still owns one of its connections.
                    for (const auto& fallback : target.endpoints_) {
                        if (fallback.endpoint_ != nullptr && fallback.local_ == registration.local_) {
                            target.receive_(target.target_, fallback.listener_index_, owned);
                            return;
                        }
                    }
                    return;
                }
            };
        });
        return status == post_status::accepted;
    } catch (...) {
        return false;
    }
}

}  // namespace ruvia::detail
