#include "ruvia/http/quic_tls_handshake.h"

#include <ngtcp2/ngtcp2.h>

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/http3/quic_connection_state.h"
#include "ruvia/http/detail/http3/quic_crypto_bridge.h"

namespace ruvia {
namespace {

ngtcp2_encryption_level to_ngtcp2_level(quic_encryption_level level) {
    switch (level) {
        case quic_encryption_level::initial:
            return NGTCP2_ENCRYPTION_LEVEL_INITIAL;
        case quic_encryption_level::handshake:
            return NGTCP2_ENCRYPTION_LEVEL_HANDSHAKE;
        case quic_encryption_level::application:
            return NGTCP2_ENCRYPTION_LEVEL_1RTT;
        case quic_encryption_level::early_data:
            throw std::invalid_argument("QUIC 0-RTT does not carry TLS CRYPTO frames");
    }
    throw std::invalid_argument("invalid QUIC TLS encryption level");
}

}  // namespace

quic_operation_status quic_tls_handshake::submit_crypto(
    quic_encryption_level level, std::span<const std::byte> bytes) {
    if (!state_->tls_driver_active_) {
        throw std::logic_error("TLS CRYPTO output is valid only during the synchronous TLS drive");
    }
    if (!state_->connection_) {
        throw std::logic_error("cannot submit TLS CRYPTO before ngtcp2 connection initialization");
    }
    const auto native_level = to_ngtcp2_level(level);
    if (bytes.empty()) {
        return quic_operation_status::accepted;
    }
    const int result = ngtcp2_conn_submit_crypto_data(state_->connection_, native_level,
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (result != 0) {
        throw quic_error(result == NGTCP2_ERR_NOMEM ? quic_error_code::resource_limit
                                                    : quic_error_code::protocol_failure,
            "ngtcp2 rejected TLS CRYPTO output");
    }
    return quic_operation_status::accepted;
}

void quic_tls_handshake::submit_secret(quic_encryption_level level,
    quic_crypto_direction direction, quic_cipher_suite suite,
    std::span<const std::byte> secret) {
    if (!state_->tls_driver_active_) {
        throw std::logic_error("TLS traffic secrets may be installed only during TLS drive");
    }
    detail::install_quic_traffic_secret(*state_, level, direction, suite, secret);
}

void quic_tls_handshake::submit_peer_transport_parameters(
    std::span<const std::byte> encoded_parameters) {
    if (!state_->tls_driver_active_) {
        throw std::logic_error("peer transport parameters may be submitted only during TLS drive");
    }
    if (!state_->connection_) {
        throw std::logic_error("cannot validate peer transport parameters before connection initialization");
    }
    const int result = ngtcp2_conn_decode_and_set_remote_transport_params(
        state_->connection_, reinterpret_cast<const uint8_t*>(encoded_parameters.data()),
        encoded_parameters.size());
    if (result == 0) {
        return;
    }

    constexpr std::string_view reason = "invalid peer QUIC transport parameters";
    const auto transport_error = result == NGTCP2_ERR_VERSION_NEGOTIATION_FAILURE
                                     ? NGTCP2_VERSION_NEGOTIATION_ERROR
                                     : NGTCP2_TRANSPORT_PARAMETER_ERROR;
    state_->latch_close_reason({.kind = quic_close_kind::transport,
        .code = transport_error,
        .frame_type = 0,
        .reason = std::span<const char>(reason.data(), reason.size())});
    throw quic_error(quic_error_code::protocol_failure,
        "ngtcp2 rejected peer QUIC transport parameters");
}

}  // namespace ruvia
