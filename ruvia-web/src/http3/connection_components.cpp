#include <bit>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#include "http3/http3_server_connection.h"
#include "http3/http3_server_request_entry.h"

// The request index, the output lane scheduler and response publication of
// http3_server_connection.

namespace ruvia::detail {
namespace {

std::uint64_t mix_stream_id(std::uint64_t stream_id) noexcept {
    stream_id ^= stream_id >> 30;
    stream_id *= 0xbf58476d1ce4e5b9ULL;
    stream_id ^= stream_id >> 27;
    stream_id *= 0x94d049bb133111ebULL;
    stream_id ^= stream_id >> 31;
    return stream_id;
}

}  // namespace

http3_server_connection::request_index::request_index(std::pmr::memory_resource* resource, std::size_t capacity)
    : capacity_(capacity),
      slots_(resource) {
    slots_.resize(index_capacity(capacity));
}

http3_server_connection::request_index_slot_type*
http3_server_connection::request_index::find_or_create(std::uint64_t stream_id, bool& created) noexcept {
    created = false;
    const auto mask = slots_.size() - 1;
    auto index = static_cast<std::size_t>(mix_stream_id(stream_id)) & mask;
    for (std::size_t probes = 0; probes < slots_.size(); ++probes) {
        auto& slot = slots_[index];
        if (!slot.occupied_) {
            if (tracked_ >= capacity_) {
                return nullptr;
            }
            slot.stream_id_ = stream_id;
            slot.entry_ = nullptr;
            slot.queue_previous_ = nullptr;
            slot.queue_next_ = nullptr;
            slot.queue_ = queue_kind_type::none;
            slot.status_ = request_status_type::unknown;
            slot.rejection_ = session_type::rejection_type::none;
            slot.occupied_ = true;
            ++tracked_;
            created = true;
            return &slot;
        }
        if (slot.stream_id_ == stream_id) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

http3_server_connection::request_index_slot_type*
http3_server_connection::request_index::find(std::uint64_t stream_id) noexcept {
    return const_cast<request_index_slot_type*>(std::as_const(*this).find(stream_id));
}

const http3_server_connection::request_index_slot_type*
http3_server_connection::request_index::find(std::uint64_t stream_id) const noexcept {
    if (slots_.empty()) {
        return nullptr;
    }
    const auto mask = slots_.size() - 1;
    auto index = static_cast<std::size_t>(mix_stream_id(stream_id)) & mask;
    for (std::size_t probes = 0; probes < slots_.size(); ++probes) {
        const auto& slot = slots_[index];
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

void http3_server_connection::request_index::attach(request_index_slot_type& slot, request_entry_type& entry_value) noexcept {
    if (slot.entry_ != nullptr || slot.rejection_entry_ != nullptr) {
        std::terminate();
    }
    slot.entry_ = &entry_value;
    ++active_;
}

void http3_server_connection::request_index::attach_rejection(request_index_slot_type& slot, rejection_entry_type& entry_value) noexcept {
    if (slot.entry_ != nullptr || slot.rejection_entry_ != nullptr) {
        std::terminate();
    }
    slot.rejection_entry_ = &entry_value;
    ++rejections_;
}

void http3_server_connection::request_index::retire(request_index_slot_type& slot, request_entry_type& entry_value) noexcept {
    if (slot.entry_ != &entry_value || active_ == 0 || slot.queue_ != queue_kind_type::none) {
        std::terminate();
    }
    slot.entry_ = nullptr;
    --active_;
}

void http3_server_connection::request_index::retire_rejection(request_index_slot_type& slot, rejection_entry_type& entry_value) noexcept {
    if (slot.rejection_entry_ != &entry_value || rejections_ == 0 || slot.queue_ != queue_kind_type::none) {
        std::terminate();
    }
    slot.rejection_entry_ = nullptr;
    --rejections_;
}

std::size_t http3_server_connection::index_capacity(std::size_t max_tracked_streams) {
    if (max_tracked_streams == 0 || max_tracked_streams > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::invalid_argument("HTTP/3 request index capacity must be positive and bounded");
    }
    const auto needed = max_tracked_streams * 2;
    const auto max_power_of_two = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (needed > max_power_of_two) {
        throw std::length_error("HTTP/3 request index capacity is too large");
    }
    return std::bit_ceil(needed);
}

http3_server_connection::intrusive_queue_type&
http3_server_connection::output_scheduler::queue(queue_kind_type kind) noexcept {
    switch (kind) {
        case queue_kind_type::data_runnable:
            return data_runnable_;
        case queue_kind_type::control_runnable:
            return control_runnable_;
        case queue_kind_type::local_runnable:
            return local_runnable_;
        case queue_kind_type::data_blocked:
            return data_blocked_;
        case queue_kind_type::control_blocked:
            return control_blocked_;
        case queue_kind_type::none:
            break;
    }
    std::terminate();
}

const http3_server_connection::intrusive_queue_type&
http3_server_connection::output_scheduler::queue(queue_kind_type kind) const noexcept {
    return const_cast<output_scheduler*>(this)->queue(kind);
}

bool http3_server_connection::output_scheduler::enqueue(request_index_slot_type& slot, queue_kind_type kind) noexcept {
    if (slot.queue_ != queue_kind_type::none || kind == queue_kind_type::none) {
        std::terminate();
    }
    auto& selected = queue(kind);
    const bool was_empty = selected.head_ == nullptr;
    slot.queue_previous_ = selected.tail_;
    slot.queue_next_ = nullptr;
    if (selected.tail_ != nullptr) {
        selected.tail_->queue_next_ = &slot;
    } else {
        selected.head_ = &slot;
    }
    selected.tail_ = &slot;
    slot.queue_ = kind;
    if (kind == queue_kind_type::data_blocked || kind == queue_kind_type::control_blocked) {
        ++blocked_;
    } else {
        ++ready_;
    }
    return was_empty;
}

void http3_server_connection::output_scheduler::remove(request_index_slot_type& slot) noexcept {
    if (slot.queue_ == queue_kind_type::none) {
        return;
    }
    auto& queue = this->queue(slot.queue_);
    if (slot.queue_previous_ != nullptr) {
        slot.queue_previous_->queue_next_ = slot.queue_next_;
    } else {
        queue.head_ = slot.queue_next_;
    }
    if (slot.queue_next_ != nullptr) {
        slot.queue_next_->queue_previous_ = slot.queue_previous_;
    } else {
        queue.tail_ = slot.queue_previous_;
    }
    const bool was_blocked = slot.queue_ == queue_kind_type::data_blocked || slot.queue_ == queue_kind_type::control_blocked;
    slot.queue_previous_ = nullptr;
    slot.queue_next_ = nullptr;
    slot.queue_ = queue_kind_type::none;
    auto& count = was_blocked ? blocked_ : ready_;
    if (count == 0) {
        std::terminate();
    }
    --count;
}

http3_server_connection::request_index_slot_type* http3_server_connection::output_scheduler::select() noexcept {
    // Round-robin across lanes: each selection moves the next start lane on.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto lane = next_lane_;
        next_lane_ = lane == work_lane_type::data      ? work_lane_type::control
                     : lane == work_lane_type::control ? work_lane_type::local
                                                       : work_lane_type::data;
        const auto kind = lane == work_lane_type::data      ? queue_kind_type::data_runnable
                          : lane == work_lane_type::control ? queue_kind_type::control_runnable
                                                            : queue_kind_type::local_runnable;
        if (auto* head = queue(kind).head_; head != nullptr) {
            return head;
        }
    }
    return nullptr;
}

bool http3_server_connection::publish(std::size_t budget) noexcept {
    for (std::size_t step = 0; step < budget; ++step) {
        auto* slot = output_.select();
        if (slot == nullptr) {
            break;
        }
        publish_slot(*slot);
    }
    return output_.ready() != 0;
}

void http3_server_connection::transport_activity() noexcept {
    std::size_t reactivated = 0;
    const auto reactivate = [this, &reactivated](queue_kind_type blocked, queue_kind_type runnable) noexcept {
        while (auto* head = std::as_const(output_).queue(blocked).head_) {
            output_.remove(*head);
            enqueue_runnable(*head, runnable, false);
            ++reactivated;
        }
    };
    reactivate(queue_kind_type::data_blocked, queue_kind_type::data_runnable);
    reactivate(queue_kind_type::control_blocked, queue_kind_type::control_runnable);
    if (reactivated != 0) {
        notify_activation();
    }
}

void http3_server_connection::publish_slot(request_index_slot_type& slot) noexcept {
    if (slot.interim_response_) {
        publish_interim_response(slot);
        return;
    }
    auto* entry_value = slot.entry_;
    auto* rejection_entry = slot.rejection_entry_;
    if ((entry_value == nullptr) == (rejection_entry == nullptr)) {
        std::terminate();
    }
    output_.remove(slot);
    if (rejection_entry != nullptr) {
        publish_rejection(slot, *rejection_entry);
    } else {
        publish_entry(*entry_value);
    }
}

void http3_server_connection::publish_entry(request_entry_type& entry_value) noexcept {
    using publish_status_type = dispatch_type::publish_status_type;
    entry_value.slot_.status_ = request_status_type::publishing;
    const auto result_value = entry_value.dispatch_.publish_step();
    switch (result_value.status_) {
        case publish_status_type::bytes_published:
        case publish_status_type::control_published:
            if (entry_value.output_enabled_ && !entry_value.cancel_requested_ && !stop_requested_) {
                if (entry_value.dispatch_.publication_demand() != dispatch_type::publication_demand_type::not_ready) {
                    enqueue_for_demand(entry_value.slot_, false);
                }
            } else {
                cancel_entry(entry_value, request_status_type::cancelled);
            }
            break;
        case publish_status_type::backpressured:
            if (entry_value.output_enabled_ && !entry_value.cancel_requested_ && !stop_requested_) {
                entry_value.slot_.status_ = request_status_type::ready_to_publish;
                enqueue_blocked(entry_value.slot_,
                    result_value.block_reason_ == dispatch_type::publish_block_reason_type::control
                        ? queue_kind_type::control_blocked
                        : queue_kind_type::data_blocked);
            } else {
                cancel_entry(entry_value, request_status_type::cancelled);
            }
            break;
        case publish_status_type::not_ready:
            entry_value.output_terminal_ = true;
            entry_value.slot_.status_ = request_status_type::failed;
            entry_value.publication_finished_.notify();
            close_connection(http3_connection_error_code::internal_error);
            break;
        case publish_status_type::fin_published:
        case publish_status_type::complete:
            entry_value.output_terminal_ = true;
            entry_value.slot_.status_ = request_status_type::published;
            entry_value.publication_finished_.notify();
            break;
        case publish_status_type::cancelled: {
            const auto reason = entry_value.dispatch_.cancellation_reason();
            const bool already_cancelled_by_owner = entry_value.cancel_requested_;
            if (reason == dispatch_type::cancellation_reason_type::deadline) {
                cancel_deadline_entry(entry_value);
            } else {
                entry_value.output_enabled_ = false;
                entry_value.cancel_requested_ = true;
                entry_value.output_terminal_ = true;
                entry_value.slot_.status_ = request_status_type::cancelled;
                entry_value.publication_finished_.notify();
            }
            if (reason == dispatch_type::cancellation_reason_type::worker_stop && !stop_requested_) {
                (void)request_stop();
            } else if (!stop_requested_ && !already_cancelled_by_owner &&
                       reason != dispatch_type::cancellation_reason_type::deadline) {
                close_connection(http3_connection_error_code::internal_error);
            }
            break;
        }
        case publish_status_type::peer_limit_rejected:
            reset_stream(entry_value.slot_);
            entry_value.output_terminal_ = true;
            entry_value.slot_.status_ = request_status_type::failed;
            entry_value.publication_finished_.notify();
            break;
        case publish_status_type::failed:
        case publish_status_type::wrong_worker:
            entry_value.output_terminal_ = true;
            entry_value.slot_.status_ = request_status_type::failed;
            entry_value.publication_finished_.notify();
            close_connection(http3_connection_error_code::internal_error);
            break;
    }
}

void http3_server_connection::publish_rejection(request_index_slot_type& slot, rejection_entry_type& entry_value) noexcept {
    if (!entry_value.output_enabled_ || entry_value.output_terminal_ || !entry_value.output_) {
        slot.status_ = request_status_type::failed;
        close_connection(http3_connection_error_code::internal_error);
        return;
    }
    const auto result_value = entry_value.output_->publish_step();
    switch (result_value.status_) {
        case http3_buffered_response_output::status_type::bytes:
            enqueue_for_demand(slot, false);
            break;
        case http3_buffered_response_output::status_type::fin:
        case http3_buffered_response_output::status_type::complete:
            entry_value.output_terminal_ = true;
            slot.status_ = request_status_type::published;
            entry_value.output_.reset();
            entry_value.response_.reset();
            finish_rejection(entry_value);
            break;
        case http3_buffered_response_output::status_type::backpressured:
            enqueue_blocked(slot, result_value.block_reason_ == http3_buffered_response_output::block_reason_type::control
                                      ? queue_kind_type::control_blocked
                                      : queue_kind_type::data_blocked);
            break;
        case http3_buffered_response_output::status_type::failed:
            slot.status_ = request_status_type::failed;
            entry_value.output_terminal_ = true;
            close_connection(http3_connection_error_code::internal_error);
            break;
    }
}

void http3_server_connection::publish_interim_response(request_index_slot_type& slot) noexcept {
    output_.remove(slot);
    auto& pending = *slot.interim_response_;
    const auto frame = pending.frame_.frame();
    const auto written = output().write(slot.stream_id_, std::as_bytes(frame.subspan(pending.offset_)));
    if (written.accepted_ == 0) {
        enqueue_blocked(slot, queue_kind_type::data_blocked);
        return;
    }
    pending.offset_ += written.accepted_;
    if (pending.offset_ == frame.size()) {
        slot.interim_response_.reset();
    }
    if (slot.interim_response_ || slot.rejection_entry_ != nullptr ||
        (slot.entry_ != nullptr &&
            slot.entry_->dispatch_.publication_demand() != dispatch_type::publication_demand_type::not_ready)) {
        enqueue_for_demand(slot, false);
    }
}

void http3_server_connection::enqueue_runnable(request_index_slot_type& slot, queue_kind_type kind, bool notify) noexcept {
    if (kind != queue_kind_type::data_runnable && kind != queue_kind_type::control_runnable &&
        kind != queue_kind_type::local_runnable) {
        std::terminate();
    }
    const auto* entry_value = slot.entry_;
    const auto* rejection = slot.rejection_entry_;
    if (slot.queue_ != queue_kind_type::none || stop_requested_ ||
        (!slot.interim_response_ && (entry_value == nullptr) == (rejection == nullptr)) ||
        (entry_value != nullptr && (!entry_value->output_enabled_ || entry_value->output_terminal_)) ||
        (rejection != nullptr && (!rejection->output_enabled_ || rejection->output_terminal_))) {
        return;
    }
    const bool was_lane_empty = output_.enqueue(slot, kind);
    if (notify && was_lane_empty) {
        notify_activation();
    }
}

void http3_server_connection::enqueue_for_demand(request_index_slot_type& slot, bool notify) noexcept {
    if (slot.interim_response_) {
        if (slot.queue_ != queue_kind_type::data_runnable) {
            output_.remove(slot);
            enqueue_runnable(slot, queue_kind_type::data_runnable, notify);
        }
        return;
    }
    auto* entry_value = slot.entry_;
    auto* rejection = slot.rejection_entry_;
    if ((entry_value == nullptr) == (rejection == nullptr)) {
        std::terminate();
    }
    queue_kind_type desired = queue_kind_type::local_runnable;
    if (rejection != nullptr) {
        switch (rejection->output_->next_step()) {
            case http3_buffered_response_output::next_step_type::bytes:
                desired = queue_kind_type::data_runnable;
                break;
            case http3_buffered_response_output::next_step_type::fin:
                desired = queue_kind_type::control_runnable;
                break;
            case http3_buffered_response_output::next_step_type::complete:
            case http3_buffered_response_output::next_step_type::failed:
                break;
        }
    } else {
        switch (entry_value->dispatch_.publication_demand()) {
            case dispatch_type::publication_demand_type::data:
                desired = queue_kind_type::data_runnable;
                break;
            case dispatch_type::publication_demand_type::control:
                desired = queue_kind_type::control_runnable;
                break;
            case dispatch_type::publication_demand_type::local_complete:
            case dispatch_type::publication_demand_type::local_cancelled:
            case dispatch_type::publication_demand_type::local_peer_limit_rejected:
            case dispatch_type::publication_demand_type::local_failed:
            case dispatch_type::publication_demand_type::not_ready:
            case dispatch_type::publication_demand_type::wrong_worker:
                break;
        }
    }
    if (slot.queue_ == desired) {
        return;
    }
    output_.remove(slot);
    enqueue_runnable(slot, desired, notify);
}

void http3_server_connection::enqueue_blocked(request_index_slot_type& slot, queue_kind_type kind) noexcept {
    if (kind != queue_kind_type::data_blocked && kind != queue_kind_type::control_blocked) {
        std::terminate();
    }
    const auto* entry_value = slot.entry_;
    const auto* rejection = slot.rejection_entry_;
    if (slot.queue_ != queue_kind_type::none || stop_requested_ ||
        (!slot.interim_response_ && (entry_value == nullptr) == (rejection == nullptr)) ||
        (entry_value != nullptr && (!entry_value->output_enabled_ || entry_value->output_terminal_)) ||
        (rejection != nullptr && (!rejection->output_enabled_ || rejection->output_terminal_))) {
        return;
    }
    (void)output_.enqueue(slot, kind);
}

}  // namespace ruvia::detail
