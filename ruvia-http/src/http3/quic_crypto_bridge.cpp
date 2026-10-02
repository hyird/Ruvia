#include "ruvia/http/detail/http3/quic_crypto_bridge.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/http3/quic_connection_state.h"
#include "ruvia/http/detail/http3/quic_key_schedule.h"

namespace ruvia::detail {
namespace {

using aead_slot_owner = std::unique_ptr<quic_aead_key_slot, void (*)(quic_aead_key_slot*) noexcept>;
using header_slot_owner = std::unique_ptr<quic_header_key_slot, void (*)(quic_header_key_slot*) noexcept>;

void delete_aead_slot(quic_aead_key_slot* slot) noexcept {
    if (!slot) {
        return;
    }
    auto* const resource = slot->resource;
    std::destroy_at(slot);
    resource->deallocate(slot, sizeof(*slot), alignof(quic_aead_key_slot));
}

void delete_header_slot(quic_header_key_slot* slot) noexcept {
    if (!slot) {
        return;
    }
    auto* const resource = slot->resource;
    std::destroy_at(slot);
    resource->deallocate(slot, sizeof(*slot), alignof(quic_header_key_slot));
}

aead_slot_owner make_aead_slot(quic_connection_state& state, quic_aead_key key,
    quic_cipher_suite_parameters parameters) {
    if (!key) {
        throw std::invalid_argument("QUIC AEAD provider returned an empty key");
    }
    std::pmr::polymorphic_allocator<quic_aead_key_slot> allocator(state.resource_);
    auto* const slot = allocator.allocate(1);
    try {
        std::construct_at(slot, quic_aead_key_slot{&state, state.resource_,
                                    std::move(key), parameters, 0, 0});
    } catch (...) {
        allocator.deallocate(slot, 1);
        throw;
    }
    return {slot, delete_aead_slot};
}

header_slot_owner make_header_slot(quic_connection_state& state,
    quic_header_protection_key key) {
    if (!key) {
        throw std::invalid_argument("QUIC header-protection provider returned an empty key");
    }
    std::pmr::polymorphic_allocator<quic_header_key_slot> allocator(state.resource_);
    auto* const slot = allocator.allocate(1);
    try {
        std::construct_at(slot, quic_header_key_slot{&state, state.resource_, std::move(key)});
    } catch (...) {
        allocator.deallocate(slot, 1);
        throw;
    }
    return {slot, delete_header_slot};
}

ngtcp2_crypto_ctx make_crypto_context(quic_cipher_suite suite) {
    const auto parameters = quic_cipher_suite_parameters_for(suite);
    ngtcp2_crypto_ctx context{};
    context.aead.max_overhead = parameters.tag_size;
    context.max_encryption = parameters.max_encryptions;
    context.max_decryption_failure = parameters.max_decryption_failures;
    return context;
}

void validate_client_initial_dcid(std::span<const std::byte> bytes) {
    if (bytes.size() > NGTCP2_MAX_CIDLEN) {
        throw std::invalid_argument("QUIC Initial DCID exceeds ngtcp2 CID capacity");
    }
}

void install_initial_keys_impl(quic_connection_state& state,
    std::span<const std::byte> client_dcid, bool version_negotiation) {
    if (!state.connection_) {
        throw std::logic_error("cannot install QUIC Initial keys before connection initialization");
    }
    if (state.config_.version != quic_version::v1) {
        throw std::invalid_argument("only QUIC version 1 Initial keys are supported");
    }

    validate_client_initial_dcid(client_dcid);
    auto initial = derive_quic_v1_initial_secrets(state.crypto_, state.resource_, client_dcid);
    const auto read_secret = state.config_.role == quic_role::client
                                 ? initial.server.view()
                                 : initial.client.view();
    const auto write_secret = state.config_.role == quic_role::client
                                  ? initial.client.view()
                                  : initial.server.view();
    auto read_keys = derive_quic_packet_keys(state.crypto_, state.resource_,
        quic_cipher_suite::aes_128_gcm_sha256, quic_crypto_direction::read, read_secret);
    auto write_keys = derive_quic_packet_keys(state.crypto_, state.resource_,
        quic_cipher_suite::aes_128_gcm_sha256, quic_crypto_direction::write, write_secret);

    auto read_aead = make_aead_slot(state, std::move(read_keys.aead()),
        quic_cipher_suite_parameters_for(quic_cipher_suite::aes_128_gcm_sha256));
    auto read_header = make_header_slot(state, std::move(read_keys.header_protection()));
    auto write_aead = make_aead_slot(state, std::move(write_keys.aead()),
        quic_cipher_suite_parameters_for(quic_cipher_suite::aes_128_gcm_sha256));
    auto write_header = make_header_slot(state, std::move(write_keys.header_protection()));

    ngtcp2_crypto_aead_ctx read_aead_ctx{.native_handle = read_aead.get()};
    ngtcp2_crypto_cipher_ctx read_header_ctx{.native_handle = read_header.get()};
    ngtcp2_crypto_aead_ctx write_aead_ctx{.native_handle = write_aead.get()};
    ngtcp2_crypto_cipher_ctx write_header_ctx{.native_handle = write_header.get()};
    auto crypto_context = make_crypto_context(quic_cipher_suite::aes_128_gcm_sha256);
    ngtcp2_conn_set_initial_crypto_ctx(state.connection_, &crypto_context);

    const int result = version_negotiation
                           ? ngtcp2_conn_install_vneg_initial_key(state.connection_, NGTCP2_PROTO_VER_V1,
                                 &read_aead_ctx, reinterpret_cast<const uint8_t*>(read_keys.iv().data()),
                                 &read_header_ctx, &write_aead_ctx,
                                 reinterpret_cast<const uint8_t*>(write_keys.iv().data()),
                                 &write_header_ctx, read_keys.iv().size())
                           : ngtcp2_conn_install_initial_key(state.connection_, &read_aead_ctx,
                                 reinterpret_cast<const uint8_t*>(read_keys.iv().data()), &read_header_ctx,
                                 &write_aead_ctx, reinterpret_cast<const uint8_t*>(write_keys.iv().data()),
                                 &write_header_ctx, read_keys.iv().size());
    if (result != 0) {
        throw quic_error(quic_error_code::crypto_failure,
            "ngtcp2 rejected QUIC Initial key installation");
    }

    read_aead.release();
    read_header.release();
    write_aead.release();
    write_header.release();
}

void configure_application_context(quic_connection_state& state, quic_cipher_suite suite) {
    if (state.installed_cipher_suite_ && *state.installed_cipher_suite_ != suite) {
        throw quic_error(quic_error_code::crypto_failure,
            "TLS changed the QUIC cipher suite after traffic keys were installed");
    }
    if (!state.installed_cipher_suite_) {
        const auto context = make_crypto_context(suite);
        ngtcp2_conn_set_crypto_ctx(state.connection_, &context);
        state.installed_cipher_suite_ = suite;
        state.negotiated_cipher_suite_ = suite;
    }
}

void install_aead_context(quic_connection_state& state, quic_encryption_level level,
    quic_crypto_direction direction, quic_cipher_suite suite,
    std::span<const std::byte> secret, std::span<const std::byte> iv, quic_aead_key key,
    quic_header_protection_key header_key) {
    auto aead = make_aead_slot(state, std::move(key), quic_cipher_suite_parameters_for(suite));
    auto header = make_header_slot(state, std::move(header_key));
    ngtcp2_crypto_aead_ctx aead_context{.native_handle = aead.get()};
    ngtcp2_crypto_cipher_ctx header_context{.native_handle = header.get()};
    const auto* const iv_bytes = reinterpret_cast<const uint8_t*>(iv.data());
    const auto* const secret_bytes = reinterpret_cast<const uint8_t*>(secret.data());
    int result{};
    if (level == quic_encryption_level::handshake) {
        result = direction == quic_crypto_direction::read
                     ? ngtcp2_conn_install_rx_handshake_key(state.connection_, &aead_context,
                           iv_bytes, iv.size(), &header_context)
                     : ngtcp2_conn_install_tx_handshake_key(state.connection_, &aead_context,
                           iv_bytes, iv.size(), &header_context);
    } else {
        result = direction == quic_crypto_direction::read
                     ? ngtcp2_conn_install_rx_key(state.connection_, secret_bytes, secret.size(),
                           &aead_context, iv_bytes, iv.size(), &header_context)
                     : ngtcp2_conn_install_tx_key(state.connection_, secret_bytes, secret.size(),
                           &aead_context, iv_bytes, iv.size(), &header_context);
    }
    if (result != 0) {
        throw quic_error(quic_error_code::crypto_failure,
            "ngtcp2 rejected QUIC TLS traffic key installation");
    }
    aead.release();
    header.release();
}

quic_connection_state* state_from(void* opaque) noexcept {
    return static_cast<quic_connection_state*>(opaque);
}

int callback_failure(quic_connection_state* state) noexcept {
    if (state) {
        auto failure = std::current_exception();
        if (!failure) {
            try {
                throw std::runtime_error("QUIC ngtcp2 callback failed without a pending exception");
            } catch (...) {
                failure = std::current_exception();
            }
        }
        state->latch_failure(std::move(failure));
    }
    return NGTCP2_ERR_CALLBACK_FAILURE;
}

void install_retry_integrity_aead(quic_connection_state& state) {
    if (state.retry_aead_installed_) {
        return;
    }
    auto key = state.crypto_.create_aead_key(state.crypto_.context,
        quic_cipher_suite::aes_128_gcm_sha256, quic_crypto_direction::write,
        quic_v1_retry_integrity_key);
    auto slot = make_aead_slot(state, std::move(key),
        quic_cipher_suite_parameters_for(quic_cipher_suite::aes_128_gcm_sha256));
    ngtcp2_crypto_aead_ctx context{.native_handle = slot.get()};
    ngtcp2_crypto_aead aead{.native_handle = nullptr, .max_overhead = 16};
    ngtcp2_conn_set_retry_aead(state.connection_, &aead, &context);
    slot.release();
    state.retry_aead_installed_ = true;
}

int drive_tls_handshake(quic_connection_state& state, ngtcp2_conn& connection) {
    if (state.local_transport_parameters_.empty()) {
        throw std::logic_error("TLS drive requires initialized local transport parameters");
    }
    if (state.tls_driver_active_ || state.tls_driver_retiring_ || state.tls_driver_retired_) {
        return callback_failure(&state);
    }
    state.tls_driver_active_ = true;
    struct driver_guard final {
        bool& active;
        ~driver_guard() {
            active = false;
        }
    } guard{state.tls_driver_active_};

    const auto driven = state.tls_driver_.drive(state.tls_driver_.context, state.tls_handshake_);
    if (driven.progress == quic_tls_progress::failed || state.tls_handshake_.failed()) {
        const auto alert = state.tls_handshake_.failed()
                               ? state.tls_handshake_.failure_alert()
                               : driven.alert;
        const std::array<char, 0> empty_reason{};
        state.latch_close_reason({.kind = quic_close_kind::tls,
            .code = 0x100U + static_cast<std::uint8_t>(alert),
            .frame_type = 0,
            .reason = empty_reason});
        state.fail_tls(alert);
        ngtcp2_conn_set_tls_alert(&connection, static_cast<uint8_t>(alert));
        return NGTCP2_ERR_CRYPTO;
    }
    if (driven.progress == quic_tls_progress::completed && !state.tls_handshake_.completed()) {
        return callback_failure(&state);
    }
    if (state.tls_handshake_.completed() && !state.tls_handshake_notified_) {
        ngtcp2_conn_tls_handshake_completed(&connection);
        state.tls_handshake_notified_ = true;
    }
    return 0;
}

int client_initial_callback(ngtcp2_conn* connection, void* user_data) noexcept {
    auto* const state = state_from(user_data);
    try {
        if (!state || state->config_.role != quic_role::client) {
            throw std::logic_error("missing QUIC client callback state or wrong role");
        }
        if (!connection) {
            throw std::invalid_argument("missing ngtcp2 client connection");
        }
        const auto* const current_dcid = ngtcp2_conn_get_dcid2(connection);
        if (!current_dcid || current_dcid->datalen > NGTCP2_MAX_CIDLEN) {
            throw std::logic_error("ngtcp2 client connection has no valid current Initial DCID");
        }
        install_initial_keys_impl(*state,
            {reinterpret_cast<const std::byte*>(current_dcid->data), current_dcid->datalen}, false);
        install_retry_integrity_aead(*state);
        if (state->local_transport_parameters_.empty()) {
            throw std::logic_error("client Initial TLS drive requires initialized local transport parameters");
        }
        return drive_tls_handshake(*state, *connection);
    } catch (...) {
        return callback_failure(state);
    }
}

int recv_client_initial_callback(ngtcp2_conn*, const ngtcp2_cid* dcid,
    void* user_data) noexcept {
    auto* const state = state_from(user_data);
    try {
        if (!state || !dcid || state->config_.role != quic_role::server) {
            throw std::invalid_argument("missing QUIC server Initial callback state, role or DCID");
        }
        install_initial_keys_impl(*state,
            {reinterpret_cast<const std::byte*>(dcid->data), dcid->datalen}, false);
        return 0;
    } catch (...) {
        return callback_failure(state);
    }
}

int recv_crypto_data_callback(ngtcp2_conn* connection, ngtcp2_encryption_level level,
    uint64_t, const uint8_t* data, size_t datalen, void* user_data) noexcept {
    auto* const state = state_from(user_data);
    try {
        if (!state || !connection || level == NGTCP2_ENCRYPTION_LEVEL_0RTT ||
            level > NGTCP2_ENCRYPTION_LEVEL_1RTT || (!data && datalen != 0)) {
            throw quic_error(quic_error_code::protocol_failure,
                "ngtcp2 supplied invalid QUIC CRYPTO data");
        }
        const auto http_level = level == NGTCP2_ENCRYPTION_LEVEL_INITIAL
                                    ? quic_encryption_level::initial
                                : level == NGTCP2_ENCRYPTION_LEVEL_HANDSHAKE
                                    ? quic_encryption_level::handshake
                                    : quic_encryption_level::application;
        if (datalen != 0) {
            state->append_crypto(http_level,
                {reinterpret_cast<const std::byte*>(data), datalen});
        }
        if (http_level > state->current_read_level_) {
            state->current_read_level_ = http_level;
        }
        return drive_tls_handshake(*state, *connection);
    } catch (...) {
        return callback_failure(state);
    }
}

int recv_retry_callback(ngtcp2_conn*, const ngtcp2_pkt_hd* header,
    void* user_data) noexcept {
    auto* const state = state_from(user_data);
    try {
        if (!state || !header || state->config_.role != quic_role::client) {
            throw std::invalid_argument("missing QUIC Retry callback state, role or header");
        }
        install_initial_keys_impl(*state,
            {reinterpret_cast<const std::byte*>(header->scid.data), header->scid.datalen}, false);
        return 0;
    } catch (...) {
        return callback_failure(state);
    }
}

int version_negotiation_callback(ngtcp2_conn*, uint32_t version,
    const ngtcp2_cid* client_dcid, void* user_data) noexcept {
    if (version != NGTCP2_PROTO_VER_V1 || !client_dcid) {
        return NGTCP2_ERR_VERSION_NEGOTIATION_FAILURE;
    }
    auto* const state = state_from(user_data);
    try {
        if (!state || state->config_.role != quic_role::client) {
            throw std::logic_error("missing QUIC version-negotiation callback state or wrong role");
        }
        install_initial_keys_impl(*state,
            {reinterpret_cast<const std::byte*>(client_dcid->data), client_dcid->datalen}, true);
        return 0;
    } catch (...) {
        return callback_failure(state);
    }
}

int handshake_completed_callback(ngtcp2_conn*, void* user_data) noexcept {
    auto* const state = state_from(user_data);
    if (!state) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    state->quic_handshake_complete_ = true;
    if (state->config_.role == quic_role::server) {
        state->confirmed_ = true;
    }
    if (state->tls_handshake_complete_) {
        state->state_ = ruvia::quic_connection_state::ready;
    }
    return 0;
}

int handshake_confirmed_callback(ngtcp2_conn*, void* user_data) noexcept {
    auto* const state = state_from(user_data);
    if (!state) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    state->confirmed_ = true;
    if (state->tls_handshake_complete_ && state->quic_handshake_complete_) {
        state->state_ = ruvia::quic_connection_state::ready;
    }
    return 0;
}

int encrypt_callback(uint8_t* destination, const ngtcp2_crypto_aead*,
    const ngtcp2_crypto_aead_ctx* context, const uint8_t* plaintext, size_t plaintext_size,
    const uint8_t* nonce, size_t nonce_size, const uint8_t* associated_data,
    size_t associated_data_size) noexcept {
    auto* slot = context ? static_cast<quic_aead_key_slot*>(context->native_handle) : nullptr;
    if (!slot) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    try {
        if (!destination || (!plaintext && plaintext_size != 0) || !nonce || nonce_size != 12 ||
            (!associated_data && associated_data_size != 0)) {
            throw std::invalid_argument("invalid ngtcp2 QUIC encrypt callback buffers");
        }
        if (slot->encryptions >= slot->parameters.max_encryptions) {
            return NGTCP2_ERR_AEAD_LIMIT_REACHED;
        }
        if (plaintext_size > std::numeric_limits<size_t>::max() - slot->parameters.tag_size) {
            throw std::length_error("QUIC AEAD output length overflows size_t");
        }
        const auto plaintext_span = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(plaintext), plaintext_size);
        const auto aad_span = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(associated_data), associated_data_size);
        const auto output_size = plaintext_size + slot->parameters.tag_size;
        slot->key.seal(std::span<const std::byte, 12>(reinterpret_cast<const std::byte*>(nonce), 12),
            aad_span, plaintext_span, {reinterpret_cast<std::byte*>(destination), output_size});
        ++slot->encryptions;
        return 0;
    } catch (...) {
        return callback_failure(slot->owner);
    }
}

int decrypt_callback(uint8_t* destination, const ngtcp2_crypto_aead*,
    const ngtcp2_crypto_aead_ctx* context, const uint8_t* ciphertext, size_t ciphertext_size,
    const uint8_t* nonce, size_t nonce_size, const uint8_t* associated_data,
    size_t associated_data_size) noexcept {
    auto* slot = context ? static_cast<quic_aead_key_slot*>(context->native_handle) : nullptr;
    if (!slot) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    try {
        if ((!destination && ciphertext_size > slot->parameters.tag_size) ||
            (!ciphertext && ciphertext_size != 0) || !nonce || nonce_size != 12 ||
            (!associated_data && associated_data_size != 0)) {
            throw std::invalid_argument("invalid ngtcp2 QUIC decrypt callback buffers");
        }
        if (ciphertext_size < slot->parameters.tag_size) {
            ++slot->decryption_failures;
            return slot->decryption_failures >= slot->parameters.max_decryption_failures
                       ? NGTCP2_ERR_AEAD_LIMIT_REACHED
                       : NGTCP2_ERR_DECRYPT;
        }
        const auto plaintext_size = ciphertext_size - slot->parameters.tag_size;
        const auto ciphertext_span = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(ciphertext), ciphertext_size);
        const auto aad_span = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(associated_data), associated_data_size);
        auto plaintext_span = std::span<std::byte>(reinterpret_cast<std::byte*>(destination),
            plaintext_size);
        const auto result = slot->key.open(
            std::span<const std::byte, 12>(reinterpret_cast<const std::byte*>(nonce), 12),
            aad_span, ciphertext_span, plaintext_span);
        if (result.value == quic_aead_key_operations::open_result::status::rejected) {
            ++slot->decryption_failures;
            return slot->decryption_failures >= slot->parameters.max_decryption_failures
                       ? NGTCP2_ERR_AEAD_LIMIT_REACHED
                       : NGTCP2_ERR_DECRYPT;
        }
        if (result.plaintext_size != plaintext_size) {
            throw std::runtime_error("QUIC AEAD provider returned an inconsistent plaintext size");
        }
        return 0;
    } catch (...) {
        return callback_failure(slot->owner);
    }
}

int hp_mask_callback(uint8_t* destination, const ngtcp2_crypto_cipher*,
    const ngtcp2_crypto_cipher_ctx* context, const uint8_t* sample) noexcept {
    auto* slot = context ? static_cast<quic_header_key_slot*>(context->native_handle) : nullptr;
    if (!slot) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    try {
        if (!destination || !sample) {
            throw std::invalid_argument("invalid ngtcp2 QUIC header-protection buffers");
        }
        slot->key.mask(std::span<const std::byte, 16>(
                           reinterpret_cast<const std::byte*>(sample), 16),
            std::span<std::byte, 5>(reinterpret_cast<std::byte*>(destination), 5));
        return 0;
    } catch (...) {
        return callback_failure(slot->owner);
    }
}

void random_callback(uint8_t* destination, size_t size, const ngtcp2_rand_ctx* context) noexcept {
    auto* state = context ? state_from(context->native_handle) : nullptr;
    if (!state || (!destination && size != 0)) {
        if (state) {
            try {
                throw std::logic_error("invalid ngtcp2 random callback context");
            } catch (...) {
                state->latch_failure(std::current_exception());
            }
        }
        if (destination && size != 0) {
            std::memset(destination, 0, size);
        }
        return;
    }
    try {
        state->crypto_.random_bytes(state->crypto_.context,
            {reinterpret_cast<std::byte*>(destination), size});
    } catch (...) {
        state->latch_failure(std::current_exception());
        if (size != 0) {
            std::memset(destination, 0, size);
        }
    }
}

int update_key_callback(ngtcp2_conn*, uint8_t* rx_secret, uint8_t* tx_secret,
    ngtcp2_crypto_aead_ctx* rx_context, uint8_t* rx_iv,
    ngtcp2_crypto_aead_ctx* tx_context, uint8_t* tx_iv,
    const uint8_t* current_rx_secret, const uint8_t* current_tx_secret,
    size_t secret_size, void* user_data) noexcept {
    auto* state = state_from(user_data);
    if (!state || !rx_secret || !tx_secret || !rx_context || !tx_context || !rx_iv || !tx_iv ||
        !current_rx_secret || !current_tx_secret || !state->installed_cipher_suite_) {
        return callback_failure(state);
    }
    std::size_t wipe_size{};
    std::size_t iv_size{};
    try {
        const auto suite = *state->installed_cipher_suite_;
        const auto parameters = quic_cipher_suite_parameters_for(suite);
        if (secret_size != parameters.hash_size) {
            throw std::invalid_argument("ngtcp2 supplied an invalid QUIC key-update secret length");
        }
        wipe_size = secret_size;
        iv_size = parameters.iv_size;
        auto updated_rx = update_quic_packet_keys(state->crypto_, state->resource_, suite,
            quic_crypto_direction::read,
            {reinterpret_cast<const std::byte*>(current_rx_secret), secret_size});
        auto updated_tx = update_quic_packet_keys(state->crypto_, state->resource_, suite,
            quic_crypto_direction::write,
            {reinterpret_cast<const std::byte*>(current_tx_secret), secret_size});
        std::ranges::copy(updated_rx.traffic_secret(), reinterpret_cast<std::byte*>(rx_secret));
        std::ranges::copy(updated_tx.traffic_secret(), reinterpret_cast<std::byte*>(tx_secret));
        std::ranges::copy(updated_rx.iv(), reinterpret_cast<std::byte*>(rx_iv));
        std::ranges::copy(updated_tx.iv(), reinterpret_cast<std::byte*>(tx_iv));
        auto rx_slot = make_aead_slot(*state, std::move(updated_rx.aead()), parameters);
        auto tx_slot = make_aead_slot(*state, std::move(updated_tx.aead()), parameters);
        rx_context->native_handle = rx_slot.get();
        tx_context->native_handle = tx_slot.get();
        rx_slot.release();
        tx_slot.release();
        return 0;
    } catch (...) {
        if (wipe_size != 0) {
            state->crypto_.secure_erase(state->crypto_.context,
                {reinterpret_cast<std::byte*>(rx_secret), wipe_size});
            state->crypto_.secure_erase(state->crypto_.context,
                {reinterpret_cast<std::byte*>(tx_secret), wipe_size});
        }
        if (iv_size != 0) {
            const auto parameters = quic_cipher_suite_parameters_for(*state->installed_cipher_suite_);
            state->crypto_.secure_erase(state->crypto_.context,
                {reinterpret_cast<std::byte*>(rx_iv), parameters.iv_size});
            state->crypto_.secure_erase(state->crypto_.context,
                {reinterpret_cast<std::byte*>(tx_iv), parameters.iv_size});
        }
        rx_context->native_handle = nullptr;
        tx_context->native_handle = nullptr;
        return callback_failure(state);
    }
}

void delete_aead_context_callback(ngtcp2_conn*, ngtcp2_crypto_aead_ctx* context,
    void*) noexcept {
    if (!context) {
        return;
    }
    auto* const slot = static_cast<quic_aead_key_slot*>(context->native_handle);
    context->native_handle = nullptr;
    delete_aead_slot(slot);
}

void delete_header_context_callback(ngtcp2_conn*, ngtcp2_crypto_cipher_ctx* context,
    void*) noexcept {
    if (!context) {
        return;
    }
    auto* const slot = static_cast<quic_header_key_slot*>(context->native_handle);
    context->native_handle = nullptr;
    delete_header_slot(slot);
}

}  // namespace

void fill_quic_crypto_callbacks(ngtcp2_callbacks& callbacks) noexcept {
    callbacks.client_initial = client_initial_callback;
    callbacks.recv_client_initial = recv_client_initial_callback;
    callbacks.recv_crypto_data = recv_crypto_data_callback;
    callbacks.handshake_completed = handshake_completed_callback;
    callbacks.encrypt = encrypt_callback;
    callbacks.decrypt = decrypt_callback;
    callbacks.hp_mask = hp_mask_callback;
    callbacks.recv_retry = recv_retry_callback;
    callbacks.rand = random_callback;
    callbacks.update_key = update_key_callback;
    callbacks.handshake_confirmed = handshake_confirmed_callback;
    callbacks.delete_crypto_aead_ctx = delete_aead_context_callback;
    callbacks.delete_crypto_cipher_ctx = delete_header_context_callback;
    callbacks.version_negotiation = version_negotiation_callback;
}

void initialize_quic_random_context(ngtcp2_rand_ctx& context,
    quic_connection_state& state) noexcept {
    context.native_handle = &state;
}

void install_quic_initial_keys(quic_connection_state& state,
    std::span<const std::byte> client_initial_dcid, bool version_negotiation) {
    install_initial_keys_impl(state, client_initial_dcid, version_negotiation);
}

void encode_quic_local_transport_parameters(quic_connection_state& state) {
    if (!state.connection_) {
        throw std::logic_error("cannot encode QUIC transport parameters before connection initialization");
    }
    const auto* const local_parameters = ngtcp2_conn_get_local_transport_params2(state.connection_);
    if (!local_parameters || !local_parameters->initial_scid_present) {
        throw quic_error(quic_error_code::invalid_state,
            "ngtcp2 local QUIC transport parameters lack the Initial source connection ID");
    }
    std::array<uint8_t, 4096> encoded{};
    const auto result = ngtcp2_conn_encode_local_transport_params2(
        state.connection_, encoded.data(), encoded.size());
    if (result < 0) {
        throw quic_error(quic_error_code::protocol_failure,
            "ngtcp2 failed to encode local QUIC transport parameters");
    }
    std::pmr::vector<std::byte> owned(state.resource_);
    owned.reserve(static_cast<std::size_t>(result));
    for (ngtcp2_ssize index = 0; index < result; ++index) {
        owned.push_back(static_cast<std::byte>(encoded[static_cast<std::size_t>(index)]));
    }
    state.local_transport_parameters_.swap(owned);
}

void install_quic_traffic_secret(quic_connection_state& state,
    quic_encryption_level level, quic_crypto_direction direction,
    quic_cipher_suite suite, std::span<const std::byte> secret) {
    if (!state.connection_) {
        throw std::logic_error("cannot install QUIC traffic keys before connection initialization");
    }
    if (level != quic_encryption_level::handshake && level != quic_encryption_level::application) {
        throw std::invalid_argument("QUIC TLS may install only Handshake or 1-RTT traffic secrets");
    }
    if (direction != quic_crypto_direction::read && direction != quic_crypto_direction::write) {
        throw std::invalid_argument("invalid QUIC traffic-secret direction");
    }
    auto keys = derive_quic_packet_keys(state.crypto_, state.resource_, suite, direction, secret);
    configure_application_context(state, suite);
    install_aead_context(state, level, direction, suite, keys.traffic_secret(), keys.iv(),
        std::move(keys.aead()), std::move(keys.header_protection()));
}

void rethrow_quic_callback_failure(const quic_connection_state& state) {
    state.rethrow_failure();
}

}  // namespace ruvia::detail
