#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/ssl.h>

#include "borrowed_text.h"
#include "connection_lifecycle.h"
#include "event_loop.h"
#include "http3_connection.h"
#include "http_client_pool.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "server_session.h"
#include "stop_token.h"
#include "task.h"
#include "websocket_handshake.h"
#include "worker_ref.h"
#include "worker_wait.h"
#ifdef RUVIA_ENABLE_HTTP3

namespace ruvia::detail {
namespace {
using error_code = std::error_code;
using clock_type = std::chrono::steady_clock;
void initialize_client_quic_crypto() {
    if (!try_initialize_quic_crypto()) {
        throw http_client_error(make_error_code(http_client_errc::tls_failed), "failed to initialize the QUIC TLS backend");
    }
}
ngtcp2_cid random_cid() {
    ngtcp2_cid cid{};
    cid.datalen = 18;
    if (!fill_random_bytes(std::span<unsigned char>(cid.data, cid.datalen))) {
        throw http_client_error(make_error_code(http_client_errc::tls_failed), "QUIC connection ID generation failed");
    }
    return cid;
}
int new_cid(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t size, void*) noexcept {
    if (size == 0 || size > NGTCP2_MAX_CIDLEN || !fill_random_bytes(std::span<unsigned char>(cid->data, size)) || !fill_random_bytes(std::span<unsigned char>(token->data, NGTCP2_STATELESS_RESET_TOKENLEN))) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    cid->datalen = size;
    return 0;
}
http3_quic_crypto_callbacks client_crypto_callbacks() noexcept {
    auto result = make_http3_quic_crypto_callbacks();
    result.client_initial_ = ngtcp2_crypto_client_initial_cb;
    result.recv_retry_ = ngtcp2_crypto_recv_retry_cb;
    return result;
}
void require_accepted(http3_submit_status status) {
    if (status != http3_submit_status::accepted) {
        throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP/3 stream operation rejected");
    }
}
void require_accepted(http3_finish_status status) {
    if (status != http3_finish_status::accepted) {
        throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP/3 stream operation rejected");
    }
}
[[nodiscard]] bool can_fallback_from_http3(std::exception_ptr failure, bool fallback, stop_token stop, const http_client_response_state& response, const operation_deadline& timeout, bool retired) noexcept {
    if (!fallback || retired || stop.stop_requested() || response.abandoned_ || timeout.expired()) {
        return false;
    }
    const auto code = http_client_failure_code(std::move(failure));
    return code == make_error_code(http_client_errc::io_error) || code == make_error_code(http_client_errc::connect_failed) || code == make_error_code(http_client_errc::resolve_failed) || code == make_error_code(http_client_errc::tls_failed) || code == make_error_code(http_client_errc::protocol_unavailable) || code == std::make_error_code(std::errc::timed_out);
}
struct free_ticket final {
    void operator()(SSL_SESSION* value) const noexcept {
        SSL_SESSION_free(value);
    }
};
}  // namespace

struct http3_client_tls::ticket final {
    explicit ticket(const http3_client_route& route) :
        host_(route.host_),
        port_(route.port_),
        server_name_(route.server_name_) {}
    [[nodiscard]] bool names(const http3_client_route& route) const noexcept {
        return port_ == route.port_ && host_ == route.host_ && server_name_ == route.server_name_;
    }
    std::string host_;
    std::uint16_t port_;
    std::string server_name_;
    std::array<std::uint8_t, 4096> params_{};
    std::size_t params_size_{};
    http3_peer_settings settings_{};
    bool settings_valid_{};
    std::unique_ptr<SSL_SESSION, free_ticket> session_;
    // The authenticated endpoint that issued the ticket: a later connection of
    // the route may send 0-RTT there without another address race.
    std::optional<asio::ip::udp::endpoint> endpoint_;
    std::uint64_t last_used_{};
};

http3_client_tls::http3_client_tls(http_client_pool& pool) :
    context_(asio::ssl::context::tls_client),
    tickets_(),
    capacity_((std::max)(pool.config().connection_limits_.max_idle_multiplexed_, std::size_t{1})) {
    initialize_client_quic_crypto();
    configure_client_tls_context(*context_.native_handle(), pool.config().tls_);
    if (SSL_CTX_set_min_proto_version(context_.native_handle(), TLS1_3_VERSION) != 1 || SSL_CTX_set_max_proto_version(context_.native_handle(), TLS1_3_VERSION) != 1) {
        throw http_client_error(make_error_code(http_client_errc::tls_failed), "QUIC requires TLS 1.3");
    }
    SSL_CTX_set_session_cache_mode(context_.native_handle(), SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
}

http3_client_tls::~http3_client_tls() = default;

http3_client_tls::ticket* http3_client_tls::find(const http3_client_route& route) noexcept {
    for (auto& entry : tickets_) {
        if (entry.names(route)) {
            entry.last_used_ = ++clock_;
            return &entry;
        }
    }
    return nullptr;
}

http3_client_tls::ticket& http3_client_tls::store(const http3_client_route& route) {
    if (auto* existing = find(route)) {
        return *existing;
    }
    if (tickets_.size() < capacity_) {
        auto& entry = tickets_.emplace_back(route);
        entry.last_used_ = ++clock_;
        return entry;
    }
    // Bounded like Go's LRU client session cache: the least recently used
    // route's ticket gives way.
    auto& oldest = *std::ranges::min_element(tickets_, {}, &ticket::last_used_);
    ticket replacement(route);
    oldest.host_.swap(replacement.host_);
    oldest.server_name_.swap(replacement.server_name_);
    oldest.port_ = route.port_;
    oldest.params_size_ = 0;
    oldest.settings_valid_ = false;
    oldest.session_.reset();
    oldest.endpoint_.reset();
    oldest.last_used_ = ++clock_;
    return oldest;
}

void http3_client_tls::forget_endpoints() noexcept {
    for (auto& entry : tickets_) {
        entry.endpoint_.reset();
    }
}

class http3_client_connection::session final {
public:
    struct operation_lease final {
        explicit operation_lease(session& owner) noexcept :
            owner_(owner) {
            ++owner_.borrowed_operations_;
        }
        ~operation_lease() {
            --owner_.borrowed_operations_;
            owner_.changed_.notify();
            if (owner_.race_signal_) {
                owner_.race_signal_->notify();
            }
            if (owner_.borrowed_operations_ == 0) {
                owner_.pool_.connection_retired();
            }
        }
        session& owner_;
    };
    struct stream final {
        stream(session& owner, http_client_response_state& response, const operation_deadline& timeout) :
            owner_(owner),
            response_(response),
            timeout_(timeout),
            signal_(owner.pool_.worker()) {}
        session& owner_;
        http_client_response_state& response_;
        operation_deadline timeout_;
        worker_signal signal_;
        std::int64_t id_{-1};
        std::uint64_t cancellation_id_{};
        std::optional<error_code> error_;
        std::exception_ptr failure_;
        http_client_stream_timer timer_;
        std::string_view target_;
        http_client_websocket_handshake* websocket_{};
        bool origin_{true};
        bool done_{};
        bool early_{};
        bool retry_{};
        bool retry_425_{};
        bool rejected_{};
        bool write_stopped_{};
        // Protocol submission precedes UDP completion and the producer's resumption.
        bool request_end_submitted_{};
        bool request_end_flushed_{};
        std::optional<http3_error_code> stop_sending_error_;
    };
    session(http_client_pool& pool, http3_client_tls& shared_tls, const http3_client_route& route, std::size_t index) :
        pool_(pool),
        shared_tls_(shared_tls),
        route_(route),
        index_(index),
        connect_slots_(1, pool.worker()),
        changed_(pool.worker()),
        writer_(pool.worker()),
        streams_(),
        endpoint_host_(),
        receive_buffers_{std::vector<std::byte>(), std::vector<std::byte>()},
        socket_{},
        old_socket_{},
        packet_() {
        SSL_CTX_sess_set_new_cb(shared_tls_.context_.native_handle(), save_ticket);
        streams_.reserve(pool.config().http3_.max_concurrent_streams_);
    }
    ~session() {
        quic_.reset();
        if (tls_) {
            SSL_set_app_data(tls_.get(), nullptr);
        }
    }
    static int save_ticket(SSL* tls, SSL_SESSION* ticket) noexcept {
        auto* ref = static_cast<ngtcp2_crypto_conn_ref*>(SSL_get_app_data(tls));
        if (!ref) {
            return 0;
        }
        auto& self = *static_cast<session*>(ref->user_data);
        if (!self.quic_ || SSL_get_verify_result(tls) != X509_V_OK) {
            return 0;
        }
        // Tickets resume only the route that issued them: never another
        // server name or dial target.
        http3_client_tls::ticket* entry = nullptr;
        try {
            entry = &self.shared_tls_.store(self.route_);
        } catch (...) {
            return 0;
        }
        const auto size = self.quic_->encode_early_transport_parameters(std::as_writable_bytes(std::span(entry->params_)));
        if (!size) {
            entry->session_.reset();
            entry->params_size_ = 0;
            return 0;
        }
        // SSL_free marks its current session non-resumable when QUIC closes
        // without TLS close_notify. Keep the cache independent of that SSL.
        auto* saved = SSL_SESSION_dup(ticket);
        if (saved == nullptr) {
            entry->session_.reset();
            entry->params_size_ = 0;
            return 0;
        }
        entry->session_.reset(saved);
        entry->endpoint_ = self.remote_;
        entry->params_size_ = *size;
        entry->settings_valid_ = false;
        if (const auto settings = self.quic_->http3().peer_settings()) {
            entry->settings_ = *settings;
            entry->settings_valid_ = true;
        }
        return 0;
    }
    ngtcp2_path path(asio::ip::udp::endpoint& local, asio::ip::udp::endpoint& remote) noexcept {
        return http3_quic_path(local, remote);
    }
    void capture_connection_info(http_client_response_state& response, bool reused) const {
        if (response.connection_info_.id_ != connection_id_) {
            response.connection_info_.peer_ip_ = peer_ip_;
            response.connection_info_.peer_port_ = remote_.port();
            response.connection_info_.id_ = connection_id_;
        }
        response.connection_info_.reused_ = reused;
        response.connection_info_.connect_duration_ = reused ? std::chrono::steady_clock::duration{} : connect_duration_;
        response.connection_info_.tls_duration_ = reused ? std::chrono::steady_clock::duration{} : tls_duration_;
    }
    bool usable() const noexcept {
        return open_ && quic_ && !quic_->closed() && !draining_;
    }
    void wake() noexcept {
        changed_.notify();
        writer_.notify();
        for (auto* item : streams_) {
            item->signal_.notify();
        }
    }
    void close(error_code code = make_error_code(http_client_errc::io_error), const stream* preserve = nullptr) noexcept {
        open_ = false;
        expiry_.cancel();
        idle_timer_.cancel();
        std::error_code ignored;
        if (socket_) {
            socket_->close(ignored);
        }
        if (old_socket_) {
            old_socket_->close(ignored);
        }
        for (auto* item : streams_) {
            if (item == preserve) {
                continue;
            }
            if (item->id_ >= 0 || item->response_.informational_active_) {
                item->response_.request_informational_stop();
            }
            if (code != make_error_code(http_client_errc::closing)) {
                const auto* output = item->response_.output();
                if ((item->done_ && (!output || output->ended_ || output->stopped_ || item->request_end_flushed_)) || item->retry_) {
                    continue;
                }
                if (item->id_ < 0 && connecting_stream_ != item) {
                    continue;
                }
                if (draining_ && code == make_error_code(http_client_errc::io_error) && !item->response_.head_ready_ && item->response_.output() == nullptr) {
                    item->retry_ = true;
                    continue;
                }
            }
            if (!item->error_) {
                item->error_ = code;
            }
            if (auto* output = item->response_.output(); output && item->id_ >= 0) {
                output->stop();
            }
        }
        wake();
        if (race_signal_) {
            race_signal_->notify();
        }
    }
    void begin_idle() noexcept {
        if (!open_ || !streams_.empty() || !pool_.config().idle_timeout_) {
            return;
        }
        try {
            (void)pool_.worker().schedule_timer(idle_timer_, deadline_after(pool_.config().idle_timeout_->value()), [this](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired && streams_.empty()) {
                    close();
                }
            });
        } catch (...) {
            close();
        }
    }
    void check(stream& item) {
        if (item.failure_) {
            std::rethrow_exception(item.failure_);
        }
        if (item.error_) {
            throw http_client_error(*item.error_, http_client_error_message(*item.error_));
        }
        if (item.response_.abandoned_) {
            throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP/3 response abandoned");
        }
        if (item.timeout_.expired()) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP/3 operation timed out");
        }
        if (retired_) {
            throw http_client_error(make_error_code(http_client_errc::closing), "HTTP/3 client stopped");
        }
    }
    void schedule(stream& item) {
        item.timer_.schedule_with_retry(pool_.worker(), item.timeout_, [&item]() noexcept {
            item.owner_.cancel(item, std::make_error_code(std::errc::timed_out));
        }, [&item](std::exception_ptr failure) noexcept {
            item.failure_ = std::move(failure);
            item.signal_.notify();
            item.owner_.changed_.notify();
        });
    }
    static void complete_websocket_tunnel(stream& item) noexcept {
        auto& response = item.response_;
        item.done_ = true;
        response.complete_ = true;
        response.tunnel_->output_.ended_ = true;
        response.tunnel_->output_.notify_data();
        response.tunnel_->output_.space_.notify();
        response.data_signal_.notify();
    }
    static void fail_closed_stream(stream& item, std::optional<http3_error_code> error) noexcept {
        item.error_ = error == http3_error_code::message_error ? make_error_code(http_client_errc::protocol_error) : make_error_code(http_client_errc::io_error);
        if (auto* output = item.response_.output()) {
            output->stop();
        }
    }
    void reset_transport() noexcept {
        quic_.reset();
        crypto_.reset();
        tls_.reset();
        old_socket_.reset();
        socket_.reset();
    }
    void read_progress(stream& item) {
        // An accepted tunnel is bounded by its own idle timeout, not read_timeout_.
        if (!item.websocket_ && item.response_.tunnel_ && item.response_.tunnel_->accepted_ && item.response_.tunnel_->config_.idle_timeout_ && !item.done_) {
            item.timer_.read_deadline_ = deadline_after(item.response_.tunnel_->config_.idle_timeout_->value());
            schedule(item);
            return;
        }
        if ((item.websocket_ && item.response_.head_ready_) || (item.response_.tunnel_ && item.response_.tunnel_->accepted_)) {
            item.timer_.read_deadline_.reset();
            if (!item.timer_.write_deadline_ && !item.timeout_.deadline()) {
                item.timer_.cancel();
            }
            return;
        }
        if (!item.response_.informational_active_ && item.response_.effective_read_timeout_ && !item.done_ && !item.response_.http3_data_credit_) {
            item.timer_.read_deadline_ = deadline_after(item.response_.effective_read_timeout_->value());
            schedule(item);
        }
    }
    void cancel(stream& item, error_code code) noexcept {
        item.error_ = code;
        item.response_.request_informational_stop();
        if (quic_ && item.id_ >= 0) {
            quic_->reset_stream(item.id_, http3_error_code::request_cancelled);
        }
        if (auto* output = item.response_.output()) {
            output->stop();
        }
        if (connecting_stream_ == &item) {
            close(code);
        }
        wake();
    }
    stream* find(std::int64_t id) noexcept {
        for (auto* item : streams_) {
            if (item->id_ == id) {
                return item;
            }
        }
        return nullptr;
    }
    void release(http_client_response_state& response) noexcept {
        if (!response.http3_data_credit_) {
            return;
        }
        // The token returns credit to the connection that issued it, or
        // nothing once that connection is gone.
        response.http3_data_credit_.reset();
        if (quic_ && response.connection_generation_ == generation_) {
            writer_.notify();
            if (auto* item = find(static_cast<std::int64_t>(response.stream_id_))) {
                try {
                    read_progress(*item);
                } catch (...) {
                    item->failure_ = std::current_exception();
                    item->signal_.notify();
                }
            }
        }
    }
    void submit_request_head(stream& item, const http_client_request_storage& request, borrowed_http_headers headers) {
        auto& connection = quic_->http3();
        if (request.is_tunnel()) {
            if (const auto* websocket = request.websocket()) {
                require_accepted(submit_http3_websocket_client_request(connection, item.id_, websocket->negotiation_, "https", request.tunnel_authority(), request.target(), headers, request.options_.field_source_));
            } else if (request.tunnel_protocol().empty()) {
                const http3_connect_request_head_view head{.authority_ = request.tunnel_authority(), .headers_ = headers, .field_source_ = request.options_.field_source_};
                require_accepted(connection.submit_request_head(item.id_, head));
            } else {
                const http3_extended_connect_request_head_view head{.protocol_ = request.tunnel_protocol(), .scheme_ = "https", .authority_ = request.tunnel_authority(), .target_ = request.target(), .headers_ = headers, .field_source_ = request.options_.field_source_};
                require_accepted(connection.submit_request_head(item.id_, head));
            }
            return;
        }
        auto expectation = http_client_request_expectation::none;
        if (const auto* upload = request.upload()) {
            expectation = upload->config_.expectation_;
        }
        const http3_regular_request_head_view head{
            .method_ = request.method(),
            .scheme_ = "https",
            .authority_ = std::optional<borrowed_text>(request.route().authority_),
            .target_ = request.target(),
            .headers_ = headers,
            .content_ = request.upload() != nullptr ? http_request_content_plan::streaming(request.upload()->config_.content_length_) : (request.has_body() ? http_request_content_plan::known_length(request.body().size()) : http_request_content_plan::none()),
            .expectation_ = expectation,
            .field_source_ = request.options_.field_source_
        };
        require_accepted(connection.submit_request_head(item.id_, head));
    }
    // An event of a stream nobody reads is dropped; a DATA event returns its
    // credit as it is destroyed.
    template <typename event_type>
    void consume_event(event_type& value) {
        auto* item = find(value.stream_id());
        if (!item || item->error_ || item->failure_) {
            return;
        }
        try {
            consume_stream_event(*item, value);
        } catch (...) {
            item->failure_ = std::current_exception();
            quic_->reset_stream(item->id_, http3_error_code::message_error);
        }
        item->signal_.notify();
    }
    void consume_stream_event(stream& item, const http3_informational_head_event& value) {
        if (item.retry_425_) {
            return;
        }
        item.response_.retain_informational(value.head().status(), value.head().headers(), value.request_content_signal());
        read_progress(item);
    }
    void consume_stream_event(stream& item, http3_response_head_event& value) {
        if (item.retry_425_) {
            return;
        }
        auto& response = item.response_;
        const auto received_status = value.head().received_status();
        const auto semantic_status = received_status.status();
        if (received_status.value() == 425 && item.early_) {
            item.retry_425_ = true;
            return;
        }
        const bool stream_ended = value.stream_ended();
        const auto content_signal = value.request_content_signal();
        retain_http_client_binary_response_head(response, std::move(value).take_head());
        if (!response.connection_info_.reused_) {
            response.connection_info_.tls_duration_ = tls_duration_;
        }
        if (item.websocket_) {
            accept_extended_connect_websocket_response(*item.websocket_, semantic_status, response.headers_, stream_ended, "invalid HTTP/3 WebSocket handshake response");
            item.timeout_ = operation_deadline();
        }
        const auto origin_target = item.origin_ ? std::optional(item.target_) : std::nullopt;
        publish_http_client_final_head(response, pool_.policy(), {.status_ = received_status, .protocol_version_ = http_protocol_version::http3, .request_content_signal_ = content_signal, .origin_target_ = origin_target, .opens_tunnel_ = response.tunnel_.has_value() && semantic_status.is_successful()});
        if (response.tunnel_ && response.tunnel_->accepted_) {
            const auto settings = quic_->http3().peer_settings();
            response.http3_native_datagrams_ = settings && settings->h3_datagram_;
            item.timeout_ = operation_deadline();
            item.timer_.read_deadline_.reset();
        }
        read_progress(item);
    }
    void consume_stream_event(stream& item, http3_data_event& value) {
        auto& response = item.response_;
        // Credit not retained below returns when this scope ends.
        auto credit = value.take_credit();
        const auto bytes = value.bytes();
        pool_.wire_counters().received_ += bytes.size();
        if (item.retry_425_) {
            return;
        }
        const bool tunnel = response.tunnel_ && response.tunnel_->accepted_;
        const auto& body_plan = response.response_body_plan_;
        const bool suppressed = body_plan && (body_plan->body_suppressed() || body_plan->content_semantics() != http_response_content_semantics::with_content);
        if (!tunnel && suppressed) {
            read_progress(item);
            return;
        }
        if (!response.append_received_body(bytes, pool_.config().max_response_bytes_)) {
            throw http_client_error(make_error_code(http_client_errc::response_too_large), "HTTP/3 response exceeds byte budget");
        }
        if (response.collect_all_ || !credit.valid()) {
            read_progress(item);
            return;
        }
        if (response.http3_data_credit_) {
            response.http3_data_credit_->merge(std::move(credit));
        } else {
            response.http3_data_credit_.emplace(std::move(credit));
        }
        item.timer_.read_deadline_.reset();
    }
    void consume_stream_event(stream& item, const http3_message_end_event& value) {
        auto& response = item.response_;
        if (const auto trailers = value.trailers(); !trailers.empty() && !item.retry_425_) {
            response.trailers_.clear();
            response.trailers_.reserve(trailers.size());
            for (const auto& field : trailers) {
                response.trailers_.emplace_back(field.name(), field.value());
            }
        }
        if (item.retry_425_) {
            item.retry_ = true;
            item.done_ = true;
        } else if (response.tunnel_ && response.tunnel_->accepted_) {
            response.tunnel_->receive_ended_ = true;
            if (item.websocket_ && item.write_stopped_ && item.stop_sending_error_ == http3_error_code::no_error) {
                complete_websocket_tunnel(item);
            } else if (response.tunnel_->output_.ended_ || response.tunnel_->output_.stopped_) {
                item.done_ = true;
            }
        } else {
            const bool content = response.response_body_plan_ && response.response_body_plan_->content_semantics() == http_response_content_semantics::with_content;
            finish_http_client_response_body(response, item.timeout_, content, pool_.config().max_response_bytes_);
            response.complete_ = true;
            if (response.upload_ && !response.upload_->output_.ended_ && !item.request_end_submitted_) {
                response.upload_->output_.stop();
            }
            item.done_ = true;
        }
        item.timer_.read_deadline_.reset();
        response.data_signal_.notify();
    }
    void consume_stream_event(stream& item, const http3_transport_action_event& value) {
        auto& response = item.response_;
        if (value.origin() == http3_transport_action_origin::local) {
            // The codec abandoned the stream: a malformed response
            // (H3_MESSAGE_ERROR), or the cancellation that follows an
            // unprocessed request, which already retries.
            if (!item.done_ && !item.retry_) {
                fail_closed_stream(item, value.error());
            }
        } else if (value.action() == http3_transport_action::stop_sending) {
            item.write_stopped_ = true;
            item.stop_sending_error_ = value.error();
            if (item.websocket_ && response.tunnel_ && response.tunnel_->accepted_ && response.tunnel_->receive_ended_ && value.error() == http3_error_code::no_error) {
                complete_websocket_tunnel(item);
            } else if (auto* output = response.output(); output && (!item.request_end_submitted_ || value.error() != http3_error_code::no_error)) {
                output->stop();
            }
        } else if (!item.done_ && !item.retry_) {
            fail_closed_stream(item, value.error());
        }
    }
    // The server did not process the request (RFC 9114 §4.1.1, §5.2). The
    // codec cancels the stream itself and publishes nothing more for it.
    void consume_stream_event(stream& item, const http3_request_unprocessed_event&) {
        auto& response = item.response_;
        if (response.head_ready_ || (!draining_ && response.output() != nullptr)) {
            fail_closed_stream(item, http3_error_code::request_rejected);
            return;
        }
        item.retry_ = true;
        item.rejected_ = true;
        item.error_.reset();
        if (auto* output = response.output()) {
            output->stop();
        }
    }
    void consume_stream_event(stream& item, const http3_stream_close_event& value) {
        auto& response = item.response_;
        const auto error = value.error();
        if (!error && item.request_end_submitted_) {
            // A clean QUIC stream close confirms both wire halves, even while
            // the upload driver is queued behind this receive completion.
            item.request_end_flushed_ = true;
            if (response.tunnel_ && response.tunnel_->accepted_ && response.tunnel_->receive_ended_) {
                item.done_ = true;
            }
        }
        if (error && item.request_end_submitted_ && !item.request_end_flushed_) {
            fail_closed_stream(item, error);
        } else if (item.websocket_ && response.tunnel_ && response.tunnel_->accepted_ && response.tunnel_->receive_ended_ && !error) {
            complete_websocket_tunnel(item);
        } else if (!item.done_ && !item.retry_) {
            fail_closed_stream(item, error);
        } else if (auto* output = response.output(); output && item.done_ && (!item.request_end_flushed_ || error)) {
            output->stop();
        }
    }
    void consume_stream_event(stream& item, const http3_datagram_event& value) {
        auto& response = item.response_;
        if (!response.tunnel_ || !response.tunnel_->accepted_ || !response.http3_datagrams_) {
            return;
        }
        const auto bound = pool_.config().max_response_bytes_;
        const auto available = bound - (std::min)(bound, response.http3_datagram_bytes_);
        const auto payload = value.payload();
        if (response.http3_datagrams_->size() >= 64 || payload.size() > available) {
            return;
        }
        response.http3_datagrams_->emplace_back(payload);
        response.http3_datagram_bytes_ += payload.size();
        response.data_signal_.notify();
    }
    task<void> deliver_informational(std::int64_t id, http_client_response_state* response, operation_deadline timeout) {
        struct callback_guard final {
            session& owner_;
            http_client_response_state* response_;
            ~callback_guard() {
                --owner_.borrowed_operations_;
                response_->release_reference();
                owner_.changed_.notify();
                if (owner_.race_signal_) {
                    owner_.race_signal_->notify();
                }
            }
        } guard{*this, response};
        std::exception_ptr failure;
        try {
            co_await response->dispatch_informational(timeout);
        } catch (...) {
            failure = std::current_exception();
        }
        response->informational_active_ = false;
        auto* item = find(id);
        if (failure) {
            response->failure_ = failure;
            if (item && &item->response_ == response) {
                item->failure_ = failure;
                cancel(*item, std::make_error_code(std::errc::operation_canceled));
            }
        } else if (item && &item->response_ == response) {
            try {
                read_progress(*item);
            } catch (...) {
                response->failure_ = std::current_exception();
                item->failure_ = response->failure_;
                cancel(*item, std::make_error_code(std::errc::operation_canceled));
            }
        }
        response->head_signal_.notify();
        response->data_signal_.notify();
        if (item && &item->response_ == response) {
            item->signal_.notify();
        }
    }
    void consume_events() {
        if (early_started_ && !early_rejected_ && quic_->early_data_rejected()) {
            early_rejected_ = true;
            if (quic_->reject_early_data() != http3_quic_status::accepted) {
                throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP/3 early-data reset failed");
            }
            for (auto* item : streams_) {
                if (item->early_) {
                    item->retry_ = true;
                    item->signal_.notify();
                }
            }
        }
        while (auto event = quic_->next_event()) {
            if (auto* head = event->informational_head()) {
                consume_event(*head);
                auto* item = find(head->stream_id());
                if (item && !item->error_ && !item->failure_ && item->response_.informational_callback_ && !item->response_.informational_active_) {
                    auto& response = item->response_;
                    response.informational_active_ = true;
                    item->timer_.read_deadline_.reset();
                    schedule(*item);
                    response.retain_reference();
                    ++borrowed_operations_;
                    try {
                        pool_.tasks().spawn(deliver_informational(item->id_, &response, item->timeout_));
                    } catch (...) {
                        --borrowed_operations_;
                        response.release_reference();
                        response.informational_active_ = false;
                        throw;
                    }
                }
            } else if (auto* final_head = event->response_head()) {
                consume_event(*final_head);
            } else if (auto* data = event->data()) {
                consume_event(*data);
            } else if (const auto* end = event->message_end()) {
                consume_event(*end);
            } else if (const auto* action = event->transport_action()) {
                consume_event(*action);
            } else if (const auto* closed = event->stream_close()) {
                consume_event(*closed);
            } else if (const auto* unprocessed = event->request_unprocessed()) {
                consume_event(*unprocessed);
            } else if (const auto* datagram = event->datagram()) {
                consume_event(*datagram);
            } else if (event->goaway()) {
                // Streams the server will not process arrive as
                // http3_request_unprocessed_event.
                draining_ = true;
            } else if (auto* origins = event->origin_advertisement()) {
                pool_.advertisements().retain(index_, http_protocol_version::http3, *origins);
            }
        }
        if (auto* entry = shared_tls_.find(route_); entry != nullptr && entry->session_) {
            if (const auto settings = quic_->http3().peer_settings()) {
                entry->settings_ = *settings;
                entry->settings_valid_ = true;
            }
        }
        changed_.notify();
    }
    void check_quic(http3_quic_status status) {
        if (status != http3_quic_status::accepted && status != http3_quic_status::would_block) {
            throw http_client_error(tls_ && SSL_get_verify_result(tls_.get()) != X509_V_OK ? make_error_code(http_client_errc::tls_failed) : make_error_code(http_client_errc::io_error), "QUIC connection failed");
        }
    }
    void step_quic(http3_quic_status status) {
        consume_events();
        if (status == http3_quic_status::transport_error || status == http3_quic_status::application_error) {
            send_close_packet();
        }
        check_quic(status);
    }
    void send_close_packet() noexcept {
        if (!quic_ || !socket_ || !socket_->is_open()) {
            return;
        }
        try {
            const auto close_result = quic_->next_datagram(pool_.config().http3_.max_datagram_bytes_, http3_quic_now(), packet_);
            if (close_result.status_ == http3_quic_status::accepted && !packet_.bytes_.empty()) {
                if (const auto remote = http3_udp_endpoint(packet_.path_.path.remote)) {
                    std::error_code error;
                    (void)socket_->send_to(asio::buffer(packet_.bytes_), *remote, 0, error);
                    if (error) {
                        (void)socket_->send(asio::buffer(packet_.bytes_), 0, error);
                    }
                }
            }
        } catch (...) {
        }
    }
    task<void> receive(asio::ip::udp::socket& socket, asio::ip::udp::endpoint local, std::vector<std::byte>& bytes) {
        try {
            while (open_ && socket.is_open()) {
                asio::ip::udp::endpoint remote;
                const auto result = co_await async_asio<std::size_t>([&](auto handler) {
                    socket.async_receive_from(asio::buffer(bytes), remote, std::move(handler));
                });
                if (!open_ || !socket.is_open()) {
                    break;
                }
                if (result.error_code()) {
                    if (http3_ignorable_receive_error(result.error_code())) {
                        continue;
                    }
                    throw std::system_error(result.error_code());
                }
                const auto received = quic_->receive_datagram(path(local, remote), 0, std::span(bytes.data(), result.result()), http3_quic_now());
                step_quic(received.status_);
                writer_.notify();
                for (auto* item : streams_) {
                    item->signal_.notify();
                }
            }
        } catch (...) {
            if (open_) {
                failure_ = std::current_exception();
                close();
            }
        }
        --io_tasks_;
        changed_.notify();
        if (race_signal_) {
            race_signal_->notify();
        }
        if (io_tasks_ == 0) {
            pool_.connection_retired();
        }
    }
    void arm_expiry() {
        expiry_.cancel();
        const auto time = quic_->expiry();
        if (time == UINT64_MAX) {
            return;
        }
        (void)pool_.worker().schedule_timer(expiry_, http3_quic_steady_time(time), [this](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                writer_.notify();
            }
        });
    }
    task<void> write_packets() {
        try {
            while (open_) {
                if (quic_->expiry() <= http3_quic_now()) {
                    const auto expired = quic_->handle_expiry(http3_quic_now());
                    step_quic(expired.status_);
                }
                std::size_t burst = 0;
                for (; open_ && burst != 32; ++burst) {
                    const auto result = quic_->next_datagram(pool_.config().http3_.max_datagram_bytes_, http3_quic_now(), packet_);
                    packet_pending_ = result.status_ == http3_quic_status::accepted;
                    step_quic(result.status_);
                    if (result.status_ == http3_quic_status::would_block) {
                        break;
                    }
                    const auto remote = http3_udp_endpoint(packet_.path_.path.remote);
                    if (!remote) {
                        throw http_client_error(make_error_code(http_client_errc::protocol_error), "invalid QUIC output path");
                    }
                    auto* output = socket_.get();
                    if (packet_.path_.path.local.addrlen == old_local_.size() && std::memcmp(packet_.path_.path.local.addr, old_local_.data(), old_local_.size()) == 0) {
                        if (!old_socket_ || !old_socket_->is_open()) {
                            packet_pending_ = false;
                            continue;
                        }
                        output = old_socket_.get();
                    }
                    sending_socket_ = output;
                    const auto sent = co_await async_asio<std::size_t>([&](auto handler) {
                        output->async_send_to(asio::buffer(packet_.bytes_), *remote, std::move(handler));
                    });
                    sending_socket_ = nullptr;
                    packet_pending_ = false;
                    changed_.notify();
                    if (!open_) {
                        break;
                    }
                    if (sent.error_code()) {
                        if (output != socket_.get()) {
                            continue;
                        }
                        throw std::system_error(sent.error_code());
                    }
                    for (auto* item : streams_) {
                        item->signal_.notify();
                    }
                }
                if (!open_) {
                    break;
                }
                arm_expiry();
                if (burst == 32) {
                    co_await worker_turn{pool_.worker()};
                } else {
                    co_await writer_.wait();
                }
            }
        } catch (...) {
            sending_socket_ = nullptr;
            packet_pending_ = false;
            if (open_) {
                failure_ = std::current_exception();
                close();
            }
        }
        --io_tasks_;
        changed_.notify();
        if (race_signal_) {
            race_signal_->notify();
        }
        if (io_tasks_ == 0) {
            pool_.connection_retired();
        }
    }
    task<void> connect(stream& item, std::string_view host, bool allow_early, const operation_deadline& acquire_timeout, stop_token stop) {
        const auto slot = co_await connect_slots_.acquire({.timeout_ = acquire_timeout.timeout(), .stop_token_ = std::move(stop)});
        if (!slot.has_value()) {
            throw_http_client_wait_failure(slot.status(), "HTTP/3 connection admission");
        }
        struct lease {
            pool_lease_scheduler& slots_;
            std::size_t slot_;
            ~lease() {
                slots_.release(slot_);
            }
        } held{connect_slots_, slot.value()};
        check(item);
        if (usable()) {
            co_return;
        }
        const auto has_active_streams = [this, &item]() noexcept {
            for (const auto* existing : streams_) {
                if (existing != &item && existing->id_ >= 0 && !existing->done_ && !existing->retry_) {
                    return true;
                }
            }
            return false;
        };
        if (has_active_streams()) {
            worker_timer_registration drain_timer;
            bool drain_expired = false;
            if (const auto deadline = acquire_timeout.deadline()) {
                (void)pool_.worker().schedule_timer(drain_timer, *deadline, [this, &drain_expired](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        drain_expired = true;
                        changed_.notify();
                    }
                });
            }
            while (open_ && has_active_streams()) {
                check(item);
                if (stop.stop_requested()) {
                    throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP/3 connection drain wait cancelled");
                }
                if (drain_expired || acquire_timeout.expired()) {
                    throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP/3 connection drain wait timed out");
                }
                co_await changed_.wait();
            }
        }
        struct connect_attempt final {
            session& owner_;
            bool completed_{false};
            ~connect_attempt() {
                if (!completed_) {
                    owner_.close();
                }
            }
        } attempt{*this};
        close(make_error_code(http_client_errc::io_error), &item);
        while (io_tasks_) {
            check(item);
            co_await changed_.wait();
        }
        check(item);
        reset_transport();
        failure_ = nullptr;
        draining_ = false;
        early_started_ = false;
        early_rejected_ = false;
        connecting_stream_ = &item;
        struct connecting_guard {
            stream*& value_;
            ~connecting_guard() {
                value_ = nullptr;
            }
        } connecting{connecting_stream_};
        item.error_.reset();
        worker_timer_registration timer;
        const auto bound = item.timeout_.constrained_by(acquire_timeout.timeout()).constrained_by(item.response_.effective_connect_timeout_);
        if (const auto deadline = bound.deadline()) {
            (void)pool_.worker().schedule_timer(timer, *deadline, [this](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    close(std::make_error_code(std::errc::timed_out));
                }
            });
        }
        endpoint_host_.assign(host);
        const auto connect_started = std::chrono::steady_clock::now();
        if (!connect_endpoint_) {
            throw std::logic_error("HTTP/3 connect requires a selected resolver endpoint");
        }
        const auto resolved_peer_ip = to_ip_address(connect_endpoint_->address()).normalized_ipv4();
        if (pool_.config().address_validator_ && !(*pool_.config().address_validator_)(resolved_peer_ip, connect_endpoint_->port())) {
            throw http_client_error(make_error_code(http_client_errc::resolve_failed), "HTTP/3 dial address rejected");
        }
        remote_ = *connect_endpoint_;
        socket_ = std::make_unique<asio::ip::udp::socket>(pool_.io_context());
        socket_->open(remote_.protocol());
        socket_->connect(remote_);
        peer_ip_ = resolved_peer_ip;
        local_ = socket_->local_endpoint();
        connect_duration_ = std::chrono::steady_clock::now() - connect_started;
        tls_started_at_ = std::chrono::steady_clock::now();
        tls_duration_ = {};
        tls_.reset(SSL_new(shared_tls_.context_.native_handle()));
        if (!tls_ || prepare_client_tls_stream(*tls_, route_.server_name_, {}) != client_tls_setup_error::none || ngtcp2_crypto_ossl_configure_client_session(tls_.get()) != 0) {
            throw http_client_error(make_error_code(http_client_errc::tls_failed), "HTTP/3 TLS setup failed");
        }
        static constexpr std::array<unsigned char, 3> alpn{2, 'h', '3'};
        if (SSL_set_alpn_protos(tls_.get(), alpn.data(), static_cast<unsigned int>(alpn.size())) != 0) {
            throw http_client_error(make_error_code(http_client_errc::tls_failed), "HTTP/3 ALPN setup failed");
        }
        reference_.get_conn = [](ngtcp2_crypto_conn_ref* ref) {
            return static_cast<session*>(ref->user_data)->quic_->native_handle();
        };
        reference_.user_data = this;
        SSL_set_app_data(tls_.get(), &reference_);
        SSL_set_connect_state(tls_.get());
        auto* const ticket = shared_tls_.find(route_);
        if (ticket != nullptr && ticket->session_) {
            // The next SSL must not mutate the cached copy on its teardown.
            std::unique_ptr<SSL_SESSION, free_ticket> restored(SSL_SESSION_dup(ticket->session_.get()));
            if (!restored || SSL_set_session(tls_.get(), restored.get()) != 1) {
                ticket->session_.reset();
            }
        }
        ngtcp2_crypto_ossl_ctx* crypto = nullptr;
        if (ngtcp2_crypto_ossl_ctx_new(&crypto, tls_.get()) != 0) {
            throw http_client_error(make_error_code(http_client_errc::tls_failed), "HTTP/3 crypto setup failed");
        }
        crypto_.reset(crypto);
        const auto& config = pool_.config();
        http3_quic_owner_callbacks owner_callbacks{.get_new_connection_id_ = new_cid};
        owner_callbacks.handshake_completed_ = [](ngtcp2_conn*, void* target) noexcept {
            auto& owner = *static_cast<session*>(target);
            owner.tls_duration_ = std::chrono::steady_clock::now() - owner.tls_started_at_;
            if (tls_selected_alpn(*owner.tls_) != "h3") {
                return NGTCP2_ERR_CALLBACK_FAILURE;
            }
            if (owner.early_started_ && SSL_get_early_data_status(owner.tls_.get()) != SSL_EARLY_DATA_ACCEPTED) {
                try {
                    return owner.quic_->reject_early_data() == http3_quic_status::accepted ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
                } catch (...) {
                    owner.failure_ = std::current_exception();
                    return NGTCP2_ERR_CALLBACK_FAILURE;
                }
            }
            return 0;
        };
        handshake_confirmed_ = false;
        owner_callbacks.handshake_confirmed_ = [](ngtcp2_conn*, void* target) noexcept {
            auto& owner = *static_cast<session*>(target);
            owner.handshake_confirmed_ = true;
            owner.changed_.notify();
            return 0;
        };
        const auto receive_window = (std::max)(std::uint64_t{64 * 1024}, (std::min)(std::uint64_t{16 * 1024 * 1024}, static_cast<std::uint64_t>(config.max_response_bytes_)));
        const http3_quic_connection_options options{.crypto_callbacks_ = client_crypto_callbacks(), .owner_callbacks_ = owner_callbacks, .source_connection_id_ = random_cid(), .destination_connection_id_ = random_cid(), .initial_path_ = path(local_, remote_), .initial_timestamp_ = http3_quic_now(), .handshake_timeout_ = bound.remaining() ? http3_quic_duration(timeout_duration(*bound.remaining())) : UINT64_MAX, .idle_timeout_ = 0, .initial_max_data_ = receive_window, .initial_max_stream_data_ = 64 * 1024, .max_udp_payload_size_ = config.http3_.max_datagram_bytes_, .callback_context_ = this, .http3_ = {.max_concurrent_streams_ = config.http3_.max_concurrent_streams_, .max_stream_output_bytes_ = config.http3_.max_stream_output_bytes_, .enable_h3_datagram_ = true}};
        quic_.emplace(http3_quic_connection::client(options));
        quic_->set_tls_native_handle(crypto_.get());
        if (allow_early && config.http3_.enable_early_data_ && ticket != nullptr && ticket->session_ && ticket->params_size_ && ticket->settings_valid_ && SSL_SESSION_get_max_early_data(ticket->session_.get()) != 0) {
            if (SSL_set_quic_tls_early_data_enabled(tls_.get(), 1) != 1) {
                throw http_client_error(make_error_code(http_client_errc::tls_failed), "cannot enable QUIC early data");
            }
            early_started_ = quic_->start_early_http3(std::as_bytes(std::span(ticket->params_.data(), ticket->params_size_)), ticket->settings_) == http3_quic_status::accepted;
        }
        next_nonzero_id(generation_);
        connection_id_ = pool_.next_connection_id();
        requests_ = 0;
        open_ = true;
        active_receive_buffer_ = 0;
        receive_buffers_[0].resize(http3_max_udp_payload_bytes);
        ++io_tasks_;
        try {
            pool_.tasks().spawn(receive(*socket_, local_, receive_buffers_[0]));
        } catch (...) {
            --io_tasks_;
            throw;
        }
        ++io_tasks_;
        try {
            pool_.tasks().spawn(write_packets());
        } catch (...) {
            --io_tasks_;
            close();
            throw;
        }
        while (!quic_->handshake_completed() && !early_started_) {
            check(item);
            if (failure_) {
                std::rethrow_exception(failure_);
            }
            co_await changed_.wait();
        }
        if (quic_->handshake_completed() && tls_selected_alpn(*tls_) != "h3") {
            throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "peer did not negotiate HTTP/3");
        }
        if (!early_started_) {
            while (!quic_->http3_started()) {
                check(item);
                check_quic(quic_->start_http3());
                if (!quic_->http3_started()) {
                    co_await changed_.wait();
                }
            }
        }
        writer_.notify();
        attempt.completed_ = true;
    }
    task<void> flush_stream(stream& item) {
        item.timer_.write_deadline_ = item.response_.effective_write_timeout_ ? std::optional(deadline_after(item.response_.effective_write_timeout_->value())) : std::nullopt;
        schedule(item);
        writer_.notify();
        while ((!item.request_end_flushed_ && !quic_->http3().output_flushed(item.id_)) || packet_pending_) {
            check(item);
            if (item.retry_ || (item.write_stopped_ && (!item.request_end_submitted_ || item.stop_sending_error_ != http3_error_code::no_error))) {
                break;
            }
            co_await changed_.wait();
        }
        check(item);
        if (item.request_end_submitted_ && !item.retry_ && quic_->http3().output_flushed(item.id_) && !packet_pending_) {
            item.request_end_flushed_ = true;
        }
        item.timer_.write_deadline_.reset();
        schedule(item);
    }
    // Submits `bytes` as request DATA, then waits for the peer to acknowledge
    // them. True once the codec accepted every byte: like a write handed to
    // the HTTP/1 socket or flushed to the HTTP/2 connection, that completes
    // the write even when a later STOP_SENDING ends the wait for the
    // acknowledgement, because the peer may stop the request only after
    // processing the bytes (RFC 9114 §4.1.2). False when the stream stopped
    // first; the rest of `bytes` was never sent.
    task<bool> send_body(stream& item, std::string_view bytes) {
        const auto stopped = [&item]() noexcept {
            return item.retry_ || item.write_stopped_ || (item.response_.output() && item.response_.output()->stopped_);
        };
        item.timer_.write_deadline_ = item.response_.effective_write_timeout_ ? std::optional(deadline_after(item.response_.effective_write_timeout_->value())) : std::nullopt;
        schedule(item);
        while (!bytes.empty()) {
            check(item);
            if (stopped()) {
                item.timer_.write_deadline_.reset();
                co_return false;
            }
            const auto sent = quic_->http3().submit_data(item.id_, std::as_bytes(std::span(bytes.data(), bytes.size())));
            if (sent.status_ == http3_data_submit_status::closed || sent.status_ == http3_data_submit_status::invalid_message) {
                throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP/3 stream operation rejected");
            }
            // backpressured keeps the unconsumed suffix until acknowledgements
            // free stream output budget.
            bytes.remove_prefix(sent.consumed_bytes_);
            pool_.wire_counters().sent_ += sent.consumed_bytes_;
            if (sent.consumed_bytes_ != 0 && item.response_.effective_write_timeout_) {
                item.timer_.write_deadline_ = deadline_after(item.response_.effective_write_timeout_->value());
                schedule(item);
            }
            // Until the final head, and through an accepted tunnel, sending
            // progress also restarts read inactivity.
            if (sent.consumed_bytes_ != 0 && item.timer_.read_deadline_ && (!item.response_.head_ready_ || (item.response_.tunnel_ && item.response_.tunnel_->accepted_))) {
                read_progress(item);
            }
            writer_.notify();
            if (!bytes.empty()) {
                co_await item.signal_.wait();
            }
        }
        auto outstanding = quic_->http3().pending_output_bytes(item.id_);
        while (outstanding != 0 && !stopped()) {
            check(item);
            co_await item.signal_.wait();
            const auto remaining = quic_->http3().pending_output_bytes(item.id_);
            if (remaining < outstanding && item.response_.effective_write_timeout_) {
                item.timer_.write_deadline_ = deadline_after(item.response_.effective_write_timeout_->value());
                schedule(item);
            }
            outstanding = remaining;
        }
        item.timer_.write_deadline_.reset();
        co_return true;
    }

    http_client_pool& pool_;
    http3_client_tls& shared_tls_;
    const http3_client_route& route_;
    std::size_t index_;
    pool_lease_scheduler connect_slots_;
    worker_signal changed_;
    worker_signal writer_;
    std::vector<stream*> streams_;
    std::string endpoint_host_;
    std::array<std::vector<std::byte>, 2> receive_buffers_;
    std::size_t active_receive_buffer_{0};
    std::unique_ptr<SSL, free_tls> tls_;
    std::unique_ptr<ngtcp2_crypto_ossl_ctx, free_crypto> crypto_;
    ngtcp2_crypto_conn_ref reference_{};
    std::optional<http3_quic_connection> quic_;
    std::unique_ptr<asio::ip::udp::socket> socket_;
    std::unique_ptr<asio::ip::udp::socket> old_socket_;
    asio::ip::udp::socket* sending_socket_{};
    asio::ip::udp::endpoint local_, old_local_, remote_;
    std::optional<asio::ip::udp::endpoint> connect_endpoint_;
    worker_signal* race_signal_{};
    http3_quic_datagram packet_;
    std::exception_ptr failure_;
    std::uint64_t generation_{};
    std::uint64_t connection_id_{};
    ip_address peer_ip_{};
    std::size_t requests_{};
    std::chrono::steady_clock::time_point tls_started_at_;
    std::chrono::steady_clock::duration connect_duration_{}, tls_duration_{};
    bool packet_pending_{false};
    std::size_t io_tasks_{};
    std::size_t borrowed_operations_{};
    stream* connecting_stream_{};
    bool open_{}, retired_{}, draining_{}, early_started_{}, early_rejected_{}, handshake_confirmed_{}, migrating_{};
    std::uint64_t migration_cancellation_id_{};
    bool migration_cancelled_{};
    worker_timer_registration expiry_;
    worker_timer_registration idle_timer_;
};

class http3_client_connection::address_race final {
public:
    struct candidate final {
        std::unique_ptr<session> connection_{};
        bool running_{false};
    };
    address_race(http_client_pool& pool, http3_client_tls& shared_tls, const http3_client_route& route, std::size_t index) :
        pool_(pool),
        shared_tls_(shared_tls),
        route_(route),
        index_(index),
        resolver_(pool.worker()),
        selection_(1, pool.worker()),
        changed_(pool.worker()) {}

    void cancel(error_code reason) noexcept {
        if (!reason_) {
            reason_ = reason;
        }
        resolver_.cancel();
        delay_.cancel();
        if (retained_) {
            retained_->close(reason);
        }
        for (auto& entry : candidates_) {
            if (entry.connection_) {
                entry.connection_->close(reason);
            }
        }
        changed_.notify();
    }
    [[nodiscard]] bool pending(std::optional<std::size_t> keep = std::nullopt) const noexcept {
        for (std::size_t index = 0; index != candidates_.size(); ++index) {
            const auto& entry = candidates_[index];
            if (entry.running_ || (entry.connection_ && index != keep && entry.connection_->io_tasks_ != 0)) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] task<void> run_candidate(std::size_t index, std::string_view host, operation_deadline bound, http_client_response_state& response) {
        auto& entry = candidates_[index];
        auto& connection = *entry.connection_;
        session::stream item(connection, response, bound);
        bool registered = false;
        try {
            connection.streams_.push_back(&item);
            registered = true;
            co_await connection.connect(item, host, false, bound, {});
            if (!reason_ && !winner_ && !response.abandoned_) {
                winner_ = index;
            }
        } catch (const std::bad_alloc&) {
            failure_ = std::current_exception();
            cancel(make_error_code(http_client_errc::io_error));
        } catch (...) {
            last_failure_ = std::current_exception();
            // A refused/unreachable endpoint advances immediately rather than
            // paying the remainder of the RFC 8305 launch delay.
            delay_.cancel();
            launch_ready_ = true;
        }
        if (registered) {
            std::erase(connection.streams_, &item);
        }
        entry.running_ = false;
        changed_.notify();
    }
    void arm_next() {
        delay_.cancel();
        (void)pool_.worker().schedule_timer(delay_, deadline_after(client_connection_attempt_delay), [this](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                launch_ready_ = true;
                changed_.notify();
            }
        });
    }
    http_client_pool& pool_;
    http3_client_tls& shared_tls_;
    const http3_client_route& route_;
    std::size_t index_;
    client_host_resolver resolver_;
    pool_lease_scheduler selection_;
    worker_signal changed_;
    // Only two QUIC/TLS sessions are live, independently of DNS answer count.
    std::array<candidate, 2> candidates_;
    session* retained_{};
    std::optional<std::size_t> winner_;
    std::optional<error_code> reason_;
    std::exception_ptr failure_;
    std::exception_ptr last_failure_;
    std::uint64_t cancellation_id_{};
    bool launch_ready_{false};
    bool selecting_{false};
    bool retired_{false};
    worker_timer_registration delay_;
};

task<void> http3_client_connection::select_endpoint(bool allow_early, const operation_deadline& timeout, const operation_deadline& acquire_timeout, stop_token stop, http_client_response_state& response) {
    auto& race = *race_;
    const std::string_view host = route_.host_;
    const auto port = route_.port_;
    const auto acquired = co_await race.selection_.acquire({.timeout_ = acquire_timeout.timeout(), .stop_token_ = stop});
    if (!acquired.has_value()) {
        throw_http_client_wait_failure(acquired.status(), "HTTP/3 endpoint selection");
    }
    struct selection_lease final {
        address_race& race_;
        std::size_t slot_;
        ~selection_lease() {
            race_.selecting_ = false;
            race_.selection_.release(slot_);
        }
    } lease{race, acquired.value()};
    if (race.retired_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP/3 client is retired");
    }
    if (response.abandoned_ || stop.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP/3 endpoint selection cancelled");
    }
    if (session_ && session_->usable()) {
        co_return;
    }
    // A previously authenticated endpoint of this route can send a safe
    // request in 0-RTT, also after its earlier owner was reclaimed. An
    // initial address race never sends request bytes on multiple paths.
    if (session_ && session_->streams_.empty() && allow_early && race.pool_.config().http3_.enable_early_data_) {
        if (const auto* ticket = tls_.find(route_); ticket != nullptr && ticket->session_ && ticket->endpoint_) {
            session_->connect_endpoint_ = *ticket->endpoint_;
            co_return;
        }
    }
    const auto selection_started = std::chrono::steady_clock::now();
    race.selecting_ = true;
    race.reason_.reset();
    race.failure_ = nullptr;
    race.last_failure_ = nullptr;
    race.winner_.reset();
    race.launch_ready_ = true;
    http_client_response_cancellation cancellation(race.pool_.cancellation_target(), race.cancellation_id_, response);
    cancellation.arm(stop);
    const auto bound = timeout.constrained_by(acquire_timeout.timeout()).constrained_by(response.effective_connect_timeout_);
    worker_timer_registration deadline_timer;
    if (const auto deadline = bound.deadline()) {
        (void)race.pool_.worker().schedule_timer(deadline_timer, *deadline, [&race](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                race.cancel(std::make_error_code(std::errc::timed_out));
            }
        });
    }
    std::unique_ptr<session> retained;
    std::exception_ptr failure;
    try {
        if (session_) {
            session_->race_signal_ = &race.changed_;
            while (session_->open_ && !session_->streams_.empty() && !race.reason_ && !response.abandoned_) {
                co_await race.changed_.wait();
            }
            if (race.reason_ || response.abandoned_ || stop.stop_requested()) {
                session_->race_signal_ = nullptr;
                throw http_client_error(race.retired_ ? make_error_code(http_client_errc::closing) : race.reason_.value_or(std::make_error_code(std::errc::operation_canceled)), "HTTP/3 endpoint selection stopped");
            }
            race.retained_ = session_.get();
            session_->close();
            while (session_->io_tasks_ != 0 || session_->borrowed_operations_ != 0) {
                co_await race.changed_.wait();
            }
            session_->race_signal_ = nullptr;
            session_->reset_transport();
            retained = std::move(session_);
        }
        if (race.reason_) {
            throw http_client_error(*race.reason_, "HTTP/3 endpoint selection stopped");
        }
        auto resolved = co_await race.pool_.resolve_addresses(host, port, race.resolver_, bound);
        if (race.reason_) {
            throw http_client_error(*race.reason_, "HTTP/3 resolution stopped");
        }
        if (resolved.error_ || (!resolved.literal_ && resolved.addresses_.empty())) {
            throw http_client_error(make_error_code(http_client_errc::resolve_failed), "HTTP/3 DNS resolution failed");
        }
        std::optional<client_address_sequence<asio::ip::tcp::resolver::results_type>> sequence;
        std::size_t remaining = 0;
        std::optional<asio::ip::udp::endpoint> next;
        if (resolved.literal_) {
            remaining = 1;
            next.emplace(resolved.literal_->address(), resolved.literal_->port());
        } else {
            remaining = resolved.addresses_.size();
            sequence.emplace(resolved.addresses_);
            if (const auto first = sequence->next()) {
                next.emplace(first->address(), first->port());
            }
        }
        while (!race.winner_ && !race.reason_ && !race.failure_) {
            if (response.abandoned_) {
                race.cancel(std::make_error_code(std::errc::operation_canceled));
                break;
            }
            if (race.launch_ready_ && next) {
                for (std::size_t index = 0; index != race.candidates_.size(); ++index) {
                    auto& entry = race.candidates_[index];
                    if (entry.running_ || (entry.connection_ && entry.connection_->io_tasks_ != 0)) {
                        continue;
                    }
                    if (entry.connection_) {
                        entry.connection_->race_signal_ = nullptr;
                        entry.connection_.reset();
                    }
                    if (retained && (!retained->connect_endpoint_ || *retained->connect_endpoint_ == *next)) {
                        race.retained_ = nullptr;
                        entry.connection_ = std::move(retained);
                    } else {
                        entry.connection_ = std::make_unique<session>(race.pool_, race.shared_tls_, race.route_, race.index_);
                    }
                    entry.connection_->connect_endpoint_ = *next;
                    entry.connection_->race_signal_ = &race.changed_;
                    // A stagger is not a handshake timeout. Divide the fixed
                    // remaining deadline across remaining two-candidate waves;
                    // the last wave gets all remaining time.
                    if (!sequence || !sequence->has_next()) {
                        remaining = 1;
                    }
                    std::size_t live = 0;
                    for (const auto& other : race.candidates_) {
                        live += other.running_;
                    }
                    const auto waves = (remaining + live + 1) / 2;
                    auto candidate_bound = bound;
                    if (const auto left = bound.remaining()) {
                        const auto divisor = static_cast<std::int64_t>((std::min)(waves, static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())));
                        const auto slice = (std::max)(client_connection_attempt_delay, *left / (std::max)(divisor, std::int64_t{1}));
                        candidate_bound = bound.constrained_by(slice);
                    }
                    entry.running_ = true;
                    try {
                        race.pool_.tasks().spawn(race.run_candidate(index, host, candidate_bound, response));
                    } catch (...) {
                        entry.running_ = false;
                        entry.connection_->close();
                        throw;
                    }
                    --remaining;
                    next.reset();
                    if (sequence) {
                        if (const auto following = sequence->next()) {
                            next.emplace(following->address(), following->port());
                        }
                    }
                    if (next) {
                        race.arm_next();
                    }
                    break;
                }
            }
            if (race.winner_ || race.reason_ || race.failure_) {
                break;
            }
            if (!next && !race.pending()) {
                break;
            }
            co_await race.changed_.wait();
        }
        if (race.failure_) {
            std::rethrow_exception(race.failure_);
        }
        if (race.reason_) {
            throw http_client_error(*race.reason_, "HTTP/3 endpoint selection stopped");
        }
        if (!race.winner_) {
            if (race.last_failure_) {
                std::rethrow_exception(race.last_failure_);
            }
            throw http_client_error(make_error_code(http_client_errc::connect_failed), "no HTTP/3 endpoint completed its handshake");
        }
    } catch (...) {
        failure = std::current_exception();
        race.winner_.reset();
    }
    race.delay_.cancel();
    deadline_timer.cancel();
    for (std::size_t index = 0; index != race.candidates_.size(); ++index) {
        if (index != race.winner_ && race.candidates_[index].connection_) {
            race.candidates_[index].connection_->close();
        }
    }
    for (;;) {
        if (race.reason_ || race.retired_ || response.abandoned_ || stop.stop_requested()) {
            race.winner_.reset();
            for (auto& entry : race.candidates_) {
                if (entry.connection_) {
                    entry.connection_->close();
                }
            }
        }
        if (!race.pending(race.winner_)) {
            break;
        }
        co_await race.changed_.wait();
    }
    race.retained_ = nullptr;
    for (std::size_t index = 0; index != race.candidates_.size(); ++index) {
        auto& entry = race.candidates_[index];
        if (entry.connection_) {
            entry.connection_->race_signal_ = nullptr;
        }
        if (index == race.winner_) {
            session_ = std::move(entry.connection_);
            session_->connect_duration_ = (std::max)(std::chrono::steady_clock::duration{}, std::chrono::steady_clock::now() - selection_started - session_->tls_duration_);
        } else {
            entry.connection_.reset();
        }
    }
    race.winner_.reset();
    if (race.failure_) {
        if (!session_) {
            session_ = std::move(retained);
        }
        std::rethrow_exception(race.failure_);
    }
    if (race.reason_ || race.retired_ || response.abandoned_ || stop.stop_requested()) {
        if (!session_) {
            session_ = std::move(retained);
        }
        throw http_client_error(race.retired_ ? make_error_code(http_client_errc::closing) : race.reason_.value_or(std::make_error_code(std::errc::operation_canceled)), "HTTP/3 endpoint selection stopped");
    }
    if (failure) {
        if (!session_) {
            session_ = std::move(retained);
        }
        try {
            std::rethrow_exception(failure);
        } catch (const http_client_error&) {
            throw;
        } catch (const std::system_error&) {
            throw http_client_error(make_error_code(http_client_errc::connect_failed), "HTTP/3 endpoint connection failed");
        }
    }
}

bool http3_client_connection::active() const noexcept {
    return session_ && session_->usable() && session_->quic_->handshake_completed();
}

http3_client_connection::http3_client_connection(http_client_pool& pool, http3_client_tls& tls, std::size_t index, std::uint64_t serial, std::string_view host, std::uint16_t port, std::string_view server_name) :
    tls_(tls),
    route_{.host_ = std::string(host), .port_ = port, .server_name_ = std::string(server_name)},
    serial_(serial),
    session_(std::make_unique<session>(pool, tls_, route_, index)),
    race_(std::make_unique<address_race>(pool, tls_, route_, index)) {}
http3_client_connection::~http3_client_connection() = default;
bool http3_client_connection::serves(std::string_view host, std::uint16_t port, std::string_view server_name) const noexcept {
    return !fresh_ && !race_->retired_ && route_.port_ == port && http_ascii_equals_ignore_case(route_.host_, host) && http_ascii_equals_ignore_case(route_.server_name_, server_name);
}
std::optional<asio::ip::address> http3_client_connection::peer_address() const noexcept {
    if (!session_ || !session_->open_) {
        return std::nullopt;
    }
    return session_->remote_.address();
}
bool http3_client_connection::connecting() const noexcept {
    return race_->selecting_;
}
bool http3_client_connection::idle() const noexcept {
    return !race_->selecting_ && session_ && session_->usable() && session_->streams_.empty();
}
bool http3_client_connection::reclaimable() const noexcept {
    if (users_ != 0 || race_->selecting_ || race_->pending()) {
        return false;
    }
    return !session_ || (!session_->open_ && session_->io_tasks_ == 0 && session_->borrowed_operations_ == 0 && session_->streams_.empty());
}
void http3_client_connection::close() noexcept {
    race_->cancel(make_error_code(http_client_errc::io_error));
    if (session_) {
        session_->close();
    }
}
void http3_client_connection::retire_addresses() noexcept {
    if (race_->selecting_) {
        race_->cancel(make_error_code(http_client_errc::io_error));
    }
    if (!session_) {
        return;
    }
    session_->draining_ = true;
    session_->connect_endpoint_.reset();
    for (auto* item : session_->streams_) {
        if (item->response_.tunnel_) {
            session_->cancel(*item, make_error_code(http_client_errc::closing));
        }
    }
    if (session_->streams_.empty()) {
        session_->close();
    }
    session_->wake();
}
std::uint64_t http3_client_connection::connection_id() const noexcept {
    return session_ && session_->open_ ? session_->connection_id_ : 0;
}
void http3_client_connection::shutdown() noexcept {
    race_->retired_ = true;
    (void)race_->selection_.close();
    race_->cancel(make_error_code(http_client_errc::closing));
    if (session_) {
        session_->retired_ = true;
        (void)session_->connect_slots_.close();
        session_->close(make_error_code(http_client_errc::closing));
    }
}
task<void> http3_client_connection::join() {
    // Pool request/UDP children have joined before this call. External tunnel
    // writes and address migration still own a linear session borrow until
    // their already-signalled cancellation resumes on the worker.
    while (session_ && (session_->io_tasks_ != 0 || session_->borrowed_operations_ != 0)) {
        co_await session_->changed_.wait();
    }
}

std::size_t http3_client_connection::available() const noexcept {
    if (race_->selecting_ || race_->retired_) {
        return 0;
    }
    if (!session_) {
        return race_->pool_.config().http3_.max_concurrent_streams_;
    }
    const auto& self = *session_;
    return !self.retired_ && (!self.open_ || self.usable()) ? self.pool_.config().http3_.max_concurrent_streams_ - (std::min)(self.streams_.size(), self.pool_.config().http3_.max_concurrent_streams_) : 0;
}
bool http3_client_connection::cancel_by_id(std::uint64_t id) noexcept {
    if (race_->selecting_ && race_->cancellation_id_ == id) {
        race_->cancel(std::make_error_code(std::errc::operation_canceled));
        return true;
    }
    if (!session_) {
        return false;
    }
    if (session_->migration_cancellation_id_ == id) {
        session_->migration_cancelled_ = true;
        session_->close(std::make_error_code(std::errc::operation_canceled));
        return true;
    }
    for (auto* item : session_->streams_) {
        if (item->cancellation_id_ == id) {
            session_->cancel(*item, std::make_error_code(std::errc::operation_canceled));
            return true;
        }
    }
    return false;
}
void http3_client_connection::abandon(http_client_response_state& response, bool malformed) noexcept {
    if (race_->selecting_ && race_->cancellation_id_ == response.cancellation_id_) {
        race_->cancel(std::make_error_code(std::errc::operation_canceled));
        return;
    }
    if (!session_) {
        return;
    }
    auto& self = *session_;
    for (auto* item : self.streams_) {
        if (&item->response_ == &response) {
            self.cancel(*item, std::make_error_code(std::errc::operation_canceled));
            if (malformed && item->id_ >= 0 && self.quic_) {
                self.quic_->reset_stream(item->id_, http3_error_code::message_error);
            }
            return;
        }
    }
}
void http3_client_connection::release_data(http_client_response_state& response) noexcept {
    if (session_) {
        session_->release(response);
    } else {
        response.http3_data_credit_.reset();
    }
}

task<bool> http3_client_connection::execute(const http_client_request_storage& request, bool fallback, const operation_deadline& timeout, const operation_deadline& acquire_timeout, stop_token stop, http_client_response_state& response) {
    const std::string_view host = route_.host_;
    if (request.is_upgrade()) {
        throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "HTTP/1 Upgrade is unavailable over HTTP/3");
    }
    if (request.options_.fresh_connection_) {
        close();
    }
    const bool may_send_early = !request.options_.fresh_connection_ && is_http_method_safe(request.method()) && !request.has_body() && request.output() == nullptr && !request.is_tunnel();
    try {
        co_await select_endpoint(may_send_early, timeout, acquire_timeout, stop, response);
    } catch (...) {
        if (can_fallback_from_http3(std::current_exception(), fallback, stop, response, timeout, race_->retired_)) {
            co_return false;
        }
        throw;
    }
    auto& self = *session_;
    session::stream item(self, response, timeout);
    item.target_ = request.target();
    item.origin_ = self.pool_.policy().addresses_origin(request);
    item.websocket_ = request.websocket();
    self.streams_.push_back(&item);
    self.idle_timer_.cancel();
    response.transport_ = http_client_response_transport::http3;
    response.connection_index_ = self.index_;
    response.connection_serial_ = serial_;
    response.request_method_ = classify_http_method(request.method());
    struct registration final {
        session& owner_;
        session::stream& item_;
        ~registration() {
            item_.timer_.cancel();
            owner_.release(item_.response_);
            if (item_.id_ >= 0 && !item_.done_ && owner_.quic_ && item_.response_.connection_generation_ == owner_.generation_) {
                owner_.quic_->reset_stream(item_.id_, http3_error_code::request_cancelled);
                owner_.writer_.notify();
            }
            if (auto* output = item_.response_.output()) {
                output->wake_ = nullptr;
                output->wake_target_ = nullptr;
            }
            std::erase(owner_.streams_, &item_);
            owner_.changed_.notify();
            if (owner_.race_signal_) {
                owner_.race_signal_->notify();
            }
            if (owner_.draining_ && owner_.streams_.empty() && owner_.open_) {
                owner_.close();
            } else {
                owner_.begin_idle();
            }
        }
    } registered{self, item};
    http_client_response_cancellation cancellation(self.pool_.cancellation_target(), item.cancellation_id_, response);
    cancellation.arm(stop);
    self.schedule(item);
    try {
        co_await self.connect(item, host, may_send_early, acquire_timeout, stop);
    } catch (...) {
        if (can_fallback_from_http3(std::current_exception(), fallback, stop, response, timeout, self.retired_)) {
            response.transport_ = http_client_response_transport::unassigned;
            co_return false;
        }
        throw;
    }
    response.connection_generation_ = self.generation_;
    self.capture_connection_info(response, self.requests_ != 0);
    if (request.udp_tunnel()) {
        response.http3_datagrams_.emplace();
    }
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
        while (!self.quic_->handshake_completed() && (!may_send_early || attempt != 0 || self.early_rejected_)) {
            self.check(item);
            co_await item.signal_.wait();
        }
        while (!self.quic_->http3_started()) {
            self.check(item);
            self.check_quic(self.quic_->start_http3());
            self.writer_.notify();
            if (!self.quic_->http3_started()) {
                co_await item.signal_.wait();
            }
        }
        if (request.is_tunnel() && !request.tunnel_protocol().empty()) {
            while (!self.quic_->http3().peer_settings()) {
                self.check(item);
                co_await item.signal_.wait();
            }
            if (!self.quic_->http3().peer_settings()->enable_connect_protocol_) {
                throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "peer disabled extended CONNECT");
            }
        }
        worker_timer_registration admission_timer;
        bool admission_expired = false;
        if (const auto deadline = acquire_timeout.deadline()) {
            (void)self.pool_.worker().schedule_timer(admission_timer, *deadline, [&item, &admission_expired](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    admission_expired = true;
                    item.signal_.notify();
                }
            });
        }
        for (;;) {
            self.check(item);
            if (admission_expired || acquire_timeout.expired()) {
                throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP/3 stream admission timed out");
            }
            if (!self.usable()) {
                if (attempt == 0) {
                    item.retry_ = true;
                    break;
                }
                throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "HTTP/3 session is draining");
            }
            std::size_t active = 0;
            for (const auto* other : self.streams_) {
                active += other->id_ >= 0 && other != &item;
            }
            if (active < self.pool_.config().http3_.max_concurrent_streams_) {
                if (auto id = self.quic_->open_request_stream()) {
                    item.id_ = *id;
                    break;
                }
            }
            co_await item.signal_.wait();
        }
        admission_timer.cancel();
        if (!item.retry_) {
            response.stream_id_ = static_cast<std::uint64_t>(item.id_);
            const bool reused = self.requests_++ != 0;
            self.capture_connection_info(response, reused);
            item.early_ = !self.quic_->handshake_completed();
            client_prepared_request_scratch scratch;
            const auto prepared = self.pool_.policy().prepare_request(request, scratch);
            try {
                self.submit_request_head(item, request, prepared.headers_);
                response.request_written_ = true;
                co_await self.flush_stream(item);
                if (!request.body().empty()) {
                    (void)co_await self.send_body(item, request.body());
                    if (!item.retry_ && !item.write_stopped_) {
                        require_accepted(self.quic_->http3().finish_request(item.id_));
                        item.request_end_submitted_ = true;
                        co_await self.flush_stream(item);
                    }
                }
                self.read_progress(item);
                if (auto* output = request.output()) {
                    co_await run_multiplexed_client_upload(self.pool_.worker(), request, *output, item.signal_, [&item]() noexcept {
                        item.signal_.notify();
                    }, [&] {
                        self.check(item);
                        return false;
                    }, []() noexcept {
                        return true;
                    }, [&](std::string_view chunk) -> task<bool> {
                        co_return co_await self.send_body(item, chunk);
                    }, [&]() -> task<bool> {
                        std::vector<http_header_view> trailers;
                        if (const auto* upload = request.upload()) {
                            trailers.reserve(upload->trailers_.size());
                            for (const auto& field : upload->trailers_) {
                                trailers.emplace_back(field.name(), field.value());
                            }
                        }
                        require_accepted(self.quic_->http3().finish_request(item.id_, trailers));
                        item.request_end_submitted_ = true;
                        co_await self.flush_stream(item);
                        co_return true;
                    });
                    if (request.upload() != nullptr && request.upload()->abandoned_ && !output->ended_ && item.id_ >= 0) {
                        // Only the sending half ends; the response keeps
                        // arriving (RFC 9114 section 4.1).
                        self.quic_->reset_send_stream(item.id_, http3_error_code::no_error);
                        self.writer_.notify();
                    }
                    if (response.tunnel_ && response.tunnel_->accepted_ && response.tunnel_->receive_ended_ && (output->ended_ || output->stopped_)) {
                        item.done_ = true;
                    }
                }
                while ((!item.done_ || response.informational_active_) && !item.retry_) {
                    self.check(item);
                    co_await item.signal_.wait();
                }
                self.check(item);
            } catch (...) {
                self.quic_->reset_stream(item.id_, http3_error_code::request_cancelled);
                self.writer_.notify();
                throw;
            }
        }
        if (!item.retry_) {
            co_return true;
        }
        if (self.usable()) {
            if ((!may_send_early && !item.rejected_) || attempt != 0 || (item.id_ >= 0 && request.output() != nullptr)) {
                throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP/3 early request replay exhausted");
            }
            if (!self.early_rejected_ && !item.rejected_ && item.id_ >= 0) {
                self.quic_->reset_stream(item.id_, http3_error_code::request_cancelled);
            }
        } else {
            if (attempt != 0 || (item.id_ >= 0 && request.output() != nullptr)) {
                throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "HTTP/3 request was not processed by the server (GOAWAY)");
            }
            // The codec already cancelled an unprocessed stream.
            if (item.id_ >= 0 && self.quic_ && !item.rejected_) {
                self.quic_->reset_stream(item.id_, http3_error_code::request_cancelled);
                self.writer_.notify();
            }
        }
        item.id_ = -1;
        self.changed_.notify();
        item.done_ = false;
        item.retry_ = false;
        item.retry_425_ = false;
        item.rejected_ = false;
        item.write_stopped_ = false;
        item.request_end_submitted_ = false;
        item.request_end_flushed_ = false;
        item.stop_sending_error_.reset();
        item.error_.reset();
        response.headers_.clear();
        response.trailers_.clear();
        response.informational_.clear();
        response.informational_budget_ = {};
        self.release(response);
        response.discard_response_body();
        if (!self.usable()) {
            co_await self.connect(item, host, false, timeout.constrained_by(response.effective_acquire_timeout_), stop);
            response.connection_generation_ = self.generation_;
            self.capture_connection_info(response, false);
        }
    }
    throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP/3 request attempts exhausted");
}

task<void> http3_client_connection::migrate(std::string_view local_address, std::uint16_t port, const operation_deadline& timeout, stop_token stop) {
    if (stop.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP/3 migration cancelled");
    }
    if (!session_) {
        throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "HTTP/3 endpoint selection is in progress");
    }
    auto& self = *session_;
    session::operation_lease borrowed(self);
    if (!self.usable() || !self.quic_->handshake_completed()) {
        throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "migration requires an authenticated active HTTP/3 connection");
    }
    if (self.migrating_ || self.old_socket_) {
        throw std::logic_error("HTTP/3 path migration is already active");
    }
    self.migrating_ = true;
    struct migration_guard {
        bool& flag_;
        ~migration_guard() {
            flag_ = false;
        }
    } guard{self.migrating_};
    self.migration_cancelled_ = false;
    worker_cancellation_registration cancellation(self.pool_.cancellation_target(), self.migration_cancellation_id_);
    cancellation.arm(stop);
    const auto check = [&] {
        if (self.migration_cancelled_ || stop.stop_requested() || timeout.expired() || !self.open_ || self.retired_) {
            // Host shutdown also cancels the merged token; retirement takes precedence.
            const auto code = self.retired_
                ? make_error_code(http_client_errc::closing)
                : self.migration_cancelled_ || stop.stop_requested()
                ? std::make_error_code(std::errc::operation_canceled)
                : timeout.expired()
                ? std::make_error_code(std::errc::timed_out)
                : make_error_code(http_client_errc::io_error);
            self.close(code);
            throw http_client_error(code, "HTTP/3 migration stopped");
        }
    };
    check();
    auto replacement = std::make_unique<asio::ip::udp::socket>(self.pool_.io_context());
    asio::ip::udp::endpoint local;
    try {
        const asio::ip::udp::endpoint requested(asio::ip::make_address(local_address), port);
        if (requested.protocol() != self.remote_.protocol()) {
            throw http_client_error(make_error_code(http_client_errc::connect_failed), "HTTP/3 migration address family mismatch");
        }
        replacement->open(requested.protocol());
        replacement->bind(requested);
        replacement->connect(self.remote_);
        local = replacement->local_endpoint();
    } catch (const http_client_error&) {
        throw;
    } catch (const std::system_error&) {
        throw http_client_error(make_error_code(http_client_errc::connect_failed), "HTTP/3 migration socket setup failed");
    }
    worker_timer_registration timer;
    (void)self.pool_.worker().schedule_timer(timer, *timeout.deadline(), [&self](worker_timer_outcome) noexcept {
        self.changed_.notify();
    });
    for (;;) {
        check();
        if (!self.handshake_confirmed_) {
            self.writer_.notify();
            co_await self.changed_.wait();
            continue;
        }
        const auto result = self.quic_->migrate(self.path(local, self.remote_), http3_quic_now());
        if (result.status_ == http3_quic_status::accepted) {
            break;
        }
        if (result.status_ != http3_quic_status::would_block) {
            throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "peer or QUIC state forbids active migration");
        }
        // CID replenishment is protocol backpressure, not a rejected migration.
        // Receive and expiry events drive progress under the same overall deadline.
        self.writer_.notify();
        co_await self.changed_.wait();
    }
    self.old_local_ = self.local_;
    self.old_socket_ = std::move(self.socket_);
    self.socket_ = std::move(replacement);
    self.local_ = local;
    const auto next_buffer = 1 - self.active_receive_buffer_;
    self.receive_buffers_[next_buffer].resize(http3_max_udp_payload_bytes);
    ++self.io_tasks_;
    std::exception_ptr spawn_failure;
    try {
        self.pool_.tasks().spawn(self.receive(*self.socket_, self.local_, self.receive_buffers_[next_buffer]));
    } catch (...) {
        spawn_failure = std::current_exception();
    }
    if (spawn_failure) {
        --self.io_tasks_;
        self.close();
        while (self.io_tasks_ != 0) {
            co_await self.changed_.wait();
        }
        self.old_socket_.reset();
        std::rethrow_exception(spawn_failure);
    }
    self.writer_.notify();
    std::exception_ptr failure;
    try {
        for (;;) {
            check();
            const auto active = self.quic_->current_path().local_;
            if (active.size() == self.local_.size() && std::memcmp(active.data(), self.local_.data(), self.local_.size()) == 0) {
                break;
            }
            co_await self.changed_.wait();
        }
        while (self.sending_socket_ == self.old_socket_.get()) {
            check();
            co_await self.changed_.wait();
        }
    } catch (...) {
        failure = std::current_exception();
    }
    std::error_code ignored;
    if (self.old_socket_) {
        self.old_socket_->close(ignored);
    }
    while (self.io_tasks_ > (self.open_ ? std::size_t{2} : std::size_t{0})) {
        co_await self.changed_.wait();
    }
    self.old_socket_.reset();
    self.active_receive_buffer_ = next_buffer;
    if (!failure) {
        check();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

task<bool> http3_client_connection::send_datagram(http_client_response_state& response, std::span<const std::byte> bytes) {
    if (!session_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP/3 datagram session is retired");
    }
    auto& self = *session_;
    session::operation_lease borrowed(self);
    auto* item = self.find(static_cast<std::int64_t>(response.stream_id_));
    if (!item || &item->response_ != &response || response.connection_generation_ != self.generation_ || !response.tunnel_ || !response.tunnel_->accepted_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP/3 datagram stream is closed");
    }
    if (bytes.size() > self.quic_->max_datagram_payload_size(item->id_) || !response.http3_native_datagrams_) {
        co_return false;
    }
    const auto bound = item->timeout_.constrained_by(response.effective_write_timeout_);
    worker_timer_registration timer;
    if (const auto deadline = bound.deadline()) {
        (void)self.pool_.worker().schedule_timer(timer, *deadline, [&self](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                self.changed_.notify();
            }
        });
    }
    for (;;) {
        item = self.find(static_cast<std::int64_t>(response.stream_id_));
        if (!item || &item->response_ != &response || response.connection_generation_ != self.generation_) {
            response.rethrow_response_failure();
            throw http_client_error(make_error_code(http_client_errc::closing), "HTTP/3 datagram stream retired");
        }
        self.check(*item);
        if (bound.expired()) {
            self.cancel(*item, std::make_error_code(std::errc::timed_out));
            throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP/3 datagram output timeout");
        }
        const auto status = self.quic_->submit_datagram(item->id_, bytes);
        if (status == http3_quic_status::accepted) {
            self.writer_.notify();
            co_return true;
        }
        if (status != http3_quic_status::would_block) {
            throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP/3 datagram submission failed");
        }
        co_await self.changed_.wait();
    }
}

void http3_client_connection::reprioritize(http_client_response_state& response, http_priority priority) {
    if (!session_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP/3 response connection is retired");
    }
    auto& self = *session_;
    if (response.connection_generation_ != self.generation_ || !self.open_ || !self.quic_ || self.quic_->closed()) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP/3 response connection is retired");
    }
    auto* item = self.find(static_cast<std::int64_t>(response.stream_id_));
    if (item == nullptr || &item->response_ != &response || item->error_ || item->failure_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP/3 response connection is retired");
    }
    require_accepted(self.quic_->http3().submit_priority_update(item->id_, {.urgency_ = priority.urgency_, .incremental_ = priority.incremental_}));
    self.writer_.notify();
}

}  // namespace ruvia::detail
#endif
