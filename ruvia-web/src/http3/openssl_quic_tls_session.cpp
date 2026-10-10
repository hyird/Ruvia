#include "http3/openssl_quic_tls_session.h"

#include <array>
#include <stdexcept>
#include <utility>

#include <openssl/core_dispatch.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

#include "client/client_transport.h"

namespace ruvia::detail {
namespace {

class error_queue_scope final {
public:
    error_queue_scope() noexcept {
        ERR_clear_error();
    }
    ~error_queue_scope() noexcept {
        ERR_clear_error();
    }
};

quic_encryption_level encryption_level(std::uint32_t level) {
    switch (level) {
        case OSSL_RECORD_PROTECTION_LEVEL_HANDSHAKE:
            return quic_encryption_level::handshake;
        case OSSL_RECORD_PROTECTION_LEVEL_APPLICATION:
            return quic_encryption_level::application;
        case OSSL_RECORD_PROTECTION_LEVEL_EARLY:
            return quic_encryption_level::early_data;
        case OSSL_RECORD_PROTECTION_LEVEL_NONE:
        default:
            throw std::invalid_argument("unsupported OpenSSL QUIC encryption level");
    }
}

quic_cipher_suite cipher_suite(const SSL* ssl) {
    const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl);
    if (!cipher) {
        throw std::runtime_error("OpenSSL did not select a TLS cipher");
    }
    switch (SSL_CIPHER_get_protocol_id(cipher)) {
        case 0x1301:
            return quic_cipher_suite::aes_128_gcm_sha256;
        case 0x1302:
            return quic_cipher_suite::aes_256_gcm_sha384;
        case 0x1303:
            return quic_cipher_suite::chacha20_poly1305_sha256;
        default:
            throw std::runtime_error("unsupported TLS cipher for QUIC");
    }
}

int callback_send(SSL* ssl, const unsigned char* data, std::size_t size,
    std::size_t* consumed, void* argument) noexcept {
    return openssl_quic_tls_session::crypto_send(ssl, data, size, consumed, argument);
}
int callback_receive(SSL* ssl, const unsigned char** data, std::size_t* size, void* argument) noexcept {
    return openssl_quic_tls_session::crypto_receive(ssl, data, size, argument);
}
int callback_release(SSL* ssl, std::size_t consumed, void* argument) noexcept {
    return openssl_quic_tls_session::crypto_release(ssl, consumed, argument);
}
int callback_secret(SSL* ssl, std::uint32_t level, int direction,
    const unsigned char* secret, std::size_t size, void* argument) noexcept {
    return openssl_quic_tls_session::yield_secret(ssl, level, direction, secret, size, argument);
}
int callback_params(SSL* ssl, const unsigned char* params, std::size_t size, void* argument) noexcept {
    return openssl_quic_tls_session::receive_transport_parameters(ssl, params, size, argument);
}
int callback_alert(SSL* ssl, unsigned char value, void* argument) noexcept {
    return openssl_quic_tls_session::alert(ssl, value, argument);
}

const OSSL_DISPATCH callbacks[]{
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND, reinterpret_cast<void (*)(void)>(callback_send)},
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD, reinterpret_cast<void (*)(void)>(callback_receive)},
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD, reinterpret_cast<void (*)(void)>(callback_release)},
    {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET, reinterpret_cast<void (*)(void)>(callback_secret)},
    {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS, reinterpret_cast<void (*)(void)>(callback_params)},
    {OSSL_FUNC_SSL_QUIC_TLS_ALERT, reinterpret_cast<void (*)(void)>(callback_alert)},
    {0, nullptr}};

}  // namespace

openssl_quic_tls_session::openssl_quic_tls_session(SSL_CTX* context_value, quic_role role,
    std::span<const unsigned char> alpn, std::string_view peer_host,
    std::pmr::memory_resource* resource, SSL_SESSION* resumption_session,
    bool enable_early_data)
    : role_(role),
      resource_(resource ? resource : std::pmr::get_default_resource()),
      peer_host_(peer_host, resource_),
      ssl_(context_value ? SSL_new(context_value) : nullptr) {
    if (!ssl_) {
        throw std::runtime_error("failed to create QUIC TLS session");
    }
    if (role_ == quic_role::client) {
        SSL_set_connect_state(ssl_.get());
        if (!peer_host_.empty()) {
            std::pmr::string normalized(peer_host_, resource_);
            const bool ip_address = is_client_ip_address(normalized);
            if (!ip_address && normalized.ends_with('.')) {
                normalized.pop_back();
            }
            const bool sni_configured = ip_address ||
                                        SSL_set_tlsext_host_name(ssl_.get(), normalized.c_str()) == 1;
            const bool identity_configured = SSL_get_verify_mode(ssl_.get()) != SSL_VERIFY_PEER ||
                                             configure_client_tls_peer_identity(*ssl_, normalized.c_str(), ip_address);
            if (!sni_configured || !identity_configured) {
                throw std::runtime_error("failed to configure QUIC TLS peer identity");
            }
        }
        if (!alpn.empty() && SSL_set_alpn_protos(ssl_.get(), alpn.data(), static_cast<unsigned int>(alpn.size())) != 0) {
            throw std::runtime_error("failed to configure QUIC TLS ALPN");
        }
    } else {
        SSL_set_accept_state(ssl_.get());
    }
    // Session callbacks are context-wide; recover the owner through this SSL's stable ex_data.
    if (role_ == quic_role::client) {
        const int index = session_owner_index();
        if (index < 0 || SSL_set_ex_data(ssl_.get(), index, this) != 1) {
            throw std::runtime_error("failed to bind QUIC TLS session owner");
        }
        // Tickets are captured through the callback; OpenSSL's internal store would only
        // retain unused client sessions until they expire.
        SSL_CTX_set_session_cache_mode(context_value, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_sess_set_new_cb(context_value, new_session_callback);
    }
    // OpenSSL snapshots the SSL role when the callback table is installed.
    if (SSL_set_quic_tls_cbs(ssl_.get(), callbacks, this) != 1) {
        throw std::runtime_error("failed to install OpenSSL QUIC TLS callbacks");
    }
    if (resumption_session && SSL_set_session(ssl_.get(), resumption_session) != 1) {
        throw std::runtime_error("failed to install QUIC TLS resumption session");
    }
    if (enable_early_data && role_ == quic_role::client &&
        (resumption_session == nullptr || SSL_SESSION_get_max_early_data(resumption_session) == 0)) {
        throw std::invalid_argument("client QUIC early data requires a ticket with an early-data allowance");
    }
    if (enable_early_data && role_ == quic_role::server && resumption_session != nullptr) {
        throw std::invalid_argument("server QUIC early data uses the received ClientHello ticket");
    }
    // This API requires QUIC TLS callbacks to have initialized the QUIC TLS state.
    if (SSL_set_quic_tls_early_data_enabled(ssl_.get(), enable_early_data ? 1 : 0) != 1) {
        throw std::runtime_error("failed to configure QUIC TLS early data");
    }
}

openssl_quic_tls_session::~openssl_quic_tls_session() noexcept {
    stop();
}

void openssl_quic_tls_session::ssl_deleter::operator()(SSL* ssl) const noexcept {
    SSL_free(ssl);
}

quic_tls_driver_view openssl_quic_tls_session::driver_view() noexcept {
    return {.context_ = this, .drive_ = drive_callback, .retire_ = retire_callback};
}

void openssl_quic_tls_session::retire_callback(void* context_value) noexcept {
    static_cast<openssl_quic_tls_session*>(context_value)->stop();
}

quic_tls_drive_result openssl_quic_tls_session::drive_callback(void* context_value,
    quic_tls_handshake& handshake) noexcept {
    return static_cast<openssl_quic_tls_session*>(context_value)->drive(handshake);
}

quic_tls_drive_result openssl_quic_tls_session::drive(quic_tls_handshake& handshake) noexcept {
    if (stopped_) {
        return {quic_tls_progress::failed, quic_tls_alert::internal_error};
    }
    if (!ssl_ || handshake.role() != role_ || (handshake_ && handshake_ != &handshake)) {
        handshake.fail(quic_tls_alert::internal_error);
        return {quic_tls_progress::failed, quic_tls_alert::internal_error};
    }
    handshake_ = &handshake;
    if (handshake.failed()) {
        return {quic_tls_progress::failed, handshake.failure_alert()};
    }

    error_queue_scope error_scope;
    try {
        if (role_ == quic_role::client && !params_submitted_) {
            const auto params = handshake.local_transport_parameters();
            if (SSL_set_quic_tls_transport_params(ssl_.get(),
                    reinterpret_cast<const unsigned char*>(params.data()), params.size()) != 1) {
                throw std::runtime_error("failed to submit local QUIC transport parameters to TLS");
            }
            params_submitted_ = true;
        }

        std::array<unsigned char, 1> post_handshake{};
        std::size_t read{};
        ERR_clear_error();
        const int result_value = handshake.completed()
                                     ? SSL_read_ex(ssl_.get(), post_handshake.data(), post_handshake.size(), &read)
                                     : SSL_do_handshake(ssl_.get());
        const int error = result_value == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl_.get(), result_value);
        if (callback_failed_) {
            handshake.fail(callback_alert_);
            return {quic_tls_progress::failed, callback_alert_};
        }
        if (result_value == 1) {
            if (!handshake.completed()) {
                if (!SSL_is_init_finished(ssl_.get())) {
                    return {quic_tls_progress::progress, quic_tls_alert::internal_error};
                }
                const int early_status = SSL_get_early_data_status(ssl_.get());
                if (early_status == SSL_EARLY_DATA_ACCEPTED ||
                    early_status == SSL_EARLY_DATA_REJECTED) {
                    handshake.complete_early_data(early_status == SSL_EARLY_DATA_ACCEPTED);
                }
                const unsigned char* alpn{};
                unsigned int alpn_size{};
                SSL_get0_alpn_selected(ssl_.get(), &alpn, &alpn_size);
                if (alpn_size == 0) {
                    handshake.fail(quic_tls_alert::no_application_protocol);
                    return {quic_tls_progress::failed, quic_tls_alert::no_application_protocol};
                }
                handshake.complete({.negotiated_alpn_ = {reinterpret_cast<const std::byte*>(alpn), alpn_size},
                    .cipher_suite_ = cipher_suite(ssl_.get())});
                return {quic_tls_progress::completed, quic_tls_alert::internal_error};
            }
            return {quic_tls_progress::progress, quic_tls_alert::internal_error};
        }

        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            return {quic_tls_progress::need_input, quic_tls_alert::internal_error};
        }
        handshake.fail(quic_tls_alert::handshake_failure);
        return {quic_tls_progress::failed, quic_tls_alert::handshake_failure};
    } catch (...) {
        const auto failure = callback_failed_ ? callback_alert_ : quic_tls_alert::internal_error;
        handshake.fail(failure);
        return {quic_tls_progress::failed, failure};
    }
}

std::unique_ptr<SSL_SESSION, openssl_quic_tls_session::session_deleter>
openssl_quic_tls_session::take_resumption_session() noexcept {
    return std::move(pending_session_);
}

void openssl_quic_tls_session::session_deleter::operator()(SSL_SESSION* session_value) const noexcept {
    SSL_SESSION_free(session_value);
}

int openssl_quic_tls_session::session_owner_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int openssl_quic_tls_session::new_session_callback(SSL* ssl, SSL_SESSION* session_value) noexcept {
    const int index = session_owner_index();
    auto* const owner_value = index < 0
                                  ? nullptr
                                  : static_cast<openssl_quic_tls_session*>(SSL_get_ex_data(ssl, index));
    if (owner_value) {
        owner_value->capture_resumption_session(session_value);
    }
    return 0;
}

void openssl_quic_tls_session::capture_resumption_session(SSL_SESSION* session_value) noexcept {
    if (!session_value || SSL_SESSION_is_resumable(session_value) != 1) {
        return;
    }
    const unsigned char* ticket{};
    std::size_t ticket_size{};
    SSL_SESSION_get0_ticket(session_value, &ticket, &ticket_size);
    if (!ticket || ticket_size == 0 || ticket_size > 16 * 1024) {
        return;
    }
    SSL_SESSION* const snapshot = SSL_SESSION_dup(session_value);
    if (snapshot) {
        pending_session_.reset(snapshot);
    } else {
        // Do not let a best-effort cache snapshot change SSL_get_error classification.
        ERR_clear_error();
    }
}

void openssl_quic_tls_session::stop() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    // SSL may still call back while being freed, so the handshake capability is borrowed
    // until SSL_free returns. Release any outstanding record only afterwards.
    ssl_.reset();
    record_.reset();
    handshake_ = nullptr;
    params_submitted_ = false;
    write_level_ = quic_encryption_level::initial;
}

int openssl_quic_tls_session::crypto_send(SSL*, const unsigned char* bytes_value,
    std::size_t size, std::size_t* consumed, void* argument) noexcept {
    auto& self = *static_cast<openssl_quic_tls_session*>(argument);
    try {
        return self.on_crypto_send(bytes_value, size, consumed);
    } catch (...) {
        self.fail(quic_tls_alert::internal_error);
        return 0;
    }
}
int openssl_quic_tls_session::crypto_receive(SSL*, const unsigned char** bytes_value,
    std::size_t* size, void* argument) noexcept {
    auto& self = *static_cast<openssl_quic_tls_session*>(argument);
    try {
        return self.on_crypto_receive(bytes_value, size);
    } catch (...) {
        self.fail(quic_tls_alert::internal_error);
        return 0;
    }
}
int openssl_quic_tls_session::crypto_release(SSL*, std::size_t size, void* argument) noexcept {
    auto& self = *static_cast<openssl_quic_tls_session*>(argument);
    try {
        return self.on_crypto_release(size);
    } catch (...) {
        self.fail(quic_tls_alert::internal_error);
        return 0;
    }
}
int openssl_quic_tls_session::yield_secret(SSL*, std::uint32_t level, int direction,
    const unsigned char* secret, std::size_t size, void* argument) noexcept {
    auto& self = *static_cast<openssl_quic_tls_session*>(argument);
    try {
        return self.on_yield_secret(level, direction, secret, size);
    } catch (...) {
        self.fail(quic_tls_alert::internal_error);
        return 0;
    }
}
int openssl_quic_tls_session::receive_transport_parameters(SSL*, const unsigned char* params,
    std::size_t size, void* argument) noexcept {
    auto& self = *static_cast<openssl_quic_tls_session*>(argument);
    try {
        return self.on_transport_parameters(params, size);
    } catch (...) {
        self.fail(quic_tls_alert::internal_error);
        return 0;
    }
}
int openssl_quic_tls_session::alert(SSL*, unsigned char value, void* argument) noexcept {
    return static_cast<openssl_quic_tls_session*>(argument)->on_alert(value);
}

int openssl_quic_tls_session::on_crypto_send(const unsigned char* bytes_value, std::size_t size,
    std::size_t* consumed) {
    if (!handshake_ || !consumed) {
        throw std::logic_error("QUIC TLS driver is not bound");
    }
    const auto result_value = handshake_->submit_crypto(write_level_,
        {reinterpret_cast<const std::byte*>(bytes_value), size});
    *consumed = result_value == quic_operation_status::accepted ? size : 0;
    return result_value == quic_operation_status::accepted || result_value == quic_operation_status::would_block;
}
int openssl_quic_tls_session::on_crypto_receive(const unsigned char** bytes_value, std::size_t* size) {
    if (!handshake_ || !bytes_value || !size) {
        throw std::logic_error("QUIC TLS driver is not bound");
    }
    if (!record_) {
        auto record = handshake_->take_crypto_record();
        if (record) {
            record_.emplace(std::move(record));
        }
    }
    if (!record_) {
        *bytes_value = nullptr;
        *size = 0;
        return 1;
    }
    const auto data = record_->bytes();
    *bytes_value = reinterpret_cast<const unsigned char*>(data.data());
    *size = data.size();
    return 1;
}
int openssl_quic_tls_session::on_crypto_release(std::size_t size) {
    if (!record_ || !*record_ || size > record_->bytes().size()) {
        throw std::logic_error("invalid QUIC CRYPTO record release");
    }
    // Return any unconsumed tail to HTTP after consuming the prefix OpenSSL used.
    record_->consume(size);
    record_->release();
    record_.reset();
    return 1;
}
int openssl_quic_tls_session::on_yield_secret(std::uint32_t level, int direction,
    const unsigned char* secret, std::size_t size) {
    if (!handshake_ || (direction != 0 && direction != 1)) {
        throw std::invalid_argument("invalid OpenSSL QUIC secret direction");
    }
    const auto encryption = encryption_level(level);
    const auto suite = cipher_suite(ssl_.get());
    handshake_->submit_secret(encryption,
        direction == 0 ? quic_crypto_direction::read : quic_crypto_direction::write,
        suite, {reinterpret_cast<const std::byte*>(secret), size});
    if (direction == 1) {
        // QUIC never carries CRYPTO in 0-RTT. After an HRR OpenSSL resets the 0-RTT
        // write key to plaintext without yielding a secret, so the second
        // ClientHello must keep using the Initial level.
        if (encryption != quic_encryption_level::early_data) {
            write_level_ = encryption;
        }
        if (role_ == quic_role::server && encryption == quic_encryption_level::handshake && !params_submitted_) {
            const auto params = handshake_->local_transport_parameters();
            if (SSL_set_quic_tls_transport_params(ssl_.get(),
                    reinterpret_cast<const unsigned char*>(params.data()), params.size()) != 1) {
                throw std::runtime_error("failed to submit server QUIC transport parameters to TLS");
            }
            params_submitted_ = true;
        }
    }
    return 1;
}
int openssl_quic_tls_session::on_transport_parameters(const unsigned char* params, std::size_t size) {
    if (!handshake_) {
        throw std::logic_error("QUIC TLS driver is not bound");
    }
    handshake_->submit_peer_transport_parameters({reinterpret_cast<const std::byte*>(params), size});
    return 1;
}
int openssl_quic_tls_session::on_alert(unsigned char value) noexcept {
    fail(static_cast<quic_tls_alert>(value));
    return 1;
}
void openssl_quic_tls_session::fail(quic_tls_alert value) noexcept {
    if (callback_failed_) {
        return;
    }
    callback_alert_ = value;
    callback_failed_ = true;
    if (handshake_) {
        handshake_->fail(value);
    }
}

}  // namespace ruvia::detail
