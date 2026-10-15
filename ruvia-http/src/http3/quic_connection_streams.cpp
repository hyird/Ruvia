#include <algorithm>
#include <exception>
#include <string>

#include "ruvia/http/quic_connection.h"

#include "http3/quic_datagram.h"
#include "http3/quic_stream.h"

namespace ruvia {
namespace {

[[noreturn]] void throw_ngtcp_error(int error, const char* operation) {
    throw quic_error(quic_error_code::protocol_failure, std::string(operation) + ": " + ngtcp2_strerror(error));
}

}  // namespace

quic_stream_open_result quic_connection::open_stream(bool unidirectional) {
    auto& state_value = *impl_;
    state_value.rethrow_failure();
    if (state_value.terminal()) {
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
        throw_ngtcp_error(opened, "opening QUIC stream failed");
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
    if (!impl_ || impl_->terminal()) {
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
    auto& stream = detail::require_quic_stream(state_value, stream_id);
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
        throw_ngtcp_error(error, "resetting QUIC stream failed");
    }
    stream.send_reset_ = true;
    return quic_operation_status::accepted;
}

quic_operation_status quic_connection::stop_sending(std::uint64_t stream_id,
    std::uint64_t application_error) {
    auto& state_value = *impl_;
    state_value.rethrow_failure();
    auto& stream = detail::require_quic_stream(state_value, stream_id);
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
        throw_ngtcp_error(error, "stopping QUIC stream receive failed");
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
    auto& stream = detail::require_quic_stream(state_value, stream_id);
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
        throw_ngtcp_error(error, "terminating QUIC stream failed");
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
    auto& stream = detail::require_quic_stream(state_value, stream_id);
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
    auto& stream = detail::require_quic_stream(state_value, stream_id);
    if (state_value.connection_ == nullptr) {
        throw quic_error(quic_error_code::invalid_state, "QUIC connection is not initialized");
    }
    if (stream.writable_ && !stream.fin_submitted_ && !stream.send_reset_) {
        const int error = ngtcp2_conn_shutdown_stream_write(
            state_value.connection_, 0, static_cast<std::int64_t>(stream_id), 0);
        if (error != 0 && error != NGTCP2_ERR_STREAM_NOT_FOUND) {
            throw_ngtcp_error(error, "closing QUIC stream write side failed");
        }
        stream.send_reset_ = true;
    }
    if (stream.readable_ && !stream.receive_end_observed_) {
        const int error = ngtcp2_conn_shutdown_stream_read(
            state_value.connection_, 0, static_cast<std::int64_t>(stream_id), 0);
        if (error != 0 && error != NGTCP2_ERR_STREAM_NOT_FOUND) {
            throw_ngtcp_error(error, "closing QUIC stream read side failed");
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
