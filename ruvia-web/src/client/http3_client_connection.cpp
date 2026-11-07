#include "client/http3_client_connection.h"

#ifdef RUVIA_ENABLE_HTTP3
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <coroutine>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/ip/udp.hpp>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "ruvia/core/async.h"
#include "ruvia/core/pool_lease_scheduler.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http3_quic_connection.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_known_method.h"

#include "client/client_happy_eyeballs.h"
#include "client/client_tls.h"
#include "client/http_client_pool.h"
#include "client/http_client_response_decoding.h"
#include "client/http_client_response_state.h"
#include "client/http_client_transport_error.h"

namespace ruvia::detail {
namespace {
using error_code = http_client_error::code_type;
using clock_type = std::chrono::steady_clock;
struct worker_turn final {
    const worker_handle& worker_;
    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) const noexcept {
        worker_.defer_resume(continuation);
    }
    void await_resume() const noexcept {}
};
ngtcp2_tstamp timestamp() noexcept {
    return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count());
}
ngtcp2_duration quic_duration(std::chrono::milliseconds value) noexcept {
    const auto count = static_cast<std::uint64_t>((std::max)(value.count(), std::int64_t{0}));
    return count > UINT64_MAX / NGTCP2_MILLISECONDS ? UINT64_MAX : count * NGTCP2_MILLISECONDS;
}
ngtcp2_cid random_cid() {
    ngtcp2_cid cid{};
    cid.datalen = 18;
    if (RAND_bytes(cid.data, 18) != 1) {
        throw http_client_error(error_code::tls_failed, "QUIC connection ID generation failed");
    }
    return cid;
}
void fill_random(std::uint8_t* bytes, std::size_t size, const ngtcp2_rand_ctx*) noexcept {
    if (RAND_bytes(bytes, static_cast<int>(size)) != 1) {
        std::terminate();
    }
}
int new_cid(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t size, void*) noexcept {
    if (RAND_bytes(cid->data, static_cast<int>(size)) != 1 || RAND_bytes(token->data, NGTCP2_STATELESS_RESET_TOKENLEN) != 1) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    cid->datalen = size;
    return 0;
}
ngtcp2_callbacks crypto_callbacks() noexcept {
    ngtcp2_callbacks result{};
    result.client_initial = ngtcp2_crypto_client_initial_cb;
    result.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    result.encrypt = ngtcp2_crypto_encrypt_cb;
    result.decrypt = ngtcp2_crypto_decrypt_cb;
    result.hp_mask = ngtcp2_crypto_hp_mask_cb;
    result.recv_retry = ngtcp2_crypto_recv_retry_cb;
    result.update_key = ngtcp2_crypto_update_key_cb;
    result.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    result.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    result.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    result.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    result.rand = fill_random;
    result.get_new_connection_id2 = new_cid;
    return result;
}
bool safe_early_method(std::string_view method) noexcept {
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE";
}
void require_accepted(http3_status status) {
    if (status != http3_status::accepted) {
        throw http_client_error(error_code::protocol_error, "HTTP/3 stream operation rejected");
    }
}
}  // namespace

class http3_client_connection::session final {
public:
    struct free_tls {
        void operator()(SSL* value) const noexcept {
            SSL_free(value);
        }
    };
    struct free_crypto {
        void operator()(ngtcp2_crypto_ossl_ctx* value) const noexcept {
            ngtcp2_crypto_ossl_ctx_del(value);
        }
    };
    struct free_ticket {
        void operator()(SSL_SESSION* value) const noexcept {
            SSL_SESSION_free(value);
        }
    };
    struct operation_lease final {
        explicit operation_lease(session& owner) noexcept
            : owner_(owner) {
            ++owner_.borrowed_operations_;
        }
        ~operation_lease() {
            --owner_.borrowed_operations_;
            owner_.changed_.notify();
            if (owner_.race_signal_) {
                owner_.race_signal_->notify();
            }
        }
        session& owner_;
    };
    struct stream final {
        stream(session& owner, http_client_response_state& response, const operation_timeout& timeout)
            : owner_(owner),
              response_(response),
              timeout_(timeout),
              signal_(owner.pool_.worker()) {}
        session& owner_;
        http_client_response_state& response_;
        operation_timeout timeout_;
        worker_signal signal_;
        std::int64_t id_{-1};
        std::uint64_t cancellation_id_{};
        std::optional<error_code> error_;
        std::exception_ptr failure_;
        std::optional<clock_type::time_point> read_deadline_;
        std::optional<clock_type::time_point> write_deadline_;
        std::optional<clock_type::time_point> timer_at_;
        std::string_view target_;
        http_client_websocket_handshake* websocket_{};
        bool origin_{true};
        bool done_{};
        bool early_{};
        bool retry_{};
        bool retry_425_{};
        worker_timer_registration timer_;
    };
    session(http_client_pool& pool, std::size_t index)
        : pool_(pool),
          index_(index),
          context_(asio::ssl::context::tls_client),
          connect_slots_(1, pool.worker(), pool.resource()),
          changed_(pool.worker()),
          writer_(pool.worker()),
          streams_(pool.resource()),
          endpoint_host_(pool.resource()),
          ticket_host_(pool.resource()),
          packet_(pool.resource()) {
        configure_client_tls_context(*context_.native_handle(), pool.config().tls_);
        if (SSL_CTX_set_min_proto_version(context_.native_handle(), TLS1_3_VERSION) != 1) {
            throw http_client_error(error_code::tls_failed, "QUIC requires TLS 1.3");
        }
        SSL_CTX_set_session_cache_mode(context_.native_handle(), SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_sess_set_new_cb(context_.native_handle(), save_ticket);
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
        const auto size = ngtcp2_conn_encode_0rtt_transport_params2(self.quic_->native_handle(), self.ticket_params_.data(), self.ticket_params_.size());
        if (size < 0) {
            return 0;
        }
        // SSL_free marks its current session non-resumable when QUIC closes
        // without TLS close_notify. Keep the cache independent of that SSL.
        auto* saved = SSL_SESSION_dup(ticket);
        if (saved == nullptr) {
            return 0;
        }
        self.ticket_.reset(saved);
        self.ticket_params_size_ = static_cast<std::size_t>(size);
        self.ticket_settings_valid_ = false;
        if (const auto settings = self.quic_->http3().peer_settings()) {
            self.ticket_settings_ = *settings;
            self.ticket_settings_valid_ = true;
        }
        return 0;
    }
    ngtcp2_path path(asio::ip::udp::endpoint& local, asio::ip::udp::endpoint& remote) noexcept {
        return {.local = {local.data(), static_cast<ngtcp2_socklen>(local.size())},
            .remote = {remote.data(), static_cast<ngtcp2_socklen>(remote.size())},
            .user_data = nullptr};
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
    void close(error_code code = error_code::io_error, const stream* preserve = nullptr) noexcept {
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
            if (!item->error_) {
                item->error_ = code;
            }
            if (auto* output = item->response_.output(); output && item->id_ >= 0) {
                output->stop();
            }
        }
        wake();
    }
    void begin_idle() noexcept {
        if (!open_ || !streams_.empty() || !pool_.config().idle_timeout_) {
            return;
        }
        try {
            (void)pool_.worker().schedule_timer(idle_timer_, worker_timer_deadline_after(*pool_.config().idle_timeout_),
                [this](worker_timer_outcome outcome) noexcept {
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
            throw http_client_error(error_code::cancelled, "HTTP/3 response abandoned");
        }
        if (item.timeout_.expired()) {
            throw http_client_error(error_code::timeout, "HTTP/3 operation timed out");
        }
        if (retired_) {
            throw http_client_error(error_code::closing, "HTTP/3 client stopped");
        }
    }
    void schedule(stream& item) {
        auto deadline = item.timeout_.deadline();
        for (const auto bound : {item.read_deadline_, item.write_deadline_}) {
            if (bound && (!deadline || *bound < *deadline)) {
                deadline = bound;
            }
        }
        if (!deadline || (item.timer_at_ && *item.timer_at_ <= *deadline)) {
            return;
        }
        item.timer_.cancel();
        item.timer_at_ = deadline;
        (void)pool_.worker().schedule_timer(item.timer_, *deadline, [&item](worker_timer_outcome outcome) noexcept {
            item.timer_at_.reset();
            if (outcome != worker_timer_outcome::expired) {
                return;
            }
            const auto now = clock_type::now();
            if (item.timeout_.expired() || (item.read_deadline_ && *item.read_deadline_ <= now) ||
                (item.write_deadline_ && *item.write_deadline_ <= now)) {
                item.owner_.cancel(item, error_code::timeout);
            } else {
                try {
                    item.owner_.schedule(item);
                } catch (...) {
                    item.failure_ = std::current_exception();
                    item.signal_.notify();
                }
            }
        });
    }
    void read_progress(stream& item) {
        if (item.websocket_ && item.response_.head_ready_) {
            item.read_deadline_.reset();
            return;
        }
        if (pool_.config().read_timeout_ && !item.done_ && item.response_.http3_data_credit_ == 0) {
            item.read_deadline_ = worker_timer_deadline_after(*pool_.config().read_timeout_);
            schedule(item);
        }
    }
    void cancel(stream& item, error_code code) noexcept {
        item.error_ = code;
        if (quic_ && item.id_ >= 0) {
            quic_->reset_stream(item.id_, static_cast<std::uint64_t>(http3_error_code::request_cancelled));
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
        const auto bytes = std::exchange(response.http3_data_credit_, 0);
        if (bytes && quic_ && response.connection_generation_ == generation_) {
            quic_->consume_stream_data(static_cast<std::int64_t>(response.stream_id_), bytes);
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
    void consume_events() {
        if (early_started_ && !early_rejected_ && ngtcp2_conn_get_tls_early_data_rejected2(quic_->native_handle())) {
            early_rejected_ = true;
            if (quic_->reject_early_data() != http3_quic_status::accepted) {
                throw http_client_error(error_code::protocol_error, "HTTP/3 early-data reset failed");
            }
            for (auto* item : streams_) {
                if (item->early_) {
                    item->retry_ = true;
                    item->signal_.notify();
                }
            }
        }
        while (auto event = quic_->next_event()) {
            if (const auto* origins = std::get_if<http_origin_advertisement>(&*event)) {
                pool_.advertisements().retain(index_, http_protocol_version::http3, *origins);
                continue;
            }
            if (const auto* away = std::get_if<http3_goaway_event>(&*event)) {
                draining_ = true;
                for (auto* item : streams_) {
                    if (item->id_ >= away->id_ && !item->response_.head_ready_) {
                        item->error_ = error_code::protocol_unavailable;
                        item->signal_.notify();
                    }
                }
                continue;
            }
            std::visit([this](auto& value) {
                using event_type = std::remove_cvref_t<decltype(value)>;
                if constexpr (requires { value.stream_id_; }) {
                    auto* item = find(value.stream_id_);
                    if (!item || item->error_ || item->failure_) {
                        if constexpr (std::same_as<event_type, http3_data_event>) {
                            quic_->consume_stream_data(value.stream_id_, value.bytes_.size());
                        }
                        return;
                    }
                    auto& response = item->response_;
                    try {
                        if constexpr (std::same_as<event_type, http3_headers_event>) {
                            if (item->retry_425_) {
                                return;
                            }
                            std::pmr::vector<http_header> fields(response.resource_);
                            unsigned status = 0;
                            for (const auto& field : value.fields_) {
                                if (field.name() == ":status") {
                                    const auto text = field.value();
                                    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), status);
                                    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
                                        throw http_client_error(error_code::protocol_error, "invalid HTTP/3 status");
                                    }
                                } else {
                                    fields.emplace_back(field.name(), field.value(), response.resource_);
                                }
                            }
                            if (value.trailers_) {
                                response.trailers_ = std::move(fields);
                            } else if (status >= 100 && status < 200) {
                                response.retain_informational(http_status_code::from_value(status), std::move(fields), status == 100 ? std::optional(http_client_request_content_signal::continue_value) : std::nullopt);
                            } else {
                                if (status < 200 || status > 599) {
                                    throw http_client_error(error_code::protocol_error, "invalid HTTP/3 final status");
                                }
                                if (status == 425 && item->early_) {
                                    item->retry_425_ = true;
                                    return;
                                }
                                response.headers_ = std::move(fields);
                                if (item->websocket_) {
                                    const auto received_status = http_status_code::from_value(status);
                                    if (status >= 300) {
                                        throw http_client_error(error_code::handshake_rejected, "HTTP/3 WebSocket CONNECT rejected", received_status);
                                    }
                                    if (value.stream_ended_) {
                                        throw http_client_error(error_code::handshake_rejected, "WebSocket CONNECT response ended its stream", received_status);
                                    }
                                    const auto negotiated = item->websocket_->negotiation_.validate_fields(response.headers_);
                                    if (const auto* error = std::get_if<websocket_client_handshake_error>(&negotiated)) {
                                        (void)error;
                                        throw http_client_error(error_code::handshake_rejected, "invalid HTTP/3 WebSocket negotiation", received_status);
                                    }
                                    const auto& selection = std::get<websocket_client_negotiation_result_view>(negotiated);
                                    item->websocket_->selected_subprotocol_.assign(selection.selected_subprotocol_);
                                    item->websocket_->compression_ = selection.compression_;
                                    item->timeout_ = operation_timeout(std::nullopt);
                                }
                                publish_http_client_final_head(response, pool_.policy(), {.status_ = http_received_status(http_status_code::from_value(status)), .protocol_version_ = http_protocol_version::http3, .request_content_signal_ = std::nullopt, .origin_target_ = item->origin_ ? std::optional(item->target_) : std::nullopt, .opens_tunnel_ = response.tunnel_.has_value() && status < 300});
                                if (response.tunnel_ && response.tunnel_->accepted_) {
                                    const auto settings = quic_->http3().peer_settings();
                                    response.http3_native_datagrams_ = settings && settings->h3_datagram_;
                                }
                            }
                            read_progress(*item);
                        } else if constexpr (std::same_as<event_type, http3_data_event>) {
                            pool_.wire_counters().received_ += value.bytes_.size();
                            if (item->retry_425_) {
                                quic_->consume_stream_data(value.stream_id_, value.bytes_.size());
                                return;
                            }
                            if (response.response_body_plan_ && !(response.tunnel_ && response.tunnel_->accepted_) &&
                                (response.response_body_plan_->body_suppressed() || response.response_body_plan_->content_semantics() != http_response_content_semantics::with_content)) {
                                quic_->consume_stream_data(value.stream_id_, value.bytes_.size());
                                read_progress(*item);
                                return;
                            }
                            if (!response.append_received_body(value.bytes_, pool_.config().max_response_bytes_)) {
                                throw http_client_error(error_code::response_too_large, "HTTP/3 response exceeds byte budget");
                            }
                            if (response.collect_all_) {
                                quic_->consume_stream_data(value.stream_id_, value.bytes_.size());
                                read_progress(*item);
                            } else {
                                response.http3_data_credit_ += value.bytes_.size();
                                item->read_deadline_.reset();
                            }
                        } else if constexpr (std::same_as<event_type, http3_stream_end_event>) {
                            if (item->retry_425_) {
                                item->retry_ = true;
                            } else if (response.tunnel_ && response.tunnel_->accepted_) {
                                response.tunnel_->receive_ended_ = true;
                            } else {
                                if (!response.head_ready_) {
                                    throw http_client_error(error_code::protocol_error, "HTTP/3 response ended without a final head");
                                }
                                const bool content = response.response_body_plan_ &&
                                                     response.response_body_plan_->content_semantics() == http_response_content_semantics::with_content;
                                finish_http_client_response_body(response, item->timeout_, content, pool_.config().max_response_bytes_);
                                response.complete_ = true;
                            }
                            item->done_ = true;
                            item->read_deadline_.reset();
                            response.data_signal_.notify();
                        } else if constexpr (std::same_as<event_type, http3_stream_close_event>) {
                            if (!item->done_ && !item->retry_) {
                                throw http_client_error(error_code::io_error, "HTTP/3 request stream reset");
                            }
                            if (auto* output = response.output(); output && item->done_) {
                                output->stop();
                            }
                        } else if constexpr (std::same_as<event_type, http3_datagram_event>) {
                            if (!response.tunnel_ || !response.tunnel_->accepted_ || !response.http3_datagrams_) {
                                return;
                            }
                            const auto bound = pool_.config().max_response_bytes_;
                            if (response.http3_datagrams_->size() >= 64 || value.payload_.size() > bound - (std::min)(bound, response.http3_datagram_bytes_)) {
                                return;
                            }
                            response.http3_datagrams_->emplace_back(value.payload_);
                            response.http3_datagram_bytes_ += value.payload_.size();
                            response.data_signal_.notify();
                        }
                    } catch (...) {
                        item->failure_ = std::current_exception();
                        quic_->reset_stream(item->id_, static_cast<std::uint64_t>(http3_error_code::message_error));
                    }
                    item->signal_.notify();
                }
            },
                *event);
        }
        if (ticket_) {
            if (const auto settings = quic_->http3().peer_settings()) {
                ticket_settings_ = *settings;
                ticket_settings_valid_ = true;
            }
        }
        changed_.notify();
    }
    void check_quic(http3_quic_status status) {
        if (status != http3_quic_status::accepted && status != http3_quic_status::would_block) {
            throw http_client_error(tls_ && SSL_get_verify_result(tls_.get()) != X509_V_OK ? error_code::tls_failed : error_code::io_error, "QUIC connection failed");
        }
    }
    task<void> receive(asio::ip::udp::socket& socket, asio::ip::udp::endpoint local) {
        try {
            std::array<std::byte, 65536> bytes;
            while (open_ && socket.is_open()) {
                asio::ip::udp::endpoint remote;
                const auto result = co_await async_asio<std::size_t>([&](auto handler) { socket.async_receive_from(asio::buffer(bytes), remote, std::move(handler)); });
                if (!open_ || !socket.is_open()) {
                    break;
                }
                if (result.error_code()) {
                    throw std::system_error(result.error_code());
                }
                check_quic(quic_->receive_datagram(path(local, remote), 0, std::span(bytes).first(result.result()), timestamp()).status_);
                consume_events();
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
    }
    void arm_expiry() {
        expiry_.cancel();
        const auto time = quic_->expiry();
        if (time == UINT64_MAX) {
            return;
        }
        const auto max = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
        const auto deadline = clock_type::time_point(std::chrono::duration_cast<clock_type::duration>(std::chrono::nanoseconds(static_cast<std::int64_t>((std::min)(time, max)))));
        (void)pool_.worker().schedule_timer(expiry_, deadline, [this](worker_timer_outcome outcome) noexcept { if (outcome == worker_timer_outcome::expired) { writer_.notify(); } });
    }
    task<void> write_packets() {
        try {
            while (open_) {
                if (quic_->expiry() <= timestamp()) {
                    check_quic(quic_->handle_expiry(timestamp()).status_);
                    consume_events();
                }
                std::size_t burst = 0;
                for (; open_ && burst != 32; ++burst) {
                    const auto result = quic_->next_datagram(pool_.config().http3_.max_datagram_bytes_, timestamp(), packet_);
                    consume_events();
                    check_quic(result.status_);
                    if (result.status_ == http3_quic_status::would_block) {
                        break;
                    }
                    asio::ip::udp::endpoint remote;
                    if (packet_.path_.path.remote.addrlen > remote.capacity()) {
                        throw http_client_error(error_code::protocol_error, "invalid QUIC output path");
                    }
                    std::memcpy(remote.data(), packet_.path_.path.remote.addr, packet_.path_.path.remote.addrlen);
                    remote.resize(packet_.path_.path.remote.addrlen);
                    auto* output = socket_.get();
                    if (old_socket_ && packet_.path_.path.local.addrlen == old_local_.size() &&
                        std::memcmp(packet_.path_.path.local.addr, old_local_.data(), old_local_.size()) == 0) {
                        output = old_socket_.get();
                    }
                    sending_socket_ = output;
                    const auto sent = co_await async_asio<std::size_t>([&](auto handler) { output->async_send_to(asio::buffer(packet_.bytes_), remote, std::move(handler)); });
                    sending_socket_ = nullptr;
                    changed_.notify();
                    if (!open_) {
                        break;
                    }
                    if (sent.error_code()) {
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
    }
    task<void> connect(stream& item, std::string_view host, std::uint16_t port, bool allow_early,
        const operation_timeout& acquire_timeout, stop_token stop) {
        const auto slot = co_await connect_slots_.acquire(acquire_timeout.remaining(), stop);
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
        for (const auto* existing : streams_) {
            if (existing != &item && existing->id_ >= 0) {
                throw http_client_error(error_code::protocol_unavailable, "HTTP/3 draining session still has active requests");
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
        close(error_code::io_error, &item);
        while (io_tasks_) {
            check(item);
            co_await changed_.wait();
        }
        check(item);
        quic_.reset();
        crypto_.reset();
        tls_.reset();
        old_socket_.reset();
        socket_.reset();
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
        const auto bound = acquire_timeout.constrained_by(pool_.config().connect_timeout_);
        if (const auto deadline = bound.deadline()) {
            (void)pool_.worker().schedule_timer(timer, *deadline, [this](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    close(error_code::timeout);
                }
            });
        }
        endpoint_host_.assign(host);
        if (!connect_endpoint_) {
            throw std::logic_error("HTTP/3 connect requires a selected resolver endpoint");
        }
        remote_ = *connect_endpoint_;
        socket_ = std::make_unique<asio::ip::udp::socket>(pool_.io_context());
        socket_->open(remote_.protocol());
        socket_->connect(remote_);
        local_ = socket_->local_endpoint();
        tls_.reset(SSL_new(context_.native_handle()));
        const auto identity = pool_.config().tls_.server_name_ ? std::string_view(*pool_.config().tls_.server_name_) : std::string_view(pool_.config().host_);
        if (!tls_ || prepare_client_tls_stream(*tls_, identity, client_alpn::none) != client_tls_setup_error::none ||
            ngtcp2_crypto_ossl_configure_client_session(tls_.get()) != 0) {
            throw http_client_error(error_code::tls_failed, "HTTP/3 TLS setup failed");
        }
        static constexpr std::array<unsigned char, 3> alpn{2, 'h', '3'};
        if (SSL_set_alpn_protos(tls_.get(), alpn.data(), static_cast<unsigned int>(alpn.size())) != 0) {
            throw http_client_error(error_code::tls_failed, "HTTP/3 ALPN setup failed");
        }
        reference_.get_conn = [](ngtcp2_crypto_conn_ref* ref) { return static_cast<session*>(ref->user_data)->quic_->native_handle(); };
        reference_.user_data = this;
        SSL_set_app_data(tls_.get(), &reference_);
        SSL_set_connect_state(tls_.get());
        const bool same_endpoint = ticket_host_ == host && ticket_port_ == port;
        if (ticket_ && same_endpoint) {
            // The next SSL must not mutate the cached copy on its teardown.
            std::unique_ptr<SSL_SESSION, free_ticket> restored(SSL_SESSION_dup(ticket_.get()));
            if (!restored || SSL_set_session(tls_.get(), restored.get()) != 1) {
                ticket_.reset();
            }
        }
        if (!same_endpoint) {
            ticket_.reset();
            ticket_params_size_ = 0;
            ticket_settings_valid_ = false;
        }
        ticket_host_.assign(host);
        ticket_port_ = port;
        ngtcp2_crypto_ossl_ctx* crypto = nullptr;
        if (ngtcp2_crypto_ossl_ctx_new(&crypto, tls_.get()) != 0) {
            throw http_client_error(error_code::tls_failed, "HTTP/3 crypto setup failed");
        }
        crypto_.reset(crypto);
        const auto& config = pool_.config();
        if (config.http3_.max_datagram_bytes_ < 1200 || config.http3_.max_datagram_bytes_ > 65527 ||
            config.http3_.max_concurrent_streams_ > (std::numeric_limits<std::uint32_t>::max)()) {
            throw std::invalid_argument("HTTP/3 transport limits exceed QUIC bounds");
        }
        auto callbacks = crypto_callbacks();
        callbacks.handshake_completed = [](ngtcp2_conn*, void* target) noexcept {
            auto& owner = *static_cast<session*>(target);
            if (selected_client_alpn(owner.tls_.get()) != "h3") {
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
        quic_.emplace(http3_quic_connection::client({.callbacks_ = callbacks, .source_connection_id_ = random_cid(), .destination_connection_id_ = random_cid(), .initial_path_ = path(local_, remote_), .initial_timestamp_ = timestamp(), .handshake_timeout_ = quic_duration(config.connect_timeout_), .idle_timeout_ = 0, .initial_max_data_ = (std::max)(std::uint64_t{64 * 1024}, (std::min)(std::uint64_t{16 * 1024 * 1024}, static_cast<std::uint64_t>(config.max_response_bytes_))), .initial_max_stream_data_ = 64 * 1024, .max_udp_payload_size_ = config.http3_.max_datagram_bytes_, .callback_user_data_ = this, .resource_ = pool_.resource(), .http3_ = {.resource_ = pool_.resource(), .max_concurrent_streams_ = config.http3_.max_concurrent_streams_, .max_stream_output_bytes_ = config.http3_.max_stream_output_bytes_, .enable_h3_datagram_ = true}}));
        quic_->set_tls_native_handle(crypto_.get());
        if (allow_early && config.http3_.enable_early_data_ && ticket_ && ticket_params_size_ && ticket_settings_valid_ && SSL_SESSION_get_max_early_data(ticket_.get()) != 0) {
            if (SSL_set_quic_tls_early_data_enabled(tls_.get(), 1) != 1) {
                throw http_client_error(error_code::tls_failed, "cannot enable QUIC early data");
            }
            early_started_ = quic_->start_early_http3(std::as_bytes(std::span(ticket_params_.data(), ticket_params_size_)), ticket_settings_) == http3_quic_status::accepted;
        }
        ++generation_;
        open_ = true;
        ++io_tasks_;
        try {
            pool_.tasks().spawn(receive(*socket_, local_));
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
        if (quic_->handshake_completed() && selected_client_alpn(tls_.get()) != "h3") {
            throw http_client_error(error_code::protocol_unavailable, "peer did not negotiate HTTP/3");
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
    task<void> send_body(stream& item, std::string_view bytes) {
        item.write_deadline_ = pool_.config().write_timeout_ ? std::optional(worker_timer_deadline_after(*pool_.config().write_timeout_)) : std::nullopt;
        schedule(item);
        while (!bytes.empty()) {
            check(item);
            if (item.retry_ || (item.response_.output() && item.response_.output()->stopped_)) {
                co_return;
            }
            const auto sent = quic_->http3().submit_data(item.id_, std::as_bytes(std::span(bytes.data(), bytes.size())));
            if (sent.status_ != http3_status::accepted && sent.status_ != http3_status::would_block) {
                require_accepted(sent.status_);
            }
            bytes.remove_prefix(sent.accepted_bytes_);
            pool_.wire_counters().sent_ += sent.accepted_bytes_;
            if (sent.accepted_bytes_ != 0 && pool_.config().write_timeout_) {
                item.write_deadline_ = worker_timer_deadline_after(*pool_.config().write_timeout_);
                schedule(item);
            }
            writer_.notify();
            if (!bytes.empty()) {
                co_await item.signal_.wait();
            }
        }
        auto outstanding = quic_->http3().pending_output_bytes(item.id_);
        while (outstanding != 0) {
            check(item);
            if (item.retry_ || (item.response_.output() && item.response_.output()->stopped_)) {
                co_return;
            }
            co_await item.signal_.wait();
            const auto remaining = quic_->http3().pending_output_bytes(item.id_);
            if (remaining < outstanding && pool_.config().write_timeout_) {
                item.write_deadline_ = worker_timer_deadline_after(*pool_.config().write_timeout_);
                schedule(item);
            }
            outstanding = remaining;
        }
        item.write_deadline_.reset();
    }

    http_client_pool& pool_;
    std::size_t index_;
    asio::ssl::context context_;
    pool_lease_scheduler connect_slots_;
    worker_signal changed_;
    worker_signal writer_;
    std::pmr::vector<stream*> streams_;
    std::pmr::string endpoint_host_;
    std::pmr::string ticket_host_;
    std::uint16_t ticket_port_{};
    std::array<std::uint8_t, 4096> ticket_params_{};
    std::size_t ticket_params_size_{};
    http3_peer_settings ticket_settings_{};
    bool ticket_settings_valid_{};
    std::unique_ptr<SSL_SESSION, free_ticket> ticket_;
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
    std::size_t io_tasks_{};
    std::size_t borrowed_operations_{};
    stream* connecting_stream_{};
    bool open_{}, retired_{}, draining_{}, early_started_{}, early_rejected_{}, migrating_{};
    worker_timer_registration expiry_;
    worker_timer_registration idle_timer_;
};

class http3_client_connection::address_race final {
public:
    struct candidate final {
        std::unique_ptr<session> connection_;
        bool running_{false};
    };
    address_race(http_client_pool& pool, std::size_t index)
        : pool_(pool),
          index_(index),
          resolver_(pool.io_context()),
          selection_(1, pool.worker(), pool.resource()),
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
    [[nodiscard]] task<void> run_candidate(std::size_t index, std::string_view host, std::uint16_t port,
        operation_timeout bound, http_client_response_state& response) {
        auto& entry = candidates_[index];
        auto& connection = *entry.connection_;
        session::stream item(connection, response, bound);
        bool registered = false;
        try {
            connection.streams_.push_back(&item);
            registered = true;
            co_await connection.connect(item, host, port, false, bound, {});
            if (!reason_ && !winner_ && !response.abandoned_) {
                winner_ = index;
            }
        } catch (const std::bad_alloc&) {
            failure_ = std::current_exception();
            cancel(error_code::io_error);
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
        (void)pool_.worker().schedule_timer(delay_, worker_timer_deadline_after(client_connection_attempt_delay),
            [this](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    launch_ready_ = true;
                    changed_.notify();
                }
            });
    }
    http_client_pool& pool_;
    std::size_t index_;
    asio::ip::udp::resolver resolver_;
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

task<void> http3_client_connection::select_endpoint(bool allow_early, std::string_view host, std::uint16_t port,
    const operation_timeout& timeout, const operation_timeout& acquire_timeout,
    stop_token stop, http_client_response_state& response) {
    auto& race = *race_;
    const auto acquired = co_await race.selection_.acquire(acquire_timeout.remaining(), stop);
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
        throw http_client_error(error_code::closing, "HTTP/3 client is retired");
    }
    if (response.abandoned_ || stop.stop_requested()) {
        throw http_client_error(error_code::cancelled, "HTTP/3 endpoint selection cancelled");
    }
    if (session_ && (session_->usable() || !session_->streams_.empty())) {
        co_return;
    }
    // A previously authenticated endpoint can send a safe request in 0-RTT.
    // An initial address race never sends request bytes on multiple paths.
    if (session_ && allow_early && race.pool_.config().http3_.enable_early_data_ &&
        session_->ticket_ && session_->connect_endpoint_ && session_->ticket_host_ == host && session_->ticket_port_ == port) {
        co_return;
    }
    race.selecting_ = true;
    race.reason_.reset();
    race.failure_ = nullptr;
    race.last_failure_ = nullptr;
    race.winner_.reset();
    race.launch_ready_ = true;
    worker_cancellation_registration cancellation(race.pool_.cancellation_target(), race.cancellation_id_);
    response.cancellation_id_ = cancellation.id();
    struct response_binding final {
        http_client_response_state& response_;
        ~response_binding() {
            response_.cancellation_id_ = 0;
        }
    } binding{response};
    cancellation.arm(stop);
    const auto bound = timeout.constrained_by(acquire_timeout.remaining()).constrained_by(race.pool_.config().connect_timeout_);
    worker_timer_registration deadline_timer;
    if (const auto deadline = bound.deadline()) {
        (void)race.pool_.worker().schedule_timer(deadline_timer, *deadline,
            [&race](worker_timer_outcome outcome) noexcept { if (outcome == worker_timer_outcome::expired) { race.cancel(error_code::timeout); } });
    }
    std::unique_ptr<session> retained;
    std::exception_ptr failure;
    try {
        if (session_) {
            race.retained_ = session_.get();
            session_->race_signal_ = &race.changed_;
            session_->close();
            while (session_->io_tasks_ != 0 || session_->borrowed_operations_ != 0) {
                co_await race.changed_.wait();
            }
            session_->race_signal_ = nullptr;
            session_->quic_.reset();
            session_->crypto_.reset();
            session_->tls_.reset();
            session_->socket_.reset();
            session_->old_socket_.reset();
            retained = std::move(session_);
        }
        if (race.reason_) {
            throw http_client_error(*race.reason_, "HTTP/3 endpoint selection stopped");
        }
        const auto resolved = co_await async_asio<asio::ip::udp::resolver::results_type>([&](auto handler) {
            race.resolver_.async_resolve(host, std::to_string(port), std::move(handler));
        });
        if (race.reason_) {
            throw http_client_error(*race.reason_, "HTTP/3 resolution stopped");
        }
        if (resolved.error_code() || resolved.result().empty()) {
            throw http_client_error(error_code::resolve_failed, "HTTP/3 DNS resolution failed");
        }
        client_address_sequence sequence(resolved.result());
        auto counter = sequence;
        std::size_t remaining = 0;
        while (counter.next()) {
            ++remaining;
        }
        auto next = sequence.next();
        while (!race.winner_ && !race.reason_ && !race.failure_) {
            if (response.abandoned_) {
                race.cancel(error_code::cancelled);
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
                        entry.connection_ = std::make_unique<session>(race.pool_, race.index_);
                    }
                    entry.connection_->connect_endpoint_ = *next;
                    entry.connection_->race_signal_ = &race.changed_;
                    // A stagger is not a handshake timeout. Divide the fixed
                    // remaining deadline across remaining two-candidate waves;
                    // the last wave gets all remaining time.
                    std::size_t live = 0;
                    for (const auto& other : race.candidates_) {
                        live += other.running_;
                    }
                    const auto waves = (remaining + live + 1) / 2;
                    const auto left = bound.remaining().value_or(race.pool_.config().connect_timeout_);
                    const auto divisor = static_cast<std::int64_t>((std::min)(waves, static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())));
                    const auto slice = (std::max)(client_connection_attempt_delay, left / (std::max)(divisor, std::int64_t{1}));
                    const auto candidate_bound = bound.constrained_by(slice);
                    entry.running_ = true;
                    try {
                        race.pool_.tasks().spawn(race.run_candidate(index, host, port, candidate_bound, response));
                    } catch (...) {
                        entry.running_ = false;
                        entry.connection_->close();
                        throw;
                    }
                    --remaining;
                    next = sequence.next();
                    race.launch_ready_ = false;
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
            throw http_client_error(error_code::connect_failed, "no HTTP/3 endpoint completed its handshake");
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
        throw http_client_error(race.retired_ ? error_code::closing : race.reason_.value_or(error_code::cancelled),
            "HTTP/3 endpoint selection stopped");
    }
    if (failure) {
        if (!session_) {
            session_ = std::move(retained);
        }
        try {
            std::rethrow_exception(failure);
        } catch (const std::system_error&) {
            throw http_client_error(error_code::connect_failed, "HTTP/3 endpoint connection failed");
        }
    }
}

bool http3_client_connection::active() const noexcept {
    return session_ && session_->usable() && session_->quic_->handshake_completed();
}

http3_client_connection::http3_client_connection(http_client_pool& pool, std::size_t index)
    : session_(std::make_unique<session>(pool, index)),
      race_(std::make_unique<address_race>(pool, index)) {}
http3_client_connection::~http3_client_connection() = default;
void http3_client_connection::close() noexcept {
    race_->cancel(error_code::io_error);
    if (session_) {
        session_->close();
    }
}
void http3_client_connection::shutdown() noexcept {
    race_->retired_ = true;
    (void)race_->selection_.close();
    race_->cancel(error_code::closing);
    if (session_) {
        session_->retired_ = true;
        (void)session_->connect_slots_.close();
        session_->close(error_code::closing);
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
        race_->cancel(error_code::cancelled);
        return true;
    }
    if (!session_) {
        return false;
    }
    for (auto* item : session_->streams_) {
        if (item->cancellation_id_ == id) {
            session_->cancel(*item, error_code::cancelled);
            return true;
        }
    }
    return false;
}
void http3_client_connection::abandon(http_client_response_state& response, bool malformed) noexcept {
    if (race_->selecting_ && race_->cancellation_id_ == response.cancellation_id_) {
        race_->cancel(error_code::cancelled);
        return;
    }
    if (!session_) {
        return;
    }
    auto& self = *session_;
    for (auto* item : self.streams_) {
        if (&item->response_ == &response) {
            self.cancel(*item, error_code::cancelled);
            if (malformed && item->id_ >= 0 && self.quic_) {
                self.quic_->reset_stream(item->id_, static_cast<std::uint64_t>(http3_error_code::message_error));
            }
            return;
        }
    }
}
void http3_client_connection::release_data(http_client_response_state& response) noexcept {
    if (session_) {
        session_->release(response);
    } else {
        response.http3_data_credit_ = 0;
    }
}

task<bool> http3_client_connection::execute(const http_client_request_storage& request,
    std::string_view host, std::uint16_t port, bool fallback, const operation_timeout& timeout,
    const operation_timeout& acquire_timeout, stop_token stop, http_client_response_state& response) {
    const bool may_send_early = safe_early_method(request.method()) && !request.has_body() &&
                                request.output() == nullptr && !request.is_tunnel();
    try {
        co_await select_endpoint(may_send_early, host, port, timeout, acquire_timeout, stop, response);
    } catch (...) {
        const auto code = http_client_failure_code(std::current_exception());
        const bool endpoint_failure = code == error_code::io_error || code == error_code::connect_failed ||
                                      code == error_code::resolve_failed || code == error_code::tls_failed ||
                                      code == error_code::protocol_unavailable || code == error_code::timeout;
        if (fallback && endpoint_failure && !stop.stop_requested() && !response.abandoned_ && !timeout.expired() && !race_->retired_) {
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
    response.request_method_ = classify_http_method(request.method());
    struct registration final {
        session& owner_;
        session::stream& item_;
        ~registration() {
            item_.timer_.cancel();
            owner_.release(item_.response_);
            if (item_.id_ >= 0 && !item_.done_ && owner_.quic_ && item_.response_.connection_generation_ == owner_.generation_) {
                owner_.quic_->reset_stream(item_.id_, static_cast<std::uint64_t>(http3_error_code::request_cancelled));
                owner_.writer_.notify();
            }
            if (auto* output = item_.response_.output()) {
                output->wake_ = nullptr;
                output->wake_target_ = nullptr;
            }
            item_.response_.cancellation_id_ = 0;
            std::erase(owner_.streams_, &item_);
            owner_.changed_.notify();
            owner_.begin_idle();
        }
    } registered{self, item};
    worker_cancellation_registration cancellation(self.pool_.cancellation_target(), item.cancellation_id_);
    response.cancellation_id_ = cancellation.id();
    cancellation.arm(stop);
    self.schedule(item);
    try {
        co_await self.connect(item, host, port, may_send_early, acquire_timeout, stop);
    } catch (...) {
        const auto failure_code = http_client_failure_code(std::current_exception());
        const bool endpoint_failure = failure_code == error_code::io_error || failure_code == error_code::tls_failed ||
                                      failure_code == error_code::resolve_failed || failure_code == error_code::connect_failed ||
                                      failure_code == error_code::protocol_unavailable || failure_code == error_code::timeout;
        if (fallback && endpoint_failure && !stop.stop_requested() && !response.abandoned_ && !timeout.expired() && !self.retired_) {
            response.transport_ = http_client_response_transport::unassigned;
            co_return false;
        }
        throw;
    }
    response.connection_generation_ = self.generation_;
    if (request.udp_tunnel()) {
        response.http3_datagrams_.emplace(response.resource_);
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
                throw http_client_error(error_code::protocol_unavailable, "peer disabled extended CONNECT");
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
                throw http_client_error(error_code::timeout, "HTTP/3 stream admission timed out");
            }
            if (!self.usable()) {
                throw http_client_error(error_code::protocol_unavailable, "HTTP/3 session is draining");
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
        response.stream_id_ = static_cast<std::uint64_t>(item.id_);
        item.early_ = !self.quic_->handshake_completed();
        client_prepared_request_scratch scratch(self.pool_.resource());
        const auto prepared = self.pool_.policy().prepare_request(request, scratch);
        std::pmr::vector<http3_field_view> fields(self.pool_.resource());
        fields.reserve(static_cast<std::span<const http_header_view>>(prepared.headers_).size() + 12);
        fields.push_back({":method", request.is_tunnel() ? std::string_view("CONNECT") : request.method()});
        fields.push_back({":authority", request.is_tunnel() ? request.tunnel_authority() : std::string_view(self.pool_.config().authority_)});
        if (!request.is_tunnel() || !request.tunnel_protocol().empty()) {
            fields.push_back({":scheme", "https"});
            fields.push_back({":path", request.target()});
        }
        if (request.is_tunnel() && !request.tunnel_protocol().empty()) {
            fields.push_back({":protocol", request.tunnel_protocol()});
        }
        for (const auto& field : static_cast<std::span<const http_header_view>>(prepared.headers_)) {
            if (field.name() == "host" || field.name() == "content-length") {
                continue;
            }
            fields.push_back({field.name(), field.value(), field.name() == "authorization" || field.name() == "cookie"});
        }
        if (request.websocket()) {
            for (const auto& field : request.websocket()->negotiation_.request_fields()) {
                fields.push_back({field.name(), field.value()});
            }
        }
        std::array<char, 32> length{};
        std::optional<std::uint64_t> content_length;
        if (request.upload()) {
            if (const auto declared = request.upload()->config_.content_length_) {
                content_length = declared->value();
            }
        } else if (request.has_body()) {
            content_length = request.body().size();
        }
        if (content_length && !request.is_tunnel()) {
            const auto text = std::to_chars(length.data(), length.data() + length.size(), *content_length);
            fields.push_back({"content-length", std::string_view(length.data(), static_cast<std::size_t>(text.ptr - length.data()))});
        }
        if (request.upload() && request.upload()->config_.expectation_ != http_client_request_expectation::none) {
            fields.push_back({"expect", "100-continue"});
        }
        const bool streaming = request.output() != nullptr || !request.body().empty();
        try {
            require_accepted(self.quic_->http3().submit_request(item.id_, fields, !streaming));
            self.writer_.notify();
            if (!request.body().empty()) {
                co_await self.send_body(item, request.body());
                if (!item.retry_) {
                    require_accepted(self.quic_->http3().finish_stream(item.id_));
                    self.writer_.notify();
                }
            }
            self.read_progress(item);
            if (auto* output = request.output()) {
                output->wake_target_ = &item.signal_;
                output->wake_ = [](void* target) noexcept { static_cast<worker_signal*>(target)->notify(); };
                worker_timer_registration continue_timer;
                if (request.upload() && !request.upload()->content_released_) {
                    (void)self.pool_.worker().schedule_timer(continue_timer, worker_timer_deadline_after(request.upload()->config_.continue_timeout_),
                        [&item, upload = request.upload()](worker_timer_outcome outcome) noexcept {
                            if (outcome == worker_timer_outcome::expired) {
                                upload->content_released_ = true;
                                item.signal_.notify();
                            }
                        });
                }
                while (!output->ended_ && !output->stopped_) {
                    self.check(item);
                    if (request.is_tunnel() ? !request.tunnel()->accepted_ : !request.upload()->content_released_) {
                        co_await item.signal_.wait();
                        continue;
                    }
                    continue_timer.cancel();
                    if (output->chunk_ready_) {
                        co_await self.send_body(item, output->chunk_);
                        if (!output->stopped_) {
                            output->acknowledge_chunk();
                        }
                    } else if (output->end_requested_) {
                        if (request.upload() && !request.upload()->trailers_.empty()) {
                            std::pmr::vector<http3_field_view> trailers(self.pool_.resource());
                            for (const auto& field : request.upload()->trailers_) {
                                trailers.push_back({field.name(), field.value()});
                            }
                            require_accepted(self.quic_->http3().submit_trailers(item.id_, trailers));
                        } else {
                            require_accepted(self.quic_->http3().finish_stream(item.id_));
                        }
                        output->finish();
                        self.writer_.notify();
                    } else {
                        co_await item.signal_.wait();
                    }
                }
            }
            while (!item.done_ && !item.retry_) {
                self.check(item);
                co_await item.signal_.wait();
            }
            self.check(item);
        } catch (...) {
            self.quic_->reset_stream(item.id_, static_cast<std::uint64_t>(http3_error_code::request_cancelled));
            self.writer_.notify();
            throw;
        }
        if (!item.retry_) {
            co_return true;
        }
        if (!may_send_early || attempt != 0) {
            throw http_client_error(error_code::protocol_error, "HTTP/3 early request replay exhausted");
        }
        if (!self.early_rejected_) {
            self.quic_->reset_stream(item.id_, static_cast<std::uint64_t>(http3_error_code::request_cancelled));
        }
        item.id_ = -1;
        item.done_ = false;
        item.retry_ = false;
        item.retry_425_ = false;
        response.headers_.clear();
        response.trailers_.clear();
        response.informational_.clear();
        self.release(response);
        response.discard_response_body();
    }
    throw http_client_error(error_code::protocol_error, "HTTP/3 request attempts exhausted");
}

task<void> http3_client_connection::migrate(std::string local_address, std::uint16_t port) {
    if (!session_) {
        throw http_client_error(error_code::protocol_unavailable, "HTTP/3 endpoint selection is in progress");
    }
    auto& self = *session_;
    session::operation_lease borrowed(self);
    if (!self.usable() || !self.quic_->handshake_completed()) {
        throw http_client_error(error_code::protocol_unavailable, "migration requires an authenticated active HTTP/3 connection");
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
    auto replacement = std::make_unique<asio::ip::udp::socket>(self.pool_.io_context());
    const asio::ip::udp::endpoint requested(asio::ip::make_address(local_address), port);
    replacement->open(requested.protocol());
    replacement->bind(requested);
    replacement->connect(self.remote_);
    auto local = replacement->local_endpoint();
    bool expired = false;
    worker_timer_registration timer;
    (void)self.pool_.worker().schedule_timer(timer, worker_timer_deadline_after(self.pool_.config().connect_timeout_),
        [&self, &expired](worker_timer_outcome outcome) noexcept { if (outcome == worker_timer_outcome::expired) { expired = true; self.changed_.notify(); } });
    for (;;) {
        if (!self.open_ || expired) {
            self.close();
            throw http_client_error(expired ? error_code::timeout : error_code::io_error, "HTTP/3 migration connection ID unavailable");
        }
        const auto result = self.quic_->migrate(self.path(local, self.remote_), timestamp());
        if (result.status_ == http3_quic_status::accepted) {
            break;
        }
        if (result.status_ != http3_quic_status::would_block) {
            throw http_client_error(error_code::protocol_unavailable, "peer or QUIC state forbids active migration");
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
    ++self.io_tasks_;
    try {
        self.pool_.tasks().spawn(self.receive(*self.socket_, self.local_));
    } catch (...) {
        --self.io_tasks_;
        self.close();
        throw;
    }
    self.writer_.notify();
    for (;;) {
        if (!self.open_ || expired) {
            self.close();
            throw http_client_error(expired ? error_code::timeout : error_code::io_error, "HTTP/3 path validation failed");
        }
        const auto* active = ngtcp2_conn_get_path(self.quic_->native_handle());
        if (active->local.addrlen == self.local_.size() && std::memcmp(active->local.addr, self.local_.data(), self.local_.size()) == 0) {
            break;
        }
        co_await self.changed_.wait();
    }
    while (self.sending_socket_ == self.old_socket_.get()) {
        if (!self.open_ || expired) {
            self.close();
            throw http_client_error(error_code::timeout, "HTTP/3 old path output did not retire");
        }
        co_await self.changed_.wait();
    }
    std::error_code ignored;
    self.old_socket_->close(ignored);
    while (self.io_tasks_ > 2) {
        co_await self.changed_.wait();
    }
    self.old_socket_.reset();
}

task<bool> http3_client_connection::send_datagram(http_client_response_state& response, std::span<const std::byte> bytes) {
    if (!session_) {
        throw http_client_error(error_code::closing, "HTTP/3 datagram session is retired");
    }
    auto& self = *session_;
    session::operation_lease borrowed(self);
    auto* item = self.find(static_cast<std::int64_t>(response.stream_id_));
    if (!item || &item->response_ != &response || response.connection_generation_ != self.generation_ || !response.tunnel_ || !response.tunnel_->accepted_) {
        throw http_client_error(error_code::closing, "HTTP/3 datagram stream is closed");
    }
    if (bytes.size() > self.quic_->max_datagram_payload_size(item->id_) || !response.http3_native_datagrams_) {
        co_return false;
    }
    const auto bound = item->timeout_.constrained_by(self.pool_.config().write_timeout_);
    worker_timer_registration timer;
    if (const auto deadline = bound.deadline()) {
        (void)self.pool_.worker().schedule_timer(timer, *deadline, [&self](worker_timer_outcome outcome) noexcept { if (outcome == worker_timer_outcome::expired) { self.changed_.notify(); } });
    }
    for (;;) {
        item = self.find(static_cast<std::int64_t>(response.stream_id_));
        if (!item || &item->response_ != &response || response.connection_generation_ != self.generation_) {
            response.rethrow_response_failure();
            throw http_client_error(error_code::closing, "HTTP/3 datagram stream retired");
        }
        self.check(*item);
        if (bound.expired()) {
            self.cancel(*item, error_code::timeout);
            throw http_client_error(error_code::timeout, "HTTP/3 datagram output timeout");
        }
        const auto status = self.quic_->submit_datagram(item->id_, bytes);
        if (status == http3_quic_status::accepted) {
            self.writer_.notify();
            co_return true;
        }
        if (status != http3_quic_status::would_block) {
            throw http_client_error(error_code::protocol_error, "HTTP/3 datagram submission failed");
        }
        co_await self.changed_.wait();
    }
}

void http3_client_connection::reprioritize(http_client_response_state& response, http_priority priority) {
    if (!session_) {
        throw http_client_error(error_code::closing, "HTTP/3 response connection is retired");
    }
    auto& self = *session_;
    if (response.connection_generation_ != self.generation_ || !self.open_ || !self.quic_ || self.quic_->closed()) {
        throw http_client_error(error_code::closing, "HTTP/3 response connection is retired");
    }
    auto* item = self.find(static_cast<std::int64_t>(response.stream_id_));
    if (item == nullptr || &item->response_ != &response || item->error_ || item->failure_) {
        throw http_client_error(error_code::closing, "HTTP/3 response connection is retired");
    }
    require_accepted(self.quic_->http3().submit_priority_update(
        item->id_, {.urgency_ = priority.urgency_, .incremental_ = priority.incremental_}));
    self.writer_.notify();
}

}  // namespace ruvia::detail
#endif
