#include "http3/http3_server_stream_input.h"

#include <bit>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http3_peer_streams.h"

namespace ruvia::detail {
namespace {

bool is_protocol_error(const http3_connection_result& result_value) noexcept {
    return result_value.scope_ != http3_connection_error_scope::none ||
           result_value.status_ == http3_connection_status::stream_error ||
           result_value.status_ == http3_connection_status::connection_error;
}

bool is_connection_error(const http3_connection_result& result_value) noexcept {
    return result_value.scope_ == http3_connection_error_scope::connection ||
           result_value.status_ == http3_connection_status::connection_error;
}

std::uint64_t mix_stream_id(std::uint64_t stream_id) noexcept {
    stream_id ^= stream_id >> 30;
    stream_id *= 0xbf58476d1ce4e5b9ULL;
    stream_id ^= stream_id >> 27;
    stream_id *= 0x94d049bb133111ebULL;
    stream_id ^= stream_id >> 31;
    return stream_id;
}

}  // namespace

http3_server_stream_input::http3_server_stream_input(http3_sans_io_session_engine& session_value,
    worker_memory& worker_value, std::size_t max_tracked_streams)
    : session_(session_value),
      max_tracked_streams_(max_tracked_streams),
      streams_(worker_value.resource()) {
    if (max_tracked_streams_ == 0) {
        throw std::invalid_argument("HTTP/3 stream input capacity must be greater than zero");
    }
    const auto capacity = table_capacity(max_tracked_streams_);
    streams_.reserve(capacity);
    for (std::size_t i = 0; i < capacity; ++i) {
        streams_.emplace_back(worker_value.resource());
    }
}

http3_server_stream_input::result_type http3_server_stream_input::accept_data(
    std::uint64_t stream_id, bool received_early_data, std::span<const std::byte> bytes_value) noexcept {
    if (!is_http3_request_stream_id(stream_id)) {
        return {status_type::invalid_input};
    }
    return accept_bytes(stream_id, received_early_data, bytes_value);
}

http3_server_stream_input::result_type http3_server_stream_input::accept_peer_stream_data(
    std::uint64_t stream_id, std::span<const std::byte> bytes_value) noexcept {
    if (!is_http3_client_unidirectional_stream_id(stream_id)) {
        return {status_type::invalid_input};
    }
    return accept_bytes(stream_id, false, bytes_value);
}

http3_server_stream_input::result_type http3_server_stream_input::accept_bytes(
    std::uint64_t stream_id, bool received_early_data, std::span<const std::byte> bytes_value) noexcept {
    if (stopped_) {
        return {status_type::stopped};
    }
    status_type failure = status_type::fed;
    auto* state_value = find_or_create(stream_id, failure);
    if (state_value == nullptr) {
        return {failure};
    }
    state_value->received_early_data_ = state_value->received_early_data_ || received_early_data;
    if (state_value->phase_ == stream_phase_type::finished && !bytes_value.empty()) {
        // QUIC delivers no stream bytes after FIN.
        return invariant_failure();
    }
    if (state_value->phase_ != stream_phase_type::open) {
        return {status_type::closed_stream};
    }
    if (bytes_value.empty()) {
        return {status_type::fed};
    }
    if (state_value->qpack_blocked_) {
        // input_capacity() reports zero for a blocked stream; the owner holds
        // QUIC input until resume_qpack() parses the retained suffix.
        return invariant_failure();
    }
    return feed_session(stream_id,
        std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()), false,
        *state_value);
}

http3_server_stream_input::result_type http3_server_stream_input::accept_fin(std::uint64_t stream_id) noexcept {
    if (stopped_) {
        return {status_type::stopped};
    }
    status_type failure = status_type::fed;
    auto* state_value = find_or_create(stream_id, failure);
    if (state_value == nullptr) {
        return {failure};
    }
    if (state_value->phase_ == stream_phase_type::finished) {
        return {status_type::duplicate_fin};
    }
    if (state_value->phase_ != stream_phase_type::open) {
        return {status_type::closed_stream};
    }
    if (state_value->qpack_blocked_) {
        state_value->pending_fin_ = true;
        return {status_type::deferred_qpack};
    }
    return feed_session(stream_id, {}, true, *state_value);
}

http3_server_stream_input::result_type http3_server_stream_input::accept_reset(std::uint64_t stream_id) noexcept {
    if (stopped_) {
        return {status_type::stopped};
    }
    status_type failure = status_type::fed;
    auto* state_value = find_or_create(stream_id, failure);
    if (state_value == nullptr) {
        return {failure};
    }
    if (state_value->phase_ != stream_phase_type::open && state_value->phase_ != stream_phase_type::finished) {
        return {status_type::closed_stream};
    }
    state_value->phase_ = stream_phase_type::reset;
    const auto result_value = session_.feed(stream_id, {}, false, true);
    clear_qpack(*state_value);
    if (is_protocol_error(result_value)) {
        return protocol_failure(*state_value, result_value);
    }
    finish_request_stream(*state_value);
    return {status_type::reset, result_value};
}

http3_server_stream_input::result_type http3_server_stream_input::cancel_request(std::uint64_t stream_id) noexcept {
    if (stopped_) {
        return {status_type::stopped};
    }
    if (!is_http3_request_stream_id(stream_id)) {
        return {status_type::invalid_input};
    }
    status_type failure = status_type::fed;
    auto* state_value = find_or_create(stream_id, failure);
    if (state_value == nullptr) {
        return {failure};
    }
    if (state_value->phase_ != stream_phase_type::open && state_value->phase_ != stream_phase_type::finished) {
        return {status_type::closed_stream};
    }
    clear_qpack(*state_value);
    state_value->phase_ = stream_phase_type::cancelled;
    finish_request_stream(*state_value);
    // Missing session state is normal before HEADERS or after response release.
    (void)session_.cancel_request(stream_id);
    return {status_type::local_cancelled};
}

void http3_server_stream_input::stop() noexcept {
    if (stopped_) {
        return;
    }
    close_streams();
    session_.stop();
}

http3_server_stream_input::result_type http3_server_stream_input::invariant_failure() noexcept {
    // Inconsistent transport input is neither peer RESET evidence nor an HTTP
    // framing error. Retire locally; the owner must close the connection.
    stop();
    return {status_type::final_size_error};
}

http3_server_stream_input::result_type http3_server_stream_input::protocol_failure(
    stream_state_type& state_value, const http3_connection_result& result_value) noexcept {
    if (is_connection_error(result_value)) {
        close_streams();
    } else {
        state_value.phase_ = stream_phase_type::failed;
        finish_request_stream(state_value);
    }
    return {status_type::protocol_error, result_value};
}

http3_server_stream_input::result_type http3_server_stream_input::feed_session(std::uint64_t stream_id,
    std::string_view bytes_value, bool fin, stream_state_type& state_value) noexcept {
    const auto result_value = session_.feed(stream_id, bytes_value, fin);
    if (is_protocol_error(result_value) || result_value.status_ == http3_connection_status::reset) {
        return protocol_failure(state_value, result_value);
    }
    if (result_value.status_ == http3_connection_status::qpack_blocked) {
        if (result_value.consumed_bytes_ > bytes_value.size()) {
            return invariant_failure();
        }
        try {
            state_value.pending_bytes_.assign(bytes_value.substr(result_value.consumed_bytes_));
        } catch (...) {
            stop();
            return {status_type::capacity_exhausted};
        }
        if (!state_value.qpack_blocked_) {
            state_value.qpack_blocked_ = true;
            ++blocked_qpack_count_;
        }
        state_value.pending_fin_ = fin;
        return {status_type::deferred_qpack, result_value};
    }
    if (fin) {
        state_value.phase_ = stream_phase_type::finished;
        finish_request_stream(state_value);
        return {status_type::finished, result_value};
    }
    return {status_type::fed, result_value};
}

std::size_t http3_server_stream_input::table_capacity(std::size_t max_tracked_streams) {
    if (max_tracked_streams > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::length_error("HTTP/3 stream input capacity is too large");
    }
    const auto needed = max_tracked_streams * 2;
    const auto max_power_of_two = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (needed > max_power_of_two) {
        throw std::length_error("HTTP/3 stream input capacity is too large");
    }
    return std::bit_ceil(needed);
}

const http3_server_stream_input::stream_slot_type* http3_server_stream_input::find(
    std::uint64_t stream_id) const noexcept {
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(mix_stream_id(stream_id)) & mask;
    for (std::size_t probes = 0; probes < streams_.size(); ++probes) {
        const auto& slot = streams_[index];
        if (!slot.occupied_) {
            return nullptr;
        }
        if (slot.stream_id_ == stream_id) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

http3_server_stream_input::stream_state_type* http3_server_stream_input::find_or_create(
    std::uint64_t stream_id, status_type& failure) noexcept {
    if (!http3_stream_id_type(stream_id)) {
        failure = status_type::invalid_input;
        return nullptr;
    }
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(mix_stream_id(stream_id)) & mask;
    for (std::size_t probes = 0; probes < streams_.size(); ++probes) {
        auto& slot = streams_[index];
        if (!slot.occupied_) {
            if (tracked_stream_count_ >= max_tracked_streams_) {
                failure = status_type::capacity_exhausted;
                stop();
                return nullptr;
            }
            slot.stream_id_ = stream_id;
            slot.state_ = stream_state_type(streams_.get_allocator().resource());
            slot.state_.request_stream_ = is_http3_request_stream_id(stream_id);
            slot.state_.request_active_ = slot.state_.request_stream_;
            slot.occupied_ = true;
            ++tracked_stream_count_;
            if (slot.state_.request_stream_) {
                ++observed_request_stream_count_;
                ++active_request_stream_count_;
            }
            return &slot.state_;
        }
        if (slot.stream_id_ == stream_id) {
            return &slot.state_;
        }
        index = (index + 1) & mask;
    }
    failure = status_type::capacity_exhausted;
    stop();
    return nullptr;
}

void http3_server_stream_input::close_streams() noexcept {
    stopped_ = true;
    for (auto& slot : streams_) {
        if (slot.occupied_) {
            clear_qpack(slot.state_);
            slot.state_.phase_ = stream_phase_type::connection_closed;
            finish_request_stream(slot.state_);
        }
    }
}

void http3_server_stream_input::clear_qpack(stream_state_type& state_value) noexcept {
    if (state_value.qpack_blocked_) {
        if (blocked_qpack_count_ == 0) {
            std::terminate();
        }
        --blocked_qpack_count_;
        state_value.qpack_blocked_ = false;
    }
    state_value.pending_fin_ = false;
    std::pmr::string(state_value.pending_bytes_.get_allocator()).swap(state_value.pending_bytes_);
}

bool http3_server_stream_input::received_early_data(std::uint64_t stream_id) const noexcept {
    const auto* slot = find(stream_id);
    return slot != nullptr && slot->state_.received_early_data_;
}

bool http3_server_stream_input::qpack_blocked(std::uint64_t stream_id) const noexcept {
    if (blocked_qpack_count_ == 0) {
        return false;
    }
    const auto* slot = find(stream_id);
    return slot != nullptr && slot->state_.qpack_blocked_;
}

std::optional<http3_server_stream_input::resumed_input_type> http3_server_stream_input::resume_qpack() noexcept {
    if (stopped_ || blocked_qpack_count_ == 0) {
        return std::nullopt;
    }
    for (std::size_t count = 0; count < streams_.size(); ++count) {
        auto& slot = streams_[next_qpack_resume_];
        next_qpack_resume_ = (next_qpack_resume_ + 1) % streams_.size();
        auto& state_value = slot.state_;
        if (!slot.occupied_ || !state_value.qpack_blocked_ ||
            session_.input_capacity(slot.stream_id_) < state_value.pending_bytes_.size()) {
            continue;
        }
        const auto result_value = session_.feed(slot.stream_id_, state_value.pending_bytes_, state_value.pending_fin_);
        if (result_value.status_ == http3_connection_status::qpack_blocked) {
            if (result_value.consumed_bytes_ > state_value.pending_bytes_.size()) {
                return resumed_input_type{slot.stream_id_, invariant_failure()};
            }
            if (result_value.consumed_bytes_ == 0) {
                continue;
            }
            state_value.pending_bytes_.erase(0, result_value.consumed_bytes_);
            return resumed_input_type{slot.stream_id_, {status_type::deferred_qpack, result_value}};
        }
        const bool fin = state_value.pending_fin_;
        clear_qpack(state_value);
        if (is_protocol_error(result_value)) {
            return resumed_input_type{slot.stream_id_, protocol_failure(state_value, result_value)};
        }
        if (fin) {
            state_value.phase_ = stream_phase_type::finished;
            finish_request_stream(state_value);
        }
        return resumed_input_type{slot.stream_id_, {fin ? status_type::finished : status_type::fed, result_value}};
    }
    return std::nullopt;
}

void http3_server_stream_input::finish_request_stream(stream_state_type& state_value) noexcept {
    if (!state_value.request_active_) {
        return;
    }
    state_value.request_active_ = false;
    if (active_request_stream_count_ == 0) {
        std::terminate();
    }
    --active_request_stream_count_;
}

}  // namespace ruvia::detail
