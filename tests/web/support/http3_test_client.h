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
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include "ruvia/http/http3_quic_connection.h"

namespace ruvia::test {

using http3_test_fields = std::vector<std::pair<std::string, std::string>>;

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
    http3_test_client_tls(const std::filesystem::path& ca_file, const std::string& server_name) {
        context_.reset(SSL_CTX_new(TLS_client_method()));
        if (!context_ || SSL_CTX_set_min_proto_version(context_.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_load_verify_locations(context_.get(), ca_file.string().c_str(), nullptr) != 1) {
            throw std::runtime_error("HTTP/3 test client cannot load its trust anchor");
        }
        SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER, nullptr);
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

    static ngtcp2_conn* get_connection(ngtcp2_crypto_conn_ref* reference) {
        return static_cast<http3_quic_connection*>(reference->user_data)->native_handle();
    }

    std::unique_ptr<SSL_CTX, free_context> context_;
    std::unique_ptr<SSL, free_tls> tls_;
    std::unique_ptr<ngtcp2_crypto_ossl_ctx, free_crypto> crypto_;
    ngtcp2_crypto_conn_ref reference_{};
};

// Blocking HTTP/3 client for loopback tests against any compliant server.
// Construction completes the QUIC handshake (TLS 1.3, ALPN "h3", certificate
// verified against `ca_file` for `server_name`, which is also the SNI unless
// it is an IP literal) and opens the HTTP/3 control streams, or throws. Every
// wait is bounded: `timeout` limits each blocking step, and QUIC timers run
// from the connection's expiry() while waiting. Received DATA returns its flow
// credit only when the body is taken, so an unread streaming response exerts
// real backpressure on the server.
class http3_test_client final {
public:
    http3_test_client(asio::ip::udp::endpoint server, const std::filesystem::path& ca_file,
        std::string server_name, std::chrono::milliseconds timeout)
        : timeout_(timeout),
          server_name_(std::move(server_name)),
          server_(server),
          socket_(io_),
          tls_(ca_file, server_name_),
          input_(64 * 1024) {
        // A connected socket receives only the server's datagrams and reports
        // ICMP port-unreachable, so a missing server fails fast.
        socket_.open(server_.protocol());
        socket_.connect(server_);
        socket_.non_blocking(true);
        local_ = socket_.local_endpoint();
        connection_.emplace(http3_quic_connection::client({
            .callbacks_ = http3_test_client_callbacks(),
            .source_connection_id_ = http3_test_random_connection_id(16),
            // RFC 9000 §7.2: at least 8 unpredictable bytes.
            .destination_connection_id_ = http3_test_random_connection_id(18),
            .initial_path_ = path(),
            .initial_timestamp_ = timestamp(),
            .http3_ = {.max_stream_output_bytes_ = max_stream_output_bytes},
        }));
        tls_.bind(*connection_);

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
        bool consume_on_arrival_{false};
        bool ended_{false};
        bool closed_{false};
    };

    static constexpr std::size_t max_stream_output_bytes = 256 * 1024;
    static constexpr std::size_t max_udp_payload_bytes = 1350;

    [[nodiscard]] static ngtcp2_tstamp timestamp() noexcept {
        return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    [[nodiscard]] ngtcp2_path path() noexcept {
        return {
            .local = {reinterpret_cast<ngtcp2_sockaddr*>(local_.data()), static_cast<ngtcp2_socklen>(local_.size())},
            .remote = {reinterpret_cast<ngtcp2_sockaddr*>(server_.data()), static_cast<ngtcp2_socklen>(server_.size())},
            .user_data = nullptr,
        };
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
        return error ? "connection closed with error " + std::to_string(*error) : "connection closed";
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
        io_.restart();
        io_.run_until(until);
        socket_.cancel();
        io_.restart();
        io_.run();
    }

    void receive_available() {
        for (;;) {
            std::error_code error;
            const auto size = socket_.receive(asio::buffer(input_), 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                return;
            }
            if (error) {
                throw std::system_error(error, "HTTP/3 test client UDP receive failed");
            }
            if (connection_->closed()) {
                continue;
            }
            static_cast<void>(connection_->receive_datagram(path(), NGTCP2_ECN_NOT_ECT,
                std::span<const std::byte>(input_.data(), size), timestamp()));
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
            for (;;) {
                std::error_code error;
                socket_.send(asio::buffer(output_.bytes_.data(), output_.bytes_.size()), 0, error);
                if (!error) {
                    break;
                }
                if (error != asio::error::would_block && error != asio::error::try_again) {
                    throw std::system_error(error, "HTTP/3 test client UDP send failed");
                }
                socket_.wait(asio::socket_base::wait_write, error);
            }
        }
    }

    void drain_events() {
        while (auto event = connection_->next_event()) {
            apply(*event);
        }
    }

    void apply(http3_event& event) {
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
    http3_test_client_tls tls_;
    // Destroyed before the TLS session it uses.
    std::optional<http3_quic_connection> connection_;
    http3_quic_datagram output_;
    std::vector<std::byte> input_;
    std::map<std::int64_t, stream_state> streams_;
    std::vector<std::int64_t> goaway_ids_;
};

}  // namespace ruvia::test
