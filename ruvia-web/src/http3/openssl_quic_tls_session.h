#pragma once

#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

#include <openssl/ssl.h>

#include "ruvia/http/quic_tls_handshake.h"

namespace ruvia::detail {

class openssl_quic_tls_session final {
public:
    struct session_deleter {
        void operator()(SSL_SESSION* session) const noexcept;
    };

    openssl_quic_tls_session(SSL_CTX* context_value, quic_role role,
        std::span<const unsigned char> alpn, std::string_view peer_host = {},
        std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
        SSL_SESSION* resumption_session = nullptr, bool enable_early_data = false);
    ~openssl_quic_tls_session() noexcept;
    openssl_quic_tls_session(const openssl_quic_tls_session&) = delete;
    openssl_quic_tls_session& operator=(const openssl_quic_tls_session&) = delete;
    openssl_quic_tls_session(openssl_quic_tls_session&&) = delete;
    openssl_quic_tls_session& operator=(openssl_quic_tls_session&&) = delete;

    [[nodiscard]] quic_tls_driver_view driver_view() noexcept;
    [[nodiscard]] quic_tls_drive_result drive(quic_tls_handshake& handshake) noexcept;
    void stop() noexcept;
    [[nodiscard]] SSL* native_handle() const noexcept {
        return ssl_.get();
    }
    [[nodiscard]] std::unique_ptr<SSL_SESSION, session_deleter> take_resumption_session() noexcept;

    static int crypto_send(SSL*, const unsigned char*, std::size_t, std::size_t*, void*) noexcept;
    static int crypto_receive(SSL*, const unsigned char**, std::size_t*, void*) noexcept;
    static int crypto_release(SSL*, std::size_t, void*) noexcept;
    static int yield_secret(SSL*, std::uint32_t, int, const unsigned char*, std::size_t, void*) noexcept;
    static int receive_transport_parameters(SSL*, const unsigned char*, std::size_t, void*) noexcept;
    static int alert(SSL*, unsigned char, void*) noexcept;

private:
    struct ssl_deleter {
        void operator()(SSL* ssl) const noexcept;
    };
    static quic_tls_drive_result drive_callback(void* context, quic_tls_handshake& handshake) noexcept;
    static void retire_callback(void* context) noexcept;
    static int new_session_callback(SSL* ssl, SSL_SESSION* session) noexcept;
    static int session_owner_index() noexcept;

    int on_crypto_send(const unsigned char*, std::size_t, std::size_t*);
    int on_crypto_receive(const unsigned char**, std::size_t*);
    int on_crypto_release(std::size_t);
    int on_yield_secret(std::uint32_t, int, const unsigned char*, std::size_t);
    int on_transport_parameters(const unsigned char*, std::size_t);
    int on_alert(unsigned char) noexcept;
    void capture_resumption_session(SSL_SESSION* session) noexcept;
    void fail(quic_tls_alert alert) noexcept;

    quic_role role_;
    quic_tls_handshake* handshake_{};
    std::pmr::memory_resource* resource_;
    std::pmr::string peer_host_;
    std::unique_ptr<SSL, ssl_deleter> ssl_;
    std::unique_ptr<SSL_SESSION, session_deleter> pending_session_;
    std::optional<quic_crypto_record_lease> record_;
    quic_tls_alert callback_alert_{quic_tls_alert::internal_error};
    bool callback_failed_{};
    bool stopped_{};
    bool params_submitted_{};
    quic_encryption_level write_level_{quic_encryption_level::initial};
};

}  // namespace ruvia::detail
