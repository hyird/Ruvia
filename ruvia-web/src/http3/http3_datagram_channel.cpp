#include "http3/http3_datagram_channel.h"

#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {
namespace {
std::size_t checked_credit_capacity(const buffer_pool& pool, std::size_t input_capacity,
    std::size_t output_window) {
    if (input_capacity == 0 || output_window == 0 ||
        input_capacity > std::numeric_limits<std::size_t>::max() - output_window) {
        throw std::invalid_argument("HTTP/3 channel budget is not representable");
    }
    const auto capacity = input_capacity + output_window;
    if (capacity > pool.capacity()) {
        throw std::invalid_argument("HTTP/3 channel budget exceeds its shared pool");
    }
    return capacity;
}
}  // namespace

http3_datagram_channel::http3_datagram_channel(buffer_pool& pool,
    WorkerNotification& acceptor_notification, std::pmr::memory_resource* resource,
    std::size_t input_capacity, std::size_t output_window)
    : acceptor_owner_(std::this_thread::get_id()),
      pool_(pool),
      acceptor_notification_(acceptor_notification),
      credit_capacity_(checked_credit_capacity(pool, input_capacity, output_window)),
      input_(input_capacity, resource),
      output_(output_window, resource),
      available_output_(output_window, resource),
      credits_(credit_capacity_, resource) {
    replenish_output();
}

http3_datagram_channel::~http3_datagram_channel() {
    require_acceptor();
    if (!input_lifecycle_.consumer_finalized() || !output_lifecycle_.consumer_finalized() ||
        issued_output_ != 0 || routed_input_ != 0 || !input_.empty() || !output_.empty() ||
        !available_output_.empty() || !credits_.empty()) {
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
    if (!worker_notification_ || worker_closed() || output_lifecycle_.producer_closed() ||
        worker_started_.load(std::memory_order_acquire)) {
        std::terminate();
    }
    worker_owner_ = std::this_thread::get_id();
    worker_started_.store(true, std::memory_order_release);
}

WorkerNotification& http3_datagram_channel::worker_notification() noexcept {
    if (!worker_notification_) {
        std::terminate();
    }
    return *worker_notification_;
}

bool http3_datagram_channel::acceptor_push(datagram&& packet) noexcept {
    require_acceptor();
    auto admission = input_lifecycle_.try_admit();
    if (!admission || routed_input_ == input_.capacity() || !packet.storage || packet.size == 0 ||
        packet.size > packet.storage.bytes().size() || packet.size > packet_capacity) {
        return false;
    }
    auto* slot = input_.prepare_push();
    if (!slot) {
        return false;
    }
    *slot = std::move(packet);
    ++routed_input_;
    input_.commit_push();
    notify_worker();
    return true;
}

std::optional<http3_datagram_channel::datagram> http3_datagram_channel::acceptor_take_output() noexcept {
    require_acceptor();
    datagram packet;
    if (!output_.try_pop(packet)) {
        return std::nullopt;
    }
    // Only the current holder rebinds. Queue publication itself never changes
    // sender failure/cancellation's owner-affine return destination.
    packet.storage.set_return_callback({this, acceptor_output_return});
    return packet;
}

void http3_datagram_channel::acceptor_poll() noexcept {
    require_acceptor();
    const auto batch = credits_.front_batch(default_input_capacity);
    const auto reclaim = [this](auto span) noexcept {
        for (auto& returned : span) {
            pool_.reclaim(std::move(returned.credit));
            if (returned.output) {
                if (issued_output_ == 0) {
                    std::terminate();
                }
                --issued_output_;
            } else {
                if (routed_input_ == 0) {
                    std::terminate();
                }
                --routed_input_;
            }
        }
    };
    reclaim(batch.first);
    reclaim(batch.second);
    if (!batch.empty()) {
        credits_.pop(batch.size());
        notify_worker();
    }
    if (!credits_.empty()) {
        (void)acceptor_notification_.notify();
    }
    if (input_lifecycle_.stop_requested() && !input_lifecycle_.producer_closed()) {
        acceptor_close();
    }
    replenish_output();
    if (!worker_started_.load(std::memory_order_acquire) && output_lifecycle_.producer_closed() &&
        !input_lifecycle_.consumer_finalized()) {
        // A cold coordinator publishes stop/close only. Pool access and queue
        // draining remain on the Acceptor, including staging rollback.
        acceptor_close();
        datagram packet;
        while (input_.try_pop(packet)) {
            pool_.reclaim(packet.storage.release_credit());
            --routed_input_;
        }
        buffer_lease lease;
        while (available_output_.try_pop(lease)) {
            pool_.reclaim(lease.release_credit());
            --issued_output_;
        }
        input_lifecycle_.finalize();
    }
}

void http3_datagram_channel::acceptor_close(std::error_code error) noexcept {
    require_acceptor();
    if (input_lifecycle_.producer_closed()) {
        return;
    }
    error_ = error;
    input_lifecycle_.close();
    output_lifecycle_.request_stop();
    notify_worker();
}

bool http3_datagram_channel::acceptor_finalize() noexcept {
    require_acceptor();
    acceptor_poll();
    if (!worker_closed() || !output_lifecycle_.producer_closed() || issued_output_ != 0 ||
        routed_input_ != 0 || !output_.empty() || !credits_.empty() || !available_output_.empty()) {
        return false;
    }
    output_lifecycle_.finalize();
    return true;
}

std::optional<http3_datagram_channel::datagram_view> http3_datagram_channel::worker_input() noexcept {
    require_worker();
    if (input_lifecycle_.stop_requested()) {
        return std::nullopt;
    }
    if (!held_input_) {
        auto* slot = input_.front();
        if (!slot) {
            return std::nullopt;
        }
        datagram packet = std::move(*slot);
        packet.storage.set_return_callback({this, worker_receive_return});
        held_input_.emplace(std::move(packet));
    }
    return held_input_->view();
}

void http3_datagram_channel::worker_consume_input() noexcept {
    require_worker();
    if (!held_input_) {
        std::terminate();
    }
    held_input_.reset();
    input_.pop();
}

std::span<std::byte> http3_datagram_channel::worker_output_buffer() noexcept {
    require_worker();
    if (prepared_output_) {
        return prepared_output_->bytes();
    }
    auto admission = output_lifecycle_.try_admit();
    if (!admission) {
        return {};
    }
    auto* slot = output_.prepare_push();
    if (!slot) {
        return {};
    }
    buffer_lease lease;
    if (cached_output_) {
        lease = std::move(*cached_output_);
        cached_output_.reset();
    } else if (!available_output_.try_pop(lease)) {
        output_.cancel_push();
        return {};
    }
    lease.set_return_callback({this, worker_output_return});
    output_admission_ = std::move(admission);
    prepared_output_.emplace(std::move(lease));
    output_slot_ = slot;
    return prepared_output_->bytes();
}

bool http3_datagram_channel::worker_send(std::span<const std::byte> bytes,
    const udp::endpoint& local_destination, const udp::endpoint& peer) noexcept {
    require_worker();
    if (!prepared_output_ || bytes.empty() || bytes.data() != prepared_output_->bytes().data() ||
        bytes.size() > prepared_output_->bytes().size() || output_lifecycle_.stop_requested()) {
        worker_cancel_output();
        return false;
    }
    *output_slot_ = datagram{std::move(*prepared_output_), bytes.size(), local_destination, peer};
    prepared_output_.reset();
    output_slot_ = nullptr;
    ++submitted_output_;
    output_.commit_push();
    output_admission_.reset();
    (void)acceptor_notification_.notify();
    return true;
}

void http3_datagram_channel::worker_cancel_output() noexcept {
    require_worker();
    if (output_lifecycle_.stop_requested()) {
        prepared_output_.reset();
        cached_output_.reset();
    } else if (prepared_output_) {
        if (cached_output_) {
            std::terminate();
        }
        cached_output_.emplace(std::move(*prepared_output_));
        prepared_output_.reset();
    }
    output_slot_ = nullptr;
    output_.cancel_push();
    output_admission_.reset();
}

bool http3_datagram_channel::worker_outbound_capacity() const noexcept {
    require_worker();
    return !output_lifecycle_.stop_requested() &&
           (prepared_output_.has_value() ||
               ((cached_output_.has_value() || !available_output_.empty()) && output_.has_capacity()));
}

std::size_t http3_datagram_channel::worker_outbound_count() const noexcept {
    require_worker();
    return submitted_output_ - completed_output_.load(std::memory_order_acquire);
}

bool http3_datagram_channel::worker_outbound_quiescent() const noexcept {
    return worker_outbound_count() == 0 && !prepared_output_ &&
           (!output_lifecycle_.stop_requested() ||
               (credits_.empty() && input_.empty() && available_output_.empty() &&
                   !held_input_ && !cached_output_));
}

void http3_datagram_channel::worker_stop() noexcept {
    require_worker();
    const bool first_stop = !output_lifecycle_.producer_closed();
    input_lifecycle_.request_stop();
    worker_cancel_output();
    output_lifecycle_.close();
    cached_output_.reset();
    if (held_input_) {
        held_input_.reset();
        input_.pop();
    }
    datagram packet;
    while (input_.try_pop(packet)) {
        packet.storage.set_return_callback({this, worker_receive_return});
        packet.storage.reset();
    }
    buffer_lease lease;
    while (available_output_.try_pop(lease)) {
        lease.set_return_callback({this, worker_output_return});
        lease.reset();
    }
    if (first_stop) {
        (void)acceptor_notification_.notify();
    }
}

void http3_datagram_channel::worker_close() noexcept {
    require_worker();
    if (worker_closed()) {
        return;
    }
    worker_stop();
    if (!input_lifecycle_.producer_closed() || !worker_outbound_quiescent()) {
        std::terminate();
    }
    try {
        worker_notification_->close();
    } catch (...) {
        std::terminate();
    }
    (void)acceptor_notification_.notify();
    // Final access. The cold Acceptor check covers notification-before-ACK.
    input_lifecycle_.finalize();
}

void http3_datagram_channel::abandon_worker() noexcept {
    if (worker_started_.load(std::memory_order_acquire)) {
        std::terminate();
    }
    if (worker_closed()) {
        return;
    }
    input_lifecycle_.request_stop();
    output_lifecycle_.close();
    (void)acceptor_notification_.notify();
}

bool http3_datagram_channel::acceptor_closed() const noexcept {
    return input_lifecycle_.producer_closed();
}

bool http3_datagram_channel::worker_closed() const noexcept {
    return input_lifecycle_.consumer_finalized();
}

std::error_code http3_datagram_channel::error() const noexcept {
    return acceptor_closed() ? error_ : std::error_code{};
}

void http3_datagram_channel::worker_receive_return(void* context, buffer_credit credit) noexcept {
    static_cast<http3_datagram_channel*>(context)->return_worker_credit(std::move(credit), false);
}

void http3_datagram_channel::worker_output_return(void* context, buffer_credit credit) noexcept {
    static_cast<http3_datagram_channel*>(context)->return_worker_credit(std::move(credit), true);
}

void http3_datagram_channel::return_worker_credit(buffer_credit credit, bool output) noexcept {
    require_worker();
    if (!credits_.try_push(returned_credit{std::move(credit), output})) {
        std::terminate();
    }
    (void)acceptor_notification_.notify();
}

void http3_datagram_channel::acceptor_output_return(void* context, buffer_credit credit) noexcept {
    auto& self = *static_cast<http3_datagram_channel*>(context);
    self.require_acceptor();
    self.pool_.reclaim(std::move(credit));
    if (self.issued_output_ == 0) {
        std::terminate();
    }
    --self.issued_output_;
    self.completed_output_.fetch_add(1, std::memory_order_release);
    self.notify_worker();
    (void)self.acceptor_notification_.notify();
}

void http3_datagram_channel::acceptor_pool_return(void* context, buffer_credit credit) noexcept {
    auto& self = *static_cast<http3_datagram_channel*>(context);
    self.require_acceptor();
    self.pool_.reclaim(std::move(credit));
}

void http3_datagram_channel::replenish_output() noexcept {
    if (output_lifecycle_.stop_requested() || input_lifecycle_.stop_requested()) {
        return;
    }
    bool published = false;
    while (issued_output_ < available_output_.capacity()) {
        auto* slot = available_output_.prepare_push();
        if (!slot) {
            break;
        }
        auto lease = pool_.try_acquire({this, acceptor_pool_return});
        if (!lease) {
            available_output_.cancel_push();
            break;
        }
        *slot = std::move(*lease);
        ++issued_output_;
        available_output_.commit_push();
        published = true;
    }
    if (published) {
        notify_worker();
    }
}

void http3_datagram_channel::require_acceptor() const noexcept {
    if (std::this_thread::get_id() != acceptor_owner_) {
        std::terminate();
    }
}

void http3_datagram_channel::require_worker() const noexcept {
    if (!worker_started_.load(std::memory_order_acquire) || std::this_thread::get_id() != worker_owner_) {
        std::terminate();
    }
}

void http3_datagram_channel::notify_worker() noexcept {
    if (worker_notification_ && !worker_closed()) {
        (void)worker_notification_->notify();
    }
}

}  // namespace ruvia::detail
