#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "ruvia/http/quic_crypto_provider.h"

namespace ruvia::detail {
class quic_connection_state;
}  // namespace ruvia::detail

namespace ruvia {

class quic_crypto_record_lease {
public:
    quic_crypto_record_lease() noexcept = default;
    quic_crypto_record_lease(const quic_crypto_record_lease&) = delete;
    quic_crypto_record_lease& operator=(const quic_crypto_record_lease&) = delete;
    quic_crypto_record_lease(quic_crypto_record_lease&& other) noexcept;
    quic_crypto_record_lease& operator=(quic_crypto_record_lease&& other) noexcept;
    ~quic_crypto_record_lease() noexcept;

    explicit operator bool() const noexcept;
    quic_encryption_level level() const noexcept;
    // Borrow remains valid until consume() or release(). Partial consume advances the view.
    std::span<const std::byte> bytes() const noexcept;
    void consume(std::size_t size);
    void release() noexcept;

private:
    friend class quic_tls_handshake;
    friend class detail::quic_connection_state;
    using consume_fn = void (*)(void* context, std::size_t size, bool release_lease) noexcept;
    quic_crypto_record_lease(void* context, consume_fn consume, quic_encryption_level level,
        std::span<const std::byte> bytes) noexcept;

    void* context_{};
    consume_fn consume_{};
    quic_encryption_level level_{quic_encryption_level::initial};
    std::span<const std::byte> bytes_{};
};

enum class quic_tls_progress : std::uint8_t { progress,
    need_input,
    completed,
    failed };

struct quic_tls_drive_result {
    quic_tls_progress progress{quic_tls_progress::need_input};
    quic_tls_alert alert{quic_tls_alert::internal_error};
};

// Stable, connection-owned TLS capability. The connection and handshake are
// address-stable until retire; a TLS driver may retain their addresses only until then.
class quic_tls_handshake {
public:
    quic_tls_handshake(const quic_tls_handshake&) = delete;
    quic_tls_handshake& operator=(const quic_tls_handshake&) = delete;
    quic_tls_handshake(quic_tls_handshake&&) = delete;
    quic_tls_handshake& operator=(quic_tls_handshake&&) = delete;
    ~quic_tls_handshake() noexcept;

    quic_role role() const noexcept;
    quic_encryption_level current_read_level() const noexcept;
    quic_tls_info_view info() const noexcept;
    // Returns a lease on queued inbound QUIC CRYPTO data for the TLS driver.
    quic_crypto_record_lease take_crypto_record();
    // Copies TLS-produced CRYPTO bytes on accepted; would_block retains no input storage.
    quic_operation_status submit_crypto(quic_encryption_level level, std::span<const std::byte> bytes);
    // Installs one read/write traffic secret synchronously; the secret is not retained.
    void submit_secret(quic_encryption_level level, quic_crypto_direction direction,
        quic_cipher_suite suite, std::span<const std::byte> secret);
    // HTTP-encoded local transport parameters, stable through connection retirement.
    std::span<const std::byte> local_transport_parameters() const noexcept;
    // TLS sends the peer extension bytes to HTTP for decoding and protocol validation.
    void submit_peer_transport_parameters(std::span<const std::byte> encoded_parameters);
    // Records the TLS decision once the handshake finishes. Client rejection rolls ngtcp2's
    // 0-RTT stream state back; retransmission remains the HTTP connection owner's duty.
    void complete_early_data(bool accepted);
    // Copies negotiated metadata; info() borrows it until connection retirement.
    void complete(quic_tls_info_view info);
    void fail(quic_tls_alert alert) noexcept;
    bool completed() const noexcept;
    bool failed() const noexcept;
    quic_tls_alert failure_alert() const noexcept;

private:
    friend class quic_connection;
    friend class detail::quic_connection_state;
    friend struct quic_tls_driver_view;
    explicit quic_tls_handshake(detail::quic_connection_state* state) noexcept;
    detail::quic_connection_state* state_{};
};

// Borrowed per-connection TLS callback table. A server must receive a distinct view
// for every admitted connection; context remains alive until that connection retires.
struct quic_tls_driver_view {
    void* context{};
    quic_tls_drive_result (*drive)(void* context, quic_tls_handshake& handshake) noexcept {};
    // Required synchronous, non-reentrant teardown. Release driver-owned CRYPTO leases and
    // borrowed handshake/transport-parameter references before returning; never wait or drive Core.
    void (*retire)(void* context) noexcept {};

    void validate() const;
};

}  // namespace ruvia
