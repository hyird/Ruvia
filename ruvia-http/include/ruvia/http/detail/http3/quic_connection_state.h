#pragma once

#include <ngtcp2/ngtcp2.h>

#include <array>
#include <deque>
#include <exception>
#include <list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "ruvia/http/detail/http3/quic_cid_registry.h"
#include "ruvia/http/detail/http3/quic_key_schedule.h"
#include "ruvia/http/quic_tls_handshake.h"

namespace ruvia::detail {

class quic_connection_state;

struct quic_aead_key_slot final {
    quic_connection_state* owner{};
    std::pmr::memory_resource* resource{};
    quic_aead_key key;
    quic_cipher_suite_parameters parameters{};
    std::uint64_t encryptions{};
    std::uint64_t decryption_failures{};
};

struct quic_header_key_slot final {
    quic_connection_state* owner{};
    std::pmr::memory_resource* resource{};
    quic_header_protection_key key;
};

struct quic_server_cid_publication_journal final {
    explicit quic_server_cid_publication_journal(std::pmr::memory_resource* resource)
        : ids(resource) {}

    std::pmr::vector<quic_connection_id> ids;
};

class quic_connection_state final {
public:
    struct crypto_record final {
        explicit crypto_record(std::pmr::memory_resource* resource) noexcept
            : bytes(resource) {}

        std::pmr::vector<std::byte> bytes;
        std::size_t offset{};
    };

    struct send_block final {
        explicit send_block(std::pmr::memory_resource* resource) noexcept
            : bytes(resource) {}

        std::pmr::string bytes;
        std::uint64_t offset{};
        std::size_t submitted{};
    };

    struct stream_state final {
        explicit stream_state(std::pmr::memory_resource* resource) noexcept
            : input(resource),
              output(resource) {}

        std::pmr::string input;
        std::pmr::deque<send_block> output;
        std::uint64_t received_offset{};
        std::uint64_t send_offset{};
        std::size_t input_offset{};
        std::size_t retained_output_bytes{};
        std::optional<std::uint64_t> peer_reset_error{};
        bool peer_initiated{};
        bool accepted{};
        bool readable{};
        bool writable{};
        bool receive_fin{};
        bool receive_end_observed{};
        bool send_fin{};
        bool fin_submitted{};
        bool send_reset{};
        bool send_stopped{};
        bool retired{};
        bool library_closed{};
    };

    struct datagram final {
        explicit datagram(std::pmr::memory_resource* resource) noexcept
            : bytes(resource) {}

        std::pmr::vector<std::byte> bytes;
        std::uint64_t id{};
    };

    explicit quic_connection_state(quic_connection_config config,
        quic_crypto_provider_view crypto, quic_tls_driver_view tls_driver,
        std::pmr::memory_resource* resource, quic_timestamp now);
    quic_connection_state(const quic_connection_state&) = delete;
    quic_connection_state& operator=(const quic_connection_state&) = delete;
    quic_connection_state(quic_connection_state&&) = delete;
    quic_connection_state& operator=(quic_connection_state&&) = delete;
    ~quic_connection_state() noexcept;

    void append_crypto(quic_encryption_level level, std::span<const std::byte> bytes);
    quic_crypto_record_lease take_crypto(quic_encryption_level level);
    void consume_crypto(std::size_t size, bool release_lease) noexcept;
    std::size_t retained_crypto_bytes() const noexcept;
    std::size_t crypto_record_count(quic_encryption_level level) const noexcept;
    void latch_close_reason(quic_close_reason_view reason);
    quic_close_reason_view close_reason() const noexcept;

    void latch_failure(std::exception_ptr failure) noexcept;
    void rethrow_failure() const;
    void retire() noexcept;
    quic_connection_info info() const noexcept;
    void complete_tls(quic_tls_info_view info);
    void fail_tls(quic_tls_alert alert) noexcept;
    bool failed() const noexcept;
    quic_tls_handshake& tls_handshake() noexcept;

    quic_connection_config config_;
    quic_crypto_provider_view crypto_;
    quic_tls_driver_view tls_driver_;
    quic_cid_registry_view server_cid_registry_{};
    std::pmr::memory_resource* resource_{};
    quic_server_cid_publication_journal server_cid_publication_journal_{resource_};
    quic_timestamp last_supplied_time_{};
    quic_tls_handshake tls_handshake_;

    ngtcp2_conn* connection_{};
    ngtcp2_mem ngtcp_memory_{};
    ngtcp2_path_storage path_{};

    std::array<std::pmr::list<crypto_record>, 3> inbound_crypto_;
    std::size_t retained_crypto_bytes_{};
    std::size_t leased_crypto_bytes_{};
    crypto_record* leased_record_{};
    quic_encryption_level leased_level_{quic_encryption_level::initial};
    bool crypto_lease_active_{};

    std::pmr::vector<std::byte> local_transport_parameters_;
    std::pmr::unordered_map<std::uint64_t, stream_state> streams_;
    std::pmr::deque<datagram> received_datagrams_;
    std::pmr::deque<datagram> send_datagrams_;
    std::pmr::string close_reason_;
    std::array<std::byte, quic_max_alpn_size> negotiated_alpn_{};
    std::size_t negotiated_alpn_size_{};
    quic_cipher_suite negotiated_cipher_suite_{quic_cipher_suite::aes_128_gcm_sha256};
    std::optional<quic_cipher_suite> installed_cipher_suite_{};
    quic_encryption_level current_read_level_{quic_encryption_level::initial};
    quic_tls_alert failure_alert_{quic_tls_alert::internal_error};
    ruvia::quic_connection_state state_{ruvia::quic_connection_state::connecting};
    quic_close_kind close_kind_{quic_close_kind::transport};
    std::uint64_t close_error_code_{};
    std::uint64_t close_frame_type_{};
    std::exception_ptr latched_failure_{};
    bool tls_handshake_complete_{};
    bool tls_handshake_notified_{};
    bool tls_driver_active_{};
    bool tls_driver_retiring_{};
    bool tls_driver_retired_{};
    bool retry_aead_installed_{};
    bool quic_handshake_complete_{};
    bool confirmed_{};
    bool close_reason_latched_{};
    bool prefer_datagram_{};
    std::size_t peer_streams_ever_{};
    std::size_t retained_stream_input_bytes_{};
    std::size_t retained_stream_output_bytes_{};
    std::size_t received_datagram_bytes_{};
    std::size_t send_datagram_bytes_{};
    std::uint64_t next_send_stream_id_{};
    std::uint64_t next_datagram_id_{};

private:
    static void consume_lease(void* context, std::size_t size, bool release_lease) noexcept;
};

}  // namespace ruvia::detail
