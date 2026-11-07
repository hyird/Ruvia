#include "http3/http3_session.h"

#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <algorithm>
#include <array>
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
#include <utility>
#include <variant>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/recycling_allocator.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/http/http3_request_head.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "body/http_request_body_facade.h"
#include "context/http_interim_response_output.h"
#include "http/http_protocol_error_info.h"
#include "http3/http3_endpoint.h"
#include "http3/http3_request_body_reader.h"
#include "http3/http3_response_stream_sink.h"
#include "http3/http3_tunnel_transport.h"
#include "integration/worker_capabilities.h"
#include "router/route_resolution.h"
#include "router/route_table.h"
#include "server/http_buffered_response.h"
#include "server/http_response_stream_dispatch.h"
#include "server/http_server_tls_contexts.h"
#include "server/http_server_tls_handshake.h"
#include "server/http_upgrade_route.h"
#include "server/peer_address.h"
#include "tls/tls_random.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t http3_request_arena_stack_bytes = request_arena_initial_bytes;
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
    if (!head.early_data()) {
        return false;
    }
    const auto method = head.method();
    return method != "GET" && method != "HEAD" && method != "OPTIONS" && method != "TRACE";
}

struct interim_target final {
    http3_session& session_;
    std::int64_t stream_id_;
};

// Resumes the coroutine on a later turn of its worker, letting other
// connections run between bursts of one connection's output.
struct worker_turn final {
    const worker_handle& worker_;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) const noexcept {
        worker_.defer_resume(continuation);
    }
    void await_resume() const noexcept {}
};

request_memory& emplace_request_memory(
    std::optional<request_memory>& storage, worker_memory& memory, std::span<std::byte> initial_buffer) {
    const auto initial_bytes = memory.request_initial_buffer_bytes();
    if (initial_bytes <= initial_buffer.size()) {
        storage.emplace(memory, initial_buffer.first(initial_bytes));
    } else {
        storage.emplace(memory);
    }
    return *storage;
}

void fill_random(std::uint8_t* destination, std::size_t size, const ngtcp2_rand_ctx*) noexcept {
    // ngtcp2 cannot fail this callback; packet protection without randomness
    // must not continue.
    if (!fill_random_bytes(std::span<unsigned char>(destination, size))) {
        std::terminate();
    }
}

[[nodiscard]] std::uint64_t duration_nanoseconds(const std::optional<std::chrono::milliseconds>& value) noexcept {
    if (!value.has_value()) {
        return 0;
    }
    constexpr auto max_milliseconds = static_cast<std::int64_t>(UINT64_MAX / NGTCP2_MILLISECONDS);
    const auto milliseconds = (std::max)(value->count(), std::int64_t{0});
    return milliseconds >= max_milliseconds ? UINT64_MAX
                                            : static_cast<std::uint64_t>(milliseconds) * NGTCP2_MILLISECONDS;
}

[[nodiscard]] std::chrono::steady_clock::time_point steady_time(ngtcp2_tstamp timestamp) noexcept {
    using nanoseconds = std::chrono::nanoseconds;
    if (timestamp >= static_cast<ngtcp2_tstamp>((std::numeric_limits<nanoseconds::rep>::max)())) {
        return (std::chrono::steady_clock::time_point::max)();
    }
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(nanoseconds(static_cast<nanoseconds::rep>(timestamp))));
}

[[nodiscard]] std::uint64_t error_value(http3_error_code code) noexcept {
    return static_cast<std::uint64_t>(code);
}

[[nodiscard]] ngtcp2_callbacks server_callbacks() noexcept {
    ngtcp2_callbacks callbacks{};
    callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    callbacks.rand = fill_random;
    return callbacks;
}

}  // namespace

void http3_session::free_tls::operator()(SSL* tls) const noexcept {
    SSL_free(tls);
}

void http3_session::free_crypto::operator()(ngtcp2_crypto_ossl_ctx* crypto) const noexcept {
    ngtcp2_crypto_ossl_ctx_del(crypto);
}

ngtcp2_conn* http3_session::native_connection(ngtcp2_crypto_conn_ref* reference) noexcept {
    return static_cast<http3_session*>(reference->user_data)->connection_->native_handle();
}

http3_session::http3_session(http3_endpoint& endpoint, connection_slot slot, const ngtcp2_pkt_hd& initial,
    const asio::ip::udp::endpoint& remote)
    : endpoint_(endpoint),
      services_(endpoint.services()),
      slot_(std::move(slot)),
      local_(endpoint.local_endpoint()),
      remote_(remote),
      path_remote_(remote),
      remote_address_(services_.memory_.resource()),
      connection_ids_(services_.memory_.resource()),
      datagram_(services_.memory_.resource()),
      client_certificate_(services_.memory_.resource()),
      inbound_buffers_(*services_.options_.inbound_budget_,
          services_.options_.server_.max_inbound_buffer_bytes_per_connection_),
      admission_policy_(http_request_admission_policy::server(services_.options_)),
      write_signal_(services_.worker_),
      handler_finished_(services_.worker_),
      streams_(services_.memory_.resource()),
      buffered_response_writer_(*this,
          response_file_io{.worker_ = services_.worker_,
              .stop_ = services_.stop_token_,
              .timeout_ = services_.options_.server_.write_timeout_}) {
    assign_peer_address(remote_address_, remote.address());
    auto& quic_context = endpoint.session_config().quic_tls_context_;
    if (!quic_context.has_value()) {
        throw std::logic_error("HTTP/3 listener has no QUIC TLS context");
    }
    tls_.reset(SSL_new(quic_context->native_handle()));
    if (!tls_ || ngtcp2_crypto_ossl_configure_server_session(tls_.get()) != 0) {
        throw std::runtime_error("failed to create the QUIC TLS session");
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
    if (!make_http3_connection_id(endpoint.secret(), services_.placement_.worker_index_, source) ||
        !make_http3_stateless_reset_token(endpoint.secret(), source, reset_token)) {
        throw std::runtime_error("failed to issue a QUIC connection ID");
    }
    const auto& server = services_.options_.server_;
    auto callbacks = server_callbacks();
    callbacks.get_new_connection_id2 = new_connection_id;
    callbacks.remove_connection_id = remove_connection_id;
    const auto handshake_timeout = duration_nanoseconds(
        server.header_completion_timeout_.has_value() ? server.header_completion_timeout_ : server.request_header_timeout_);
    const http3_connection_options http3_options{
        .resource_ = services_.memory_.resource(),
        .max_concurrent_streams_ = static_cast<std::size_t>(http3_max_concurrent_requests),
        .max_stream_output_bytes_ = http3_stream_output_bytes,
        .enable_connect_protocol_ = true,
        .enable_h3_datagram_ = true};
    connection_.emplace(http3_quic_connection::server({
        .callbacks_ = callbacks,
        .source_connection_id_ = source,
        .destination_connection_id_ = initial.scid,
        .original_destination_connection_id_ = initial.dcid,
        .stateless_reset_token_ = reset_token,
        .initial_path_ = path(remote_),
        .version_ = initial.version,
        .initial_timestamp_ = now(),
        .handshake_timeout_ = handshake_timeout == 0 ? UINT64_MAX : handshake_timeout,
        // Zero disables the QUIC idle timeout, as an absent idle_timeout_
        // disables the HTTP one.
        .idle_timeout_ = duration_nanoseconds(server.idle_timeout_),
        .initial_max_data_ = (std::min)(http3_max_connection_receive_window,
            static_cast<std::uint64_t>(server.max_inbound_buffer_bytes_per_connection_)),
        .initial_max_stream_data_ = (std::min)(http3_max_stream_receive_window,
            static_cast<std::uint64_t>((std::max)(server.max_buffered_body_bytes_, std::size_t{1}))),
        .initial_max_streams_bidi_ = http3_max_concurrent_requests,
        .max_udp_payload_size_ = http3_max_datagram_bytes,
        .callback_user_data_ = this,
        .resource_ = services_.memory_.resource(),
        .http3_ = http3_options,
    }));
    connection_->set_tls_native_handle(crypto_.get());
    configure_http3_server_tls_session(*tls_, *connection_, http3_options,
        endpoint.session_config().http3_->early_data_, tls_resumption_context_);

    scanner_guard_.emplace(services_.scanner_, scanner_entry_,
        connection_transport(
            this, [](void* target) noexcept { static_cast<http3_session*>(target)->close_now(); },
            [](void* target) noexcept { static_cast<http3_session*>(target)->request_drain(); }));
    scanner_entry_.register_periodic_check(
        scanner_checks_, this,
        [](void* target, std::int64_t now_ms) noexcept {
            static_cast<http3_session*>(target)->expire_stream_deadlines(now_ms);
        },
        [](void* target) noexcept { static_cast<http3_session*>(target)->request_drain(); });
    set_inactivity_phase();

    // Registered last: a constructor that throws leaves no table entry behind.
    connection_ids_.reserve(2);
    register_connection_id(source);
    try {
        // Retransmitted client Initials still carry the client's chosen ID.
        register_connection_id(initial.dcid);
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
    return static_cast<ngtcp2_tstamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

ngtcp2_path http3_session::path(asio::ip::udp::endpoint& remote) noexcept {
    return {
        .local = {local_.data(), static_cast<ngtcp2_socklen>(local_.size())},
        .remote = {remote.data(), static_cast<ngtcp2_socklen>(remote.size())},
        .user_data = nullptr,
    };
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

int http3_session::new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token,
    std::size_t size, void* user_data) noexcept {
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
            if (!make_http3_connection_id(self.endpoint_.secret(), self.services_.placement_.worker_index_, *cid) ||
                !make_http3_stateless_reset_token(self.endpoint_.secret(), *cid, reset_token)) {
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

const worker_handle& http3_session::worker() const noexcept {
    return services_.worker_;
}

std::pmr::memory_resource* http3_session::worker_resource() const noexcept {
    return services_.memory_.resource();
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
        connection_->reset_stream(stream_id, error_value(error));
    }
    if (auto* stream = find_stream(stream_id)) {
        stream->mark_aborted();
    }
    wake_writer();
}

void http3_session::consume_stream_data(std::int64_t stream_id, std::size_t bytes) noexcept {
    if (bytes != 0 && lifecycle_ == lifecycle_type::open) {
        connection_->consume_stream_data(stream_id, bytes);
        wake_writer();
    }
}

http3_quic_status http3_session::submit_datagram(std::int64_t stream_id, std::span<const std::byte> payload) {
    const auto status = connection_->submit_datagram(stream_id, payload);
    if (status == http3_quic_status::accepted || status == http3_quic_status::would_block) {
        wake_writer();
    }
    return status;
}

void http3_session::on_datagram(http3_datagram_event& event) {
    auto* stream = find_stream(event.stream_id_);
    if (stream == nullptr || stream->aborted() || stream->handler_done() || !stream->head().connect()) {
        return;
    }
    stream->note_request_body_progress(scanner_entry_.last_active_ms());
    stream->store_datagram(event.payload_, services_.options_.server_.max_buffered_body_bytes_);
}

void http3_session::receive(std::span<const std::byte> datagram, const asio::ip::udp::endpoint& remote) noexcept {
    if (lifecycle_ != lifecycle_type::open || connection_->closed()) {
        // The closing period absorbs late packets (RFC 9000 §10.2).
        return;
    }
    scanner_entry_.touch();
    path_remote_ = remote;
    try {
        (void)connection_->receive_datagram(path(path_remote_), NGTCP2_ECN_NOT_ECT, datagram, now());
    } catch (...) {
        services_.options_.connection_failure_.invoke(remote_address_, std::current_exception());
        connection_->close(error_value(http3_error_code::internal_error));
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
        connection_->close(error_value(http3_error_code::internal_error));
        terminate(std::make_error_code(std::errc::not_enough_memory));
    }
}

void http3_session::drain_events() {
    start_application();
    while (auto event = connection_->next_event()) {
        if (auto* headers = std::get_if<http3_headers_event>(&*event)) {
            on_headers(*headers);
        } else if (const auto* data = std::get_if<http3_data_event>(&*event)) {
            on_data(*data);
        } else if (auto* datagram = std::get_if<http3_datagram_event>(&*event)) {
            on_datagram(*datagram);
        } else if (const auto* end = std::get_if<http3_stream_end_event>(&*event)) {
            on_stream_end(end->stream_id_);
        } else if (const auto* action = std::get_if<http3_transport_action_event>(&*event)) {
            on_stream_abort(action->stream_id_);
        } else if (const auto* closed = std::get_if<http3_stream_close_event>(&*event)) {
            on_stream_close(*closed);
        }
        // A client GOAWAY (RFC 9114 §5.2) bounds only server push, which this
        // server never sends.
    }
}

void http3_session::start_application() {
    if (base_services_.has_value() || !connection_->handshake_completed() || connection_->closed()) {
        return;
    }
    // RFC 9114 §6.2: a client must allow the three server unidirectional
    // streams; one that does not cannot be served.
    if (connection_->start_http3() != http3_quic_status::accepted) {
        connection_->close(error_value(http3_error_code::internal_error));
        return;
    }
    std::optional<std::string_view> client_subject;
    if (endpoint_.session_config().tls()->client_certificates_.has_value()) {
        client_subject = extract_tls_client_certificate(tls_.get(), client_certificate_);
    }
    served_certificate_.emplace(SSL_get_certificate(tls_.get()));
    base_services_.emplace(services_.capabilities_.make_context_services(services_.stop_token_)
            .with_tls_transport(remote_address_, client_subject, remote_.port(), *served_certificate_)
            .with_inbound_buffer_pool(inbound_buffers_));
    base_services_->bind_routes(services_.routes_);
    set_inactivity_phase();
    // Authenticate the connection before releasing its bounded 0-RTT requests.
    for (auto& [stream_id, stream] : streams_) {
        if (!stream->aborted() && stream->dispatch_pending() && !admit_stream(stream_id)) {
            reset_stream(stream_id, http3_error_code::internal_error);
        }
    }
}

void http3_session::on_headers(http3_headers_event& event) {
    const auto stream_id = event.stream_id_;
    if (!event.trailers_) {
        on_request_head(event);
        return;
    }
    auto* stream = find_stream(stream_id);
    if (stream == nullptr || stream->aborted() || stream->handler_done()) {
        return;
    }
    if (append_http3_request_trailers(stream->trailers(), event.fields_).has_value()) {
        // RFC 9114 §4.1.2: a malformed trailer section makes the request
        // malformed.
        reset_stream(stream_id, http3_error_code::message_error);
    }
}

void http3_session::on_request_head(http3_headers_event& event) {
    const auto stream_id = event.stream_id_;
    if (find_stream(stream_id) != nullptr) {
        reset_stream(stream_id, http3_error_code::internal_error);
        return;
    }
    const auto& server = services_.options_.server_;
    ++accepted_request_heads_;
    if (!drain_begun_ && server.max_requests_per_connection_.has_value() &&
        accepted_request_heads_ >= *server.max_requests_per_connection_) {
        begin_drain();
    }
    auto head = http3_request_head::parse(std::move(event), worker_resource());
    if (!head.has_value()) {
        // RFC 9114 §4.1.2: a malformed request is a stream error of type
        // H3_MESSAGE_ERROR.
        reset_stream(stream_id, http3_error_code::message_error);
        return;
    }
    const auto method = classify_http_method(head->method());
    const auto target = head->path();
    const auto connect = head->connect();
    const auto protocol = http_ascii_equals_ignore_case(head->protocol(), "websocket")
                              ? std::string_view("websocket")
                              : head->protocol();
    auto resolution = services_.routes_.resolve(route_request_view{
        method, head->method(), target.substr(0, target.find('?')), head->authority(), protocol});
    auto body_mode = request_body_mode::buffered;
    const auto* resolved = resolution.resolved();
    if (resolved != nullptr) {
        body_mode = resolved->route().endpoint().request_body_mode();
    }
    if (connect) {
        body_mode = request_body_mode::stream;
    }
    const bool resolved_route = resolved != nullptr && !connect;
    const auto expectation_plan = head->expectation_plan(http_unsupported_expectation_policy::reject);
    const auto declared_length = head->declared_content_length();
    const bool ended = head->stream_ended();
    auto stream_value = make_pmr_object<http3_stream>(worker_resource(), std::move(*head), std::move(resolution),
        body_mode, worker_resource(), static_cast<std::pmr::memory_resource*>(&inbound_buffers_));
    auto& stream = *stream_value;
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
        const auto failure = http_request_body_size_failure(*declared_length,
            http_request_body_limit(stream.resolution(), body_mode, admission_policy_));
        if (failure.has_value()) {
            stream.mark_body_failure(*failure);
            body_limit_exceeded = true;
        }
    }
    // Same policy as HTTP/1 and HTTP/2: 100 Continue is sent when the server
    // starts consuming the content -- now for a buffered route, at the first
    // read for a streaming one -- and never for a request answered from its
    // head alone (RFC 9110 §10.1.1).
    if (expectation_plan == http_server_expectation_plan::send_continue && resolved_route && !ended &&
        !body_limit_exceeded && !reject_early_request(stream.head()) && body_mode == request_body_mode::buffered) {
        if (codec().submit_interim_response_head(
                stream_id, http_interim_response_head(ruvia::http_status::continue_value)) != http3_status::accepted) {
            reset_stream(stream_id, http3_error_code::internal_error);
            return;
        }
        wake_writer();
    }
    // A buffered body is dispatched once complete; everything else -- a
    // streamed body, CONNECT, an unresolved or oversized
    // request, a rejected expectation -- is answered from its head.
    if (ended || connect || body_mode == request_body_mode::stream || !resolved_route || body_limit_exceeded ||
        expectation_plan == http_server_expectation_plan::reject_unsupported || reject_early_request(stream.head())) {
        if (!admit_stream(stream_id)) {
            reset_stream(stream_id, http3_error_code::internal_error);
        }
    }
}

void http3_session::on_data(const http3_data_event& event) {
    const auto stream_id = event.stream_id_;
    const auto size = event.bytes_.size();
    auto* stream = find_stream(stream_id);
    // Content nobody will read -- of a stream already answered or abandoned,
    // a failed body, or a buffered body dispatched early -- returns
    // its credit at once. The request borrows a dispatched buffered body, so
    // appending to it would reallocate under that view.
    if (stream == nullptr || stream->aborted() || stream->handler_done() ||
        reject_early_request(stream->head()) || stream->body_failure().has_value() ||
        (stream->body_mode() == request_body_mode::buffered && stream->dispatched())) {
        consume_stream_data(stream_id, size);
        return;
    }
    stream->note_request_body_progress(scanner_entry_.last_active_ms());
    const auto total_limit = stream->head().connect() ? protocol_byte_limit::unlimited()
                                                      : http_request_body_limit(stream->resolution(), stream->body_mode(), admission_policy_);
    const auto status = stream->store(event.bytes_, total_limit, services_.options_.server_.max_buffered_body_bytes_);
    if (status == http3_body_store_status::backlog_overflow) {
        consume_stream_data(stream_id, size);
        reset_stream(stream_id, http3_error_code::request_cancelled);
        return;
    }
    if (status == http3_body_store_status::too_large) {
        stream->mark_body_failure(http_request_body_failure::too_large);
        consume_stream_data(stream_id, size);
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
    if (stream->body_mode() == request_body_mode::buffered) {
        // Buffered content is consumed as it is stored.
        consume_stream_data(stream_id, size);
    } else if (auto* signal = stream->signal()) {
        signal->wake();
    }
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
    // The peer reset its request or stopped reading the response; the core
    // already abandoned the other direction (RFC 9114 §4.1.1).
    if (auto* stream = find_stream(stream_id)) {
        stream->mark_aborted();
    }
}

void http3_session::on_stream_close(const http3_stream_close_event& event) noexcept {
    auto* stream = find_stream(event.stream_id_);
    if (stream == nullptr) {
        return;
    }
    stream->mark_closed();
    if (event.application_error_code_.has_value()) {
        stream->mark_aborted();
    }
    if (auto* signal = stream->signal()) {
        signal->wake();
    }
    remove_stream_if_done(event.stream_id_);
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
    if (!stream.closed() || (stream.dispatched() && !stream.handler_done())) {
        return;
    }
    consume_stream_data(stream_id, stream.unconsumed_bytes());
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
    if (!stream->request_ended() && !stream->aborted() && lifecycle_ == lifecycle_type::open) {
        // RFC 9114 §4.1.2: once the response is complete the server may stop
        // reading the request with H3_NO_ERROR.
        connection_->stop_receiving(stream_id, error_value(http3_error_code::no_error));
    }
    // Content that arrived but was never read holds connection credit.
    const auto unconsumed = stream->unconsumed_bytes();
    stream->close_request_body_timing();
    consume_stream_data(stream_id, unconsumed);
    stream->discard_body();
    remove_stream_if_done(stream_id);
    wake_writer();
}

void http3_session::set_inactivity_phase() noexcept {
    if (lifecycle_ != lifecycle_type::open) {
        return;
    }
    // The handshake is bounded like a request head; then, as for HTTP/2, an
    // open stream keeps the handler's inactivity deadman and idleness starts
    // when the last stream ends.
    if (!base_services_.has_value()) {
        scanner_entry_.set_phase(connection_scanner::phase::reading_initial);
    } else if (!streams_.empty() || active_handler_tasks_ != 0) {
        scanner_entry_.set_phase(connection_scanner::phase::idle);
    } else {
        scanner_entry_.set_phase(connection_scanner::phase::awaiting_stream);
    }
}

void http3_session::expire_stream_deadlines(std::int64_t now_ms) noexcept {
    const auto& server = services_.options_.server_;
    // Each expiry is one-shot (the body timing closes, an output wait expires
    // once), so resetting inside the walk never revisits a stream; a reset
    // never removes a stream from the table synchronously.
    for (auto& [stream_id, stream] : streams_) {
        if (stream->aborted()) {
            continue;
        }
        if (stream->request_body_overdue(now_ms, server.request_body_timeout_, server.body_completion_timeout_)) {
            stream->close_request_body_timing();
            reset_stream(stream_id, http3_error_code::request_cancelled);
            continue;
        }
        auto* signal = stream->signal();
        if (signal != nullptr && server.write_timeout_.has_value() &&
            signal->output_wait_expired(now_ms, *server.write_timeout_)) {
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
    if (stream == nullptr || stream->begin_dispatch(services_.worker_, termination_) == nullptr) {
        return false;
    }
    ++active_handler_tasks_;
    try {
        asio::co_spawn(services_.io_.get_executor(), ruvia::as_awaitable(dispatch_one(stream_id)),
            asio::bind_allocator(asio::recycling_allocator<void>(), [this, stream_id](std::exception_ptr exception) noexcept {
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
        --active_handler_tasks_;
        stream->mark_handler_done();
        return false;
    }
    return true;
}

task<http_response> http3_session::respond_body_failure(const http_request& request,
    request_memory& request_memory_value, http_request_body_failure failure,
    const context_services& request_services, http_response_tail& tail) {
    tail.disable_response_coding();
    return respond_http_request_rejection(services_.routes_, request, request_memory_value,
        http_request_rejection::error(copy_http_protocol_error_info(
            request_memory_value.resource(), http_request_body_protocol_error(failure))),
        request_services);
}

task<void> http3_session::dispatch_one(std::int64_t stream_id) {
    const auto& routes = services_.routes_;
    const auto& options = services_.options_;
    try {
        const auto request_start =
            options.access_log_.callback_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        std::array<std::byte, http3_request_arena_stack_bytes> arena_block;
        std::optional<request_memory> request_memory_storage;
        auto& request_memory_value =
            emplace_request_memory(request_memory_storage, services_.memory_, std::span<std::byte>(arena_block));
        auto* stream = find_stream(stream_id);
        auto* signal = stream != nullptr ? stream->signal() : nullptr;
        if (signal == nullptr) {
            throw std::logic_error("HTTP/3 request dispatched without its stream");
        }
        const auto& head = stream->head();
        http_request request = head.build_request(request_memory_value.resource(),
            stream->body_mode() == request_body_mode::buffered ? stream->buffered_body() : std::string_view{});
        http_response response(request_memory_value.resource());
        interim_target target{*this, stream_id};
        http_interim_response_output interim_output(
            worker_resource(), &target, [](void* raw, const http_interim_response_head& interim) -> task<void> {
                auto& output = *static_cast<interim_target*>(raw);
                // RFC 9114 §4.1: any number of 1xx heads may precede the final one.
                if (output.session_.codec().submit_interim_response_head(output.stream_id_, interim) !=
                    http3_status::accepted) {
                    throw std::invalid_argument("HTTP/3 interim response rejected");
                }
                output.session_.wake_writer();
                co_return;
            });
        const auto expectation_plan = head.expectation_plan(http_unsupported_expectation_policy::reject);
        auto admission_services = *base_services_;
        admission_services.bind_early_data(head.early_data());
        // HTTP/3 has no ORIGIN or ALTSVC frame and this server never pushes,
        // so those request lanes stay unbound.
        admission_services.bind_request_channels({.interim_output_ = &interim_output});
        admission_services.bind_request_trailers(stream->trailers());
        auto admission = admit_http_request(
            {
                .request_ = request,
                .resolution_ = stream->resolution(),
                .coding_ = http_response_coding_selection_for(request),
                .resource_ = request_memory_value.resource(),
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

        bool body_failure_handled = false;
        if (reject_early_request(head)) {
            body_failure_handled = true;
            admission.tail().disable_response_coding();
            // Replay rejection bypasses application and error handlers alike.
            response.status(ruvia::http_status::too_early);
            response.static_body("Replay-unsafe request requires a completed handshake");
        } else if (const auto& body_failure = stream->body_failure();
            body_failure.has_value() && admission.endpoint() != http_request_endpoint::rejection) {
            body_failure_handled = true;
            response = co_await respond_body_failure(request, request_memory_value, *body_failure, request_services, admission.tail());
        } else {
            switch (admission.endpoint()) {
                case http_request_endpoint::rejection:
                    admission.tail().disable_response_coding();
                    response = co_await respond_http_request_rejection(
                        routes, request, request_memory_value, *admission.rejection(), request_services);
                    break;
                case http_request_endpoint::tunnel: {
                    auto commit = [this, stream, signal, &admission, &options](
                                      context& context_value, const http_response& head_response) {
                        if (codec().submit_connect_response_head(stream->id(), head_response)) {
                            throw std::invalid_argument("HTTP/3 CONNECT response head rejected");
                        }
                        stream->mark_response_committed();
                        context_access::commit_upgrade(context_value);
                        wake_writer();
                        admission.tail().record_access(options.access_log_, head_response.status());
                        return http3_tunnel_transport(*this, *stream, *signal);
                    };
                    auto buffered = co_await execute_tunnel_upgrade_session<http3_tunnel_transport>(
                        routes, request, *resolved, request_memory_value, dispatch_services, options,
                        scanner_entry_, inbound_buffers_, admission.udp_tunnel(), std::string_view{}, commit);
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
                        response = co_await routes.handle_error(request, request_memory_value,
                            copy_http_protocol_error_info(request_memory_value.resource(), failure->protocol_error()), request_services);
                        failure->apply_required_response_headers(response);
                        break;
                    }
                    auto commit = [this, stream, signal, &request, &validation, &endpoint, &admission, &options](
                                      context& context_value, std::span<const http_header_view> headers) {
                        const auto result = codec().submit_websocket_handshake(stream->id(), request, validation,
                            {.supported_subprotocols_ = endpoint.subprotocols(), .response_headers_ = headers, .deflate_ = endpoint.deflate()});
                        const auto* handshake = std::get_if<websocket_extended_connect_negotiation>(&result);
                        if (handshake == nullptr) {
                            throw std::invalid_argument("HTTP/3 WebSocket handshake rejected");
                        }
                        stream->mark_response_committed();
                        context_access::commit_upgrade(context_value);
                        wake_writer();
                        admission.tail().record_access(options.access_log_, http_status::ok);
                        return websocket_upgrade_commit<http3_tunnel_transport>(
                            http3_tunnel_transport(*this, *stream, *signal), handshake->compression(),
                            handshake->subprotocol(), *context_value.pool());
                    };
                    auto buffered = co_await execute_websocket_upgrade_session<http3_tunnel_transport>(
                        routes, request, *resolved, request_memory_value, dispatch_services, options,
                        scanner_entry_, inbound_buffers_, std::string_view{}, commit);
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
                    http3_response_stream_sink sink_value(*this, *stream, *signal, worker_resource(),
                        request.known_method(), *options.codecs_, *admission.tail().coding_policy().selection(),
                        options.compression_ ? &*options.compression_ : nullptr, options.cors_ ? &*options.cors_ : nullptr);
                    auto result_value = co_await dispatch_response_stream_with(sink_value, routes, request, *resolved,
                        request_memory_value, dispatch_services, [stream]() noexcept { return stream->aborted(); });
                    if (result_value.peer_aborted_before_commit() != nullptr) {
                        co_return;
                    }
                    if (const auto committed_status = result_value.committed_status()) {
                        if (const auto* failed = result_value.failed_after_commit()) {
                            const bool request_body_failed = stream->body_failure().has_value();
                            reset_stream(stream_id, request_body_failed ? http3_error_code::request_cancelled
                                                                        : http3_error_code::internal_error);
                            if (!request_body_failed) {
                                options.connection_failure_.invoke(remote_address_, failed->exception());
                            }
                        } else if (stream->body_failure().has_value()) {
                            reset_stream(stream_id, http3_error_code::request_cancelled);
                        }
                        admission.tail().record_access(options.access_log_, *committed_status);
                        co_return;
                    }
                    if (auto* route_response = result_value.route_response()) {
                        response = std::move(*route_response).take_response();
                    } else if (auto* recovered = result_value.recovered_failure()) {
                        body_failure_handled = stream->body_failure().has_value();
                        response = std::move(*recovered).take_response();
                    } else {
                        throw std::logic_error("response stream dispatch returned no HTTP/3 terminal alternative");
                    }
                    break;
                }
                case http_request_endpoint::buffered:
                    if (resolved == nullptr) {
                        co_await routes.dispatch_buffered_response(request, stream->resolution(), request_memory_value,
                            options.document_root_.binding(), dispatch_services, admission.static_file_mode(), response);
                        break;
                    }
                    [[fallthrough]];
                case http_request_endpoint::stream_body:
                    body_failure_handled = co_await routes.dispatch_resolved_route(
                        request, request_memory_value, *resolved, dispatch_services, &stream->body_failure(), response);
                    break;
            }
        }

        if (const auto& body_failure = stream->body_failure(); body_failure.has_value() && !body_failure_handled) {
            body_failure_handled = true;
            response = co_await respond_body_failure(request, request_memory_value, *body_failure, request_services, admission.tail());
        }
        auto first_preparation = admission.tail().prepare(response, options, request_services.worker());
        auto preparation = first_preparation.ready() != nullptr
                               ? *first_preparation.ready()
                               : co_await admission.tail().finish_prepare(
                                     first_preparation, response, options, routes, request_memory_value, request_services);
        if (const auto& body_failure = stream->body_failure(); body_failure.has_value() && !body_failure_handled) {
            response = co_await respond_body_failure(request, request_memory_value, *body_failure, request_services, admission.tail());
            first_preparation = admission.tail().prepare(response, options, request_services.worker());
            preparation = first_preparation.ready() != nullptr
                              ? *first_preparation.ready()
                              : co_await admission.tail().finish_prepare(
                                    first_preparation, response, options, routes, request_memory_value, request_services);
        }
        if (request_services.get_stop_token().stop_requested()) {
            reset_stream(stream_id, http3_error_code::request_cancelled);
            co_return;
        }
        auto written = co_await buffered_response_writer_.write(stream_id, response, preparation.write_plan_);
        if (written.file_failure_ && !written.committed_status_) {
            // Nothing of the response was sent: the stream gets the error
            // response instead of a reset.
            options.connection_failure_.invoke(remote_address_, written.file_failure_->fault());
            admission.tail().disable_response_coding();
            response = co_await routes.handle_exception(
                request, request_memory_value, written.file_failure_->response_error(), request_services);
            first_preparation = admission.tail().prepare(response, options, request_services.worker());
            preparation = first_preparation.ready() != nullptr
                              ? *first_preparation.ready()
                              : co_await admission.tail().finish_prepare(
                                    first_preparation, response, options, routes, request_memory_value, request_services);
            written = co_await buffered_response_writer_.write(stream_id, response, preparation.write_plan_);
            if (written.file_failure_ && !written.committed_status_) {
                reset_stream(stream_id, http3_error_code::internal_error);
            }
        }
        if (written.file_failure_) {
            options.connection_failure_.invoke(remote_address_, written.file_failure_->fault());
        }
        if (written.committed_status_) {
            admission.tail().record_access(options.access_log_, *written.committed_status_);
        }
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
    asio::ip::udp::endpoint destination;
    const auto& written_remote = datagram_.path_.path.remote;
    if (written_remote.addr == nullptr || written_remote.addrlen == 0 || written_remote.addrlen > destination.capacity()) {
        throw std::logic_error("QUIC output has no valid remote path");
    }
    std::memcpy(destination.data(), written_remote.addr, written_remote.addrlen);
    destination.resize(written_remote.addrlen);
#if defined(_WIN32)
    // The acceptor sends it from the listener's socket; a datagram past its
    // bounded queue is lost, as UDP may lose any, and QUIC recovery resends.
    (void)endpoint_.send(destination, datagram_.bytes_);
    co_return;
#else
    auto error = endpoint_.send(destination, datagram_.bytes_);
    while (error == asio::error::would_block || error == asio::error::try_again) {
        // Only a full socket send buffer stalls the writer; the scanner's
        // write clock bounds it like a blocked TCP write.
        const connection_scanner::entry::write_scope write_stall_clock(scanner_entry_);
        if (co_await endpoint_.wait_writable()) {
            // The socket closed: the datagram is lost, as UDP may lose any.
            co_return;
        }
        error = endpoint_.send(destination, datagram_.bytes_);
        if (!error) {
            scanner_entry_.note_write_progress();
        }
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
        const auto status = services_.worker_.schedule_timer(
            timer_, steady_time(expiry), [this](worker_timer_outcome outcome) { timer_fired(outcome); });
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
        (void)connection_->handle_expiry(now());
    } catch (...) {
        services_.options_.connection_failure_.invoke(remote_address_, std::current_exception());
        connection_->close(error_value(http3_error_code::internal_error));
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
        const auto status = services_.worker_.schedule_timer(timer_,
            worker_timer_deadline_after(std::chrono::nanoseconds(static_cast<std::int64_t>(
                (std::min)(connection_->close_period(), static_cast<ngtcp2_duration>(INT64_MAX))))),
            [this](worker_timer_outcome outcome) { timer_fired(outcome); });
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
        connection_->close(error_value(http3_error_code::no_error));
        // Written at once: a stopping worker closes the socket before the
        // writer would run again.
        try {
            http3_quic_datagram close_packet(worker_resource());
            const auto result = connection_->next_datagram(http3_max_datagram_bytes, now(), close_packet);
            if (result.status_ == http3_quic_status::accepted && !close_packet.bytes_.empty()) {
                asio::ip::udp::endpoint destination;
                const auto& address = close_packet.path_.path.remote;
                if (address.addr != nullptr && address.addrlen != 0 && address.addrlen <= destination.capacity()) {
                    std::memcpy(destination.data(), address.addr, address.addrlen);
                    destination.resize(address.addrlen);
                    (void)endpoint_.send(destination, close_packet.bytes_);
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
    timeout_drain_ = true;
    begin_drain();
    finish_drain_if_ready();
}

void http3_session::begin_drain() noexcept {
    if (lifecycle_ != lifecycle_type::open) {
        return;
    }
    if (!connection_->http3_started()) {
        // No request can be in flight before the handshake completed.
        connection_->close(error_value(http3_error_code::no_error));
        wake_writer();
        return;
    }
    if (!drain_begun_) {
        drain_begun_ = true;
        // RFC 9114 §5.2: the shutdown notice still admits requests the client
        // already sent; the final GOAWAY follows a round trip later.
        if (codec().begin_drain() != http3_status::accepted) {
            connection_->close(error_value(http3_error_code::internal_error));
        }
        wake_writer();
    }
    arm_drain_timer();
}

void http3_session::arm_drain_timer() noexcept {
    // A final boundary needs no time bound, unless a timeout's drain must
    // still end the connection.
    if (drain_timer_.registered() || termination_.terminated() || (codec().drain_final() && !timeout_drain_)) {
        return;
    }
    const auto round_trip = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds(static_cast<std::int64_t>(
            (std::min)(connection_->close_period(), static_cast<ngtcp2_duration>(INT64_MAX)))));
    const auto bound = (std::min)(http3_drain_round_trip_bound, (std::max)(round_trip, std::chrono::milliseconds(1)));
    try {
        const auto status = services_.worker_.schedule_timer(
            drain_timer_, worker_timer_deadline_after(bound), [this](worker_timer_outcome outcome) {
                // Cancellation is a stopping worker, which closes the connection.
                if (outcome == worker_timer_outcome::expired) {
                    drain_timer_expired();
                }
            });
        if (status == worker_timer_schedule_status::worker_stopping) {
            drain_timer_expired();
        }
    } catch (...) {
        close_now();
    }
}

void http3_session::drain_timer_expired() noexcept {
    if (lifecycle_ != lifecycle_type::open || termination_.terminated()) {
        return;
    }
    if (codec().finish_drain() != http3_status::accepted) {
        connection_->close(error_value(http3_error_code::internal_error));
    }
    drain_expired_ = timeout_drain_;
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
    if ((streams_.empty() && active_handler_tasks_ == 0 && codec().drained()) || drain_expired_) {
        if (drain_expired_) {
            terminate(std::make_error_code(std::errc::timed_out));
        }
        connection_->close(error_value(http3_error_code::no_error));
        wake_writer();
    }
}

}  // namespace ruvia::detail
