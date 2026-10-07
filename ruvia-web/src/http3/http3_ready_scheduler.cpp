#include "ruvia/web/detail/http3/http3_ready_scheduler.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia::detail {
namespace {

constexpr std::uint8_t capacity_data_lane = 1;
constexpr std::uint8_t capacity_control_lane = 2;

}  // namespace

http3_ready_scheduler::http3_ready_scheduler(const WorkerHandle& worker,
    std::size_t max_connections, std::pmr::memory_resource* resource,
    local_ready_callback ready_callback, std::size_t control_burst_limit)
    : worker_(worker),
      max_connections_(max_connections),
      ready_callback_(ready_callback),
      control_burst_limit_(control_burst_limit),
      slots_(resource != nullptr ? resource : processResource()),
      free_slots_(slots_.get_allocator().resource()) {
    if (!worker_.valid() || !worker_.isCurrent()) {
        throw std::invalid_argument("HTTP/3 ready scheduler must be built on its worker");
    }
    if (max_connections_ == 0 || control_burst_limit_ == 0) {
        throw std::invalid_argument("HTTP/3 scheduler limits must be positive");
    }
    slots_.resize(max_connections_);
    free_slots_.reserve(max_connections_);
    for (std::size_t index = max_connections_; index != 0; --index) {
        auto& slot = slots_[index - 1];
        slot.scheduler = this;
        slot.index = index - 1;
        free_slots_.push_back(index - 1);
    }
}

http3_ready_scheduler::~http3_ready_scheduler() {
    if (!worker_.isCurrent() || attached_connections_ != 0 ||
        std::any_of(queues_.begin(), queues_.end(), [](const slot_queue& queue) {
            return queue.head != nullptr || queue.tail != nullptr || queue.size != 0;
        })) {
        std::terminate();
    }
    for (const auto& slot : slots_) {
        if (slot.state == slot_state::reserved || slot.state == slot_state::active ||
            slot.state == slot_state::retiring) {
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
        if (slot.state != slot_state::free || slot.scheduler != this || slot.owner != nullptr) {
            std::terminate();
        }
        if (slot.slot_generation == std::numeric_limits<std::uint64_t>::max()) {
            slot.state = slot_state::exhausted;
            continue;
        }
        ++slot.slot_generation;
        slot.epoch = epoch;
        slot.connection_generation = connection_generation;
        slot.state = slot_state::reserved;
        slot.offered_intent.reset();
        return registration{
            .token = token_for(slot),
            .activation = {.context = &slot,
                .activate = &activation_thunk,
                .slotGeneration = slot.slot_generation}};
    }
    return std::nullopt;
}

bool http3_ready_scheduler::attach(connection_token token,
    connection_type& connection) noexcept {
    auto* slot = validate(token);
    if (slot == nullptr || slot->state != slot_state::reserved ||
        !connection.onWorker() || connection.epoch_ != token.epoch ||
        connection.connectionGeneration_ != token.connection_generation ||
        connection.activation_.context != slot ||
        connection.activation_.activate != &activation_thunk ||
        connection.activation_.slotGeneration != token.slot_generation) {
        return false;
    }
    slot->owner = &connection;
    slot->state = slot_state::active;
    ++attached_connections_;
    sync_owner(*slot);
    return true;
}

bool http3_ready_scheduler::abandon(connection_token token) noexcept {
    auto* slot = validate(token);
    if (slot == nullptr || slot->state != slot_state::reserved) {
        return false;
    }
    clear_slot(*slot);
    slot->state = slot_state::free;
    free_slots_.push_back(slot->index);
    return true;
}

bool http3_ready_scheduler::begin_retirement(connection_token token) noexcept {
    auto* slot = validate(token);
    if (slot == nullptr || slot->state != slot_state::active || slot->owner == nullptr ||
        !slot->owner->stopped()) {
        return false;
    }
    slot->state = slot_state::retiring;
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
    if (slot == nullptr || slot->state != slot_state::retiring || slot->owner == nullptr ||
        slot->owner->pendingTransportIntentCount() != 0 ||
        !slot->owner->detachActivationAfterJoin()) {
        return false;
    }
    auto* owner = slot->owner;
    if (!owner->transportRetired_ && !owner->transportRetirementTakenOver_ &&
        !owner->closeIntentHandedOff_) {
        std::terminate();
    }
    clear_slot(*slot);
    slot->state = slot_state::free;
    if (attached_connections_ == 0) {
        std::terminate();
    }
    --attached_connections_;
    free_slots_.push_back(slot->index);
    return true;
}

http3_ready_scheduler::step_result http3_ready_scheduler::step() noexcept {
    if (!on_worker()) {
        return {.kind = step_kind::wrong_worker};
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
                if (slot == nullptr || slot->owner == nullptr ||
                    !slot->owner->reactivateBlockedOne(connection_type::WorkLane::kData)) {
                    if (slot != nullptr && slot->owner != nullptr) {
                        sync_owner(*slot);
                    }
                    result.kind = step_kind::reconciled;
                    break;
                }
                result = step_publication(*slot, lane::data);
                break;
            }
            auto* slot = pop_front(queue_id::data_runnable);
            result = slot == nullptr ? step_result{.kind = step_kind::reconciled}
                                     : step_publication(*slot, lane::data);
            break;
        }
        case lane::control: {
            if ((capacity_pass_lanes_ & capacity_control_lane) != 0 &&
                has_recoverable(queue_id::control_blocked)) {
                auto* slot = pop_front(queue_id::control_blocked);
                if (slot == nullptr || slot->owner == nullptr ||
                    !slot->owner->reactivateBlockedOne(connection_type::WorkLane::kControl)) {
                    if (slot != nullptr && slot->owner != nullptr) {
                        sync_owner(*slot);
                    }
                    result.kind = step_kind::reconciled;
                } else {
                    result = step_publication(*slot, lane::control);
                }
                break;
            }
            auto* slot = pop_front(queue_id::control_runnable);
            result = slot == nullptr ? step_result{.kind = step_kind::reconciled}
                                     : step_publication(*slot, lane::control);
            break;
        }
        case lane::local: {
            auto* slot = pop_front(queue_id::local_runnable);
            result = slot == nullptr ? step_result{.kind = step_kind::reconciled}
                                     : step_publication(*slot, lane::local);
            break;
        }
        case lane::intent: {
            auto* slot = pop_front(queue_id::intent_runnable);
            result = slot == nullptr ? step_result{.kind = step_kind::reconciled}
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
    const connection_type::TransportIntentToken& intent, std::optional<connection_type::PushStreamOpenResult> opened) noexcept {
    auto* slot = validate(connection);
    if (slot == nullptr || slot->owner == nullptr || !slot->offered_intent ||
        *slot->offered_intent != intent) {
        return false;
    }
    if (!slot->owner->ackTransportIntent(intent, opened)) {
        return false;
    }
    slot->offered_intent.reset();
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
        return {.wrong_worker = true};
    }
    snapshot_type result;
    result.runnable = {queues_[queue_index(queue_id::data_runnable)].size,
        queues_[queue_index(queue_id::control_runnable)].size,
        queues_[queue_index(queue_id::local_runnable)].size,
        queues_[queue_index(queue_id::intent_runnable)].size};
    result.blocked = {queues_[queue_index(queue_id::data_blocked)].size,
        queues_[queue_index(queue_id::control_blocked)].size};
    result.attached_connections = attached_connections_;
    result.free_connections = free_slots_.size();
    result.capacity_pass_lanes = capacity_pass_lanes_;
    return result;
}

bool http3_ready_scheduler::on_worker() const noexcept {
    return worker_.isCurrent();
}

http3_ready_scheduler::connection_slot*
http3_ready_scheduler::validate(connection_token token) noexcept {
    if (!on_worker() || token.slot >= slots_.size()) {
        return nullptr;
    }
    auto& slot = slots_[token.slot];
    if (slot.scheduler != this || slot.state == slot_state::free ||
        slot.state == slot_state::exhausted || slot.epoch != token.epoch ||
        slot.connection_generation != token.connection_generation ||
        slot.slot_generation != token.slot_generation) {
        return nullptr;
    }
    return &slot;
}

http3_ready_scheduler::connection_token
http3_ready_scheduler::token_for(const connection_slot& slot) noexcept {
    return {.slot = slot.index,
        .epoch = slot.epoch,
        .connection_generation = slot.connection_generation,
        .slot_generation = slot.slot_generation};
}

void http3_ready_scheduler::set_linked(connection_slot& slot, queue_id id, bool linked) noexcept {
    auto& link = slot.links[queue_index(id)];
    if (linked) {
        if (!link.linked) {
            push_back(slot, id);
        }
    } else if (link.linked) {
        remove(slot, id);
    }
}

void http3_ready_scheduler::push_back(connection_slot& slot, queue_id id) noexcept {
    auto& link = slot.links[queue_index(id)];
    if (link.linked) {
        std::terminate();
    }
    if (id == queue_id::data_blocked || id == queue_id::control_blocked) {
        if (blocked_sequence_ == std::numeric_limits<std::size_t>::max()) {
            std::terminate();
        }
        link.recovery_sequence = ++blocked_sequence_;
    }
    auto& queue = queues_[queue_index(id)];
    link.previous = queue.tail;
    link.next = nullptr;
    link.linked = true;
    if (queue.tail != nullptr) {
        queue.tail->links[queue_index(id)].next = &slot;
    } else {
        queue.head = &slot;
    }
    queue.tail = &slot;
    ++queue.size;
}

void http3_ready_scheduler::remove(connection_slot& slot, queue_id id) noexcept {
    auto& link = slot.links[queue_index(id)];
    if (!link.linked) {
        std::terminate();
    }
    auto& queue = queues_[queue_index(id)];
    if (link.previous != nullptr) {
        link.previous->links[queue_index(id)].next = link.next;
    } else {
        queue.head = link.next;
    }
    if (link.next != nullptr) {
        link.next->links[queue_index(id)].previous = link.previous;
    } else {
        queue.tail = link.previous;
    }
    link.previous = nullptr;
    link.next = nullptr;
    link.recovery_sequence = 0;
    link.linked = false;
    if (queue.size == 0) {
        std::terminate();
    }
    --queue.size;
}

http3_ready_scheduler::connection_slot*
http3_ready_scheduler::pop_front(queue_id id) noexcept {
    auto* slot = queues_[queue_index(id)].head;
    if (slot != nullptr) {
        remove(*slot, id);
    }
    return slot;
}

void http3_ready_scheduler::sync_slot(
    connection_slot& slot, const connection_type::WorkerActivation& activation) noexcept {
    if (activation.work.wrongWorker) {
        std::terminate();
    }
    const bool active = slot.state == slot_state::active;
    const bool accepts_intents = active || slot.state == slot_state::retiring;
    set_linked(slot, queue_id::data_runnable, active && activation.work.runnable.data);
    set_linked(slot, queue_id::control_runnable, active && activation.work.runnable.control);
    set_linked(slot, queue_id::local_runnable, active && activation.work.runnable.local);
    set_linked(slot, queue_id::data_blocked, active && activation.work.blocked.data);
    set_linked(slot, queue_id::control_blocked, active && activation.work.blocked.control);

    // An offered token stays owed until actual transport execution is acknowledged,
    // even when a close or a merged reset changes the owner's current head.
    set_linked(slot, queue_id::intent_runnable,
        accepts_intents && activation.transportIntent && !slot.offered_intent);
}

void http3_ready_scheduler::sync_owner(connection_slot& slot) noexcept {
    if (slot.owner == nullptr) {
        return;
    }
    sync_slot(slot, slot.owner->activationSnapshot());
}

void http3_ready_scheduler::receive_activation(connection_slot& slot, std::uint64_t epoch,
    std::uint64_t connection_generation, std::uint64_t slot_generation,
    const connection_type::WorkerActivation& activation) noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    if (slot.scheduler != this || slot.state != slot_state::active ||
        slot.epoch != epoch || slot.connection_generation != connection_generation ||
        slot.slot_generation != slot_generation) {
        return;
    }
    sync_slot(slot, activation);
    const auto& work = activation.work;
    if (work.runnable.data || work.runnable.control || work.runnable.local ||
        activation.transportIntent || activation.inputCapacityAvailable) {
        signal_ready();
    }
}

void http3_ready_scheduler::activation_thunk(void* context, std::uint64_t epoch,
    std::uint64_t connection_generation, std::uint64_t slot_generation,
    const connection_type::WorkerActivation& activation) noexcept {
    if (context == nullptr) {
        std::terminate();
    }
    auto& slot = *static_cast<connection_slot*>(context);
    if (slot.scheduler == nullptr) {
        std::terminate();
    }
    slot.scheduler->receive_activation(
        slot, epoch, connection_generation, slot_generation, activation);
}

void http3_ready_scheduler::signal_ready() noexcept {
    if (ready_callback_.ready != nullptr) {
        ready_callback_.ready(ready_callback_.context);
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
    const auto* head = queues_[queue_index(id)].head;
    return head != nullptr && head->links[queue_index(id)].recovery_sequence <=
                                  recovery_cutoff_[id == queue_id::data_blocked ? 0 : 1];
}

bool http3_ready_scheduler::has_lane_work(lane selected_lane) noexcept {
    switch (selected_lane) {
        case lane::data:
            return queues_[queue_index(queue_id::data_runnable)].head != nullptr ||
                   (((capacity_pass_lanes_ & capacity_data_lane) != 0) &&
                       has_recoverable(queue_id::data_blocked));
        case lane::control:
            return queues_[queue_index(queue_id::control_runnable)].head != nullptr ||
                   (((capacity_pass_lanes_ & capacity_control_lane) != 0) &&
                       has_recoverable(queue_id::control_blocked));
        case lane::local:
            return queues_[queue_index(queue_id::local_runnable)].head != nullptr;
        case lane::intent:
            return queues_[queue_index(queue_id::intent_runnable)].head != nullptr;
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
    if (slot.owner == nullptr) {
        return {.kind = step_kind::reconciled};
    }
    connection_type::WorkLanes lanes{};
    switch (selected_lane) {
        case lane::data:
            lanes.data = true;
            break;
        case lane::control:
            lanes.control = true;
            break;
        case lane::local:
            lanes.local = true;
            break;
        case lane::intent:
            return {.kind = step_kind::reconciled};
    }
    const auto token = token_for(slot);
    const auto publication = slot.owner->publishOne(lanes);
    sync_owner(slot);
    if (publication.status == connection_type::PublishStatus::kNoReadyRequest) {
        return {.kind = step_kind::reconciled, .connection = token};
    }
    if (publication.status == connection_type::PublishStatus::kWrongWorker) {
        return {.kind = step_kind::wrong_worker, .connection = token};
    }
    return {.kind = step_kind::publication,
        .connection = token,
        .publication = publication};
}

http3_ready_scheduler::step_result
http3_ready_scheduler::step_intent(connection_slot& slot) noexcept {
    if (slot.owner == nullptr) {
        return {.kind = step_kind::reconciled};
    }
    const auto intent = slot.owner->peekTransportIntent();
    if (!intent) {
        sync_owner(slot);
        return {.kind = step_kind::reconciled, .connection = token_for(slot)};
    }
    slot.offered_intent = intent->token;
    set_linked(slot, queue_id::intent_runnable, false);
    return {.kind = step_kind::transport_intent,
        .connection = token_for(slot),
        .intent = *intent};
}

void http3_ready_scheduler::clear_slot(connection_slot& slot) noexcept {
    for (std::size_t index = 0; index < queue_index(queue_id::count); ++index) {
        if (slot.links[index].linked) {
            remove(slot, static_cast<queue_id>(index));
        }
    }
    slot.owner = nullptr;
    slot.epoch = 0;
    slot.connection_generation = 0;
    slot.offered_intent.reset();
}

}  // namespace ruvia::detail
