#include "http3/http3_server_stream_input.h"

#include <bit>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http3_peer_streams.h"
#include "ruvia/http/http3_var_int.h"

namespace ruvia::detail {
namespace {

bool is_protocol_error(const http3_connection_result& result_value) noexcept {
    return result_value.scope_ != http3_connection_error_scope::none ||
           result_value.status_ == http3_connection_status::stream_error ||
           result_value.status_ == http3_connection_status::connection_error;
}

}  // namespace

http3_server_stream_input::http3_server_stream_input(http3_sans_io_session_engine& session_value,
    worker_memory& worker_value, std::uint64_t epoch, std::uint64_t connection_generation,
    std::size_t max_tracked_streams)
    : session_(session_value),
      epoch_(epoch),
      connection_generation_(connection_generation),
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
    const http3_stream_buffer::borrowed_block& block) noexcept {
    if (!block || block.critical() != nullptr) {
        return {status_type::invalid_input};
    }
    return accept_bytes(block.id(), block.bytes());
}

http3_server_stream_input::result_type http3_server_stream_input::accept_peer_stream_data(
    http3_stream_id id, std::span<const std::byte> bytes_value) noexcept {
    if (!is_http3_client_unidirectional_stream_id(id.stream_id_)) {
        return {status_type::invalid_input};
    }
    return accept_bytes(id, bytes_value);
}

http3_server_stream_input::result_type http3_server_stream_input::accept_bytes(
    http3_stream_id id, std::span<const std::byte> bytes_value) noexcept {
    if (const auto identity = identity_status(id); identity != status_type::fed) {
        return {identity};
    }
    if (stopped_) {
        return {status_type::stopped};
    }

    status_type failure = status_type::fed;
    auto* state_value = find_or_create(id.stream_id_, failure);
    if (state_value == nullptr) {
        return {failure};
    }
    state_value->received_early_data_ = state_value->received_early_data_ || id.received_early_data_;
    if (state_value->phase_ == stream_phase_type::finished && !bytes_value.empty()) {
        return final_size_failure();
    }
    if (state_value->phase_ == stream_phase_type::reset && !bytes_value.empty()) {
        return final_size_failure();
    }
    const bool reset_pending = state_value->phase_ == stream_phase_type::reset_pending;
    if (state_value->qpack_blocked_ && !reset_pending) {
        return {status_type::invalid_input};
    }
    if (state_value->phase_ != stream_phase_type::open && !reset_pending) {
        return {status_type::closed_stream};
    }

    if (bytes_value.size() > std::numeric_limits<std::uint64_t>::max() - state_value->wire_bytes_) {
        return final_size_failure();
    }
    const auto new_wire_bytes = state_value->wire_bytes_ + bytes_value.size();
    if (new_wire_bytes > http3_var_int_max ||
        (state_value->final_size_.has_value() && new_wire_bytes > *state_value->final_size_) ||
        (reset_pending && (!state_value->reset_published_bytes_.has_value() ||
                              new_wire_bytes > *state_value->reset_published_bytes_))) {
        return final_size_failure();
    }
    const bool fin = !reset_pending && state_value->final_size_.has_value() &&
                     new_wire_bytes == *state_value->final_size_;
    const auto wire = std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size());
    if (state_value->qpack_blocked_ && reset_pending) {
        state_value->wire_bytes_ = new_wire_bytes;
        if (new_wire_bytes == *state_value->reset_published_bytes_) {
            return apply_peer_reset(id.stream_id_, *state_value);
        }
        return {status_type::deferred_reset};
    }
    auto result_value = feed_session(id.stream_id_, wire, fin, *state_value);
    if (result_value.status_ == status_type::fed && fin) {
        result_value.status_ = status_type::finished;
    }
    if (reset_pending && result_value.status_ == status_type::fed) {
        if (state_value->wire_bytes_ == *state_value->reset_published_bytes_) {
            return apply_peer_reset(id.stream_id_, *state_value);
        }
        result_value.status_ = status_type::deferred_reset;
    }
    return result_value;
}

http3_server_stream_input::result_type http3_server_stream_input::accept_control(
    const http3_stream_control& control) noexcept {
    if (const auto identity = identity_status(control.id_); identity != status_type::fed) {
        return {identity};
    }
    if (stopped_) {
        return {status_type::stopped};
    }

    switch (control.kind_) {
        case http3_stream_control::kind::connection_closed:
            stop();
            return {status_type::connection_closed};
        case http3_stream_control::kind::stream_reset: {
            status_type failure = status_type::fed;
            auto* state_value = find_or_create(control.id_.stream_id_, failure);
            if (state_value == nullptr) {
                return {failure};
            }
            state_value->received_early_data_ = state_value->received_early_data_ ||
                                                control.id_.received_early_data_;
            if (state_value->reset_published_bytes_.has_value()) {
                if (*state_value->reset_published_bytes_ != control.value_ ||
                    state_value->reset_error_code_ != control.stream_reset_error_code_) {
                    return final_size_failure();
                }
                return {state_value->phase_ == stream_phase_type::reset_pending
                            ? status_type::deferred_reset
                            : status_type::closed_stream};
            }
            if (state_value->phase_ != stream_phase_type::open && state_value->phase_ != stream_phase_type::finished) {
                return {status_type::closed_stream};
            }
            if ((state_value->final_size_.has_value() && state_value->phase_ != stream_phase_type::finished &&
                    !(state_value->qpack_blocked_ && state_value->pending_fin_ && *state_value->final_size_ == state_value->wire_bytes_)) ||
                control.value_ > http3_var_int_max || control.value_ < state_value->wire_bytes_ ||
                (state_value->phase_ == stream_phase_type::finished && control.value_ != state_value->wire_bytes_)) {
                return final_size_failure();
            }
            state_value->reset_published_bytes_ = control.value_;
            state_value->reset_error_code_ = control.stream_reset_error_code_;
            if (control.value_ != state_value->wire_bytes_) {
                state_value->phase_ = stream_phase_type::reset_pending;
                return {status_type::deferred_reset};
            }
            return apply_peer_reset(control.id_.stream_id_, *state_value);
        }
        case http3_stream_control::kind::stream_fin:
            return accept_fin(control);
        case http3_stream_control::kind::writable:
            return {status_type::ignored_control};
        case http3_stream_control::kind::tunnel_established:
            return {status_type::invalid_input};
    }
    return {status_type::invalid_input};
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
    stopped_ = true;
    for (auto& slot : streams_) {
        if (slot.occupied_) {
            clear_qpack(slot.state_);
            slot.state_.phase_ = stream_phase_type::connection_closed;
            finish_request_stream(slot.state_);
        }
    }
    session_.stop();
}

std::size_t http3_server_stream_input::tracked_stream_count() const noexcept {
    return tracked_stream_count_;
}

std::size_t http3_server_stream_input::observed_request_stream_count() const noexcept {
    return observed_request_stream_count_;
}

std::size_t http3_server_stream_input::active_request_stream_count() const noexcept {
    return active_request_stream_count_;
}

bool http3_server_stream_input::stopped() const noexcept {
    return stopped_;
}

http3_server_stream_input::status_type http3_server_stream_input::identity_status(
    const http3_stream_id& id) const noexcept {
    if (id.epoch_ != epoch_) {
        return status_type::foreign_epoch;
    }
    if (id.connection_generation_ != connection_generation_) {
        return status_type::stale_connection;
    }
    return status_type::fed;
}

http3_server_stream_input::result_type http3_server_stream_input::accept_fin(
    const http3_stream_control& control) noexcept {
    status_type failure = status_type::fed;
    auto* state_value = find_or_create(control.id_.stream_id_, failure);
    if (state_value == nullptr) {
        return {failure};
    }
    state_value->received_early_data_ = state_value->received_early_data_ ||
                                        control.id_.received_early_data_;
    if (state_value->reset_published_bytes_.has_value()) {
        return final_size_failure();
    }

    if (state_value->phase_ == stream_phase_type::reset || state_value->phase_ == stream_phase_type::cancelled ||
        state_value->phase_ == stream_phase_type::failed ||
        state_value->phase_ == stream_phase_type::connection_closed) {
        return {status_type::closed_stream};
    }
    if (state_value->final_size_.has_value()) {
        if (*state_value->final_size_ == control.value_) {
            return {status_type::duplicate_fin};
        }
        return final_size_failure();
    }
    if (state_value->phase_ != stream_phase_type::open) {
        return {status_type::closed_stream};
    }
    if (control.value_ > http3_var_int_max || control.value_ < state_value->wire_bytes_) {
        return final_size_failure();
    }

    state_value->final_size_ = control.value_;
    if (control.value_ != state_value->wire_bytes_) {
        return {status_type::deferred_fin};
    }
    if (state_value->qpack_blocked_) {
        state_value->pending_fin_ = true;
        return {status_type::deferred_qpack};
    }
    return feed_session(control.id_.stream_id_, {}, true, *state_value);
}

http3_server_stream_input::result_type http3_server_stream_input::apply_peer_reset(
    std::uint64_t stream_id, stream_state_type& state_value) noexcept {
    state_value.phase_ = stream_phase_type::reset;
    const auto result_value = session_.feed(stream_id, {}, false, true);
    clear_qpack(state_value);
    if (is_protocol_error(result_value)) {
        if (result_value.scope_ == http3_connection_error_scope::connection ||
            result_value.status_ == http3_connection_status::connection_error) {
            close_for_connection_error();
        } else {
            state_value.phase_ = stream_phase_type::failed;
            finish_request_stream(state_value);
        }
        return {status_type::protocol_error, result_value};
    }
    finish_request_stream(state_value);
    return {status_type::reset, result_value};
}

http3_server_stream_input::result_type http3_server_stream_input::final_size_failure() noexcept {
    // Inconsistent transport/buffer counts are not peer RESET evidence or
    // HTTP framing errors. Retire locally; the owner must close transport.
    stop();
    return {status_type::final_size_error};
}

http3_server_stream_input::result_type http3_server_stream_input::feed_session(std::uint64_t stream_id,
    std::string_view bytes_value, bool fin, stream_state_type& state_value) noexcept {
    const auto result_value = session_.feed(stream_id, bytes_value, fin);
    if (is_protocol_error(result_value) || result_value.status_ == http3_connection_status::reset) {
        if (result_value.scope_ == http3_connection_error_scope::connection ||
            result_value.status_ == http3_connection_status::connection_error) {
            close_for_connection_error();
        } else {
            state_value.phase_ = stream_phase_type::failed;
            finish_request_stream(state_value);
        }
        return {status_type::protocol_error, result_value};
    }
    state_value.wire_bytes_ += bytes_value.size();
    if (result_value.status_ == http3_connection_status::qpack_blocked) {
        if (result_value.consumed_bytes_ > bytes_value.size()) {
            return final_size_failure();
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

http3_server_stream_input::stream_state_type* http3_server_stream_input::find_or_create(
    std::uint64_t stream_id, status_type& failure) noexcept {
    if (!http3_stream_id_type(stream_id)) {
        failure = status_type::invalid_input;
        return nullptr;
    }
    std::uint64_t hash = stream_id;
    hash ^= hash >> 30;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27;
    hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31;
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(hash) & mask;
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

void http3_server_stream_input::close_for_connection_error() noexcept {
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
    for (const auto& slot : streams_) {
        if (slot.occupied_ && slot.stream_id_ == stream_id) {
            return slot.state_.received_early_data_;
        }
    }
    return false;
}

bool http3_server_stream_input::can_accept_input(std::uint64_t stream_id) const noexcept {
    if (blocked_qpack_count_ == 0) {
        return true;
    }
    for (const auto& slot : streams_) {
        if (slot.occupied_ && slot.stream_id_ == stream_id) {
            return !slot.state_.qpack_blocked_ || slot.state_.phase_ == stream_phase_type::reset_pending;
        }
    }
    return true;
}

std::optional<http3_server_stream_input::resumed_input_type> http3_server_stream_input::resume_qpack() noexcept {
    if (stopped_ || blocked_qpack_count_ == 0) {
        return std::nullopt;
    }
    for (std::size_t count = 0; count < streams_.size(); ++count) {
        auto& slot = streams_[next_qpack_resume_];
        next_qpack_resume_ = (next_qpack_resume_ + 1) % streams_.size();
        auto& state_value = slot.state_;
        if (!slot.occupied_ || !state_value.qpack_blocked_ || !session_.can_accept_input(slot.stream_id_, state_value.pending_bytes_.size())) {
            continue;
        }
        const auto result_value = session_.feed(slot.stream_id_, state_value.pending_bytes_, state_value.pending_fin_);
        if (result_value.status_ == http3_connection_status::qpack_blocked) {
            if (result_value.consumed_bytes_ > state_value.pending_bytes_.size()) {
                return resumed_input_type{slot.stream_id_, final_size_failure()};
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
            if (result_value.scope_ == http3_connection_error_scope::connection) {
                close_for_connection_error();
            } else {
                state_value.phase_ = stream_phase_type::failed;
                finish_request_stream(state_value);
            }
            return resumed_input_type{slot.stream_id_, {status_type::protocol_error, result_value}};
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
