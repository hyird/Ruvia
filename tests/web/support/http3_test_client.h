#pragma once

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/udp.hpp>
#include <asio/socket_base.hpp>
#include <asio/steady_timer.hpp>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include "ruvia/http/http3_quic_connection.h"

namespace ruvia::test {

using http3_test_fields = std::vector<std::pair<std::string, std::string>>;

struct http3_test_resumption final {
    std::shared_ptr<SSL_SESSION> session_;
    std::vector<std::byte> transport_parameters_;
    http3_peer_settings settings_;

    [[nodiscard]] bool permits_early_data() const noexcept {
        return session_ && SSL_SESSION_get_max_early_data(session_.get()) != 0;
    }
};
struct http3_test_response final {
    // The final status; zero until the final response head arrives.
    int status_{0};
    // Regular fields of the final head, as received (lowercase names).
    http3_test_fields headers_;
    std::string body_;
    http3_test_fields trailers_;
    std::vector<int> interim_statuses_;
    // The application error code of a stream that ended by RESET_STREAM or
    // STOP_SENDING instead of completing (RFC 9114 §4.1.1).
    std::optional<std::uint64_t> reset_error_;

    // The first field named `name`, compared ASCII case-insensitively.
    [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
        for (const auto& [field_name, value] : headers_) {
            if (std::equal(field_name.begin(), field_name.end(), name.begin(), name.end(), [](char left, char right) {
                    return ascii_lower(left) == ascii_lower(right);
                })) {
                return value;
            }
        }
        return std::nullopt;
    }

private:
    [[nodiscard]] static char ascii_lower(char value) noexcept {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
    }
};

inline void http3_test_fill_random(std::uint8_t* destination, std::size_t size, const ngtcp2_rand_ctx*) {
    if (RAND_bytes(destination, static_cast<int>(size)) != 1) {
        std::terminate();
    }
}

inline int http3_test_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token,
    std::size_t size, void*) {
    if (RAND_bytes(cid->data, static_cast<int>(size)) != 1 ||
        RAND_bytes(token->data, NGTCP2_STATELESS_RESET_TOKENLEN) != 1) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    cid->datalen = size;
    return 0;
}

[[nodiscard]] inline ngtcp2_cid http3_test_random_connection_id(std::size_t size) {
    ngtcp2_cid cid{};
    if (size > NGTCP2_MAX_CIDLEN || RAND_bytes(cid.data, static_cast<int>(size)) != 1) {
        throw std::runtime_error("cannot generate a QUIC connection ID");
    }
    cid.datalen = size;
    return cid;
}

// The ngtcp2 crypto callbacks of a client using ngtcp2_crypto_ossl.
[[nodiscard]] inline ngtcp2_callbacks http3_test_client_callbacks() noexcept {
    ngtcp2_callbacks callbacks{};
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    callbacks.rand = http3_test_fill_random;
    callbacks.get_new_connection_id2 = http3_test_new_connection_id;
    return callbacks;
}

// The client TLS side of one QUIC connection: TLS 1.3 only, ALPN "h3" only,
// and the server certificate verified against `ca_file` for `server_name`,
// which is also sent as SNI unless it is an IP literal (RFC 6066 §3 allows
// host names only; a literal is matched against an IP subjectAltName).
class http3_test_client_tls final {
public:
    http3_test_client_tls(const std::filesystem::path& ca_file, const std::string& server_name,
        const http3_test_resumption* resumption = nullptr, bool early_data = false) {
        context_.reset(SSL_CTX_new(TLS_client_method()));
        if (!context_ || SSL_CTX_set_min_proto_version(context_.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_load_verify_locations(context_.get(), ca_file.string().c_str(), nullptr) != 1) {
            throw std::runtime_error("HTTP/3 test client cannot load its trust anchor");
        }
        SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_session_cache_mode(context_.get(), SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_set_app_data(context_.get(), this);
        SSL_CTX_sess_set_new_cb(context_.get(), capture_ticket);
        tls_.reset(SSL_new(context_.get()));
        if (!tls_ || ngtcp2_crypto_ossl_configure_client_session(tls_.get()) != 0) {
            throw std::runtime_error("HTTP/3 test client cannot create its TLS session");
        }
        SSL_set_app_data(tls_.get(), &reference_);
        SSL_set_connect_state(tls_.get());
        static constexpr std::array<unsigned char, 3> h3_alpn{2, 'h', '3'};
        std::error_code literal_error;
        static_cast<void>(asio::ip::make_address(server_name, literal_error));
        const bool configured =
            SSL_set_alpn_protos(tls_.get(), h3_alpn.data(), static_cast<unsigned int>(h3_alpn.size())) == 0 &&
            (literal_error ? SSL_set_tlsext_host_name(tls_.get(), server_name.c_str()) == 1 &&
                                 SSL_set1_host(tls_.get(), server_name.c_str()) == 1
                           : X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(tls_.get()), server_name.c_str()) == 1);
        ngtcp2_crypto_ossl_ctx* crypto = nullptr;
        if (!configured || ngtcp2_crypto_ossl_ctx_new(&crypto, tls_.get()) != 0) {
            throw std::runtime_error("HTTP/3 test client cannot configure TLS");
        }
        crypto_.reset(crypto);
        if (resumption != nullptr && resumption->session_) {
            std::unique_ptr<SSL_SESSION, free_session> restored(SSL_SESSION_dup(resumption->session_.get()));
            if (!restored || SSL_set_session(tls_.get(), restored.get()) != 1 ||
                SSL_set_quic_tls_early_data_enabled(tls_.get(),
                    early_data && resumption->permits_early_data() ? 1 : 0) != 1) {
                throw std::runtime_error("HTTP/3 test client cannot restore its TLS ticket");
            }
        }
    }

    ~http3_test_client_tls() {
        SSL_set_app_data(tls_.get(), nullptr);
    }

    http3_test_client_tls(const http3_test_client_tls&) = delete;
    http3_test_client_tls& operator=(const http3_test_client_tls&) = delete;
    http3_test_client_tls(http3_test_client_tls&&) = delete;
    http3_test_client_tls& operator=(http3_test_client_tls&&) = delete;

    // Attaches this session to `connection`, whose address must stay stable
    // and which must be destroyed before this session.
    void bind(http3_quic_connection& connection) noexcept {
        reference_.get_conn = get_connection;
        reference_.user_data = &connection;
        connection.set_tls_native_handle(crypto_.get());
    }

    [[nodiscard]] bool h3_selected() const noexcept {
        const unsigned char* protocol = nullptr;
        unsigned int protocol_size = 0;
        SSL_get0_alpn_selected(tls_.get(), &protocol, &protocol_size);
        return std::string_view(reinterpret_cast<const char*>(protocol), protocol_size) == "h3";
    }

    // X509_V_OK unless certificate verification failed.
    [[nodiscard]] long verify_result() const noexcept {
        return SSL_get_verify_result(tls_.get());
    }

    [[nodiscard]] bool has_ticket() const noexcept {
        return ticket_ != nullptr;
    }

    [[nodiscard]] std::shared_ptr<SSL_SESSION> ticket() const {
        auto* copy = ticket_ ? SSL_SESSION_dup(ticket_.get()) : nullptr;
        if (copy == nullptr) {
            throw std::runtime_error("HTTP/3 test client cannot retain its resumption ticket");
        }
        return std::shared_ptr<SSL_SESSION>(copy, SSL_SESSION_free);
    }

    [[nodiscard]] bool resumed() const noexcept {
        return SSL_session_reused(tls_.get()) == 1;
    }

    [[nodiscard]] int early_data_status() const noexcept {
        return SSL_get_early_data_status(tls_.get());
    }

private:
    struct free_context final {
        void operator()(SSL_CTX* context) const noexcept {
            SSL_CTX_free(context);
        }
    };
    struct free_tls final {
        void operator()(SSL* tls) const noexcept {
            SSL_free(tls);
        }
    };
    struct free_crypto final {
        void operator()(ngtcp2_crypto_ossl_ctx* crypto) const noexcept {
            ngtcp2_crypto_ossl_ctx_del(crypto);
        }
    };
    struct free_session final {
        void operator()(SSL_SESSION* session) const noexcept {
            SSL_SESSION_free(session);
        }
    };

    static int capture_ticket(SSL* ssl, SSL_SESSION* session) noexcept {
        auto* self = static_cast<http3_test_client_tls*>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
        if (SSL_SESSION_up_ref(session) != 1) {
            return 0;
        }
        self->ticket_.reset(session);
        return 0;
    }

    static ngtcp2_conn* get_connection(ngtcp2_crypto_conn_ref* reference) {
        return static_cast<http3_quic_connection*>(reference->user_data)->native_handle();
    }

    std::unique_ptr<SSL_CTX, free_context> context_;
    std::unique_ptr<SSL, free_tls> tls_;
    std::unique_ptr<ngtcp2_crypto_ossl_ctx, free_crypto> crypto_;
    ngtcp2_crypto_conn_ref reference_{};
    std::unique_ptr<SSL_SESSION, free_session> ticket_;
};

// Blocking HTTP/3 client for loopback tests against any compliant server.
// Construction normally completes the QUIC handshake (TLS 1.3, ALPN "h3",
// verified certificate). With a resumable early-data ticket it returns before
// receiving any packet so start_request() really sends 0-RTT. Every wait is
// bounded by timeout, and QUIC timers run from expiry() while waiting.
// Received DATA returns credit only when taken, providing real backpressure.
class http3_test_client final {
public:
    http3_test_client(asio::ip::udp::endpoint server, const std::filesystem::path& ca_file,
        std::string server_name, std::chrono::milliseconds timeout,
        const http3_test_resumption* resumption = nullptr, bool early_data = false, bool datagrams = true)
        : timeout_(timeout),
          server_name_(std::move(server_name)),
          server_(server),
          socket_(io_),
          tls_(ca_file, server_name_, resumption, early_data),
          input_(64 * 1024) {
        // A connected socket receives only the server's datagrams and reports
        // ICMP port-unreachable, so a missing server fails fast.
        socket_.open(server_.protocol());
        socket_.connect(server_);
        socket_.non_blocking(true);
        local_ = socket_.local_endpoint();
        auto callbacks = http3_test_client_callbacks();
        callbacks.handshake_completed = handshake_complete;
        callbacks.handshake_confirmed = handshake_confirmed;
        connection_.emplace(http3_quic_connection::client({
            .callbacks_ = callbacks,
            .source_connection_id_ = http3_test_random_connection_id(16),
            // RFC 9000 §7.2: at least 8 unpredictable bytes.
            .destination_connection_id_ = http3_test_random_connection_id(18),
            .initial_path_ = path(),
            .initial_timestamp_ = timestamp(),
            .callback_user_data_ = this,
            .http3_ = {.max_stream_output_bytes_ = max_stream_output_bytes,
                .enable_connect_protocol_ = true,
                .enable_h3_datagram_ = datagrams},
        }));
        tls_.bind(*connection_);

        early_attempted_ = early_data && resumption != nullptr && resumption->permits_early_data();
        if (early_attempted_) {
            if (connection_->start_early_http3(resumption->transport_parameters_, resumption->settings_) != http3_quic_status::accepted) {
                throw std::runtime_error("HTTP/3 test client cannot restore early transport settings");
            }
        } else {
            complete_handshake();
        }
    }

    void complete_handshake() {
        if (!wait_until([this] {
                return connection_->handshake_completed() &&
                       connection_->start_http3() == http3_quic_status::accepted;
            },
                timeout_)) {
            const auto verify = tls_.verify_result();
            throw std::runtime_error(std::string("HTTP/3 test client handshake failed: ") +
                                     (verify != X509_V_OK ? X509_verify_cert_error_string(verify)
                                                          : describe_wait_failure()));
        }
        if (!tls_.h3_selected()) {
            throw std::runtime_error("HTTP/3 test client: server did not select ALPN h3");
        }
    }

    [[nodiscard]] http3_test_resumption capture_resumption() {
        complete_handshake();
        wait_or_throw([this] { return tls_.has_ticket(); }, "HTTP/3 TLS ticket did not arrive");
        std::array<std::uint8_t, 512> parameters{};
        const auto size = ngtcp2_conn_encode_0rtt_transport_params2(
            connection_->native_handle(), parameters.data(), parameters.size());
        if (size < 0) {
            throw std::runtime_error("HTTP/3 test client cannot remember QUIC parameters");
        }
        const auto bytes = std::as_bytes(std::span(parameters.data(), static_cast<std::size_t>(size)));
        return {tls_.ticket(), {bytes.begin(), bytes.end()}, peer_settings()};
    }

    [[nodiscard]] bool resumed() const noexcept {
        return tls_.resumed();
    }

    [[nodiscard]] bool early_data_attempted() const noexcept {
        return early_attempted_;
    }

    [[nodiscard]] bool early_data_accepted() const noexcept {
        return connection_->handshake_completed() && tls_.early_data_status() == SSL_EARLY_DATA_ACCEPTED;
    }

    [[nodiscard]] bool early_data_rejected() const noexcept {
        return early_rejected_;
    }

    ~http3_test_client() {
        if (!connection_->closed()) {
            try {
                close();
            } catch (...) {
                // The peer may already be gone; closing is best effort here.
            }
        }
    }

    http3_test_client(const http3_test_client&) = delete;
    http3_test_client& operator=(const http3_test_client&) = delete;
    http3_test_client(http3_test_client&&) = delete;
    http3_test_client& operator=(http3_test_client&&) = delete;

    // Sends one request with :scheme https and :authority server_name:port and
    // waits for its complete response, or for the stream's reset. Header
    // names must be lowercase (RFC 9114 §4.2). A non-empty body is sent as
    // DATA within the stream's output budget; add content-length to declare it.
    [[nodiscard]] http3_test_response request(std::string_view method, std::string_view path,
        const http3_test_fields& headers = {}, std::string_view body = {}) {
        return finish_response(open_request(method, path, headers, body, true));
    }

    // Sends a request like request() and returns its stream without waiting
    // for the response; read it with wait_for_response_head(),
    // read_body_chunk() and finish_response().
    [[nodiscard]] std::int64_t start_request(std::string_view method, std::string_view path,
        const http3_test_fields& headers = {}, std::string_view body = {}) {
        return open_request(method, path, headers, body, false);
    }

    // Keep the server handshake incomplete until the request flight is sent.
    // Merely enabling TLS early data does not prevent pacing from deferring
    // request HEADERS until after the normal receive pump completes the handshake.
    [[nodiscard]] std::int64_t start_early_request(std::string_view method, std::string_view path) {
        if (!early_attempted_ || connection_->handshake_completed()) {
            throw std::logic_error("HTTP/3 early request requires an uncompleted resumed handshake");
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout_;
        flush();
        // A server flight proves its Initial has established the CID mapping,
        // including the Windows acceptor-to-worker handoff. Do not consume it.
        wait_readable(deadline);
        if (socket_.available() == 0) {
            throw std::runtime_error("HTTP/3 server Initial did not arrive");
        }
        const auto stream_id = open_request(method, path, {}, {}, false);
        asio::steady_timer pacing(io_);
        for (;;) {
            http3_stream_output pending;
            const auto result = connection_->http3().peek_output(max_udp_payload_bytes, pending);
            if (result.status_ == http3_status::would_block) {
                return stream_id;
            }
            if (result.status_ != http3_status::accepted || connection_->closed() ||
                std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("HTTP/3 early request flight did not drain");
            }
            auto wake = deadline;
            const auto expiry = connection_->expiry();
            if (expiry < static_cast<ngtcp2_tstamp>((std::numeric_limits<std::int64_t>::max)())) {
                wake = (std::min)(wake, std::chrono::steady_clock::time_point(
                                            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                std::chrono::nanoseconds(static_cast<std::int64_t>(expiry)))));
            }
            pacing.expires_at(wake);
            pacing.wait();
            if (timestamp() >= connection_->expiry()) {
                static_cast<void>(connection_->handle_expiry(timestamp()));
                drain_events();
            }
            flush();
        }
    }

    // Complete pseudo-header list, including classic or extended CONNECT.
    // The request remains open for bidirectional DATA until finish=true.
    [[nodiscard]] std::int64_t start_tunnel(const http3_test_fields& headers) {
        std::optional<std::int64_t> opened;
        wait_or_throw([&] {
            opened = connection_->open_request_stream();
            return opened.has_value();
        },
            "HTTP/3 tunnel stream did not open");
        streams_.try_emplace(*opened);
        std::vector<http3_field_view> fields;
        fields.reserve(headers.size());
        for (const auto& [name, value] : headers) {
            fields.push_back({name, value});
        }
        if (connection_->http3().submit_request(*opened, fields, false) != http3_status::accepted) {
            throw std::runtime_error("HTTP/3 tunnel request was not accepted");
        }
        flush();
        return *opened;
    }

    void send_stream_data(std::int64_t stream_id, std::span<const std::byte> bytes, bool finish = false) {
        auto& stream = tracked_stream(stream_id);
        auto& codec = connection_->http3();
        while (!bytes.empty()) {
            const auto submitted = codec.submit_data(stream_id, bytes);
            if (submitted.status_ == http3_status::accepted && submitted.accepted_bytes_ != 0) {
                bytes = bytes.subspan(submitted.accepted_bytes_);
            } else if (submitted.status_ == http3_status::would_block) {
                wait_or_throw([&] {
                    return codec.pending_output_bytes(stream_id) < max_stream_output_bytes || response_complete(stream);
                },
                    "HTTP/3 tunnel write did not progress");
                if (response_complete(stream)) {
                    throw std::runtime_error("HTTP/3 tunnel closed during write");
                }
            } else {
                throw std::runtime_error("HTTP/3 tunnel DATA was not accepted");
            }
        }
        if (finish && codec.finish_stream(stream_id) != http3_status::accepted) {
            throw std::runtime_error("HTTP/3 tunnel FIN was not accepted");
        }
        flush();
    }

    void send_datagram(std::int64_t stream_id, std::span<const std::byte> payload) {
        wait_or_throw([&] {
            const auto result = connection_->submit_datagram(stream_id, payload);
            if (result == http3_quic_status::would_block) {
                return false;
            }
            if (result != http3_quic_status::accepted) {
                throw std::runtime_error("HTTP/3 DATAGRAM was not accepted");
            }
            return true;
        },
            "HTTP/3 DATAGRAM output did not progress");
        flush();
    }

    [[nodiscard]] std::string receive_datagram(std::int64_t stream_id) {
        auto& stream = tracked_stream(stream_id);
        wait_or_throw([&] { return !stream.datagrams_.empty() || response_complete(stream); },
            "HTTP/3 DATAGRAM did not arrive");
        if (stream.datagrams_.empty()) {
            throw std::runtime_error("HTTP/3 tunnel closed before DATAGRAM");
        }
        auto payload = std::move(stream.datagrams_.front());
        stream.datagrams_.erase(stream.datagrams_.begin());
        return payload;
    }

    [[nodiscard]] http3_peer_settings peer_settings() {
        wait_or_throw([this] { return connection_->http3().peer_settings().has_value(); },
            "HTTP/3 peer SETTINGS did not arrive");
        return *connection_->http3().peer_settings();
    }

    [[nodiscard]] std::uint16_t local_port() const {
        return local_.port();
    }

    // Model a NAT rebinding: retain the connection ID and switch UDP source
    // port without initiating QUIC migration or replacing the connection.
    void rebind() {
        auto replacement = replacement_socket();
        local_ = replacement.local_endpoint();
        socket_ = std::move(replacement);
        const auto rebound = path();
        ngtcp2_conn_set_local_addr(connection_->native_handle(), &rebound.local);
    }

    void migrate() {
        complete_handshake();
        wait_or_throw([this] { return handshake_confirmed_; },
            "HTTP/3 handshake confirmation did not arrive");
        migration_socket_.emplace(replacement_socket());
        migration_local_ = migration_socket_->local_endpoint();
        const auto candidate = path(migration_local_);
        const auto result = connection_->migrate(candidate, timestamp());
        if (result.status_ != http3_quic_status::accepted) {
            migration_socket_.reset();
            throw std::runtime_error("HTTP/3 client migration was not accepted");
        }
        wait_or_throw([&] {
            return ngtcp2_path_eq(ngtcp2_conn_get_path2(connection_->native_handle()), &candidate) != 0;
        },
            "HTTP/3 client migration validation did not complete");
        local_ = migration_local_;
        socket_ = std::move(*migration_socket_);
        migration_socket_.reset();
        flush();
    }

    // Waits for the final head or the stream's reset. The reference stays
    // valid until finish_response() or reset_request() for the stream.
    [[nodiscard]] const http3_test_response& wait_for_response_head(std::int64_t stream_id) {
        auto& stream = tracked_stream(stream_id);
        wait_or_throw([&] { return stream.response_.status_ != 0 || response_complete(stream); },
            "HTTP/3 response head did not arrive");
        return stream.response_;
    }

    // The body bytes received since the last take, at least one, returning
    // their flow credit; nullopt once the response ended or was reset with
    // nothing left to read. Throws when neither happens within `wait`.
    [[nodiscard]] std::optional<std::string> read_body_chunk(
        std::int64_t stream_id, std::chrono::milliseconds wait) {
        auto& stream = tracked_stream(stream_id);
        if (!wait_until([&] { return !stream.unread_body_.empty() || response_complete(stream); }, wait) &&
            stream.unread_body_.empty()) {
            throw std::runtime_error("HTTP/3 response body did not progress: " + describe_wait_failure());
        }
        if (stream.unread_body_.empty()) {
            return std::nullopt;
        }
        auto chunk = std::exchange(stream.unread_body_, {});
        connection_->consume_stream_data(stream_id, chunk.size());
        flush();
        return chunk;
    }

    // Waits for the response to complete or be reset and releases the stream.
    // body_ holds the bytes read_body_chunk() has not already taken.
    [[nodiscard]] http3_test_response finish_response(std::int64_t stream_id) {
        auto& stream = tracked_stream(stream_id);
        stream.consume_on_arrival_ = true;
        if (!stream.unread_body_.empty()) {
            stream.response_.body_ += stream.unread_body_;
            connection_->consume_stream_data(stream_id, stream.unread_body_.size());
            stream.unread_body_.clear();
        }
        wait_or_throw([&] { return response_complete(stream); }, "HTTP/3 response did not complete");
        auto response = std::move(stream.response_);
        streams_.erase(stream_id);
        return response;
    }

    // Abandons the request in both directions with RESET_STREAM and
    // STOP_SENDING carrying `application_error` (RFC 9114 §4.1.1).
    void reset_request(std::int64_t stream_id, std::uint64_t application_error) {
        static_cast<void>(tracked_stream(stream_id));
        streams_.erase(stream_id);
        connection_->reset_stream(stream_id, application_error);
        flush();
    }

    // Drives the connection until `done()` holds or `wait` elapses. Returns
    // false on the deadline, or once the connection closed while `done()`
    // still does not hold.
    template <typename predicate_type>
    [[nodiscard]] bool wait_until(predicate_type&& done, std::chrono::milliseconds wait) {
        const auto deadline = std::chrono::steady_clock::now() + wait;
        for (;;) {
            if (done()) {
                return true;
            }
            if (connection_->closed() || std::chrono::steady_clock::now() >= deadline) {
                flush();
                return done();
            }
            pump(deadline);
        }
    }

    // Every GOAWAY identifier received, in order; NGHTTP3_SHUTDOWN_NOTICE_STREAM_ID
    // marks a shutdown notice (RFC 9114 §5.2).
    [[nodiscard]] const std::vector<std::int64_t>& goaway_ids() const noexcept {
        return goaway_ids_;
    }

    // The closing or draining period began, by either endpoint.
    [[nodiscard]] bool closed() const noexcept {
        return connection_->closed();
    }

    // Sends CONNECTION_CLOSE with H3_NO_ERROR (RFC 9114 §5.2).
    void close() {
        connection_->close(NGHTTP3_H3_NO_ERROR);
        flush();
    }

    // For transport-level assertions such as the server's transport
    // parameters or the received CONNECTION_CLOSE error.
    [[nodiscard]] const ngtcp2_conn* native_handle() const noexcept {
        return connection_->native_handle();
    }

private:
    struct stream_state final {
        http3_test_response response_;
        std::string unread_body_;
        std::vector<std::string> datagrams_;
        bool consume_on_arrival_{false};
        bool ended_{false};
        bool closed_{false};
    };

    static constexpr std::size_t max_stream_output_bytes = 256 * 1024;
    static constexpr std::size_t max_udp_payload_bytes = 1350;

    static int handshake_confirmed(ngtcp2_conn*, void* user_data) noexcept {
        static_cast<http3_test_client*>(user_data)->handshake_confirmed_ = true;
        return 0;
    }

    static int handshake_complete(ngtcp2_conn*, void* user_data) noexcept {
        auto& self = *static_cast<http3_test_client*>(user_data);
        self.early_rejected_ = self.early_attempted_ &&
                               self.tls_.early_data_status() != SSL_EARLY_DATA_ACCEPTED;
        self.early_rejection_pending_ = self.early_rejected_;
        if (!self.early_rejected_) {
            return 0;
        }
        try {
            return self.connection_->reject_early_data() == http3_quic_status::accepted
                       ? 0
                       : NGTCP2_ERR_CALLBACK_FAILURE;
        } catch (...) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }

    void apply_early_rejection() {
        if (!std::exchange(early_rejection_pending_, false)) {
            return;
        }
        for (auto& [id, stream] : streams_) {
            static_cast<void>(id);
            stream.response_.reset_error_ = NGHTTP3_H3_REQUEST_REJECTED;
            stream.closed_ = true;
        }
        if (connection_->start_http3() != http3_quic_status::accepted) {
            throw std::runtime_error("HTTP/3 client cannot restart after early rejection");
        }
    }

    [[nodiscard]] asio::ip::udp::socket replacement_socket() {
        asio::ip::udp::socket replacement(io_);
        replacement.open(server_.protocol());
        replacement.connect(server_);
        replacement.non_blocking(true);
        return replacement;
    }

    [[nodiscard]] static ngtcp2_tstamp timestamp() noexcept {
        return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    [[nodiscard]] ngtcp2_path path(asio::ip::udp::endpoint& local) noexcept {
        return {
            .local = {reinterpret_cast<ngtcp2_sockaddr*>(local.data()), static_cast<ngtcp2_socklen>(local.size())},
            .remote = {reinterpret_cast<ngtcp2_sockaddr*>(server_.data()), static_cast<ngtcp2_socklen>(server_.size())},
            .user_data = nullptr,
        };
    }

    [[nodiscard]] ngtcp2_path path() noexcept {
        return path(local_);
    }

    [[nodiscard]] std::string authority() const {
        std::error_code literal_error;
        const auto address = asio::ip::make_address(server_name_, literal_error);
        const auto host = !literal_error && address.is_v6() ? "[" + server_name_ + "]" : server_name_;
        return host + ":" + std::to_string(server_.port());
    }

    [[nodiscard]] std::string describe_wait_failure() const {
        if (!connection_->closed()) {
            return "timed out";
        }
        const auto error = connection_->connection_error();
        if (error) {
            return "connection closed with error " + std::to_string(*error);
        }
        const auto* remote = ngtcp2_conn_get_ccerr2(connection_->native_handle());
        return remote == nullptr ? "connection closed"
                                 : "connection closed (QUIC type " + std::to_string(remote->type) +
                                       ", error " + std::to_string(remote->error_code) + ")";
    }

    template <typename predicate_type>
    void wait_or_throw(predicate_type&& done, const char* what) {
        if (!wait_until(std::forward<predicate_type>(done), timeout_)) {
            throw std::runtime_error(std::string(what) + ": " + describe_wait_failure());
        }
    }

    [[nodiscard]] stream_state& tracked_stream(std::int64_t stream_id) {
        const auto found = streams_.find(stream_id);
        if (found == streams_.end()) {
            throw std::invalid_argument("HTTP/3 test client: unknown request stream");
        }
        return found->second;
    }

    [[nodiscard]] static bool response_complete(const stream_state& stream) noexcept {
        return stream.ended_ || stream.closed_ || stream.response_.reset_error_.has_value();
    }

    [[nodiscard]] std::int64_t open_request(std::string_view method, std::string_view request_path,
        const http3_test_fields& headers, std::string_view body, bool consume_on_arrival) {
        std::optional<std::int64_t> opened;
        wait_or_throw([&] {
            opened = connection_->open_request_stream();
            return opened.has_value();
        },
            "HTTP/3 request stream did not open");
        const auto stream_id = *opened;
        auto& stream = streams_[stream_id];
        stream.consume_on_arrival_ = consume_on_arrival;

        const auto request_authority = authority();
        std::vector<http3_field_view> fields{
            {":method", method},
            {":scheme", "https"},
            {":authority", request_authority},
            {":path", request_path},
        };
        for (const auto& [name, value] : headers) {
            fields.push_back({name, value});
        }
        if (connection_->http3().submit_request(stream_id, fields, body.empty()) != http3_status::accepted) {
            throw std::runtime_error("HTTP/3 test client cannot submit the request head");
        }
        if (!body.empty()) {
            send_body(stream_id, stream, std::as_bytes(std::span(body.data(), body.size())));
        }
        flush();
        return stream_id;
    }

    void send_body(std::int64_t stream_id, const stream_state& stream, std::span<const std::byte> body) {
        auto& codec = connection_->http3();
        while (!body.empty()) {
            const auto submitted = codec.submit_data(stream_id, body);
            if (submitted.status_ == http3_status::accepted) {
                body = body.subspan(submitted.accepted_bytes_);
                continue;
            }
            if (submitted.status_ != http3_status::would_block) {
                // The server stopped the upload (STOP_SENDING or a reset);
                // its response or reset is still awaited.
                return;
            }
            // The stream's output budget is full until the server acknowledges
            // DATA; an early complete response also ends the upload.
            wait_or_throw([&] {
                return codec.pending_output_bytes(stream_id) < max_stream_output_bytes || response_complete(stream);
            },
                "HTTP/3 request body was not acknowledged");
            if (response_complete(stream)) {
                return;
            }
        }
        if (codec.finish_stream(stream_id) != http3_status::accepted && !response_complete(stream)) {
            throw std::runtime_error("HTTP/3 test client cannot finish the request body");
        }
    }

    void pump(std::chrono::steady_clock::time_point deadline) {
        flush();
        auto wake = deadline;
        const auto expiry = connection_->expiry();
        if (expiry < static_cast<ngtcp2_tstamp>((std::numeric_limits<std::int64_t>::max)())) {
            const std::chrono::steady_clock::time_point timer(
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::nanoseconds(static_cast<std::int64_t>(expiry))));
            wake = (std::min)(wake, timer);
        }
        wait_readable(wake);
        receive_available();
        if (!connection_->closed() && timestamp() >= connection_->expiry()) {
            static_cast<void>(connection_->handle_expiry(timestamp()));
            drain_events();
        }
        flush();
    }

    void wait_readable(std::chrono::steady_clock::time_point until) {
        if (until <= std::chrono::steady_clock::now()) {
            return;
        }
        socket_.async_wait(asio::socket_base::wait_read, [](const std::error_code&) {});
        if (migration_socket_) {
            migration_socket_->async_wait(asio::socket_base::wait_read, [](const std::error_code&) {});
        }
        io_.restart();
        io_.run_one_until(until);
        socket_.cancel();
        if (migration_socket_) {
            migration_socket_->cancel();
        }
        io_.restart();
        io_.run();
    }

    void receive_available() {
        receive_available(socket_, local_);
        if (migration_socket_) {
            receive_available(*migration_socket_, migration_local_);
        }
    }

    void receive_available(asio::ip::udp::socket& socket, asio::ip::udp::endpoint& local) {
        for (;;) {
            std::error_code error;
            const auto size = socket.receive(asio::buffer(input_), 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                return;
            }
            if (error) {
                throw std::system_error(error, "HTTP/3 test client UDP receive failed");
            }
            if (connection_->closed()) {
                continue;
            }
            static_cast<void>(connection_->receive_datagram(path(local), NGTCP2_ECN_NOT_ECT,
                std::span<const std::byte>(input_.data(), size), timestamp()));
            apply_early_rejection();
            drain_events();
        }
    }

    void flush() {
        for (;;) {
            const auto result = connection_->next_datagram(max_udp_payload_bytes, timestamp(), output_);
            drain_events();
            if (result.status_ != http3_quic_status::accepted || output_.bytes_.empty()) {
                return;
            }
            auto* output_socket = &socket_;
            if (migration_socket_) {
                const auto candidate = path(migration_local_);
                if (ngtcp2_path_eq(&output_.path_.path, &candidate) != 0) {
                    output_socket = &*migration_socket_;
                }
            }
            for (;;) {
                std::error_code error;
                output_socket->send(asio::buffer(output_.bytes_.data(), output_.bytes_.size()), 0, error);
                if (!error) {
                    break;
                }
                if (error != asio::error::would_block && error != asio::error::try_again) {
                    throw std::system_error(error, "HTTP/3 test client UDP send failed");
                }
                output_socket->wait(asio::socket_base::wait_write, error);
            }
        }
    }

    void drain_events() {
        while (auto event = connection_->next_event()) {
            apply(*event);
        }
    }

    void apply(http3_event& event) {
        if (const auto* datagram = std::get_if<http3_datagram_event>(&event)) {
            if (const auto found = streams_.find(datagram->stream_id_); found != streams_.end()) {
                found->second.datagrams_.emplace_back(datagram->payload_);
            }
            return;
        }
        if (const auto* goaway = std::get_if<http3_goaway_event>(&event)) {
            goaway_ids_.push_back(goaway->id_);
            return;
        }
        if (const auto* data = std::get_if<http3_data_event>(&event)) {
            const auto found = streams_.find(data->stream_id_);
            if (found == streams_.end()) {
                // A released stream's late DATA still returns its credit.
                connection_->consume_stream_data(data->stream_id_, data->bytes_.size());
            } else if (found->second.consume_on_arrival_) {
                found->second.response_.body_.append(data->bytes_);
                connection_->consume_stream_data(data->stream_id_, data->bytes_.size());
            } else {
                found->second.unread_body_.append(data->bytes_);
            }
            return;
        }
        std::visit(
            [this](const auto& value) {
                using event_type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<event_type, http3_headers_event>) {
                    apply_headers(value);
                } else if constexpr (std::is_same_v<event_type, http3_stream_end_event>) {
                    if (const auto found = streams_.find(value.stream_id_); found != streams_.end()) {
                        found->second.ended_ = true;
                    }
                } else if constexpr (std::is_same_v<event_type, http3_transport_action_event>) {
                    if (const auto found = streams_.find(value.stream_id_); found != streams_.end()) {
                        auto& reset_error = found->second.response_.reset_error_;
                        reset_error = reset_error.value_or(value.application_error_code_);
                    }
                } else if constexpr (std::is_same_v<event_type, http3_stream_close_event>) {
                    if (const auto found = streams_.find(value.stream_id_); found != streams_.end()) {
                        found->second.closed_ = true;
                        auto& reset_error = found->second.response_.reset_error_;
                        if (!reset_error) {
                            reset_error = value.application_error_code_;
                        }
                    }
                }
            },
            event);
    }

    void apply_headers(const http3_headers_event& event) {
        const auto found = streams_.find(event.stream_id_);
        if (found == streams_.end()) {
            return;
        }
        auto& stream = found->second;
        std::optional<int> status;
        http3_test_fields fields;
        for (const auto& field : event.fields_) {
            const auto name = field.name();
            const auto value = field.value();
            if (name == ":status") {
                int parsed = 0;
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (error == std::errc{} && end == value.data() + value.size()) {
                    status = parsed;
                }
            } else if (!name.starts_with(':')) {
                fields.emplace_back(std::string(name), std::string(value));
            }
        }
        if (stream.response_.status_ != 0) {
            stream.response_.trailers_ = std::move(fields);
        } else if (status && *status < 200) {
            stream.response_.interim_statuses_.push_back(*status);
        } else if (status) {
            stream.response_.status_ = *status;
            stream.response_.headers_ = std::move(fields);
        }
        if (event.stream_ended_) {
            stream.ended_ = true;
        }
    }

    std::chrono::milliseconds timeout_;
    std::string server_name_;
    asio::ip::udp::endpoint server_;
    asio::ip::udp::endpoint local_;
    asio::io_context io_;
    asio::ip::udp::socket socket_;
    std::optional<asio::ip::udp::socket> migration_socket_;
    asio::ip::udp::endpoint migration_local_;
    http3_test_client_tls tls_;
    // Destroyed before the TLS session it uses.
    std::optional<http3_quic_connection> connection_;
    http3_quic_datagram output_;
    std::vector<std::byte> input_;
    std::map<std::int64_t, stream_state> streams_;
    std::vector<std::int64_t> goaway_ids_;
    bool early_attempted_{false};
    bool early_rejected_{false};
    bool early_rejection_pending_{false};
    bool handshake_confirmed_{false};
};

}  // namespace ruvia::test
