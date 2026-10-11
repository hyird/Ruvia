#include "ruvia/http/quic_tls_handshake.h"

#include <ngtcp2/ngtcp2.h>

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <variant>

#include "ruvia/http/http3_var_int.h"

#include "http3/quic_connection_state.h"
#include "http3/quic_crypto_bridge.h"

namespace ruvia {
namespace {

std::uint64_t peer_idle_timeout_ms(std::span<const std::byte> validated_parameters) {
    constexpr std::uint64_t idle_timeout_parameter = 0x01;
    auto remaining = std::span<const char>(
        reinterpret_cast<const char*>(validated_parameters.data()), validated_parameters.size());
    std::uint64_t idle_timeout{};
    while (!remaining.empty()) {
        const auto identifier = std::get<http3_var_int>(decode_http3_var_int(remaining));
        remaining = remaining.subspan(identifier.encoded_bytes_);
        const auto length = std::get<http3_var_int>(decode_http3_var_int(remaining));
        remaining = remaining.subspan(length.encoded_bytes_);
        if (identifier.value_ == idle_timeout_parameter) {
            idle_timeout = std::get<http3_var_int>(decode_http3_var_int(remaining)).value_;
        }
        remaining = remaining.subspan(static_cast<std::size_t>(length.value_));
    }
    return idle_timeout;
}

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
    quic_encryption_level level, std::span<const std::byte> bytes_value) {
    if (!state_->tls_driver_active_) {
        throw std::logic_error("TLS CRYPTO output is valid only during the synchronous TLS drive");
    }
    if (!state_->connection_) {
        throw std::logic_error("cannot submit TLS CRYPTO before ngtcp2 connection initialization");
    }
    const auto native_level = to_ngtcp2_level(level);
    if (bytes_value.empty()) {
        return quic_operation_status::accepted;
    }
    const int result_value = ngtcp2_conn_submit_crypto_data(state_->connection_, native_level,
        reinterpret_cast<const uint8_t*>(bytes_value.data()), bytes_value.size());
    if (result_value != 0) {
        throw quic_error(result_value == NGTCP2_ERR_NOMEM ? quic_error_code::resource_limit
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
    const int result_value = ngtcp2_conn_decode_and_set_remote_transport_params(
        state_->connection_, reinterpret_cast<const uint8_t*>(encoded_parameters.data()),
        encoded_parameters.size());
    if (result_value == 0) {
        if (state_->peer_transport_parameters_set_) {
            return;
        }
        auto idle_timeout = state_->config_.local_transport_parameters_.idle_timeout_ms_;
        // ngtcp2 has validated the complete block. Read the advertised milliseconds
        // directly because its native nanosecond duration cannot represent every wire value.
        const auto peer_idle_timeout = peer_idle_timeout_ms(encoded_parameters);
        // RFC 9000 section 10.1: zero disables only that endpoint's limit.
        if (idle_timeout == 0 || (peer_idle_timeout != 0 && peer_idle_timeout < idle_timeout)) {
            idle_timeout = peer_idle_timeout;
        }
        state_->negotiated_idle_timeout_ms_ = idle_timeout;
        state_->peer_transport_parameters_set_ = true;
        return;
    }

    constexpr std::string_view reason = "invalid peer QUIC transport parameters";
    const auto transport_error = result_value == NGTCP2_ERR_VERSION_NEGOTIATION_FAILURE
                                     ? NGTCP2_VERSION_NEGOTIATION_ERROR
                                     : NGTCP2_TRANSPORT_PARAMETER_ERROR;
    state_->latch_close_reason({.kind_ = quic_close_kind::transport,
        .code_ = transport_error,
        .frame_type_ = 0,
        .reason_ = std::span<const char>(reason.data(), reason.size())});
    throw quic_error(quic_error_code::protocol_failure,
        "ngtcp2 rejected peer QUIC transport parameters");
}

}  // namespace ruvia
