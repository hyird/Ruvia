#include "http3/quic_stream.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>

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

void latch_callback_exception(quic_connection_state* state_value) noexcept {
    if (state_value != nullptr) {
        state_value->latch_failure(std::current_exception());
    }
}

}  // namespace

quic_connection_state::stream_state& require_quic_stream(quic_connection_state& state_value, std::uint64_t stream_id) {
    const auto found = state_value.streams_.find(stream_id);
    if (found == state_value.streams_.end() || found->second.retired_) {
        throw_invalid_stream("unknown QUIC stream");
    }
    return found->second;
}

quic_stream_write_result queue_stream_write(quic_connection_state& state_value,
    std::uint64_t stream_id, std::span<const std::byte> input, bool fin) {
    state_value.rethrow_failure();
    auto& stream = require_quic_stream(state_value, stream_id);
    if (!stream.writable_) {
        throw_invalid_stream("QUIC stream is not writable");
    }
    if (state_value.terminal()) {
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
        state_value.available_buffer()});
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
    auto& stream = require_quic_stream(state_value, stream_id);
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
    if (state_value.terminal()) {
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
    if (state_value.terminal()) {
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
    if (state_value.terminal() || state_value.streams_.empty()) {
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
    auto& stream = require_quic_stream(state_value, offered.stream_id_);
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

namespace {

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
            size > state_value->available_buffer()) {
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

}  // namespace

void configure_quic_stream_callbacks(ngtcp2_callbacks& callbacks) noexcept {
    callbacks.stream_open = &quic_stream_open_callback;
    callbacks.recv_stream_data = &quic_stream_data_callback;
    callbacks.stream_reset = &quic_stream_reset_callback;
    callbacks.recv_stop_sending = &quic_stream_stop_sending_callback;
    callbacks.stream_close2 = &quic_stream_close_callback;
    callbacks.acked_stream_data_offset = &quic_stream_acknowledged_callback;
}

}  // namespace ruvia::detail
