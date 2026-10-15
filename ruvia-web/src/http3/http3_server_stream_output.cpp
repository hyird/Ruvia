#include "http3/http3_server_stream_output.h"

#include <bit>
#include <limits>
#include <stdexcept>

#include "ruvia/http/http3_peer_streams.h"
#include "ruvia/http/http3_var_int.h"

namespace ruvia::detail {
namespace {

std::uint64_t hash_stream_id(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

bool direction_retired(ruvia::quic_operation_status status) noexcept {
    return status == ruvia::quic_operation_status::accepted ||
           status == ruvia::quic_operation_status::completed ||
           status == ruvia::quic_operation_status::retired;
}

}  // namespace

http3_server_stream_output::http3_server_stream_output(ruvia::quic_connection& connection,
    std::pmr::memory_resource* resource, http3_server_stream_output_config config)
    : connection_(&connection),
      max_tracked_streams_(config.max_tracked_streams_),
      write_timeout_(config.write_timeout_),
      streams_(resource) {
    if (max_tracked_streams_ == 0 || (write_timeout_ && write_timeout_->count() <= 0)) {
        throw std::invalid_argument("HTTP/3 server output capacities must be nonzero");
    }
    streams_.resize(table_capacity(max_tracked_streams_));
}

http3_server_stream_output::stream_slot_type* http3_server_stream_output::find(
    stream_id_type stream_id) noexcept {
    return const_cast<stream_slot_type*>(std::as_const(*this).find(stream_id));
}

const http3_server_stream_output::stream_slot_type* http3_server_stream_output::find(
    stream_id_type stream_id) const noexcept {
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(hash_stream_id(stream_id)) & mask;
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

http3_server_stream_output::stream_slot_type* http3_server_stream_output::find_or_create(
    stream_id_type stream_id) {
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(hash_stream_id(stream_id)) & mask;
    for (std::size_t probes = 0; probes < streams_.size(); ++probes) {
        auto& slot = streams_[index];
        if (!slot.occupied_) {
            if (tracked_streams_ >= max_tracked_streams_) {
                return nullptr;
            }
            slot = stream_slot_type{.stream_id_ = stream_id, .occupied_ = true};
            ++tracked_streams_;
            return &slot;
        }
        if (slot.stream_id_ == stream_id) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

http3_server_stream_output::stream_slot_type* http3_server_stream_output::writable_slot(
    stream_id_type stream_id) {
    if (connection_ == nullptr) {
        return nullptr;
    }
    if (!is_http3_request_stream_id(stream_id)) {
        // Server unidirectional output is limited to push streams opened here.
        auto* push = find(stream_id);
        return push != nullptr && push->info_.push_id_ ? push : nullptr;
    }
    auto* slot = find_or_create(stream_id);
    if (slot == nullptr) {
        failed_ = true;
    }
    return slot;
}

void http3_server_stream_output::start(stream_slot_type& slot) noexcept {
    if (!slot.started_) {
        slot.started_ = true;
        ++live_streams_;
    }
}

void http3_server_stream_output::blocked(stream_slot_type& slot,
    std::chrono::steady_clock::time_point now) noexcept {
    if (!slot.output_pending_) {
        slot.output_pending_ = true;
        slot.last_progress_ = now;
        ++pending_streams_;
    }
}

void http3_server_stream_output::progressed(stream_slot_type& slot,
    std::chrono::steady_clock::time_point now) noexcept {
    slot.last_progress_ = now;
}

void http3_server_stream_output::end(stream_slot_type& slot, stream_state_type state) noexcept {
    if (slot.output_pending_) {
        slot.output_pending_ = false;
        --pending_streams_;
    }
    if (slot.started_ && slot.info_.state_ == stream_state_type::open) {
        --live_streams_;
    }
    slot.info_.state_ = state;
}

http3_server_stream_output::status_type http3_server_stream_output::fail() noexcept {
    failed_ = true;
    return status_type::closed;
}

http3_server_stream_output::write_result_type http3_server_stream_output::write(
    stream_id_type stream_id, std::span<const std::byte> bytes) {
    auto* slot = writable_slot(stream_id);
    if (slot == nullptr || slot->info_.state_ != stream_state_type::open ||
        slot->info_.send_fin_accepted_) {
        return {.status_ = status_type::closed};
    }
    if (bytes.empty()) {
        return {.status_ = status_type::accepted};
    }
    if (bytes.size() > http3_var_int_max - slot->info_.accepted_wire_bytes_) {
        return {.status_ = fail()};
    }
    start(*slot);
    const auto now = std::chrono::steady_clock::now();
    ruvia::quic_stream_write_result written;
    try {
        written = connection_->write_stream(stream_id, bytes);
    } catch (const ruvia::quic_error&) {
        written.status_ = ruvia::quic_operation_status::closing;
    }
    if (written.status_ == ruvia::quic_operation_status::would_block ||
        written.status_ == ruvia::quic_operation_status::need_input ||
        (written.status_ == ruvia::quic_operation_status::accepted && written.accepted_ == 0)) {
        blocked(*slot, now);
        return {.status_ = status_type::blocked};
    }
    if (written.status_ == ruvia::quic_operation_status::stream_closed) {
        // The peer stopped reading; the owner retires this request.
        terminate(*slot, stream_state_type::cancelled,
            static_cast<std::uint64_t>(http3_connection_error_code::request_cancelled));
        return {.status_ = status_type::closed};
    }
    if (written.status_ != ruvia::quic_operation_status::accepted || written.accepted_ > bytes.size()) {
        terminate(*slot, stream_state_type::failed,
            static_cast<std::uint64_t>(http3_connection_error_code::internal_error));
        return {.status_ = fail()};
    }
    slot->info_.accepted_wire_bytes_ += written.accepted_;
    progressed(*slot, now);
    if (written.accepted_ == bytes.size()) {
        if (slot->output_pending_) {
            slot->output_pending_ = false;
            --pending_streams_;
        }
    } else {
        blocked(*slot, now);
    }
    return {.status_ = status_type::accepted, .accepted_ = written.accepted_};
}

http3_server_stream_output::status_type http3_server_stream_output::finish(stream_id_type stream_id) {
    auto* slot = writable_slot(stream_id);
    if (slot == nullptr || slot->info_.state_ != stream_state_type::open) {
        return status_type::closed;
    }
    if (!slot->info_.send_fin_accepted_) {
        start(*slot);
        auto finished = ruvia::quic_operation_status::closing;
        try {
            finished = connection_->finish_stream(stream_id);
        } catch (const ruvia::quic_error&) {
        }
        if (finished == ruvia::quic_operation_status::would_block ||
            finished == ruvia::quic_operation_status::need_input) {
            blocked(*slot, std::chrono::steady_clock::now());
            return status_type::blocked;
        }
        if (finished != ruvia::quic_operation_status::accepted) {
            terminate(*slot, stream_state_type::failed,
                static_cast<std::uint64_t>(http3_connection_error_code::internal_error));
            return finished == ruvia::quic_operation_status::stream_closed ? status_type::closed : fail();
        }
        slot->info_.send_fin_accepted_ = true;
    }
    auto retired = ruvia::quic_operation_status::closing;
    try {
        retired = connection_->retire_completed_stream(stream_id);
    } catch (const ruvia::quic_error&) {
    }
    if (retired == ruvia::quic_operation_status::would_block ||
        retired == ruvia::quic_operation_status::need_input || direction_retired(retired)) {
        // A request stream still receiving retires with its input direction.
        end(*slot, stream_state_type::finished);
        return status_type::accepted;
    }
    end(*slot, stream_state_type::failed);
    return fail();
}

http3_server_stream_output::status_type http3_server_stream_output::establish_tunnel(
    stream_id_type stream_id) {
    auto* slot = connection_ == nullptr || !is_http3_request_stream_id(stream_id) ? nullptr : find(stream_id);
    if (slot == nullptr || slot->info_.state_ != stream_state_type::open || slot->output_pending_ ||
        slot->info_.tunnel_established_) {
        return status_type::closed;
    }
    slot->info_.tunnel_established_ = true;
    return status_type::accepted;
}

std::optional<http3_server_stream_output::stream_id_type> http3_server_stream_output::open_push_stream(
    std::uint64_t push_id) {
    if (connection_ == nullptr || push_id > http3_var_int_max || tracked_streams_ >= max_tracked_streams_) {
        return std::nullopt;
    }
    ruvia::quic_stream_open_result opened;
    try {
        opened = connection_->open_stream(true);
    } catch (const ruvia::quic_error&) {
        failed_ = true;
        return std::nullopt;
    }
    if (opened.status_ == ruvia::quic_operation_status::would_block ||
        opened.status_ == ruvia::quic_operation_status::need_input) {
        return std::nullopt;
    }
    if (opened.status_ != ruvia::quic_operation_status::accepted ||
        http3_stream_id_type(opened.stream_id_) != http3_stream_id_type::server_unidirectional ||
        find(opened.stream_id_) != nullptr) {
        failed_ = true;
        return std::nullopt;
    }
    auto* slot = find_or_create(opened.stream_id_);
    slot->info_.push_id_ = push_id;
    return opened.stream_id_;
}

void http3_server_stream_output::cancel_stream(stream_id_type stream_id, std::uint64_t error_code) {
    if (connection_ == nullptr) {
        return;
    }
    auto* slot = find(stream_id);
    if (slot == nullptr) {
        if (!is_http3_request_stream_id(stream_id)) {
            return;
        }
        slot = find_or_create(stream_id);
        if (slot == nullptr) {
            failed_ = true;
            return;
        }
    }
    if (slot->info_.state_ == stream_state_type::cancelled || slot->info_.state_ == stream_state_type::failed) {
        return;
    }
    terminate(*slot, stream_state_type::cancelled, error_code);
    // The owner initiated this cancellation; it is not reported back.
    if (slot->terminated_unreported_) {
        slot->terminated_unreported_ = false;
        --unreported_streams_;
    }
}

void http3_server_stream_output::terminate(stream_slot_type& slot, stream_state_type state,
    std::uint64_t error_code) noexcept {
    const auto stream_id = slot.stream_id_;
    ruvia::quic_operation_status send{ruvia::quic_operation_status::retired};
    ruvia::quic_operation_status close{ruvia::quic_operation_status::retired};
    try {
        if (connection_->read_health(stream_id).status_ == ruvia::quic_stream_read_status::closed &&
            connection_->write_health(stream_id) == ruvia::quic_operation_status::retired) {
            // QUIC already retired both directions.
        } else if (slot.info_.push_id_) {
            send = slot.info_.send_fin_accepted_ ? ruvia::quic_operation_status::completed
                                                 : connection_->reset_stream(stream_id, error_code);
            close = connection_->close_stream(stream_id);
        } else if (slot.info_.send_fin_accepted_) {
            // FIN was accepted: stop only the receive direction and retire.
            send = ruvia::quic_operation_status::completed;
            close = connection_->close_stream(stream_id);
        } else {
            send = connection_->terminate_bidirectional_stream(stream_id, error_code);
            close = send;
        }
        if (!direction_retired(send) || !direction_retired(close)) {
            failed_ = true;
        }
    } catch (...) {
        failed_ = true;
    }
    end(slot, state);
    slot.info_.state_ = state;
    if (!slot.terminated_unreported_) {
        slot.terminated_unreported_ = true;
        ++unreported_streams_;
    }
}

std::optional<http3_server_stream_output::stream_info_type> http3_server_stream_output::stream_info(
    stream_id_type stream_id) const noexcept {
    const auto* slot = find(stream_id);
    return slot == nullptr ? std::nullopt : std::optional<stream_info_type>(slot->info_);
}

void http3_server_stream_output::detach() noexcept {
    if (connection_ == nullptr) {
        return;
    }
    for (auto& slot : streams_) {
        if (slot.occupied_ && slot.info_.state_ == stream_state_type::open && slot.started_) {
            end(slot, stream_state_type::cancelled);
        }
    }
    live_streams_ = 0;
    pending_streams_ = 0;
    connection_ = nullptr;
}

std::size_t http3_server_stream_output::table_capacity(std::size_t max_tracked_streams) {
    if (max_tracked_streams > std::numeric_limits<std::size_t>::max() / 4) {
        throw std::length_error("HTTP/3 server output stream capacity is too large");
    }
    return std::bit_ceil(max_tracked_streams * 2);
}

}  // namespace ruvia::detail
