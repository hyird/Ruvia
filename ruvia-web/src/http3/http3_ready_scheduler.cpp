#include "http3/http3_ready_scheduler.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/process_resource.h"

namespace ruvia::detail {
namespace {

constexpr std::uint8_t capacity_data_lane = 1;
constexpr std::uint8_t capacity_control_lane = 2;

}  // namespace

http3_ready_scheduler::http3_ready_scheduler(const worker_handle& worker_value,
    std::size_t max_connections, std::pmr::memory_resource* resource,
    local_ready_callback ready_callback, std::size_t control_burst_limit)
    : worker_(worker_value),
      max_connections_(max_connections),
      ready_callback_(ready_callback),
      control_burst_limit_(control_burst_limit),
      slots_(resource != nullptr ? resource : process_resource()),
      free_slots_(slots_.get_allocator().resource()) {
    if (!worker_.valid() || !worker_.is_current()) {
        throw std::invalid_argument("HTTP/3 ready scheduler must be built on its worker");
    }
    if (max_connections_ == 0 || control_burst_limit_ == 0) {
        throw std::invalid_argument("HTTP/3 scheduler limits must be positive");
    }
    slots_.resize(max_connections_);
    free_slots_.reserve(max_connections_);
    for (std::size_t index = max_connections_; index != 0; --index) {
        auto& slot = slots_[index - 1];
        slot.scheduler_ = this;
        slot.index_ = index - 1;
        free_slots_.push_back(index - 1);
    }
}

http3_ready_scheduler::~http3_ready_scheduler() {
    if (!worker_.is_current() || attached_connections_ != 0 ||
        std::any_of(queues_.begin(), queues_.end(), [](const slot_queue& queue) {
            return queue.head_ != nullptr || queue.tail_ != nullptr || queue.size_ != 0;
        })) {
        std::terminate();
    }
    for (const auto& slot : slots_) {
        if (slot.state_ == slot_state::reserved || slot.state_ == slot_state::active ||
            slot.state_ == slot_state::retiring) {
            std::terminate();
        }
    }
}

std::optional<http3_ready_scheduler::registration>
http3_ready_scheduler::reserve(
    std::uint64_t epoch, std::uint64_t connection_generation) noexcept {
    if (!on_worker()) {
        return std::nullopt;
    }
    while (!free_slots_.empty()) {
        const auto index = free_slots_.back();
        free_slots_.pop_back();
        auto& slot = slots_[index];
        if (slot.state_ != slot_state::free || slot.scheduler_ != this || slot.owner_ != nullptr) {
            std::terminate();
        }
        if (slot.slot_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            slot.state_ = slot_state::exhausted;
            continue;
        }
        ++slot.slot_generation_;
        slot.epoch_ = epoch;
        slot.connection_generation_ = connection_generation;
        slot.state_ = slot_state::reserved;
        slot.offered_intent_.reset();
        return registration{
            .token_ = token_for(slot),
            .activation_ = {.context_ = &slot,
                .activate_ = &activation_thunk,
                .slot_generation_ = slot.slot_generation_}};
    }
    return std::nullopt;
}

bool http3_ready_scheduler::attach(connection_token token,
    connection_type& connection) noexcept {
    auto* slot = validate(token);
    if (slot == nullptr || slot->state_ != slot_state::reserved ||
        !connection.on_worker() || connection.epoch_ != token.epoch_ ||
        connection.connection_generation_ != token.connection_generation_ ||
        connection.activation_.context_ != slot ||
        connection.activation_.activate_ != &activation_thunk ||
        connection.activation_.slot_generation_ != token.slot_generation_) {
        return false;
    }
    slot->owner_ = &connection;
    slot->state_ = slot_state::active;
    ++attached_connections_;
    sync_owner(*slot);
    return true;
}

bool http3_ready_scheduler::abandon(connection_token token) noexcept {
    auto* slot = validate(token);
    if (slot == nullptr || slot->state_ != slot_state::reserved) {
        return false;
    }
    clear_slot(*slot);
    slot->state_ = slot_state::free;
    free_slots_.push_back(slot->index_);
    return true;
}

bool http3_ready_scheduler::begin_retirement(connection_token token) noexcept {
    auto* slot = validate(token);
    if (slot == nullptr || slot->state_ != slot_state::active || slot->owner_ == nullptr ||
        !slot->owner_->stopped()) {
        return false;
    }
    slot->state_ = slot_state::retiring;
    set_linked(*slot, queue_id::data_runnable, false);
    set_linked(*slot, queue_id::control_runnable, false);
    set_linked(*slot, queue_id::local_runnable, false);
    set_linked(*slot, queue_id::data_blocked, false);
    set_linked(*slot, queue_id::control_blocked, false);
    sync_owner(*slot);
    return true;
}

bool http3_ready_scheduler::retire(connection_token token) noexcept {
    auto* slot = validate(token);
    if (slot == nullptr || slot->state_ != slot_state::retiring || slot->owner_ == nullptr ||
        slot->owner_->pending_transport_intent_count() != 0 ||
        !slot->owner_->detach_activation_after_join()) {
        return false;
    }
    auto* owner_value = slot->owner_;
    if (!owner_value->retirement_.retired() && !owner_value->retirement_.taken_over() &&
        !owner_value->retirement_.handed_off()) {
        std::terminate();
    }
    clear_slot(*slot);
    slot->state_ = slot_state::free;
    if (attached_connections_ == 0) {
        std::terminate();
    }
    --attached_connections_;
    free_slots_.push_back(slot->index_);
    return true;
}

http3_ready_scheduler::step_result http3_ready_scheduler::step() noexcept {
    if (!on_worker()) {
        return {.kind_ = step_kind::wrong_worker};
    }
    normalize_capacity_pass();
    const bool forced_data = control_burst_ >= control_burst_limit_ && has_lane_work(lane::data);
    const auto selected = select_lane();
    if (!selected) {
        return {};
    }

    step_result result;
    switch (*selected) {
        case lane::data: {
            if ((capacity_pass_lanes_ & capacity_data_lane) != 0 &&
                has_recoverable(queue_id::data_blocked)) {
                auto* slot = pop_front(queue_id::data_blocked);
                if (slot == nullptr || slot->owner_ == nullptr ||
                    !slot->owner_->reactivate_blocked_one(connection_type::work_lane_type::data)) {
                    if (slot != nullptr && slot->owner_ != nullptr) {
                        sync_owner(*slot);
                    }
                    result.kind_ = step_kind::reconciled;
                    break;
                }
                result = step_publication(*slot, lane::data);
                break;
            }
            auto* slot = pop_front(queue_id::data_runnable);
            result = slot == nullptr ? step_result{.kind_ = step_kind::reconciled}
                                     : step_publication(*slot, lane::data);
            break;
        }
        case lane::control: {
            if ((capacity_pass_lanes_ & capacity_control_lane) != 0 &&
                has_recoverable(queue_id::control_blocked)) {
                auto* slot = pop_front(queue_id::control_blocked);
                if (slot == nullptr || slot->owner_ == nullptr ||
                    !slot->owner_->reactivate_blocked_one(connection_type::work_lane_type::control)) {
                    if (slot != nullptr && slot->owner_ != nullptr) {
                        sync_owner(*slot);
                    }
                    result.kind_ = step_kind::reconciled;
                } else {
                    result = step_publication(*slot, lane::control);
                }
                break;
            }
            auto* slot = pop_front(queue_id::control_runnable);
            result = slot == nullptr ? step_result{.kind_ = step_kind::reconciled}
                                     : step_publication(*slot, lane::control);
            break;
        }
        case lane::local: {
            auto* slot = pop_front(queue_id::local_runnable);
            result = slot == nullptr ? step_result{.kind_ = step_kind::reconciled}
                                     : step_publication(*slot, lane::local);
            break;
        }
        case lane::intent: {
            auto* slot = pop_front(queue_id::intent_runnable);
            result = slot == nullptr ? step_result{.kind_ = step_kind::reconciled}
                                     : step_intent(*slot);
            break;
        }
    }

    if (*selected == lane::control || *selected == lane::intent) {
        if (control_burst_ < control_burst_limit_) {
            ++control_burst_;
        }
    } else {
        control_burst_ = 0;
    }
    // A forced DATA turn is inserted into the rotation. Advancing the cursor
    // here would skip LOCAL/intent every time the control burst limit is one.
    if (!forced_data) {
        next_lane_ = static_cast<lane>((static_cast<std::uint8_t>(*selected) + 1U) % 4U);
    }
    normalize_capacity_pass();
    return result;
}

bool http3_ready_scheduler::acknowledge_intent(connection_token connection,
    const connection_type::transport_intent_token_type& intent, std::optional<connection_type::push_stream_open_result_type> opened) noexcept {
    auto* slot = validate(connection);
    if (slot == nullptr || slot->owner_ == nullptr || !slot->offered_intent_ ||
        *slot->offered_intent_ != intent) {
        return false;
    }
    if (!slot->owner_->ack_transport_intent(intent, opened)) {
        return false;
    }
    slot->offered_intent_.reset();
    sync_owner(*slot);
    return true;
}

void http3_ready_scheduler::receive_capacity(std::uint8_t lanes) noexcept {
    if (!on_worker()) {
        return;
    }
    start_capacity_pass(lanes & capacity_all);
}

http3_ready_scheduler::snapshot_type
http3_ready_scheduler::snapshot() const noexcept {
    if (!on_worker()) {
        return {.wrong_worker_ = true};
    }
    snapshot_type result;
    result.runnable_ = {queues_[queue_index(queue_id::data_runnable)].size_,
        queues_[queue_index(queue_id::control_runnable)].size_,
        queues_[queue_index(queue_id::local_runnable)].size_,
        queues_[queue_index(queue_id::intent_runnable)].size_};
    result.blocked_ = {queues_[queue_index(queue_id::data_blocked)].size_,
        queues_[queue_index(queue_id::control_blocked)].size_};
    result.attached_connections_ = attached_connections_;
    result.free_connections_ = free_slots_.size();
    result.capacity_pass_lanes_ = capacity_pass_lanes_;
    return result;
}

bool http3_ready_scheduler::on_worker() const noexcept {
    return worker_.is_current();
}

http3_ready_scheduler::connection_slot*
http3_ready_scheduler::validate(connection_token token) noexcept {
    if (!on_worker() || token.slot_ >= slots_.size()) {
        return nullptr;
    }
    auto& slot = slots_[token.slot_];
    if (slot.scheduler_ != this || slot.state_ == slot_state::free ||
        slot.state_ == slot_state::exhausted || slot.epoch_ != token.epoch_ ||
        slot.connection_generation_ != token.connection_generation_ ||
        slot.slot_generation_ != token.slot_generation_) {
        return nullptr;
    }
    return &slot;
}

http3_ready_scheduler::connection_token
http3_ready_scheduler::token_for(const connection_slot& slot) noexcept {
    return {.slot_ = slot.index_,
        .epoch_ = slot.epoch_,
        .connection_generation_ = slot.connection_generation_,
        .slot_generation_ = slot.slot_generation_};
}

void http3_ready_scheduler::set_linked(connection_slot& slot, queue_id id, bool linked) noexcept {
    auto& link = slot.links_[queue_index(id)];
    if (linked) {
        if (!link.linked_) {
            push_back(slot, id);
        }
    } else if (link.linked_) {
        remove(slot, id);
    }
}

void http3_ready_scheduler::push_back(connection_slot& slot, queue_id id) noexcept {
    auto& link = slot.links_[queue_index(id)];
    if (link.linked_) {
        std::terminate();
    }
    if (id == queue_id::data_blocked || id == queue_id::control_blocked) {
        if (blocked_sequence_ == std::numeric_limits<std::size_t>::max()) {
            std::terminate();
        }
        link.recovery_sequence_ = ++blocked_sequence_;
    }
    auto& queue = queues_[queue_index(id)];
    link.previous_ = queue.tail_;
    link.next_ = nullptr;
    link.linked_ = true;
    if (queue.tail_ != nullptr) {
        queue.tail_->links_[queue_index(id)].next_ = &slot;
    } else {
        queue.head_ = &slot;
    }
    queue.tail_ = &slot;
    ++queue.size_;
}

void http3_ready_scheduler::remove(connection_slot& slot, queue_id id) noexcept {
    auto& link = slot.links_[queue_index(id)];
    if (!link.linked_) {
        std::terminate();
    }
    auto& queue = queues_[queue_index(id)];
    if (link.previous_ != nullptr) {
        link.previous_->links_[queue_index(id)].next_ = link.next_;
    } else {
        queue.head_ = link.next_;
    }
    if (link.next_ != nullptr) {
        link.next_->links_[queue_index(id)].previous_ = link.previous_;
    } else {
        queue.tail_ = link.previous_;
    }
    link.previous_ = nullptr;
    link.next_ = nullptr;
    link.recovery_sequence_ = 0;
    link.linked_ = false;
    if (queue.size_ == 0) {
        std::terminate();
    }
    --queue.size_;
}

http3_ready_scheduler::connection_slot*
http3_ready_scheduler::pop_front(queue_id id) noexcept {
    auto* slot = queues_[queue_index(id)].head_;
    if (slot != nullptr) {
        remove(*slot, id);
    }
    return slot;
}

void http3_ready_scheduler::sync_slot(
    connection_slot& slot, const connection_type::worker_activation_type& activation) noexcept {
    if (activation.work_.wrong_worker_) {
        std::terminate();
    }
    const bool active = slot.state_ == slot_state::active;
    const bool accepts_intents = active || slot.state_ == slot_state::retiring;
    set_linked(slot, queue_id::data_runnable, active && activation.work_.runnable_.data_);
    set_linked(slot, queue_id::control_runnable, active && activation.work_.runnable_.control_);
    set_linked(slot, queue_id::local_runnable, active && activation.work_.runnable_.local_);
    set_linked(slot, queue_id::data_blocked, active && activation.work_.blocked_.data_);
    set_linked(slot, queue_id::control_blocked, active && activation.work_.blocked_.control_);

    // An offered token stays owed until actual transport execution is acknowledged,
    // even when a close or a merged reset changes the owner's current head.
    set_linked(slot, queue_id::intent_runnable,
        accepts_intents && activation.transport_intent_ && !slot.offered_intent_);
}

void http3_ready_scheduler::sync_owner(connection_slot& slot) noexcept {
    if (slot.owner_ == nullptr) {
        return;
    }
    sync_slot(slot, slot.owner_->activation_snapshot());
}

void http3_ready_scheduler::receive_activation(connection_slot& slot, std::uint64_t epoch,
    std::uint64_t connection_generation, std::uint64_t slot_generation,
    const connection_type::worker_activation_type& activation) noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    if (slot.scheduler_ != this || slot.state_ != slot_state::active ||
        slot.epoch_ != epoch || slot.connection_generation_ != connection_generation ||
        slot.slot_generation_ != slot_generation) {
        return;
    }
    sync_slot(slot, activation);
    const auto& work = activation.work_;
    if (work.runnable_.data_ || work.runnable_.control_ || work.runnable_.local_ ||
        activation.transport_intent_ || activation.input_capacity_available_) {
        signal_ready();
    }
}

void http3_ready_scheduler::activation_thunk(void* context_value, std::uint64_t epoch,
    std::uint64_t connection_generation, std::uint64_t slot_generation,
    const connection_type::worker_activation_type& activation) noexcept {
    if (context_value == nullptr) {
        std::terminate();
    }
    auto& slot = *static_cast<connection_slot*>(context_value);
    if (slot.scheduler_ == nullptr) {
        std::terminate();
    }
    slot.scheduler_->receive_activation(
        slot, epoch, connection_generation, slot_generation, activation);
}

void http3_ready_scheduler::signal_ready() noexcept {
    if (ready_callback_.ready_ != nullptr) {
        ready_callback_.ready_(ready_callback_.context_);
    }
}

void http3_ready_scheduler::start_capacity_pass(std::uint8_t lanes) noexcept {
    for (std::size_t index = 0; index < recovery_cutoff_.size(); ++index) {
        if ((lanes & (1U << index)) != 0) {
            recovery_cutoff_[index] = blocked_sequence_;
        }
    }
    capacity_pass_lanes_ |= lanes;
    normalize_capacity_pass();
    if (capacity_pass_lanes_ != 0) {
        signal_ready();
    }
}

void http3_ready_scheduler::normalize_capacity_pass() noexcept {
    if ((capacity_pass_lanes_ & capacity_data_lane) != 0 &&
        !has_recoverable(queue_id::data_blocked)) {
        capacity_pass_lanes_ &= static_cast<std::uint8_t>(~capacity_data_lane);
    }
    if ((capacity_pass_lanes_ & capacity_control_lane) != 0 &&
        !has_recoverable(queue_id::control_blocked)) {
        capacity_pass_lanes_ &= static_cast<std::uint8_t>(~capacity_control_lane);
    }
}

bool http3_ready_scheduler::has_recoverable(queue_id id) noexcept {
    if (id != queue_id::data_blocked && id != queue_id::control_blocked) {
        return false;
    }
    const auto* head = queues_[queue_index(id)].head_;
    return head != nullptr && head->links_[queue_index(id)].recovery_sequence_ <=
                                  recovery_cutoff_[id == queue_id::data_blocked ? 0 : 1];
}

bool http3_ready_scheduler::has_lane_work(lane selected_lane) noexcept {
    switch (selected_lane) {
        case lane::data:
            return queues_[queue_index(queue_id::data_runnable)].head_ != nullptr ||
                   (((capacity_pass_lanes_ & capacity_data_lane) != 0) &&
                       has_recoverable(queue_id::data_blocked));
        case lane::control:
            return queues_[queue_index(queue_id::control_runnable)].head_ != nullptr ||
                   (((capacity_pass_lanes_ & capacity_control_lane) != 0) &&
                       has_recoverable(queue_id::control_blocked));
        case lane::local:
            return queues_[queue_index(queue_id::local_runnable)].head_ != nullptr;
        case lane::intent:
            return queues_[queue_index(queue_id::intent_runnable)].head_ != nullptr;
    }
    return false;
}

std::optional<http3_ready_scheduler::lane>
http3_ready_scheduler::select_lane() noexcept {
    const bool data_ready = has_lane_work(lane::data);
    if (control_burst_ >= control_burst_limit_ && data_ready) {
        return lane::data;
    }
    for (std::uint8_t offset = 0; offset < 4; ++offset) {
        const auto index = (static_cast<std::uint8_t>(next_lane_) + offset) % 4U;
        const auto selected_lane = static_cast<lane>(index);
        if (has_lane_work(selected_lane)) {
            return selected_lane;
        }
    }
    return std::nullopt;
}

http3_ready_scheduler::step_result
http3_ready_scheduler::step_publication(connection_slot& slot, lane selected_lane) noexcept {
    if (slot.owner_ == nullptr) {
        return {.kind_ = step_kind::reconciled};
    }
    connection_type::work_lanes_type lanes{};
    switch (selected_lane) {
        case lane::data:
            lanes.data_ = true;
            break;
        case lane::control:
            lanes.control_ = true;
            break;
        case lane::local:
            lanes.local_ = true;
            break;
        case lane::intent:
            return {.kind_ = step_kind::reconciled};
    }
    const auto token = token_for(slot);
    const auto publication = slot.owner_->publish_one(lanes);
    sync_owner(slot);
    if (publication.status_ == connection_type::publish_status_type::no_ready_request) {
        return {.kind_ = step_kind::reconciled, .connection_ = token};
    }
    if (publication.status_ == connection_type::publish_status_type::wrong_worker) {
        return {.kind_ = step_kind::wrong_worker, .connection_ = token};
    }
    return {.kind_ = step_kind::publication,
        .connection_ = token,
        .publication_ = publication};
}

http3_ready_scheduler::step_result
http3_ready_scheduler::step_intent(connection_slot& slot) noexcept {
    if (slot.owner_ == nullptr) {
        return {.kind_ = step_kind::reconciled};
    }
    const auto intent = slot.owner_->peek_transport_intent();
    if (!intent) {
        sync_owner(slot);
        return {.kind_ = step_kind::reconciled, .connection_ = token_for(slot)};
    }
    slot.offered_intent_ = intent->token_;
    set_linked(slot, queue_id::intent_runnable, false);
    return {.kind_ = step_kind::transport_intent,
        .connection_ = token_for(slot),
        .intent_ = *intent};
}

void http3_ready_scheduler::clear_slot(connection_slot& slot) noexcept {
    for (std::size_t index = 0; index < queue_index(queue_id::count); ++index) {
        if (slot.links_[index].linked_) {
            remove(slot, static_cast<queue_id>(index));
        }
    }
    slot.owner_ = nullptr;
    slot.epoch_ = 0;
    slot.connection_generation_ = 0;
    slot.offered_intent_.reset();
}

}  // namespace ruvia::detail
