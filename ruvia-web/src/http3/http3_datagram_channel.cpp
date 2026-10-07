#include "ruvia/web/detail/http3/http3_datagram_channel.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>

namespace ruvia::detail {

http3_datagram_channel::http3_datagram_channel(WorkerNotification& acceptor_notification,
    std::pmr::memory_resource* resource, std::size_t input_capacity)
    : acceptor_owner_(std::this_thread::get_id()),
      acceptor_notification_(acceptor_notification),
      input_(resource != nullptr ? resource : std::pmr::get_default_resource()) {
    if (input_capacity == 0 || input_capacity > std::numeric_limits<std::uint64_t>::max() / 2) {
        throw std::invalid_argument("HTTP/3 datagram input capacity must be positive and bounded");
    }
    input_.resize(input_capacity);
}

http3_datagram_channel::~http3_datagram_channel() {
    require_acceptor();
    if (!acceptor_closed() || !worker_closed() || output_loaned_ ||
        output_published_.load(std::memory_order_relaxed) != output_consumed_.load(std::memory_order_relaxed)) {
        std::terminate();
    }
}

void http3_datagram_channel::stage_worker(WorkerRuntimeContext& worker) {
    if (worker_notification_ || worker_started_.load(std::memory_order_acquire) || worker_closed()) {
        throw std::logic_error("HTTP/3 datagram worker already staged or retired");
    }
    worker_notification_.emplace(worker);
}

void http3_datagram_channel::worker_start() noexcept {
    if (!worker_notification_ || worker_closed() || worker_started_.load(std::memory_order_acquire)) {
        std::terminate();
    }
    worker_owner_ = std::this_thread::get_id();
    worker_started_.store(true, std::memory_order_release);
}

WorkerNotification& http3_datagram_channel::acceptor_notification() noexcept {
    return acceptor_notification_;
}

WorkerNotification& http3_datagram_channel::worker_notification() noexcept {
    if (!worker_notification_) {
        std::terminate();
    }
    return *worker_notification_;
}

bool http3_datagram_channel::acceptor_push(std::span<const std::byte> bytes,
    const udp::endpoint& local_destination, const udp::endpoint& peer) noexcept {
    require_acceptor();
    if (acceptor_closed() || worker_closed() || bytes.empty() || bytes.size() > packet_capacity) {
        return false;
    }
    const auto published = input_published_.load(std::memory_order_relaxed);
    const auto consumed = input_consumed_.load(std::memory_order_acquire);
    if (published - consumed == input_.size()) {
        return false;
    }
    fill(input_[input_write_slot_], bytes, local_destination, peer);
    if (++input_write_slot_ == input_.size()) {
        input_write_slot_ = 0;
    }
    input_published_.store(published + 1, std::memory_order_release);
    notify_worker();
    return true;
}

std::optional<http3_datagram_channel::datagram_view>
http3_datagram_channel::acceptor_output() noexcept {
    require_acceptor();
    if (output_consumed_.load(std::memory_order_relaxed) ==
        output_published_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    output_loaned_ = true;
    return view(output_);
}

void http3_datagram_channel::acceptor_consume_output(std::error_code error) noexcept {
    require_acceptor();
    if (!output_loaned_) {
        std::terminate();
    }
    output_loaned_ = false;
    const auto consumed = output_consumed_.load(std::memory_order_relaxed);
    output_consumed_.store(consumed + 1, std::memory_order_release);
    if (error) {
        acceptor_close(error);
    }
    notify_worker();
}

void http3_datagram_channel::acceptor_close(std::error_code error) noexcept {
    require_acceptor();
    if (acceptor_closed()) {
        return;
    }
    error_ = error;
    acceptor_closed_.store(true, std::memory_order_release);
    notify_worker();
}

std::optional<http3_datagram_channel::datagram_view>
http3_datagram_channel::worker_input() const noexcept {
    require_worker();
    if (acceptor_closed() || worker_closed()) {
        return std::nullopt;
    }
    const auto consumed = input_consumed_.load(std::memory_order_relaxed);
    if (consumed == input_published_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return view(input_[input_read_slot_]);
}

void http3_datagram_channel::worker_consume_input() noexcept {
    require_worker();
    const auto consumed = input_consumed_.load(std::memory_order_relaxed);
    if (consumed == input_published_.load(std::memory_order_acquire)) {
        std::terminate();
    }
    if (++input_read_slot_ == input_.size()) {
        input_read_slot_ = 0;
    }
    input_consumed_.store(consumed + 1, std::memory_order_release);
}

std::span<std::byte> http3_datagram_channel::worker_output_buffer() noexcept {
    require_worker();
    if (acceptor_closed() || worker_closed() || worker_output_pending()) {
        return {};
    }
    return output_.bytes;
}

bool http3_datagram_channel::worker_send(std::span<const std::byte> bytes,
    const udp::endpoint& local_destination, const udp::endpoint& peer) noexcept {
    require_worker();
    if (acceptor_closed() || worker_closed() || bytes.empty() || bytes.size() > packet_capacity ||
        worker_output_pending()) {
        return false;
    }
    const auto published = output_published_.load(std::memory_order_relaxed);
    fill(output_, bytes, local_destination, peer);
    output_published_.store(published + 1, std::memory_order_release);
    (void)acceptor_notification_.notify();
    return true;
}

bool http3_datagram_channel::worker_output_pending() const noexcept {
    require_worker();
    return output_published_.load(std::memory_order_relaxed) !=
           output_consumed_.load(std::memory_order_acquire);
}

void http3_datagram_channel::wake_worker() noexcept {
    require_worker();
    notify_worker();
}

void http3_datagram_channel::worker_close() noexcept {
    require_worker();
    if (worker_closed()) {
        return;
    }
    // No wait may remain suspended here. Closing a native target does not
    // reclaim packet storage and rejects concurrent late acceptor notifications.
    try {
        worker_notification_->close();
    } catch (...) {
        std::terminate();
    }
    (void)acceptor_notification_.notify();
    // Final access: observing this ACK permits the acceptor to reclaim us.
    worker_closed_.store(true, std::memory_order_release);
}

void http3_datagram_channel::abandon_worker() noexcept {
    if (worker_closed()) {
        return;
    }
    if (worker_started_.load(std::memory_order_acquire)) {
        std::terminate();
    }
    (void)acceptor_notification_.notify();
    worker_closed_.store(true, std::memory_order_release);
}

bool http3_datagram_channel::acceptor_closed() const noexcept {
    return acceptor_closed_.load(std::memory_order_acquire);
}

bool http3_datagram_channel::worker_closed() const noexcept {
    return worker_closed_.load(std::memory_order_acquire);
}

std::error_code http3_datagram_channel::error() const noexcept {
    return acceptor_closed() ? error_ : std::error_code{};
}

void http3_datagram_channel::fill(packet_slot& slot, std::span<const std::byte> bytes,
    const udp::endpoint& local_destination, const udp::endpoint& peer) noexcept {
    if (bytes.data() != slot.bytes.data()) {
        std::ranges::copy(bytes, slot.bytes.begin());
    }
    slot.size = bytes.size();
    slot.local_destination = local_destination;
    slot.peer = peer;
}

http3_datagram_channel::datagram_view http3_datagram_channel::view(const packet_slot& slot) noexcept {
    return {std::span<const std::byte>(slot.bytes).first(slot.size), slot.local_destination, slot.peer};
}

void http3_datagram_channel::require_acceptor() const noexcept {
    if (std::this_thread::get_id() != acceptor_owner_) {
        std::terminate();
    }
}

void http3_datagram_channel::require_worker() const noexcept {
    if (!worker_started_.load(std::memory_order_acquire) ||
        std::this_thread::get_id() != worker_owner_) {
        std::terminate();
    }
}

void http3_datagram_channel::notify_worker() noexcept {
    if (worker_notification_ && !worker_closed()) {
        (void)worker_notification_->notify();
    }
}

}  // namespace ruvia::detail
