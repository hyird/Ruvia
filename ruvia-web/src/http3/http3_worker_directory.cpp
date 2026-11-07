#include "http3/http3_worker_directory.h"

#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/memory_pool.h"

namespace ruvia::detail {

http3_worker_directory::http3_worker_directory(std::size_t worker_count, std::pmr::memory_resource* resource)
    : slots_(resource) {
    slots_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        slots_.push_back(make_pmr_object<slot>(resource));
    }
}

void http3_worker_directory::bind_worker(std::size_t index, const worker_handle& worker_value) {
    if (index >= slots_.size() || !worker_value.valid()) {
        throw std::invalid_argument("invalid HTTP/3 worker directory slot");
    }
    slots_[index]->worker_ = worker_value;
}

void http3_worker_directory::attach(std::size_t index, void* target, receive_type receive) noexcept {
    auto& value = *slots_[index];
    value.target_ = target;
    value.receive_ = receive;
}

void http3_worker_directory::detach(std::size_t index) noexcept {
    auto& value = *slots_[index];
    value.target_ = nullptr;
    value.receive_ = nullptr;
}

bool http3_worker_directory::hand_off(std::size_t owner, std::size_t listener_index,
    std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, std::uint32_t ecn) noexcept {
    if (owner >= slots_.size()) {
        return false;
    }
    auto& target = *slots_[owner];
    auto token = http3_in_flight_token::reserve(target.in_flight_);
    if (!token) {
        return false;
    }
    try {
        const auto status = target.worker_.post_factory([&]() -> move_only_function<void()> {
            std::pmr::vector<std::byte> copied(bytes.begin(), bytes.end(), process_resource());
            http3_handoff_datagram datagram{
                .bytes_ = std::move(copied),
                .remote_ = remote,
                .ecn_ = ecn,
            };
            return [&target, listener_index, owned = std::move(datagram), token = std::move(token)]() mutable noexcept {
                // The target pointer is confined to this, the owner's, thread.
                if (target.receive_ != nullptr) {
                    target.receive_(target.target_, listener_index, owned);
                }
            };
        });
        return status == post_status::accepted;
    } catch (...) {
        return false;
    }
}

}  // namespace ruvia::detail
