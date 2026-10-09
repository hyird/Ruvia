#include "http3/http3_stream_buffer.h"

#include <cstring>
#include <exception>
#include <utility>

namespace ruvia::detail {

http3_stream_buffer::data_reservation::data_reservation(http3_stream_buffer& owner_value, buffer_lease&& lease_value, http3_stream_id id) noexcept
    : owner_(&owner_value),
      lease_(std::move(lease_value)),
      id_(id) {}

http3_stream_buffer::data_reservation::~data_reservation() {
    abort();
}

http3_stream_buffer::data_reservation::data_reservation(data_reservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      lease_(std::move(other.lease_)),
      id_(other.id_) {}

http3_stream_buffer::data_reservation& http3_stream_buffer::data_reservation::operator=(data_reservation&& other) noexcept {
    if (this != &other) {
        abort();
        owner_ = std::exchange(other.owner_, nullptr);
        lease_ = std::move(other.lease_);
        id_ = other.id_;
    }
    return *this;
}

std::span<std::byte> http3_stream_buffer::data_reservation::writable_bytes() noexcept {
    return lease_.bytes();
}

http3_stream_buffer::commit_result http3_stream_buffer::data_reservation::commit(std::size_t size) noexcept {
    if (owner_ == nullptr) {
        return commit_result::inactive;
    }
    if (size == 0 || size > max_block_bytes) {
        abort();
        return size == 0 ? commit_result::zero_bytes : commit_result::too_large;
    }
    auto* owner_value = std::exchange(owner_, nullptr);
    // The prepared slot remains reserved until this exact commit or abort.
    auto* slot = owner_value->reserved_slot_;
    slot->lease_ = std::move(lease_);
    slot->size_ = size;
    slot->id_ = id_;
    owner_value->data_slots_.commit_push();
    owner_value->reserved_slot_ = nullptr;
    owner_value->notify_ready(data_lane);
    return commit_result::sent;
}

void http3_stream_buffer::data_reservation::abort() noexcept {
    if (owner_ != nullptr) {
        auto* owner_value = std::exchange(owner_, nullptr);
        owner_value->data_slots_.cancel_push();
        owner_value->reserved_slot_ = nullptr;
        // The producer still owns an unpublished reservation. Returning it
        // cannot unblock the peer and must not manufacture a capacity wake.
        owner_value->pool_.reclaim(lease_.release_credit());
    }
}

http3_stream_buffer::borrowed_block::borrowed_block(http3_stream_buffer& owner_value, buffer_lease&& lease_value, std::size_t size, http3_stream_destination id) noexcept
    : owner_(&owner_value),
      lease_(std::move(lease_value)),
      size_(size),
      id_(id) {}

http3_stream_buffer::borrowed_block::~borrowed_block() {
    release();
}

http3_stream_buffer::borrowed_block::borrowed_block(borrowed_block&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      lease_(std::move(other.lease_)),
      size_(other.size_),
      id_(other.id_) {}

http3_stream_buffer::borrowed_block& http3_stream_buffer::borrowed_block::operator=(borrowed_block&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        lease_ = std::move(other.lease_);
        size_ = other.size_;
        id_ = other.id_;
    }
    return *this;
}

std::span<const std::byte> http3_stream_buffer::borrowed_block::bytes() const noexcept {
    return owner_ == nullptr ? std::span<const std::byte>{} : lease_.bytes().first(size_);
}

void http3_stream_buffer::borrowed_block::release() noexcept {
    if (owner_ != nullptr) {
        auto* owner_value = std::exchange(owner_, nullptr);
        --owner_value->outstanding_borrows_;
        lease_.reset();
    }
}

http3_stream_buffer::http3_stream_buffer(std::uint32_t count, std::uint32_t data_slots, std::uint32_t control_slots, std::pmr::memory_resource* resource)
    : http3_stream_buffer(count, data_slots, control_slots, resource, {}) {}

http3_stream_buffer::http3_stream_buffer(std::uint32_t count, std::uint32_t data_slots, std::uint32_t control_slots, std::pmr::memory_resource* resource, local_notifications notifications)
    : notifications_(notifications),
      pool_(count, max_block_bytes, resource),
      data_slots_(data_slots, resource),
      control_slots_(control_slots, resource) {}

http3_stream_buffer::~http3_stream_buffer() {
    if (!stop()) {
        std::terminate();
    }
}

void http3_stream_buffer::set_local_notifications(local_notifications notifications) noexcept {
    notifications_ = notifications;
}

http3_stream_buffer::send_result http3_stream_buffer::try_send(http3_stream_id id, std::span<const std::byte> bytes_value) noexcept {
    return send_address(id, bytes_value);
}

http3_stream_buffer::send_result http3_stream_buffer::try_send_critical(http3_critical_stream_id id, std::span<const std::byte> bytes_value) noexcept {
    return send_address(id, bytes_value);
}

http3_stream_buffer::send_result http3_stream_buffer::send_address(http3_stream_destination id, std::span<const std::byte> bytes_value) noexcept {
    if (reserved_slot_ != nullptr) {
        return send_result::reservation_active;
    }
    if (stopped_) {
        return send_result::stopped;
    }
    if (bytes_value.size() > max_block_bytes) {
        return send_result::too_large;
    }
    if (!data_slots_.has_capacity()) {
        return send_result::full;
    }
    auto lease_value = pool_.try_acquire({this, reclaim_block});
    if (!lease_value) {
        return send_result::no_block;
    }
    if (!bytes_value.empty()) {
        std::memcpy(lease_value->bytes().data(), bytes_value.data(), bytes_value.size());
    }
    auto* slot = data_slots_.prepare_push();
    slot->lease_ = std::move(*lease_value);
    slot->size_ = bytes_value.size();
    slot->id_ = id;
    data_slots_.commit_push();
    notify_ready(data_lane);
    return send_result::sent;
}

http3_stream_buffer::reservation_result http3_stream_buffer::reserve_data(http3_stream_id id, data_reservation& reservation) noexcept {
    if (reservation || reserved_slot_ != nullptr) {
        return reservation_result::reservation_active;
    }
    if (stopped_) {
        return reservation_result::stopped;
    }
    if (!data_slots_.has_capacity()) {
        return reservation_result::full;
    }
    auto lease_value = pool_.try_acquire({this, reclaim_block});
    if (!lease_value) {
        return reservation_result::no_block;
    }
    reserved_slot_ = data_slots_.prepare_push();
    reservation = data_reservation(*this, std::move(*lease_value), id);
    return reservation_result::reserved;
}

http3_stream_buffer::control_result http3_stream_buffer::try_send_control(const http3_stream_control& event) noexcept {
    if (stopped_) {
        return control_result::stopped;
    }
    if (!control_slots_.try_push(event)) {
        return control_result::full;
    }
    notify_ready(control_lane);
    return control_result::sent;
}

bool http3_stream_buffer::try_receive(borrowed_block& block) noexcept {
    block.release();
    auto* slot = data_slots_.front();
    if (slot == nullptr) {
        return false;
    }
    ++outstanding_borrows_;
    block = borrowed_block(*this, std::move(slot->lease_), slot->size_, slot->id_);
    data_slots_.pop();
    notify_capacity(data_lane);
    return true;
}

bool http3_stream_buffer::try_receive_control(http3_stream_control& event) noexcept {
    if (!control_slots_.try_pop(event)) {
        return false;
    }
    notify_capacity(control_lane);
    return true;
}

bool http3_stream_buffer::stop() noexcept {
    stopped_ = true;
    return quiescent();
}

bool http3_stream_buffer::has_pending() const noexcept {
    return !data_slots_.empty() || !control_slots_.empty();
}

std::uint32_t http3_stream_buffer::block_capacity() const noexcept {
    return static_cast<std::uint32_t>(pool_.capacity());
}

void http3_stream_buffer::reclaim_block(void* context_value, buffer_credit credit) noexcept {
    auto& owner_value = *static_cast<http3_stream_buffer*>(context_value);
    owner_value.pool_.reclaim(std::move(credit));
    owner_value.notify_capacity(data_lane);
}

void http3_stream_buffer::notify_ready(std::uint8_t lanes) noexcept {
    // A pre-stop reservation may still publish: its drain must remain visible.
    if (notifications_.ready_.notify_ != nullptr) {
        notifications_.ready_.notify_(notifications_.ready_.context_, lanes);
    }
}

void http3_stream_buffer::notify_capacity(std::uint8_t lanes) noexcept {
    if (!stopped_ && notifications_.capacity_.notify_ != nullptr) {
        notifications_.capacity_.notify_(notifications_.capacity_.context_, lanes);
    }
}

}  // namespace ruvia::detail
