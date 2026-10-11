#include "http3/quic_datagram.h"

#include <algorithm>
#include <exception>
#include <limits>

#include "ruvia/http/http3_var_int.h"
#include "ruvia/http/quic_connection.h"

namespace ruvia::detail {

std::size_t maximum_datagram_payload_size(const quic_connection_state& state_value) noexcept {
    if (state_value.connection_ == nullptr || state_value.terminal()) {
        return 0;
    }
    const auto* const peer = ngtcp2_conn_get_remote_transport_params2(state_value.connection_);
    if (peer == nullptr || peer->max_datagram_frame_size == 0 || peer->max_udp_payload_size == 0) {
        return 0;
    }
    const auto* const dcid = ngtcp2_conn_get_dcid2(state_value.connection_);
    if (dcid == nullptr) {
        return 0;
    }
    constexpr std::size_t short_header_bytes = 1;
    constexpr std::size_t packet_number_bytes = 4;
    constexpr std::size_t aead_tag_bytes = 16;
    const auto path_limit = std::min({ngtcp2_conn_get_path_max_tx_udp_payload_size2(state_value.connection_),
        static_cast<std::size_t>(peer->max_udp_payload_size), state_value.config_.limits_.max_datagram_size_});
    const auto packet_overhead = short_header_bytes + dcid->datalen + packet_number_bytes + aead_tag_bytes;
    if (path_limit <= packet_overhead) {
        return 0;
    }
    const auto packet_limit = path_limit - packet_overhead;
    const auto frame_limit = static_cast<std::size_t>(std::min<std::uint64_t>(
        peer->max_datagram_frame_size, std::numeric_limits<std::size_t>::max()));
    auto payload_limit = std::min(packet_limit, frame_limit);
    while (payload_limit != 0) {
        const auto frame_overhead = std::size_t{1} + http3_var_int_encoded_size(payload_limit);
        if (frame_limit >= frame_overhead && payload_limit <= frame_limit - frame_overhead) {
            return payload_limit;
        }
        --payload_limit;
    }
    return 0;
}

quic_datagram_write_status queue_datagram(quic_connection_state& state_value,
    std::span<const std::byte> payload_value) {
    const auto payload_limit = maximum_datagram_payload_size(state_value);
    state_value.rethrow_failure();
    if (state_value.terminal() || payload_limit == 0) {
        return quic_datagram_write_status::unavailable;
    }
    if (payload_value.size() > payload_limit || payload_value.size() > state_value.config_.limits_.max_datagram_size_) {
        return quic_datagram_write_status::too_large;
    }
    if (state_value.send_datagrams_.size() >= state_value.config_.limits_.max_datagrams_ ||
        payload_value.size() > state_value.available_buffer()) {
        return quic_datagram_write_status::dropped;
    }
    if (state_value.next_datagram_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw quic_error(quic_error_code::resource_limit, "QUIC DATAGRAM identifier space exhausted");
    }
    state_value.send_datagrams_.emplace_back(state_value.resource_);
    try {
        if (!payload_value.empty()) {
            state_value.send_datagrams_.back().bytes_.assign(payload_value.begin(), payload_value.end());
        }
    } catch (...) {
        state_value.send_datagrams_.pop_back();
        throw;
    }
    state_value.send_datagrams_.back().id_ = state_value.next_datagram_id_++;
    state_value.send_datagram_bytes_ += payload_value.size();
    return quic_datagram_write_status::queued;
}

quic_datagram_write_view next_datagram_write(quic_connection_state& state_value) noexcept {
    const auto payload_limit = maximum_datagram_payload_size(state_value);
    while (!state_value.send_datagrams_.empty()) {
        auto& datagram = state_value.send_datagrams_.front();
        if (datagram.bytes_.size() <= payload_limit) {
            return {.id_ = datagram.id_, .bytes_ = datagram.bytes_, .available_ = true};
        }
        state_value.send_datagram_bytes_ -= datagram.bytes_.size();
        state_value.send_datagrams_.pop_front();
    }
    return {};
}

void commit_datagram_write(quic_connection_state& state_value, std::uint64_t id,
    bool accepted) noexcept {
    if (!accepted || state_value.send_datagrams_.empty() || state_value.send_datagrams_.front().id_ != id) {
        return;
    }
    state_value.send_datagram_bytes_ -= state_value.send_datagrams_.front().bytes_.size();
    state_value.send_datagrams_.pop_front();
}

quic_datagram_result read_datagram_buffer(quic_connection_state& state_value,
    std::span<std::byte> output) noexcept {
    if (state_value.received_datagrams_.empty()) {
        return {.status_ = quic_datagram_status::would_block};
    }
    const auto& datagram = state_value.received_datagrams_.front();
    if (output.size() < datagram.bytes_.size()) {
        return {.status_ = quic_datagram_status::too_large,
            .size_ = datagram.bytes_.size(),
            .local_ = state_value.config_.local_address_,
            .peer_ = state_value.config_.peer_address_};
    }
    std::copy(datagram.bytes_.begin(), datagram.bytes_.end(), output.begin());
    const auto size = datagram.bytes_.size();
    state_value.received_datagram_bytes_ -= size;
    state_value.received_datagrams_.pop_front();
    return {.status_ = quic_datagram_status::received,
        .size_ = size,
        .local_ = state_value.config_.local_address_,
        .peer_ = state_value.config_.peer_address_};
}

namespace {

int datagram_received_callback(ngtcp2_conn*, std::uint32_t, const std::uint8_t* data, std::size_t size,
    void* user_data) noexcept {
    auto* const state_value = static_cast<quic_connection_state*>(user_data);
    if (state_value == nullptr) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    try {
        if (size != 0 && data == nullptr) {
            throw quic_error(quic_error_code::protocol_failure, "invalid ngtcp2 QUIC DATAGRAM callback arguments");
        }
        if (size > state_value->config_.limits_.max_datagram_size_ ||
            state_value->received_datagrams_.size() >= state_value->config_.limits_.max_datagrams_ ||
            size > state_value->available_buffer()) {
            return 0;
        }
        state_value->received_datagrams_.emplace_back(state_value->resource_);
        try {
            if (size != 0) {
                const auto* const first = reinterpret_cast<const std::byte*>(data);
                state_value->received_datagrams_.back().bytes_.assign(first, first + size);
            }
        } catch (...) {
            state_value->received_datagrams_.pop_back();
            throw;
        }
        state_value->received_datagram_bytes_ += size;
        return 0;
    } catch (...) {
        state_value->latch_failure(std::current_exception());
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

// ACK and loss reports need no bookkeeping: a queued DATAGRAM is released
// once ngtcp2 accepts it. Only IDs that were never issued are protocol errors.
int datagram_outcome_callback(std::uint64_t datagram_id, void* user_data, const char* unknown_message) noexcept {
    auto* const state_value = static_cast<quic_connection_state*>(user_data);
    if (state_value == nullptr) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    if (datagram_id >= state_value->next_datagram_id_) {
        state_value->latch_failure(std::make_exception_ptr(quic_error(quic_error_code::protocol_failure, unknown_message)));
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

int datagram_acknowledged_callback(ngtcp2_conn*, std::uint64_t datagram_id, void* user_data) noexcept {
    return datagram_outcome_callback(datagram_id, user_data, "ngtcp2 acknowledged an unknown QUIC DATAGRAM");
}

int datagram_lost_callback(ngtcp2_conn*, std::uint64_t datagram_id, void* user_data) noexcept {
    return datagram_outcome_callback(datagram_id, user_data, "ngtcp2 lost an unknown QUIC DATAGRAM");
}

}  // namespace

void configure_quic_datagram_callbacks(ngtcp2_callbacks& callbacks) noexcept {
    callbacks.recv_datagram = &datagram_received_callback;
    callbacks.ack_datagram = &datagram_acknowledged_callback;
    callbacks.lost_datagram = &datagram_lost_callback;
}

}  // namespace ruvia::detail
