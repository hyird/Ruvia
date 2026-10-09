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

#include "ruvia/http/quic_tls_handshake.h"

#include "http3/quic_cid_registry.h"
#include "http3/quic_key_schedule.h"

namespace ruvia::detail {

class quic_connection_state;

struct quic_aead_key_slot final {
    quic_connection_state* owner_{};
    std::pmr::memory_resource* resource_{};
    quic_aead_key key_;
    quic_cipher_suite_parameters parameters_{};
    std::uint64_t encryptions_{};
    std::uint64_t decryption_failures_{};
};

struct quic_header_key_slot final {
    quic_connection_state* owner_{};
    std::pmr::memory_resource* resource_{};
    quic_header_protection_key key_;
};

struct quic_server_cid_publication_journal final {
    explicit quic_server_cid_publication_journal(std::pmr::memory_resource* resource)
        : ids_(resource) {}

    std::pmr::vector<quic_connection_id> ids_;
};

class quic_connection_state final {
public:
    struct crypto_record final {
        explicit crypto_record(std::pmr::memory_resource* resource) noexcept
            : bytes_(resource) {}

        std::pmr::vector<std::byte> bytes_;
        std::size_t offset_{};
    };

    struct send_block final {
        explicit send_block(std::pmr::memory_resource* resource) noexcept
            : bytes_(resource) {}

        std::pmr::string bytes_;
        std::uint64_t offset_{};
        std::size_t submitted_{};
    };

    struct stream_state final {
        explicit stream_state(std::pmr::memory_resource* resource) noexcept
            : input_(resource),
              output_(resource) {}

        std::pmr::string input_;
        std::pmr::deque<send_block> output_;
        std::uint64_t received_offset_{};
        std::uint64_t send_offset_{};
        std::size_t input_offset_{};
        std::size_t retained_output_bytes_{};
        std::optional<std::uint64_t> peer_reset_error_{};
        bool peer_initiated_{};
        bool accepted_{};
        bool readable_{};
        bool writable_{};
        bool receive_fin_{};
        bool receive_end_observed_{};
        bool received_early_data_{};
        bool early_data_candidate_{};
        bool send_fin_{};
        bool fin_submitted_{};
        bool send_reset_{};
        bool send_stopped_{};
        bool retired_{};
        bool library_closed_{};
    };

    struct datagram final {
        explicit datagram(std::pmr::memory_resource* resource) noexcept
            : bytes_(resource) {}

        std::pmr::vector<std::byte> bytes_;
        std::uint64_t id_{};
    };

    explicit quic_connection_state(quic_connection_config config,
        quic_crypto_provider_view crypto, quic_tls_driver_view tls_driver,
        std::pmr::memory_resource* resource, quic_timestamp now,
        std::span<const std::byte> early_transport_parameters = {});
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
    quic_path_migration start_path_migration(const quic_address& local_address,
        ngtcp2_tstamp now);
    std::optional<quic_path_migration> path_migration(std::uint64_t id) const noexcept;
    quic_operation_status cancel_path_migration(std::uint64_t id);
    quic_operation_status fail_path_migration(std::uint64_t id) noexcept;
    void on_path_validation(const ngtcp2_path& path,
        ngtcp2_path_validation_result result) noexcept;
    void complete_tls(quic_tls_info_view info);
    void fail_tls(quic_tls_alert alert) noexcept;
    bool failed() const noexcept;
    quic_tls_handshake& tls_handshake() noexcept;

    quic_connection_config config_;
    quic_version negotiated_version_;
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
    ngtcp2_path_storage migration_path_{};
    std::optional<quic_path_migration> migration_{};
    quic_address current_local_address_{};
    bool migration_validation_pending_{};
    std::uint64_t next_migration_id_{1};

    std::array<std::pmr::list<crypto_record>, 4> inbound_crypto_;
    std::pmr::vector<std::byte> early_transport_parameters_;
    std::size_t retained_crypto_bytes_{};
    std::size_t leased_crypto_bytes_{};
    crypto_record* leased_record_{};
    quic_encryption_level leased_level_{quic_encryption_level::initial};
    bool crypto_lease_active_{};

    std::pmr::vector<std::byte> local_transport_parameters_;
    std::pmr::unordered_map<std::uint64_t, stream_state> streams_;
    std::pmr::vector<std::uint64_t> rejected_early_streams_;
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
    bool early_transport_parameters_set_{};
    bool early_data_key_installed_{};
    bool tls_drive_started_{};
    quic_early_data_state early_data_state_{quic_early_data_state::unavailable};
    bool tls_driver_active_{};
    bool tls_driver_retiring_{};
    bool tls_driver_retired_{};
    std::optional<quic_version> retry_aead_version_{};
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
