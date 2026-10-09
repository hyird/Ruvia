#include "http3/quic_stream.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/http/quic_connection.h"

namespace ruvia::detail {
namespace {

using stream_state = quic_connection_state::stream_state;
using send_block = quic_connection_state::send_block;

[[noreturn]] void throw_invalid_stream(const char* message) {
    throw quic_error(quic_error_code::invalid_stream, message);
}

[[noreturn]] void throw_protocol_error(const char* message) {
    throw quic_error(quic_error_code::protocol_failure, message);
}

[[noreturn]] void throw_ngtcp_error(int error, const char* operation) {
    throw quic_error(quic_error_code::protocol_failure,
        std::string(operation) + ": " + ngtcp2_strerror(error));
}

stream_state& require_stream(quic_connection_state& state_value, std::uint64_t stream_id) {
    const auto found = state_value.streams_.find(stream_id);
    if (found == state_value.streams_.end() || found->second.retired_) {
        throw_invalid_stream("unknown QUIC stream");
    }
    return found->second;
}

const stream_state* find_stream(const quic_connection_state& state_value,
    std::uint64_t stream_id) noexcept {
    const auto found = state_value.streams_.find(stream_id);
    return found == state_value.streams_.end() || found->second.retired_ ? nullptr : &found->second;
}

void compact_input(stream_state& stream) {
    if (stream.input_offset_ == 0) {
        return;
    }
    const auto remaining = stream.input_.size() - stream.input_offset_;
    if (remaining != 0) {
        std::memmove(stream.input_.data(), stream.input_.data() + stream.input_offset_, remaining);
    }
    stream.input_.resize(remaining);
    stream.input_offset_ = 0;
}

std::size_t available_connection_buffer(const quic_connection_state& state_value) noexcept {
    const auto limit = state_value.config_.limits_.max_connection_buffer_size_;
    std::size_t retained{};
    for (const auto bytes : {state_value.retained_stream_input_bytes_, state_value.retained_stream_output_bytes_,
             state_value.received_datagram_bytes_, state_value.send_datagram_bytes_}) {
        if (bytes > limit - retained) {
            return 0;
        }
        retained += bytes;
    }
    return limit - retained;
}

bool terminal(const quic_connection_state& state_value) noexcept {
    return state_value.state_ == ruvia::quic_connection_state::failed ||
           state_value.state_ == ruvia::quic_connection_state::closing ||
           state_value.state_ == ruvia::quic_connection_state::draining ||
           state_value.state_ == ruvia::quic_connection_state::retired;
}

std::size_t varint_size(std::size_t value) noexcept {
    if (value <= 63) {
        return 1;
    }
    if (value <= 16383) {
        return 2;
    }
    if (value <= 1073741823) {
        return 4;
    }
    return 8;
}

void latch_callback_exception(quic_connection_state* state_value) noexcept {
    if (state_value != nullptr) {
        state_value->latch_failure(std::current_exception());
    }
}

}  // namespace

quic_stream_write_result queue_stream_write(quic_connection_state& state_value,
    std::uint64_t stream_id, std::span<const std::byte> input, bool fin) {
    state_value.rethrow_failure();
    auto& stream = require_stream(state_value, stream_id);
    if (!stream.writable_) {
        throw_invalid_stream("QUIC stream is not writable");
    }
    if (terminal(state_value)) {
        return {.status_ = quic_operation_status::closing};
    }
    if (stream.send_reset_ || stream.send_stopped_ || stream.library_closed_ || stream.send_fin_) {
        return {.status_ = quic_operation_status::stream_closed};
    }
    if (input.empty()) {
        if (fin) {
            stream.send_fin_ = true;
        }
        return {.status_ = quic_operation_status::accepted};
    }
    const auto stream_limit = state_value.config_.limits_.max_stream_buffer_size_;
    if (stream.retained_output_bytes_ > stream_limit) {
        throw_protocol_error("QUIC stream output accounting exceeds configured limits");
    }
    const auto count = std::min({input.size(), stream_limit - stream.retained_output_bytes_,
        available_connection_buffer(state_value)});
    if (count == 0) {
        return {.status_ = quic_operation_status::would_block};
    }
    if (count > NGTCP2_MAX_VARINT - stream.send_offset_) {
        throw quic_error(quic_error_code::resource_limit, "QUIC stream offset limit exceeded");
    }
    stream.output_.emplace_back(state_value.resource_);
    try {
        stream.output_.back().bytes_.assign(reinterpret_cast<const char*>(input.data()), count);
    } catch (...) {
        stream.output_.pop_back();
        throw;
    }
    stream.output_.back().offset_ = stream.send_offset_;
    stream.send_offset_ += count;
    stream.retained_output_bytes_ += count;
    state_value.retained_stream_output_bytes_ += count;
    if (fin && count == input.size()) {
        stream.send_fin_ = true;
    }
    return {.status_ = quic_operation_status::accepted, .accepted_ = count};
}

quic_stream_read_result read_stream_buffer(quic_connection_state& state_value,
    std::uint64_t stream_id, std::span<std::byte> output) {
    state_value.rethrow_failure();
    auto& stream = require_stream(state_value, stream_id);
    if (!stream.readable_) {
        throw_invalid_stream("QUIC stream is not readable");
    }
    const auto available = stream.input_.size() - stream.input_offset_;
    if (available == 0) {
        if (stream.peer_reset_error_) {
            stream.receive_end_observed_ = true;
            return {.status_ = quic_stream_read_status::reset,
                .peer_reset_error_code_ = stream.peer_reset_error_};
        }
        if (stream.receive_fin_ || stream.receive_end_observed_) {
            stream.receive_end_observed_ = true;
            return {.status_ = quic_stream_read_status::fin};
        }
        return {};
    }
    if (output.empty()) {
        return {};
    }
    const auto count = std::min(available, output.size());
    std::memcpy(output.data(), stream.input_.data() + stream.input_offset_, count);
    stream.input_offset_ += count;
    state_value.retained_stream_input_bytes_ -= count;
    if (stream.input_offset_ == stream.input_.size()) {
        stream.input_.clear();
        stream.input_offset_ = 0;
    }
    return {.status_ = quic_stream_read_status::data, .size_ = count};
}

bool stream_received_early_data(
    const quic_connection_state& state_value, std::uint64_t stream_id) noexcept {
    const auto found = state_value.streams_.find(stream_id);
    return found != state_value.streams_.end() && found->second.received_early_data_;
}

quic_stream_read_result inspect_stream_read(const quic_connection_state& state_value,
    std::uint64_t stream_id) noexcept {
    if (terminal(state_value)) {
        return {.status_ = quic_stream_read_status::closed};
    }
    const auto* const stream = find_stream(state_value, stream_id);
    if (stream == nullptr || !stream->readable_) {
        return {.status_ = quic_stream_read_status::closed};
    }
    if (stream->input_offset_ < stream->input_.size()) {
        return {.status_ = quic_stream_read_status::data,
            .size_ = stream->input_.size() - stream->input_offset_};
    }
    if (stream->peer_reset_error_) {
        return {.status_ = quic_stream_read_status::reset,
            .peer_reset_error_code_ = stream->peer_reset_error_};
    }
    if (stream->receive_fin_ || stream->receive_end_observed_) {
        return {.status_ = quic_stream_read_status::fin};
    }
    return {};
}

quic_operation_status inspect_stream_write(const quic_connection_state& state_value,
    std::uint64_t stream_id) noexcept {
    if (terminal(state_value)) {
        return quic_operation_status::closing;
    }
    const auto* const stream = find_stream(state_value, stream_id);
    if (stream == nullptr || !stream->writable_) {
        return quic_operation_status::retired;
    }
    if (stream->send_fin_ || stream->send_reset_ || stream->send_stopped_ || stream->library_closed_) {
        return quic_operation_status::stream_closed;
    }
    if (stream->retained_output_bytes_ >= state_value.config_.limits_.max_stream_buffer_size_ ||
        state_value.retained_stream_output_bytes_ >= state_value.config_.limits_.max_connection_buffer_size_) {
        return quic_operation_status::would_block;
    }
    return quic_operation_status::accepted;
}

quic_stream_write_view next_stream_write(quic_connection_state& state_value) noexcept {
    quic_stream_write_view result;
    if (terminal(state_value) || state_value.streams_.empty()) {
        return result;
    }
    auto selected = state_value.streams_.end();
    auto wrapped = state_value.streams_.end();
    for (auto current = state_value.streams_.begin(); current != state_value.streams_.end(); ++current) {
        const auto& stream = current->second;
        if (stream.retired_ || stream.send_reset_ || stream.send_stopped_ || stream.library_closed_) {
            continue;
        }
        bool has_data{};
        for (const auto& block : stream.output_) {
            if (block.submitted_ < block.bytes_.size()) {
                has_data = true;
                break;
            }
        }
        const bool has_fin = stream.send_fin_ && !stream.fin_submitted_ && !has_data;
        if (!has_data && !has_fin) {
            continue;
        }
        if (wrapped == state_value.streams_.end() || current->first < wrapped->first) {
            wrapped = current;
        }
        if (current->first >= state_value.next_send_stream_id_ &&
            (selected == state_value.streams_.end() || current->first < selected->first)) {
            selected = current;
        }
    }
    if (selected == state_value.streams_.end()) {
        selected = wrapped;
    }
    if (selected == state_value.streams_.end()) {
        return result;
    }
    result.stream_id_ = selected->first;
    state_value.next_send_stream_id_ = selected->first == std::numeric_limits<std::uint64_t>::max()
                                           ? 0
                                           : selected->first + 1;
    auto& stream = selected->second;
    for (const auto& block : stream.output_) {
        if (block.submitted_ == block.bytes_.size()) {
            continue;
        }
        result.ranges_[result.range_count_++] = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(block.bytes_.data() + block.submitted_),
            block.bytes_.size() - block.submitted_);
        if (result.range_count_ == result.ranges_.size()) {
            break;
        }
    }
    bool all_submitted = true;
    for (const auto& block : stream.output_) {
        if (block.submitted_ != block.bytes_.size()) {
            all_submitted = false;
            break;
        }
    }
    result.fin_ = stream.send_fin_ && !stream.fin_submitted_ && all_submitted;
    return result;
}

void commit_stream_write(quic_connection_state& state_value, const quic_stream_write_view& offered,
    std::size_t accepted, bool fin_submitted) {
    if (!offered || offered.range_count_ > offered.ranges_.size()) {
        throw_protocol_error("cannot commit an invalid QUIC stream write view");
    }
    auto& stream = require_stream(state_value, offered.stream_id_);
    std::size_t offered_size{};
    for (std::size_t index = 0; index < offered.range_count_; ++index) {
        if (offered.ranges_[index].size() > std::numeric_limits<std::size_t>::max() - offered_size) {
            throw_protocol_error("QUIC stream write view size overflows");
        }
        offered_size += offered.ranges_[index].size();
    }
    if (accepted > offered_size || (fin_submitted && !offered.fin_)) {
        throw_protocol_error("ngtcp2 accepted progress outside the offered QUIC stream view");
    }
    auto remaining = accepted;
    for (auto& block : stream.output_) {
        const auto available = block.bytes_.size() - block.submitted_;
        const auto count = std::min(remaining, available);
        block.submitted_ += count;
        remaining -= count;
        if (remaining == 0) {
            break;
        }
    }
    if (remaining != 0) {
        throw_protocol_error("ngtcp2 accepted more QUIC stream data than was offered");
    }
    const bool all_submitted = std::ranges::all_of(stream.output_, [](const auto& block) {
        return block.submitted_ == block.bytes_.size();
    });
    if (fin_submitted && (!stream.send_fin_ || !all_submitted || !offered.fin_)) {
        throw_protocol_error("ngtcp2 submitted an unrequested or premature QUIC stream FIN");
    }
    stream.fin_submitted_ = stream.fin_submitted_ || fin_submitted;
    if (state_value.streams_.empty()) {
        state_value.next_send_stream_id_ = 0;
    } else {
        state_value.next_send_stream_id_ = offered.stream_id_ == std::numeric_limits<std::uint64_t>::max()
                                               ? 0
                                               : offered.stream_id_ + 1;
    }
}

std::size_t acknowledge_stream_data_through(quic_connection_state& state_value,
    std::uint64_t stream_id, std::uint64_t offset, std::uint64_t size) {
    const auto found = state_value.streams_.find(stream_id);
    if (found == state_value.streams_.end()) {
        return 0;
    }
    auto& stream = found->second;
    if (size > NGTCP2_MAX_VARINT - offset || offset > stream.send_offset_ ||
        size > stream.send_offset_ - offset) {
        throw_protocol_error("QUIC stream ACK range is outside submitted stream offsets");
    }
    const auto ack_end = offset + size;
    std::size_t released{};
    // ngtcp2 reports its cumulative gap-free watermark here, not a single network
    // ACK frame range. Only pop the front so later unacknowledged SSO borrows stay put.
    while (!stream.output_.empty()) {
        const auto& block = stream.output_.front();
        const auto block_end = block.offset_ + block.bytes_.size();
        if (block.submitted_ != block.bytes_.size() || block_end > ack_end) {
            break;
        }
        const auto count = block.bytes_.size();
        if (count > stream.retained_output_bytes_ || count > state_value.retained_stream_output_bytes_) {
            throw_protocol_error("QUIC stream ACK accounting is inconsistent");
        }
        stream.retained_output_bytes_ -= count;
        state_value.retained_stream_output_bytes_ -= count;
        released += count;
        stream.output_.pop_front();
    }
    return released;
}

void retire_closed_streams(quic_connection_state& state_value, ngtcp2_conn* connection) noexcept {
    for (auto stream = state_value.streams_.begin(); stream != state_value.streams_.end();) {
        if (!stream->second.retired_ || !stream->second.library_closed_) {
            ++stream;
            continue;
        }
        if (connection != nullptr && stream->second.peer_initiated_ &&
            state_value.peer_streams_ever_ < state_value.config_.limits_.max_lifetime_peer_streams_) {
            if ((stream->first & 2U) != 0) {
                ngtcp2_conn_extend_max_streams_uni(connection, 1);
            } else {
                ngtcp2_conn_extend_max_streams_bidi(connection, 1);
            }
        }
        stream = state_value.streams_.erase(stream);
    }
}

std::size_t maximum_datagram_payload_size(const quic_connection_state& state_value) noexcept {
    if (state_value.connection_ == nullptr || terminal(state_value)) {
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
        const auto frame_overhead = std::size_t{1} + varint_size(payload_limit);
        if (frame_limit >= frame_overhead && payload_limit <= frame_limit - frame_overhead) {
            return payload_limit;
        }
        --payload_limit;
    }
    return 0;
}

quic_datagram_write_status queue_datagram(quic_connection_state& state_value,
    std::span<const std::byte> payload_value) {
    return queue_datagram_with_limit(state_value, payload_value, maximum_datagram_payload_size(state_value));
}

quic_datagram_write_status queue_datagram_with_limit(quic_connection_state& state_value,
    std::span<const std::byte> payload_value, std::size_t payload_limit) {
    state_value.rethrow_failure();
    if (terminal(state_value) || payload_limit == 0) {
        return quic_datagram_write_status::unavailable;
    }
    if (payload_value.size() > payload_limit || payload_value.size() > state_value.config_.limits_.max_datagram_size_) {
        return quic_datagram_write_status::too_large;
    }
    if (state_value.send_datagrams_.size() >= state_value.config_.limits_.max_datagrams_ ||
        payload_value.size() > available_connection_buffer(state_value)) {
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
    return next_datagram_write_with_limit(state_value, maximum_datagram_payload_size(state_value));
}

quic_datagram_write_view next_datagram_write_with_limit(quic_connection_state& state_value,
    std::size_t payload_limit) noexcept {
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

int quic_stream_open_callback(ngtcp2_conn*, std::int64_t stream_id,
    void* user_data) noexcept {
    auto* const state_value = static_cast<quic_connection_state*>(user_data);
    try {
        if (state_value == nullptr || stream_id < 0 ||
            static_cast<std::uint64_t>(stream_id) > NGTCP2_MAX_VARINT) {
            throw_protocol_error("ngtcp2 opened an invalid QUIC stream ID");
        }
        if (state_value->streams_.contains(static_cast<std::uint64_t>(stream_id))) {
            return 0;
        }
        if (state_value->streams_.size() >= state_value->config_.limits_.max_streams_ ||
            state_value->peer_streams_ever_ >= state_value->config_.limits_.max_lifetime_peer_streams_) {
            throw quic_error(quic_error_code::resource_limit, "QUIC peer stream capacity exceeded");
        }
        const bool local_initiator_bit = state_value->config_.role_ == quic_role::server;
        const bool peer_initiated = ((stream_id & 1) != 0) != local_initiator_bit;
        if (!peer_initiated) {
            throw_protocol_error("ngtcp2 peer stream callback used a locally initiated stream ID");
        }
        const bool unidirectional = (stream_id & 2) != 0;
        auto& stream = state_value->streams_.try_emplace(
                                                static_cast<std::uint64_t>(stream_id), state_value->resource_)
                           .first->second;
        stream.peer_initiated_ = true;
        stream.readable_ = true;
        stream.writable_ = !unidirectional;
        ++state_value->peer_streams_ever_;
        return 0;
    } catch (...) {
        latch_callback_exception(state_value);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

int quic_stream_data_callback(ngtcp2_conn* connection, std::uint32_t flags,
    std::int64_t stream_id, std::uint64_t offset, const std::uint8_t* data,
    std::size_t size, void* user_data, void*) noexcept {
    auto* const state_value = static_cast<quic_connection_state*>(user_data);
    try {
        if (state_value == nullptr || stream_id < 0 || (size != 0 && data == nullptr)) {
            throw_protocol_error("invalid ngtcp2 QUIC stream data callback arguments");
        }
        const auto found = state_value->streams_.find(static_cast<std::uint64_t>(stream_id));
        if (found == state_value->streams_.end()) {
            throw_invalid_stream("ngtcp2 delivered data for an unknown QUIC stream");
        }
        auto& stream = found->second;
        stream.received_early_data_ = stream.received_early_data_ ||
                                      (flags & NGTCP2_STREAM_DATA_FLAG_0RTT) != 0;
        if (!stream.readable_ || offset != stream.received_offset_ ||
            size > NGTCP2_MAX_VARINT - offset ||
            stream.receive_fin_ || stream.peer_reset_error_) {
            throw_protocol_error("ngtcp2 delivered non-contiguous or post-terminal stream data");
        }
        stream.received_offset_ += size;
        if (stream.retired_) {
            if (size != 0) {
                if (connection == nullptr) {
                    throw quic_error(quic_error_code::invalid_state, "QUIC stream callback has no connection");
                }
                ngtcp2_conn_extend_max_offset(connection, size);
            }
            return 0;
        }
        const auto buffered = stream.input_.size() - stream.input_offset_;
        if (size > state_value->config_.limits_.max_stream_buffer_size_ -
                       std::min(buffered, state_value->config_.limits_.max_stream_buffer_size_) ||
            size > available_connection_buffer(*state_value)) {
            throw quic_error(quic_error_code::resource_limit, "QUIC received stream buffer limit exceeded");
        }
        compact_input(stream);
        if (size != 0) {
            stream.input_.append(reinterpret_cast<const char*>(data), size);
            state_value->retained_stream_input_bytes_ += size;
        }
        if ((flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0) {
            stream.receive_fin_ = true;
        }
        return 0;
    } catch (...) {
        latch_callback_exception(state_value);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

int quic_stream_reset_callback(ngtcp2_conn*, std::int64_t stream_id,
    std::uint64_t final_size, std::uint64_t error_code, void* user_data,
    void*) noexcept {
    auto* const state_value = static_cast<quic_connection_state*>(user_data);
    try {
        if (state_value == nullptr || stream_id < 0 || final_size > NGTCP2_MAX_VARINT) {
            throw_protocol_error("invalid ngtcp2 QUIC stream reset callback arguments");
        }
        const auto found = state_value->streams_.find(static_cast<std::uint64_t>(stream_id));
        if (found == state_value->streams_.end()) {
            throw_invalid_stream("ngtcp2 reset an unknown QUIC stream");
        }
        // Application retirement does not retire ngtcp2's stream: the peer's
        // RESET_STREAM can still arrive in response to our STOP_SENDING.
        auto& stream = found->second;
        if (!stream.readable_ || final_size < stream.received_offset_ ||
            (stream.receive_fin_ && final_size != stream.received_offset_) ||
            error_code > NGTCP2_MAX_VARINT) {
            throw_protocol_error("ngtcp2 reset final size is inconsistent with received data");
        }
        // ngtcp2 restores credit for the final-size suffix not delivered to
        // this callback's owner; only record it here, without granting it twice.
        stream.received_offset_ = final_size;
        stream.peer_reset_error_ = error_code;
        return 0;
    } catch (...) {
        latch_callback_exception(state_value);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

int quic_stream_stop_sending_callback(ngtcp2_conn*, std::int64_t stream_id,
    std::uint64_t, void* user_data, void*) noexcept {
    auto* const state_value = static_cast<quic_connection_state*>(user_data);
    try {
        if (state_value == nullptr || stream_id < 0) {
            throw_protocol_error("invalid ngtcp2 QUIC STOP_SENDING callback arguments");
        }
        const auto found = state_value->streams_.find(static_cast<std::uint64_t>(stream_id));
        if (found == state_value->streams_.end()) {
            throw_invalid_stream("ngtcp2 stopped an unknown QUIC stream");
        }
        auto& stream = found->second;
        if (!stream.writable_) {
            throw_protocol_error("ngtcp2 STOP_SENDING callback targets a non-writable stream");
        }
        stream.send_stopped_ = true;
        return 0;
    } catch (...) {
        latch_callback_exception(state_value);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

int quic_stream_close_callback(ngtcp2_conn* connection, std::uint32_t,
    std::int64_t stream_id, std::uint64_t, std::uint64_t, void* user_data,
    void*) noexcept {
    auto* const state_value = static_cast<quic_connection_state*>(user_data);
    try {
        if (state_value == nullptr || stream_id < 0) {
            throw_protocol_error("invalid ngtcp2 QUIC stream close callback arguments");
        }
        const auto found = state_value->streams_.find(static_cast<std::uint64_t>(stream_id));
        if (found == state_value->streams_.end()) {
            return 0;
        }
        auto& stream = found->second;
        stream.library_closed_ = true;
        if (stream.retained_output_bytes_ > state_value->retained_stream_output_bytes_) {
            throw_protocol_error("QUIC retained stream output accounting underflow");
        }
        state_value->retained_stream_output_bytes_ -= stream.retained_output_bytes_;
        stream.retained_output_bytes_ = 0;
        stream.output_.clear();
        retire_closed_streams(*state_value, connection);
        return 0;
    } catch (...) {
        latch_callback_exception(state_value);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

int quic_stream_acknowledged_callback(ngtcp2_conn*, std::int64_t stream_id,
    std::uint64_t offset, std::uint64_t size, void* user_data, void*) noexcept {
    auto* const state_value = static_cast<quic_connection_state*>(user_data);
    try {
        if (state_value == nullptr || stream_id < 0) {
            throw_protocol_error("invalid ngtcp2 QUIC ACK callback arguments");
        }
        (void)acknowledge_stream_data_through(
            *state_value, static_cast<std::uint64_t>(stream_id), offset, size);
        return 0;
    } catch (...) {
        latch_callback_exception(state_value);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

int quic_datagram_received_callback(ngtcp2_conn*, std::uint32_t,
    const std::uint8_t* data, std::size_t size, void* user_data) noexcept {
    auto* const state_value = static_cast<quic_connection_state*>(user_data);
    try {
        if (state_value == nullptr || (size != 0 && data == nullptr)) {
            throw_protocol_error("invalid ngtcp2 QUIC DATAGRAM callback arguments");
        }
        if (size > state_value->config_.limits_.max_datagram_size_ ||
            state_value->received_datagrams_.size() >= state_value->config_.limits_.max_datagrams_ ||
            size > available_connection_buffer(*state_value)) {
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
        latch_callback_exception(state_value);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

int quic_datagram_acknowledged_callback(ngtcp2_conn*, std::uint64_t datagram_id,
    void* user_data) noexcept {
    auto* const state_value = static_cast<quic_connection_state*>(user_data);
    if (state_value == nullptr) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    if (datagram_id >= state_value->next_datagram_id_) {
        try {
            throw_protocol_error("ngtcp2 acknowledged an unknown QUIC DATAGRAM");
        } catch (...) {
            latch_callback_exception(state_value);
        }
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

int quic_datagram_lost_callback(ngtcp2_conn*, std::uint64_t datagram_id,
    void* user_data) noexcept {
    auto* const state_value = static_cast<quic_connection_state*>(user_data);
    if (state_value == nullptr) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    if (datagram_id >= state_value->next_datagram_id_) {
        try {
            throw_protocol_error("ngtcp2 lost an unknown QUIC DATAGRAM");
        } catch (...) {
            latch_callback_exception(state_value);
        }
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

void configure_quic_stream_callbacks(ngtcp2_callbacks& callbacks) noexcept {
    callbacks.stream_open = &quic_stream_open_callback;
    callbacks.recv_stream_data = &quic_stream_data_callback;
    callbacks.stream_reset = &quic_stream_reset_callback;
    callbacks.recv_stop_sending = &quic_stream_stop_sending_callback;
    callbacks.stream_close2 = &quic_stream_close_callback;
    callbacks.acked_stream_data_offset = &quic_stream_acknowledged_callback;
    callbacks.recv_datagram = &quic_datagram_received_callback;
    callbacks.ack_datagram = &quic_datagram_acknowledged_callback;
    callbacks.lost_datagram = &quic_datagram_lost_callback;
}

}  // namespace ruvia::detail

namespace ruvia {

quic_stream_open_result quic_connection::open_stream(bool unidirectional) {
    auto& state_value = *impl_;
    state_value.rethrow_failure();
    if (detail::terminal(state_value)) {
        return {.status_ = quic_operation_status::closing};
    }
    if (state_value.connection_ == nullptr) {
        throw quic_error(quic_error_code::invalid_state, "QUIC connection is not initialized");
    }
    if (state_value.config_.role_ == quic_role::client && !state_value.tls_handshake_complete_ &&
        !state_value.early_transport_parameters_set_) {
        if (state_value.early_transport_parameters_.empty()) {
            return {.status_ = quic_operation_status::would_block};
        }
        const int imported = ngtcp2_conn_decode_and_set_0rtt_transport_params(
            state_value.connection_,
            reinterpret_cast<const uint8_t*>(state_value.early_transport_parameters_.data()),
            state_value.early_transport_parameters_.size());
        if (imported != 0) {
            auto failure = std::make_exception_ptr(quic_error(
                quic_error_code::protocol_failure,
                "ngtcp2 rejected remembered QUIC transport parameters"));
            state_value.latch_failure(failure);
            std::rethrow_exception(failure);
        }
        state_value.early_transport_parameters_set_ = true;
        state_value.early_data_state_ = quic_early_data_state::available;
    }
    if (state_value.streams_.size() >= state_value.config_.limits_.max_streams_) {
        return {.status_ = quic_operation_status::would_block};
    }
    const auto local_count = static_cast<std::size_t>(std::ranges::count_if(
        state_value.streams_, [](const auto& item) { return !item.second.peer_initiated_; }));
    if (local_count >= state_value.config_.limits_.max_local_streams_) {
        return {.status_ = quic_operation_status::would_block};
    }
    std::int64_t stream_id{};
    const int opened = unidirectional
                           ? ngtcp2_conn_open_uni_stream(state_value.connection_, &stream_id, nullptr)
                           : ngtcp2_conn_open_bidi_stream(state_value.connection_, &stream_id, nullptr);
    if (opened == NGTCP2_ERR_STREAM_ID_BLOCKED) {
        return {.status_ = quic_operation_status::would_block};
    }
    if (opened != 0) {
        detail::throw_ngtcp_error(opened, "opening QUIC stream failed");
    }
    try {
        auto& stream = state_value.streams_.try_emplace(
                                               static_cast<std::uint64_t>(stream_id), state_value.resource_)
                           .first->second;
        stream.readable_ = !unidirectional;
        stream.writable_ = true;
        stream.accepted_ = true;
        stream.early_data_candidate_ = state_value.config_.role_ == quic_role::client &&
                                       !state_value.tls_handshake_complete_;
    } catch (...) {
        state_value.latch_failure(std::current_exception());
        throw;
    }
    return {.status_ = quic_operation_status::accepted,
        .stream_id_ = static_cast<std::uint64_t>(stream_id)};
}

quic_stream_accept_batch quic_connection::accept_streams() noexcept {
    quic_stream_accept_batch result;
    if (!impl_ || detail::terminal(*impl_)) {
        result.status_ = quic_operation_status::closing;
        return result;
    }
    for (auto& [stream_id, stream] : impl_->streams_) {
        if (!stream.peer_initiated_ || stream.accepted_ || stream.retired_ ||
            result.size_ == result.streams_.size()) {
            continue;
        }
        stream.accepted_ = true;
        result.streams_[result.size_++] = quic_stream_metadata{
            .stream_id_ = stream_id,
            .readable_ = stream.readable_,
            .writable_ = stream.writable_};
    }
    result.status_ = result.size_ == 0 ? quic_operation_status::need_input
                                       : quic_operation_status::accepted;
    return result;
}

quic_stream_read_result quic_connection::read_stream(std::uint64_t stream_id,
    std::span<std::byte> output) {
    auto& state_value = *impl_;
    const auto result_value = detail::read_stream_buffer(state_value, stream_id, output);
    if (result_value.status_ == quic_stream_read_status::data && state_value.connection_ != nullptr) {
        const int stream_error = ngtcp2_conn_extend_max_stream_offset(
            state_value.connection_, static_cast<std::int64_t>(stream_id), result_value.size_);
        if (stream_error != 0) {
            state_value.latch_failure(std::make_exception_ptr(quic_error(
                quic_error_code::protocol_failure, ngtcp2_strerror(stream_error))));
            state_value.rethrow_failure();
        }
        ngtcp2_conn_extend_max_offset(state_value.connection_, result_value.size_);
    }
    return result_value;
}

quic_stream_info quic_connection::stream_info(std::uint64_t stream_id) const noexcept {
    return impl_ ? quic_stream_info{.received_early_data_ =
                                        detail::stream_received_early_data(*impl_, stream_id)}
                 : quic_stream_info{};
}

std::size_t quic_connection::take_rejected_early_streams(
    std::span<std::uint64_t> output) noexcept {
    if (!impl_) {
        return 0;
    }
    auto& rejected = impl_->rejected_early_streams_;
    const auto count = std::min(output.size(), rejected.size());
    std::copy_n(rejected.begin(), count, output.begin());
    rejected.erase(rejected.begin(), rejected.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
}

quic_stream_read_result quic_connection::read_health(std::uint64_t stream_id) const noexcept {
    return impl_ ? detail::inspect_stream_read(*impl_, stream_id)
                 : quic_stream_read_result{.status_ = quic_stream_read_status::closed};
}

quic_stream_write_result quic_connection::write_stream(std::uint64_t stream_id,
    std::span<const std::byte> input, bool fin) {
    return detail::queue_stream_write(*impl_, stream_id, input, fin);
}

quic_operation_status quic_connection::finish_stream(std::uint64_t stream_id) {
    const auto result_value = detail::queue_stream_write(*impl_, stream_id, {}, true);
    return result_value.status_;
}

quic_operation_status quic_connection::write_health(std::uint64_t stream_id) const noexcept {
    return impl_ ? detail::inspect_stream_write(*impl_, stream_id)
                 : quic_operation_status::closing;
}

quic_operation_status quic_connection::reset_stream(std::uint64_t stream_id,
    std::uint64_t application_error) {
    auto& state_value = *impl_;
    state_value.rethrow_failure();
    auto& stream = detail::require_stream(state_value, stream_id);
    if (!stream.writable_) {
        throw quic_error(quic_error_code::invalid_stream, "QUIC stream is not writable");
    }
    if (application_error > NGTCP2_MAX_VARINT) {
        throw quic_error(quic_error_code::invalid_configuration, "QUIC stream reset code exceeds varint range");
    }
    if (stream.send_reset_ || stream.library_closed_) {
        return quic_operation_status::accepted;
    }
    if (state_value.connection_ == nullptr) {
        throw quic_error(quic_error_code::invalid_state, "QUIC connection is not initialized");
    }
    const int error = ngtcp2_conn_shutdown_stream_write(
        state_value.connection_, 0, static_cast<std::int64_t>(stream_id), application_error);
    if (error != 0 && error != NGTCP2_ERR_STREAM_NOT_FOUND) {
        detail::throw_ngtcp_error(error, "resetting QUIC stream failed");
    }
    stream.send_reset_ = true;
    return quic_operation_status::accepted;
}

quic_operation_status quic_connection::stop_sending(std::uint64_t stream_id,
    std::uint64_t application_error) {
    auto& state_value = *impl_;
    state_value.rethrow_failure();
    auto& stream = detail::require_stream(state_value, stream_id);
    if (!stream.readable_) {
        throw quic_error(quic_error_code::invalid_stream, "QUIC stream is not readable");
    }
    if (application_error > NGTCP2_MAX_VARINT) {
        throw quic_error(quic_error_code::invalid_configuration, "QUIC STOP_SENDING code exceeds varint range");
    }
    if (stream.receive_end_observed_ || stream.library_closed_) {
        return quic_operation_status::accepted;
    }
    if (state_value.connection_ == nullptr) {
        throw quic_error(quic_error_code::invalid_state, "QUIC connection is not initialized");
    }
    const int error = ngtcp2_conn_shutdown_stream_read(
        state_value.connection_, 0, static_cast<std::int64_t>(stream_id), application_error);
    if (error != 0 && error != NGTCP2_ERR_STREAM_NOT_FOUND) {
        detail::throw_ngtcp_error(error, "stopping QUIC stream receive failed");
    }
    const auto unread = stream.input_.size() - stream.input_offset_;
    if (unread != 0) {
        ngtcp2_conn_extend_max_offset(state_value.connection_, unread);
        state_value.retained_stream_input_bytes_ -= unread;
    }
    stream.input_.clear();
    stream.input_offset_ = 0;
    stream.receive_end_observed_ = true;
    return quic_operation_status::accepted;
}

quic_operation_status quic_connection::terminate_bidirectional_stream(
    std::uint64_t stream_id, std::uint64_t application_error) {
    auto& state_value = *impl_;
    state_value.rethrow_failure();
    auto& stream = detail::require_stream(state_value, stream_id);
    if (!stream.readable_ || !stream.writable_) {
        throw quic_error(quic_error_code::invalid_stream, "QUIC stream is not bidirectional");
    }
    if (application_error > NGTCP2_MAX_VARINT) {
        throw quic_error(quic_error_code::invalid_configuration, "QUIC stream termination code exceeds varint range");
    }
    if (state_value.connection_ == nullptr) {
        throw quic_error(quic_error_code::invalid_state, "QUIC connection is not initialized");
    }
    const int error = ngtcp2_conn_shutdown_stream(
        state_value.connection_, 0, static_cast<std::int64_t>(stream_id), application_error);
    if (error != 0 && error != NGTCP2_ERR_STREAM_NOT_FOUND) {
        detail::throw_ngtcp_error(error, "terminating QUIC stream failed");
    }
    const auto unread = stream.input_.size() - stream.input_offset_;
    if (unread != 0) {
        ngtcp2_conn_extend_max_offset(state_value.connection_, unread);
    }
    state_value.retained_stream_input_bytes_ -= unread;
    stream.input_.clear();
    stream.input_offset_ = 0;
    stream.send_reset_ = true;
    stream.receive_end_observed_ = true;
    stream.retired_ = true;
    detail::retire_closed_streams(state_value, state_value.connection_);
    return quic_operation_status::accepted;
}

quic_operation_status quic_connection::retire_completed_stream(std::uint64_t stream_id) {
    auto& state_value = *impl_;
    state_value.rethrow_failure();
    auto& stream = detail::require_stream(state_value, stream_id);
    const bool send_complete = !stream.writable_ ||
                               (stream.fin_submitted_ && !stream.send_reset_ && !stream.send_stopped_);
    const bool receive_complete = !stream.readable_ ||
                                  ((stream.receive_fin_ || stream.peer_reset_error_.has_value()) &&
                                      stream.receive_end_observed_ && stream.input_offset_ == stream.input_.size());
    if (!send_complete || !receive_complete) {
        return quic_operation_status::would_block;
    }
    stream.retired_ = true;
    detail::retire_closed_streams(state_value, state_value.connection_);
    return quic_operation_status::accepted;
}

quic_operation_status quic_connection::close_stream(std::uint64_t stream_id) {
    auto& state_value = *impl_;
    state_value.rethrow_failure();
    auto& stream = detail::require_stream(state_value, stream_id);
    if (state_value.connection_ == nullptr) {
        throw quic_error(quic_error_code::invalid_state, "QUIC connection is not initialized");
    }
    if (stream.writable_ && !stream.fin_submitted_ && !stream.send_reset_) {
        const int error = ngtcp2_conn_shutdown_stream_write(
            state_value.connection_, 0, static_cast<std::int64_t>(stream_id), 0);
        if (error != 0 && error != NGTCP2_ERR_STREAM_NOT_FOUND) {
            detail::throw_ngtcp_error(error, "closing QUIC stream write side failed");
        }
        stream.send_reset_ = true;
    }
    if (stream.readable_ && !stream.receive_end_observed_) {
        const int error = ngtcp2_conn_shutdown_stream_read(
            state_value.connection_, 0, static_cast<std::int64_t>(stream_id), 0);
        if (error != 0 && error != NGTCP2_ERR_STREAM_NOT_FOUND) {
            detail::throw_ngtcp_error(error, "closing QUIC stream read side failed");
        }
    }
    const auto unread = stream.input_.size() - stream.input_offset_;
    if (unread != 0) {
        ngtcp2_conn_extend_max_offset(state_value.connection_, unread);
        state_value.retained_stream_input_bytes_ -= unread;
    }
    stream.input_.clear();
    stream.input_offset_ = 0;
    stream.receive_end_observed_ = true;
    stream.retired_ = true;
    detail::retire_closed_streams(state_value, state_value.connection_);
    return quic_operation_status::accepted;
}

std::size_t quic_connection::max_datagram_payload_size() const noexcept {
    return impl_ ? detail::maximum_datagram_payload_size(*impl_) : 0;
}

quic_datagram_write_status quic_connection::write_datagram(
    std::span<const std::byte> payload_value) {
    return detail::queue_datagram(*impl_, payload_value);
}

quic_datagram_result quic_connection::read_datagram(std::span<std::byte> output) noexcept {
    return impl_ ? detail::read_datagram_buffer(*impl_, output)
                 : quic_datagram_result{.status_ = quic_datagram_status::would_block};
}

}  // namespace ruvia
