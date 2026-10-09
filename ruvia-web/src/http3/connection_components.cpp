#include <array>
#include <exception>
#include <utility>

#include "http3/http3_server_connection.h"

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
http3_server_connection::request_index::find_or_create(
    std::uint64_t stream_id, bool& created) noexcept {
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
            slot.rejection_ = session::rejection_type::none;
            slot.run_status_.reset();
            slot.reset_intent_previous_ = no_intent_slot;
            slot.reset_intent_next_ = no_intent_slot;
            slot.reset_intent_sequence_ = 0;
            slot.reset_intent_error_code_ = http3_connection_error_code::request_cancelled;
            slot.reset_intent_origin_ = reset_intent_origin_type::local_cancellation;
            slot.reset_intent_pending_ = false;
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
    return const_cast<request_index_slot_type*>(
        std::as_const(*this).find(stream_id));
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
    const bool was_blocked = slot.queue_ == queue_kind_type::data_blocked ||
                             slot.queue_ == queue_kind_type::control_blocked;
    slot.queue_previous_ = nullptr;
    slot.queue_next_ = nullptr;
    slot.queue_ = queue_kind_type::none;
    auto& count = was_blocked ? blocked_ : ready_;
    if (count == 0) {
        std::terminate();
    }
    --count;
}

http3_server_connection::request_index_slot_type*
http3_server_connection::output_scheduler::select(work_lanes_type eligible_lanes) const noexcept {
    std::array<work_lane_type, 3> lanes{};
    switch (next_lane_) {
        case work_lane_type::data:
            lanes = {work_lane_type::data, work_lane_type::control, work_lane_type::local};
            break;
        case work_lane_type::control:
            lanes = {work_lane_type::control, work_lane_type::local, work_lane_type::data};
            break;
        case work_lane_type::local:
            lanes = {work_lane_type::local, work_lane_type::data, work_lane_type::control};
            break;
    }
    for (const auto lane : lanes) {
        if (!eligible_lanes.contains(lane)) {
            continue;
        }
        const auto kind = lane == work_lane_type::data      ? queue_kind_type::data_runnable
                          : lane == work_lane_type::control ? queue_kind_type::control_runnable
                                                            : queue_kind_type::local_runnable;
        if (const auto* queue = &this->queue(kind); queue->head_ != nullptr) {
            return queue->head_;
        }
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
void http3_server_connection::output_scheduler::advance() noexcept {
    switch (next_lane_) {
        case work_lane_type::data:
            next_lane_ = work_lane_type::control;
            break;
        case work_lane_type::control:
            next_lane_ = work_lane_type::local;
            break;
        case work_lane_type::local:
            next_lane_ = work_lane_type::data;
            break;
    }
}

http3_server_connection::transport_retirement::reset_result http3_server_connection::transport_retirement::enqueue_reset(
    request_index_slot_type& slot, http3_connection_error_code error_code,
    reset_intent_origin_type origin) noexcept {
    if (!slot.occupied_) {
        return reset_result::capacity_exhausted;
    }
    if (retired_ || taken_over_ || handed_off_) {
        return reset_result::unavailable;
    }
    if (slot.reset_intent_pending_) {
        // Identical codes are idempotent. The first protocol error wins, and a
        // local H3_REQUEST_CANCELLED never downgrades an existing protocol code;
        // only a protocol error may supersede a pending local cancellation.
        if (slot.reset_intent_error_code_ == error_code) {
            if (origin == reset_intent_origin_type::stream_protocol_error) {
                slot.reset_intent_origin_ = origin;
            }
            return reset_result::queued;
        }
        if (slot.reset_intent_origin_ == reset_intent_origin_type::stream_protocol_error ||
            origin != reset_intent_origin_type::stream_protocol_error) {
            return reset_result::queued;
        }
        if (next_sequence_ == 0 ||
            next_sequence_ == reserved_close_intent_sequence) {
            return reset_result::sequence_exhausted;
        }
        // A protocol error supersedes only a local cancellation. Give the new
        // payload a new token so an ack for the old code cannot clear it.
        // Reinsert at the tail so channel publication remains in sequence
        // order across sibling resets and push-open intents.
        unlink_reset(slot);
        return enqueue_reset(slot, error_code, origin);
    }
    if (reset_count_ >= capacity_) {
        return reset_result::capacity_exhausted;
    }
    if (next_sequence_ == 0 ||
        next_sequence_ == reserved_close_intent_sequence) {
        return reset_result::sequence_exhausted;
    }

    const auto slot_index = static_cast<std::size_t>(&slot - requests_.slots().data());
    if (slot_index >= requests_.slots().size()) {
        return reset_result::capacity_exhausted;
    }
    slot.reset_intent_sequence_ = next_sequence_++;
    slot.reset_intent_error_code_ = error_code;
    slot.reset_intent_origin_ = origin;
    slot.reset_intent_pending_ = true;
    slot.reset_intent_previous_ = reset_tail_;
    slot.reset_intent_next_ = no_intent_slot;
    if (reset_tail_ != no_intent_slot) {
        if (reset_tail_ >= requests_.slots().size()) {
            std::terminate();
        }
        requests_.slots()[reset_tail_].reset_intent_next_ = slot_index;
    } else {
        reset_head_ = slot_index;
    }
    reset_tail_ = slot_index;
    ++reset_count_;
    return reset_result::queued;
}

void http3_server_connection::transport_retirement::unlink_reset(request_index_slot_type& slot) noexcept {
    if (!slot.reset_intent_pending_) {
        std::terminate();
    }
    if (slot.reset_intent_previous_ != no_intent_slot) {
        if (slot.reset_intent_previous_ >= requests_.slots().size()) {
            std::terminate();
        }
        requests_.slots()[slot.reset_intent_previous_].reset_intent_next_ = slot.reset_intent_next_;
    } else {
        reset_head_ = slot.reset_intent_next_;
    }
    if (slot.reset_intent_next_ != no_intent_slot) {
        if (slot.reset_intent_next_ >= requests_.slots().size()) {
            std::terminate();
        }
        requests_.slots()[slot.reset_intent_next_].reset_intent_previous_ = slot.reset_intent_previous_;
    } else {
        reset_tail_ = slot.reset_intent_previous_;
    }
    slot.reset_intent_previous_ = no_intent_slot;
    slot.reset_intent_next_ = no_intent_slot;
    slot.reset_intent_sequence_ = 0;
    slot.reset_intent_error_code_ = http3_connection_error_code::request_cancelled;
    slot.reset_intent_origin_ = reset_intent_origin_type::local_cancellation;
    slot.reset_intent_pending_ = false;
    if (reset_count_ == 0) {
        std::terminate();
    }
    --reset_count_;
}

std::optional<http3_server_connection::transport_intent_type>
http3_server_connection::transport_retirement::next_intent() const noexcept {
    if (close_pending_) {
        return transport_intent_type{
            .token_ = {.kind_ = transport_intent_kind_type::connection_close,
                .id_ = {epoch_, generation_, 0},
                .sequence_ = reserved_close_intent_sequence},
            .close_reason_ = close_reason_,
            .connection_error_code_ = close_code_};
    }
    std::optional<transport_intent_type> selected;
    if (reset_head_ != no_intent_slot) {
        if (reset_head_ >= requests_.slots().size()) {
            std::terminate();
        }
        const auto& slot = requests_.slots()[reset_head_];
        if (!slot.occupied_ || !slot.reset_intent_pending_) {
            std::terminate();
        }
        selected = transport_intent_type{
            .token_ = {.kind_ = transport_intent_kind_type::stream_reset,
                .id_ = {epoch_, generation_, slot.stream_id_, slot.push_id_},
                .sequence_ = slot.reset_intent_sequence_},
            .stream_reset_error_code_ = slot.reset_intent_error_code_};
    }

    return selected;
}

http3_server_connection::transport_retirement::transport_retirement(request_index& requests, std::size_t capacity, std::uint64_t epoch, std::uint64_t generation) noexcept
    : requests_(requests),
      capacity_(capacity),
      epoch_(epoch),
      generation_(generation) {}
bool http3_server_connection::transport_retirement::sequence_available() const noexcept {
    return next_sequence_ != 0 && next_sequence_ != reserved_close_intent_sequence;
}
std::uint64_t http3_server_connection::transport_retirement::allocate_sequence() noexcept {
    if (!sequence_available()) {
        std::terminate();
    }
    return next_sequence_++;
}
bool http3_server_connection::transport_retirement::acknowledge(const transport_intent_token_type& token) noexcept {
    if (token.id_.epoch_ != epoch_ || token.id_.connection_generation_ != generation_) {
        return false;
    }
    if (token.kind_ == transport_intent_kind_type::connection_close) {
        if (token.id_.push_id_ || !close_pending_ || token.id_.stream_id_ != 0 || token.sequence_ != reserved_close_intent_sequence) {
            return false;
        }
        close_pending_ = false;
        handed_off_ = true;
        return true;
    }
    if (token.kind_ != transport_intent_kind_type::stream_reset) {
        return false;
    }
    auto* slot = requests_.find(token.id_.stream_id_);
    if (slot == nullptr || slot->push_id_ != token.id_.push_id_ || !slot->reset_intent_pending_ || token.sequence_ == 0 || token.sequence_ > slot->reset_intent_sequence_) {
        return false;
    }
    // A superseded token settles only the earlier publication; the newer debt stays.
    if (token.sequence_ == slot->reset_intent_sequence_) {
        unlink_reset(*slot);
    }
    return true;
}
void http3_server_connection::transport_retirement::require_close(transport_close_reason_type reason, std::optional<http3_connection_error_code> code) noexcept {
    if (reason == transport_close_reason_type::none) {
        std::terminate();
    }
    close_required_ = true;
    if (!close_pending_ && !handed_off_ && !retired_ && !taken_over_) {
        close_reason_ = reason;
        close_code_ = code;
        close_pending_ = true;
    }
}
bool http3_server_connection::transport_retirement::take_over() noexcept {
    if (!close_pending_ || handed_off_ || retired_ || taken_over_) {
        return false;
    }
    taken_over_ = true;
    return true;
}
bool http3_server_connection::transport_retirement::confirm() noexcept {
    if (retired_) {
        return false;
    }
    retired_ = true;
    return true;
}

}  // namespace ruvia::detail
