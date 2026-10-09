#include "http3/http3_server_stream_output.h"

#include <bit>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/pmr_resource.h"
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

}  // namespace

http3_server_stream_output::http3_server_stream_output(ruvia::quic_connection& connection,
    worker_memory& worker_value, std::uint64_t epoch, std::uint64_t connection_generation,
    http3_server_stream_output_config config)
    : http3_server_stream_output(connection, worker_value.resource(), epoch,
          connection_generation, config) {}

http3_server_stream_output::http3_server_stream_output(ruvia::quic_connection& connection,
    std::pmr::memory_resource* resource, std::uint64_t epoch,
    std::uint64_t connection_generation, http3_server_stream_output_config config)
    : connection_(connection),
      epoch_(epoch),
      connection_generation_(connection_generation),
      max_tracked_streams_(config.max_tracked_streams_),
      max_queued_blocks_(config.max_queued_blocks_),
      max_drive_work_items_(config.max_drive_work_items_),
      write_timeout_(config.write_timeout_),
      owner_thread_(std::this_thread::get_id()),
      streams_(pmr_resource_or_default(resource)),
      nodes_(pmr_resource_or_default(resource)) {
    if (max_tracked_streams_ == 0 || max_queued_blocks_ == 0 ||
        max_queued_blocks_ >= no_node || max_drive_work_items_ == 0 ||
        (write_timeout_ && write_timeout_->count() <= 0)) {
        throw std::invalid_argument("HTTP/3 server output capacities must be nonzero and representable");
    }
    if (connection_.info().state_ == ruvia::quic_connection_state::retired ||
        connection_.info().state_ == ruvia::quic_connection_state::failed) {
        throw std::invalid_argument("HTTP/3 server output requires a live QUIC connection");
    }
    streams_.resize(table_capacity(max_tracked_streams_));
    nodes_.resize(max_queued_blocks_);
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        nodes_[i].next_ = i + 1 == nodes_.size()
                              ? no_node
                              : static_cast<std::uint32_t>(i + 1);
    }
    free_node_ = 0;
}

http3_server_stream_output::~http3_server_stream_output() {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
    if (retirement_failed_ || queued_block_count_ != 0) {
        std::terminate();
    }
    for (const auto& slot : streams_) {
        if (slot.occupied_ &&
            (slot.info_.state_ == stream_state_type::stopping || !is_terminal(slot.info_.state_))) {
            std::terminate();
        }
    }
}

void http3_server_stream_output::require_owner_thread() const {
    if (std::this_thread::get_id() != owner_thread_) {
        throw std::logic_error("HTTP/3 server output used outside its owning worker thread");
    }
}

http3_server_stream_output::identity_status_type http3_server_stream_output::identity_status(
    const http3_stream_id& id) const noexcept {
    if (id.epoch_ != epoch_) {
        return identity_status_type::foreign_epoch;
    }
    if (id.connection_generation_ != connection_generation_) {
        return identity_status_type::stale_connection;
    }
    return identity_status_type::match;
}

bool http3_server_stream_output::valid_response_stream_id(stream_id_type stream_id) const noexcept {
    if (is_http3_request_stream_id(stream_id)) {
        return true;
    }
    if (http3_stream_id_type(stream_id) != http3_stream_id_type::server_unidirectional) {
        return false;
    }
    const auto* slot = find_stream(stream_id);
    return slot != nullptr && slot->info_.push_id_.has_value();
}

http3_server_stream_output::stream_slot_type* http3_server_stream_output::find_stream(stream_id_type stream_id) noexcept {
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(hash_stream_id(stream_id)) & mask;
    for (std::size_t probes = 0; probes < streams_.size(); ++probes) {
        auto& slot = streams_[index];
        if (!slot.occupied_) {
            return nullptr;
        }
        if (slot.info_.stream_id_ == stream_id) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

const http3_server_stream_output::stream_slot_type*
http3_server_stream_output::find_stream(stream_id_type stream_id) const noexcept {
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(hash_stream_id(stream_id)) & mask;
    for (std::size_t probes = 0; probes < streams_.size(); ++probes) {
        const auto& slot = streams_[index];
        if (!slot.occupied_) {
            return nullptr;
        }
        if (slot.info_.stream_id_ == stream_id) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

http3_server_stream_output::stream_slot_type*
http3_server_stream_output::find_or_create_stream(stream_id_type stream_id) {
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(hash_stream_id(stream_id)) & mask;
    for (std::size_t probes = 0; probes < streams_.size(); ++probes) {
        auto& slot = streams_[index];
        if (!slot.occupied_) {
            if (tracked_stream_count_ >= max_tracked_streams_) {
                return nullptr;
            }
            slot.info_ = stream_info_type{.stream_id_ = stream_id};
            slot.head_ = no_node;
            slot.tail_ = no_node;
            slot.occupied_ = true;
            if (tracked_stream_count_ == 0) {
                first_tracked_slot_ = index;
                round_robin_slot_ = index;
            } else {
                streams_[last_tracked_slot_].next_tracked_slot_ = index;
            }
            slot.next_tracked_slot_ = first_tracked_slot_;
            last_tracked_slot_ = index;
            ++tracked_stream_count_;
            return &slot;
        }
        if (slot.info_.stream_id_ == stream_id) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

bool http3_server_stream_output::is_terminal(stream_state_type state_value) const noexcept {
    return state_value == stream_state_type::stopping || state_value == stream_state_type::finished ||
           state_value == stream_state_type::reset || state_value == stream_state_type::cancelled ||
           state_value == stream_state_type::failed || state_value == stream_state_type::connection_closed;
}

http3_server_stream_output::result_type http3_server_stream_output::register_push_stream(stream_id_type stream_id, std::uint64_t push_id) {
    require_owner_thread();
    if (stopped_) {
        return {.status_ = status_type::stopped};
    }
    if (http3_stream_id_type(stream_id) != http3_stream_id_type::server_unidirectional ||
        push_id > http3_var_int_max || find_stream(stream_id) != nullptr) {
        return {.status_ = status_type::invalid_input};
    }
    auto* slot = find_or_create_stream(stream_id);
    if (slot == nullptr) {
        return {.status_ = status_type::capacity_exhausted};
    }
    slot->info_.push_id_ = push_id;
    return {.status_ = status_type::accepted};
}

http3_server_stream_output::result_type http3_server_stream_output::accept_data(http3_stream_buffer::borrowed_block& block) {
    require_owner_thread();
    if (!block || block.critical() != nullptr) {
        return {.status_ = status_type::invalid_input};
    }
    if (block.id().epoch_ != epoch_) {
        return {.status_ = status_type::foreign_epoch};
    }
    if (block.id().connection_generation_ != connection_generation_) {
        return {.status_ = status_type::stale_connection};
    }
    if (!valid_response_stream_id(block.id().stream_id_)) {
        return fail_connection(status_type::invalid_stream_id);
    }
    const auto* registered = find_stream(block.id().stream_id_);
    const auto expected_push = registered == nullptr ? std::nullopt : registered->info_.push_id_;
    if (block.id().push_id_ != expected_push) {
        return fail_connection(status_type::invalid_input);
    }
    return accept_addressed_data(block, block.id().stream_id_, block.id().epoch_, block.id().connection_generation_);
}
http3_server_stream_output::result_type http3_server_stream_output::accept_critical_data(http3_stream_buffer::borrowed_block& block, stream_id_type stream_id) {
    require_owner_thread();
    const auto* target = block.critical();
    if (!block || target == nullptr ||
        http3_stream_id_type(stream_id) != http3_stream_id_type::server_unidirectional) {
        return {.status_ = status_type::invalid_input};
    }
    if (const auto* slot = find_stream(stream_id); slot != nullptr && slot->info_.push_id_) {
        return {.status_ = status_type::invalid_input};
    }
    return accept_addressed_data(block, stream_id, target->epoch_, target->connection_generation_);
}
http3_server_stream_output::result_type http3_server_stream_output::accept_addressed_data(http3_stream_buffer::borrowed_block& block,
    stream_id_type stream_id, std::uint64_t epoch, std::uint64_t generation) {
    if (epoch != epoch_) {
        return {.status_ = status_type::foreign_epoch};
    }
    if (generation != connection_generation_) {
        return {.status_ = status_type::stale_connection};
    }
    if (stopped_) {
        return {.status_ = status_type::stopped};
    }
    if (const auto* existing = find_stream(stream_id);
        existing != nullptr &&
        (existing->info_.state_ == stream_state_type::cancelled ||
            existing->info_.state_ == stream_state_type::reset ||
            existing->info_.state_ == stream_state_type::failed ||
            existing->info_.state_ == stream_state_type::connection_closed)) {
        return {.status_ = status_type::closed_stream};
    }
    if (free_node_ == no_node) {
        return {.status_ = status_type::backpressured};
    }

    auto* slot = find_or_create_stream(stream_id);
    if (slot == nullptr) {
        return fail_connection(status_type::capacity_exhausted);
    }
    const auto bytes_value = block.bytes();
    if (slot->info_.send_fin_accepted_) {
        return bytes_value.empty() ? result_type{.status_ = status_type::closed_stream}
                                   : fail_connection(status_type::final_size_error);
    }
    if (slot->info_.state_ == stream_state_type::finished && !bytes_value.empty()) {
        return fail_connection(status_type::final_size_error);
    }
    if (is_terminal(slot->info_.state_)) {
        return {.status_ = status_type::closed_stream};
    }
    if (bytes_value.size() > std::numeric_limits<std::uint64_t>::max() - slot->info_.received_wire_bytes_) {
        return fail_connection(status_type::final_size_error);
    }
    const auto new_received = slot->info_.received_wire_bytes_ + bytes_value.size();
    if (new_received > http3_var_int_max ||
        (slot->info_.final_wire_bytes_ && new_received > *slot->info_.final_wire_bytes_)) {
        return fail_connection(status_type::final_size_error);
    }
    // A newly queued block is the first real pending output (or new write
    // activity); a deferred FIN alone must not start the timeout clock.
    slot->last_write_activity_ = std::chrono::steady_clock::now();

    const auto node_index = free_node_;
    auto& node_value = nodes_[node_index];
    free_node_ = node_value.next_;
    node_value.next_ = no_node;
    node_value.offset_ = 0;
    node_value.block_ = std::move(block);
    if (slot->tail_ == no_node) {
        slot->head_ = node_index;
    } else {
        nodes_[slot->tail_].next_ = node_index;
    }
    slot->tail_ = node_index;
    slot->info_.received_wire_bytes_ = new_received;
    slot->info_.queued_wire_bytes_ += bytes_value.size();
    ++slot->info_.queued_blocks_;
    ++queued_block_count_;
    if (slot->info_.final_wire_bytes_) {
        slot->info_.state_ = stream_state_type::fin_pending;
    }
    request_scan();
    return {.status_ = status_type::accepted};
}

http3_server_stream_output::result_type http3_server_stream_output::accept_control(
    const http3_stream_control& control) {
    require_owner_thread();
    const auto identity = identity_status(control.id_);
    if (identity == identity_status_type::foreign_epoch) {
        return {.status_ = status_type::foreign_epoch};
    }
    if (identity == identity_status_type::stale_connection) {
        return {.status_ = status_type::stale_connection};
    }
    if (control.kind_ == http3_stream_control::kind::connection_closed) {
        if (stopped_ && stop_complete_) {
            return {.status_ = status_type::connection_closed};
        }
        stopped_ = true;
        for (auto& slot : streams_) {
            if (slot.occupied_ && !is_terminal(slot.info_.state_)) {
                slot.info_.state_ = stream_state_type::connection_closed;
            }
        }
        const bool retired = close_connection_and_release();
        if (!retired) {
            return {.status_ = status_type::unsafe_to_release};
        }
        stop_complete_ = true;
        return {.status_ = status_type::connection_closed};
    }
    if (stopped_) {
        return {.status_ = status_type::stopped};
    }
    if (!valid_response_stream_id(control.id_.stream_id_)) {
        return fail_connection(status_type::invalid_stream_id);
    }
    const auto* registered = find_stream(control.id_.stream_id_);
    const auto expected_push = registered == nullptr ? std::nullopt : registered->info_.push_id_;
    if (control.id_.push_id_ != expected_push) {
        return fail_connection(status_type::invalid_input);
    }
    auto* slot = find_or_create_stream(control.id_.stream_id_);
    if (slot == nullptr) {
        return fail_connection(status_type::capacity_exhausted);
    }
    if (is_terminal(slot->info_.state_)) {
        if (slot->info_.state_ == stream_state_type::finished &&
            control.kind_ == http3_stream_control::kind::stream_fin &&
            slot->info_.final_wire_bytes_ && *slot->info_.final_wire_bytes_ == control.value_) {
            return {.status_ = status_type::duplicate_fin};
        }
        if (slot->info_.state_ == stream_state_type::finished &&
            control.kind_ == http3_stream_control::kind::stream_fin &&
            slot->info_.final_wire_bytes_ && *slot->info_.final_wire_bytes_ != control.value_) {
            return fail_connection(status_type::final_size_error);
        }
        return {.status_ = status_type::closed_stream};
    }

    switch (control.kind_) {
        case http3_stream_control::kind::connection_closed:
            break;
        case http3_stream_control::kind::stream_reset: {
            slot->info_.state_ = stream_state_type::stopping;
            const bool retired = retire_stream(*slot, stream_state_type::reset,
                static_cast<std::uint64_t>(control.stream_reset_error_code_));
            if (!retired) {
                return {.status_ = status_type::unsafe_to_release,
                    .termination_ = slot->info_.termination_};
            }
            return {.status_ = connection_retired_ ? status_type::connection_closed : status_type::reset,
                .termination_ = slot->info_.termination_};
        }
        case http3_stream_control::kind::writable:
            notify_transport_activity();
            return {.status_ = status_type::writable};
        case http3_stream_control::kind::tunnel_established:
            return {.status_ = status_type::invalid_input};
        case http3_stream_control::kind::stream_fin:
            if (control.value_ > http3_var_int_max || control.value_ < slot->info_.received_wire_bytes_) {
                return fail_connection(status_type::final_size_error);
            }
            if (slot->info_.final_wire_bytes_) {
                if (*slot->info_.final_wire_bytes_ == control.value_) {
                    return {.status_ = status_type::duplicate_fin};
                }
                return fail_connection(status_type::final_size_error);
            }
            slot->info_.final_wire_bytes_ = control.value_;
            slot->info_.state_ = stream_state_type::fin_pending;
            if (control.value_ == slot->info_.accepted_wire_bytes_ &&
                slot->info_.queued_wire_bytes_ == 0 && slot->head_ == no_node) {
                slot->last_write_activity_ = std::chrono::steady_clock::now();
            }
            request_scan();
            const bool finish_ready = control.value_ == slot->info_.accepted_wire_bytes_ &&
                                      slot->info_.queued_wire_bytes_ == 0 && slot->head_ == no_node;
            return {.status_ = finish_ready ? status_type::accepted : status_type::fin_deferred};
    }
    return {.status_ = status_type::closed_stream};
}

http3_server_stream_output::result_type http3_server_stream_output::cancel_stream(
    stream_id_type stream_id, std::uint64_t error_code) {
    require_owner_thread();
    if (stopped_) {
        return {.status_ = status_type::stopped};
    }
    if (!valid_response_stream_id(stream_id)) {
        return {.status_ = status_type::invalid_stream_id};
    }
    if (error_code > http3_var_int_max) {
        return {.status_ = status_type::invalid_input};
    }
    auto* slot = find_or_create_stream(stream_id);
    if (slot == nullptr) {
        return fail_connection(status_type::capacity_exhausted);
    }
    if (is_terminal(slot->info_.state_)) {
        return {.status_ = status_type::closed_stream,
            .termination_ = slot->info_.termination_};
    }
    slot->info_.state_ = stream_state_type::stopping;
    const bool retired = retire_stream(*slot, stream_state_type::cancelled, error_code);
    if (!retired) {
        return {.status_ = status_type::unsafe_to_release,
            .termination_ = slot->info_.termination_};
    }
    return {.status_ = connection_retired_ ? status_type::connection_closed : status_type::cancelled,
        .termination_ = slot->info_.termination_};
}

http3_server_stream_output::drive_result_type http3_server_stream_output::drive() {
    require_owner_thread();
    drive_result_type result;
    if (stopped_) {
        result.status_ = connection_retired_ ? status_type::connection_closed : status_type::stopped;
        return result;
    }
    if (retirement_failed_) {
        result.status_ = status_type::unsafe_to_release;
        return result;
    }
    if (round_remaining_slots_ == 0) {
        if (!write_timeout_ || live_stream_count() == 0) {
            return result;
        }
        round_remaining_slots_ = tracked_stream_count_;
        round_made_progress_ = false;
        scan_again_ = false;
    }

    while (round_remaining_slots_ != 0 && result.operations_ < max_drive_work_items_) {
        const auto index = round_robin_slot_;
        round_robin_slot_ = streams_[index].next_tracked_slot_;
        --round_remaining_slots_;
        ++result.scanned_slots_;
        auto& slot = streams_[index];
        if (is_terminal(slot.info_.state_)) {
            continue;
        }

        if (write_timed_out(slot, std::chrono::steady_clock::now())) {
            result.last_stream_id_ = slot.info_.stream_id_;
            slot.info_.timed_out_ = true;
            ++result.operations_;
            const bool retired = retire_stream(slot, stream_state_type::cancelled,
                static_cast<std::uint64_t>(http3_connection_error_code::request_cancelled));
            if (!retired) {
                result.status_ = status_type::unsafe_to_release;
                result.error_stream_id_ = slot.info_.stream_id_;
                result.transport_error_ = slot.info_.termination_.close_;
                result.termination_ = slot.info_.termination_;
                break;
            }
            if (connection_retired_) {
                result.status_ = status_type::connection_closed;
                result.termination_ = slot.info_.termination_;
                break;
            }
            ++result.timed_out_streams_;
            result.made_progress_ = true;
            round_made_progress_ = true;
            continue;
        }

        if (slot.head_ != no_node) {
            auto& node_value = nodes_[slot.head_];
            const auto bytes_value = node_value.block_.bytes();
            if (node_value.offset_ > bytes_value.size()) {
                result.last_stream_id_ = slot.info_.stream_id_;
                ++result.operations_;
                const auto failure = handle_write_failure(slot, transport_error_type::closing);
                result.status_ = failure.status_;
                result.error_stream_id_ = slot.info_.stream_id_;
                result.write_status_ = transport_error_type::closing;
                result.termination_ = failure.termination_;
                result.transport_error_ = failure.transport_error_;
                result.made_progress_ = slot.info_.state_ != stream_state_type::stopping;
                round_made_progress_ = round_made_progress_ || result.made_progress_;
                break;
            }
            const auto remaining = bytes_value.subspan(node_value.offset_);
            if (remaining.empty()) {
                result.last_stream_id_ = slot.info_.stream_id_;
                const auto index_to_release = slot.head_;
                slot.head_ = node_value.next_;
                if (slot.head_ == no_node) {
                    slot.tail_ = no_node;
                }
                --slot.info_.queued_blocks_;
                release_node(index_to_release);
                ++result.operations_;
                result.made_progress_ = true;
                round_made_progress_ = true;
                continue;
            }

            const auto input = std::span<const char>(
                reinterpret_cast<const char*>(remaining.data()), remaining.size());
            result.last_stream_id_ = slot.info_.stream_id_;
            const auto written = connection_.write_stream(slot.info_.stream_id_, std::as_bytes(input));
            ++result.operations_;
            ++result.write_calls_;
            slot.info_.last_write_status_ = written.status_;
            if (written.status_ == transport_error_type::would_block) {
                ++result.would_block_writes_;
                continue;
            }
            if (written.status_ != transport_error_type::accepted || written.accepted_ == 0 ||
                written.accepted_ > remaining.size() ||
                written.accepted_ > std::numeric_limits<std::uint64_t>::max() - slot.info_.accepted_wire_bytes_ ||
                written.accepted_ > slot.info_.queued_wire_bytes_) {
                const auto failure = handle_write_failure(slot, written.status_);
                result.status_ = failure.status_;
                result.error_stream_id_ = slot.info_.stream_id_;
                result.write_status_ = written.status_;
                result.termination_ = failure.termination_;
                result.transport_error_ = failure.transport_error_;
                result.made_progress_ = slot.info_.state_ != stream_state_type::stopping;
                round_made_progress_ = round_made_progress_ || result.made_progress_;
                break;
            }
            slot.info_.accepted_wire_bytes_ += written.accepted_;
            slot.info_.queued_wire_bytes_ -= written.accepted_;
            slot.last_write_activity_ = std::chrono::steady_clock::now();
            node_value.offset_ += written.accepted_;
            result.accepted_bytes_ += written.accepted_;
            result.made_progress_ = true;
            round_made_progress_ = true;
            if (node_value.offset_ == bytes_value.size()) {
                const auto index_to_release = slot.head_;
                slot.head_ = node_value.next_;
                if (slot.head_ == no_node) {
                    slot.tail_ = no_node;
                }
                --slot.info_.queued_blocks_;
                release_node(index_to_release);
            }
            continue;
        }

        if (slot.info_.final_wire_bytes_ && slot.info_.state_ == stream_state_type::fin_pending &&
            slot.info_.queued_wire_bytes_ == 0 &&
            slot.info_.accepted_wire_bytes_ == *slot.info_.final_wire_bytes_) {
            result.last_stream_id_ = slot.info_.stream_id_;
            ++result.operations_;
            const bool send_fin_was_accepted = slot.info_.send_fin_accepted_;
            transport_error_type error = transport_error_type::accepted;
            if (!finish_stream(slot, error)) {
                if (error == transport_error_type::would_block ||
                    error == transport_error_type::need_input) {
                    ++result.would_block_writes_;
                    continue;
                }
                result.status_ = connection_retired_  ? status_type::connection_closed
                                 : retirement_failed_ ? status_type::unsafe_to_release
                                                      : status_type::transport_error;
                result.error_stream_id_ = slot.info_.stream_id_;
                result.transport_error_ = error;
                result.made_progress_ = slot.info_.state_ != stream_state_type::stopping;
                round_made_progress_ = round_made_progress_ || result.made_progress_;
                break;
            }
            const bool completed = slot.info_.state_ == stream_state_type::finished;
            result.made_progress_ = result.made_progress_ || !send_fin_was_accepted || completed ||
                                    connection_retired_;
            round_made_progress_ = round_made_progress_ || result.made_progress_;
            if (connection_retired_) {
                result.status_ = status_type::connection_closed;
                result.transport_error_ = error;
                break;
            }
            if (completed) {
                ++result.finished_streams_;
            }
        }
    }

    if (result.status_ == status_type::idle && result.made_progress_) {
        result.status_ = result.finished_streams_ != 0 ? status_type::finished : status_type::progress;
    }
    if (stopped_ || retirement_failed_) {
        return result;
    }
    if (round_remaining_slots_ != 0) {
        result.needs_reschedule_ = true;
    } else if (round_made_progress_ || scan_again_) {
        round_remaining_slots_ = tracked_stream_count_;
        round_made_progress_ = false;
        scan_again_ = false;
        result.needs_reschedule_ = true;
    }
    return result;
}

void http3_server_stream_output::notify_transport_activity() {
    require_owner_thread();
    request_scan();
}

http3_server_stream_output::result_type http3_server_stream_output::stop() {
    require_owner_thread();
    if (stop_complete_) {
        return {.status_ = connection_retired_ ? status_type::connection_closed : status_type::stopped};
    }
    stopped_ = true;
    if (connection_retired_) {
        release_all_queues();
        stop_complete_ = true;
        return {.status_ = status_type::connection_closed};
    }
    if (retirement_failed_) {
        if (!close_connection_and_release()) {
            return {.status_ = status_type::unsafe_to_release};
        }
        stop_complete_ = true;
        return {.status_ = status_type::connection_closed};
    }

    for (auto& slot : streams_) {
        if (slot.occupied_ && !is_terminal(slot.info_.state_)) {
            slot.info_.state_ = stream_state_type::stopping;
        }
    }
    for (auto& slot : streams_) {
        if (!slot.occupied_ || slot.info_.state_ != stream_state_type::stopping) {
            continue;
        }
        if (!retire_stream(slot, stream_state_type::cancelled,
                static_cast<std::uint64_t>(http3_connection_error_code::request_cancelled))) {
            return {.status_ = status_type::unsafe_to_release,
                .termination_ = slot.info_.termination_};
        }
        if (connection_retired_) {
            stop_complete_ = true;
            return {.status_ = status_type::connection_closed};
        }
    }
    stop_complete_ = true;
    return {.status_ = status_type::stopped};
}

std::optional<http3_server_stream_output::stream_info_type>
http3_server_stream_output::stream_info(stream_id_type stream_id) const {
    require_owner_thread();
    const auto* const slot = find_stream(stream_id);
    if (slot == nullptr) {
        return std::nullopt;
    }
    return slot->info_;
}

std::size_t http3_server_stream_output::tracked_stream_count() const {
    require_owner_thread();
    return tracked_stream_count_;
}

std::size_t http3_server_stream_output::live_stream_count() const {
    require_owner_thread();
    std::size_t count{};
    auto index = first_tracked_slot_;
    for (std::size_t remaining = tracked_stream_count_; remaining != 0; --remaining) {
        const auto& slot = streams_[index];
        index = slot.next_tracked_slot_;
        if (slot.last_write_activity_ && !slot.info_.send_fin_accepted_ &&
            !is_terminal(slot.info_.state_)) {
            ++count;
        }
    }
    return count;
}

std::size_t http3_server_stream_output::pending_stream_count() const {
    require_owner_thread();
    std::size_t count{};
    auto index = first_tracked_slot_;
    for (std::size_t remaining = tracked_stream_count_; remaining != 0; --remaining) {
        const auto& slot = streams_[index];
        index = slot.next_tracked_slot_;
        if (!slot.last_write_activity_ || slot.info_.send_fin_accepted_ ||
            is_terminal(slot.info_.state_)) {
            continue;
        }
        const bool data_pending = slot.head_ != no_node;
        const bool fin_ready = slot.info_.final_wire_bytes_ && slot.info_.queued_wire_bytes_ == 0 &&
                               slot.info_.accepted_wire_bytes_ == *slot.info_.final_wire_bytes_;
        if (data_pending || fin_ready) {
            ++count;
        }
    }
    return count;
}

std::size_t http3_server_stream_output::queued_block_count() const {
    require_owner_thread();
    return queued_block_count_;
}

bool http3_server_stream_output::stopped() const {
    require_owner_thread();
    return stopped_;
}

bool http3_server_stream_output::connection_retired() const {
    require_owner_thread();
    return connection_retired_;
}

bool http3_server_stream_output::connection_identity_gone() {
    return connection_.info().state_ == ruvia::quic_connection_state::retired;
}

bool http3_server_stream_output::close_connection_and_release() {
    if (connection_retired_) {
        stopped_ = true;
        stop_complete_ = true;
        mark_connection_closed();
        release_all_queues();
        return true;
    }
    static constexpr std::string_view reason = "HTTP/3 server output closed";
    const auto close = connection_.close({.kind_ = ruvia::quic_close_kind::application,
        .code_ = static_cast<std::uint64_t>(http3_connection_error_code::internal_error),
        .reason_ = {reason.data(), reason.size()}});
    if (close == transport_error_type::accepted || close == transport_error_type::completed ||
        close == transport_error_type::closing || close == transport_error_type::draining ||
        (close == transport_error_type::retired && connection_identity_gone())) {
        connection_retired_ = true;
        stopped_ = true;
        stop_complete_ = true;
        mark_connection_closed();
        release_all_queues();
        retirement_failed_ = false;
        return true;
    }
    retirement_failed_ = true;
    return false;
}

bool http3_server_stream_output::retire_stream(stream_slot_type& slot, stream_state_type terminal_state,
    std::uint64_t error_code) {
    slot.info_.state_ = stream_state_type::stopping;
    if (http3_stream_id_type(slot.info_.stream_id_) == http3_stream_id_type::server_unidirectional &&
        !slot.info_.push_id_) {
        // A local critical stream cannot be reset independently (RFC 9114).
        return close_connection_and_release();
    }
    if (connection_.read_health(slot.info_.stream_id_).status_ == ruvia::quic_stream_read_status::closed &&
        connection_.write_health(slot.info_.stream_id_) == transport_error_type::retired) {
        slot.info_.termination_ = {.send_ = transport_error_type::retired, .close_ = transport_error_type::retired};
    } else if (slot.info_.push_id_) {
        slot.info_.termination_ = {
            .send_ = slot.info_.send_fin_accepted_ ? transport_error_type::completed
                                                   : connection_.reset_stream(slot.info_.stream_id_, error_code),
            .close_ = connection_.close_stream(slot.info_.stream_id_)};
    } else {
        const auto terminated = connection_.terminate_bidirectional_stream(
            slot.info_.stream_id_, error_code);
        slot.info_.termination_ = {.send_ = terminated, .close_ = terminated};
    }
    const auto direction_retired = [](transport_error_type status) noexcept {
        return status == transport_error_type::accepted || status == transport_error_type::completed ||
               status == transport_error_type::retired;
    };
    if (direction_retired(slot.info_.termination_.send_) &&
        direction_retired(slot.info_.termination_.close_)) {
        slot.info_.state_ = terminal_state;
        release_stream_queue(slot);
        request_scan();
        return true;
    }
    if (slot.info_.termination_.close_ == transport_error_type::retired &&
        connection_identity_gone()) {
        connection_retired_ = true;
        stopped_ = true;
        stop_complete_ = true;
        mark_connection_closed();
        release_all_queues();
        return true;
    }
    if (close_connection_and_release()) {
        return true;
    }
    return false;
}

void http3_server_stream_output::release_node(std::uint32_t index) noexcept {
    auto& node_value = nodes_[index];
    node_value.block_.release();
    node_value.offset_ = 0;
    node_value.next_ = free_node_;
    free_node_ = index;
    if (queued_block_count_ == 0) {
        std::terminate();
    }
    --queued_block_count_;
}

void http3_server_stream_output::release_stream_queue(stream_slot_type& slot) noexcept {
    auto index = slot.head_;
    while (index != no_node) {
        const auto next_value = nodes_[index].next_;
        release_node(index);
        index = next_value;
    }
    slot.head_ = no_node;
    slot.tail_ = no_node;
    slot.info_.queued_blocks_ = 0;
    slot.info_.queued_wire_bytes_ = 0;
}

void http3_server_stream_output::release_all_queues() noexcept {
    for (auto& slot : streams_) {
        if (slot.occupied_) {
            release_stream_queue(slot);
        }
    }
}

void http3_server_stream_output::mark_connection_closed() noexcept {
    for (auto& slot : streams_) {
        if (slot.occupied_ && slot.info_.state_ != stream_state_type::finished &&
            slot.info_.state_ != stream_state_type::reset && slot.info_.state_ != stream_state_type::cancelled &&
            slot.info_.state_ != stream_state_type::failed) {
            slot.info_.state_ = stream_state_type::connection_closed;
        }
    }
}

http3_server_stream_output::result_type http3_server_stream_output::fail_connection(
    status_type status, transport_error_type error) {
    stopped_ = true;
    for (auto& slot : streams_) {
        if (slot.occupied_ && !is_terminal(slot.info_.state_)) {
            slot.info_.state_ = stream_state_type::failed;
        }
    }
    if (!close_connection_and_release()) {
        return {.status_ = status_type::unsafe_to_release, .transport_error_ = error};
    }
    stop_complete_ = true;
    return {.status_ = status, .transport_error_ = error};
}

http3_server_stream_output::result_type http3_server_stream_output::handle_write_failure(
    stream_slot_type& slot, transport_error_type write_status) {
    slot.info_.last_write_status_ = write_status;
    slot.info_.state_ = stream_state_type::stopping;
    const bool stream_closed = write_status == transport_error_type::stream_closed;
    const bool retired = retire_stream(slot,
        stream_closed ? stream_state_type::cancelled : stream_state_type::failed,
        static_cast<std::uint64_t>(stream_closed ? http3_connection_error_code::request_cancelled
                                                 : http3_connection_error_code::internal_error));
    if (!retired) {
        return {.status_ = status_type::unsafe_to_release,
            .write_status_ = write_status,
            .termination_ = slot.info_.termination_};
    }
    return {.status_ = connection_retired_ ? status_type::connection_closed
                       : stream_closed     ? status_type::cancelled
                                           : status_type::transport_error,
        .transport_error_ = slot.info_.termination_.send_,
        .write_status_ = write_status,
        .termination_ = slot.info_.termination_};
}

bool http3_server_stream_output::finish_stream(stream_slot_type& slot, transport_error_type& error) {
    if (!slot.info_.send_fin_accepted_) {
        error = connection_.finish_stream(slot.info_.stream_id_);
        slot.info_.finish_error_ = error;
        if (error == transport_error_type::would_block || error == transport_error_type::need_input) {
            return false;
        }
        if (error != transport_error_type::accepted) {
            slot.info_.state_ = stream_state_type::stopping;
            const bool retired = retire_stream(slot, stream_state_type::failed,
                static_cast<std::uint64_t>(http3_connection_error_code::internal_error));
            if (!retired) {
                retirement_failed_ = true;
            }
            return false;
        }
        slot.info_.send_fin_accepted_ = true;
    }

    const auto retired = connection_.retire_completed_stream(slot.info_.stream_id_);
    if (retired == transport_error_type::would_block || retired == transport_error_type::need_input) {
        error = retired;
        return false;
    }
    if (retired == transport_error_type::accepted || retired == transport_error_type::completed ||
        retired == transport_error_type::retired) {
        slot.info_.state_ = stream_state_type::finished;
        slot.info_.finish_error_ = transport_error_type::accepted;
        return true;
    }
    error = retired;
    slot.info_.finish_error_ = retired;
    if (retired == transport_error_type::retired && connection_identity_gone()) {
        connection_retired_ = true;
        stopped_ = true;
        stop_complete_ = true;
        mark_connection_closed();
        release_all_queues();
        return true;
    }
    if (close_connection_and_release()) {
        return true;
    }
    retirement_failed_ = true;
    return false;
}

bool http3_server_stream_output::write_timed_out(const stream_slot_type& slot,
    std::chrono::steady_clock::time_point now) const noexcept {
    const bool output_pending = slot.head_ != no_node ||
                                (slot.info_.final_wire_bytes_ &&
                                    slot.info_.queued_wire_bytes_ == 0 &&
                                    slot.info_.accepted_wire_bytes_ == *slot.info_.final_wire_bytes_);
    if (!write_timeout_ || !slot.last_write_activity_ || !output_pending ||
        slot.info_.send_fin_accepted_ || now < *slot.last_write_activity_) {
        return false;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now - *slot.last_write_activity_) >= *write_timeout_;
}

void http3_server_stream_output::request_scan() noexcept {
    if (stopped_) {
        return;
    }
    if (round_remaining_slots_ == 0) {
        round_remaining_slots_ = tracked_stream_count_;
        round_made_progress_ = false;
        scan_again_ = false;
    } else {
        scan_again_ = true;
    }
}

std::size_t http3_server_stream_output::table_capacity(std::size_t max_tracked_streams) {
    if (max_tracked_streams > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::length_error("HTTP/3 server output stream capacity is too large");
    }
    const auto needed = max_tracked_streams * 2;
    const auto max_power_of_two = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (needed > max_power_of_two) {
        throw std::length_error("HTTP/3 server output stream capacity is too large");
    }
    return std::bit_ceil(needed);
}

}  // namespace ruvia::detail
