#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <vector>

#include <asio/ip/udp.hpp>

#include "ruvia/core/WorkerNotification.h"
#include "ruvia/core/WorkerRuntimeContext.h"

namespace ruvia::detail {

#if defined(_MSC_VER)
#pragma warning(push)
// Padding deliberately keeps the SPSC counters on separate cache lines.
#pragma warning(disable : 4324)
#endif

// Acceptor-owned, startup-allocated SPSC packet storage in each direction. The
// acceptor owns the UDP socket; neither endpoint exposes protocol state. All
// notification calls and both owner loops must retire before destruction.
class http3_datagram_channel final {
public:
    using udp = asio::ip::udp;
    static constexpr std::size_t packet_capacity = 65536;

    struct datagram_view final {
        std::span<const std::byte> bytes;
        udp::endpoint local_destination;
        udp::endpoint peer;
    };
    http3_datagram_channel(WorkerNotification& acceptor_notification,
        std::pmr::memory_resource* resource, std::size_t input_capacity = 64);
    ~http3_datagram_channel();

    http3_datagram_channel(const http3_datagram_channel&) = delete;
    http3_datagram_channel& operator=(const http3_datagram_channel&) = delete;

    // Cold startup only, before either owner serves. A never-started worker may
    // be rolled back from its lifecycle coordinator with abandon_worker().
    void stage_worker(WorkerRuntimeContext& worker);
    void worker_start() noexcept;
    // All channels may borrow the acceptor's single native wake latch.
    [[nodiscard]] WorkerNotification& acceptor_notification() noexcept;
    [[nodiscard]] WorkerNotification& worker_notification() noexcept;

    // Acceptor-affine. Full input is an ordinary UDP overload drop. Output is a
    // loan: consume ONLY after the UDP send callback (or when no send started).
    [[nodiscard]] bool acceptor_push(std::span<const std::byte> bytes,
        const udp::endpoint& local_destination, const udp::endpoint& peer) noexcept;
    [[nodiscard]] std::optional<datagram_view> acceptor_output() noexcept;
    void acceptor_consume_output(std::error_code error = {}) noexcept;
    void acceptor_close(std::error_code error = {}) noexcept;

    // Worker-affine after worker_start(). One output slot preserves the wire
    // owner's single-send semantics; its bytes survive worker endpoint detach.
    [[nodiscard]] std::optional<datagram_view> worker_input() const noexcept;
    void worker_consume_input() noexcept;
    // Unpublished writable output storage. Commit with worker_send(); no
    // overwrite is permitted while the previous send is pending.
    [[nodiscard]] std::span<std::byte> worker_output_buffer() noexcept;
    [[nodiscard]] bool worker_send(std::span<const std::byte> bytes,
        const udp::endpoint& local_destination, const udp::endpoint& peer) noexcept;
    [[nodiscard]] bool worker_output_pending() const noexcept;
    void wake_worker() noexcept;

    // worker_close requires the forwarded endpoint to have detached and every
    // worker waiter to have returned. The ACK does not reclaim an outbound loan.
    void worker_close() noexcept;
    void abandon_worker() noexcept;
    [[nodiscard]] bool acceptor_closed() const noexcept;
    [[nodiscard]] bool worker_closed() const noexcept;
    [[nodiscard]] std::error_code error() const noexcept;

private:
    struct packet_slot final {
        std::array<std::byte, packet_capacity> bytes;
        udp::endpoint local_destination;
        udp::endpoint peer;
        std::size_t size{};
    };

    static void fill(packet_slot& slot, std::span<const std::byte> bytes,
        const udp::endpoint& local_destination, const udp::endpoint& peer) noexcept;
    [[nodiscard]] static datagram_view view(const packet_slot& slot) noexcept;
    void require_acceptor() const noexcept;
    void require_worker() const noexcept;
    void notify_worker() noexcept;

    const std::thread::id acceptor_owner_;
    std::thread::id worker_owner_;
    WorkerNotification& acceptor_notification_;
    std::optional<WorkerNotification> worker_notification_;
    std::pmr::vector<packet_slot> input_;
    packet_slot output_;
    alignas(64) std::atomic<std::uint64_t> input_published_{};
    std::size_t input_write_slot_{};
    alignas(64) std::atomic<std::uint64_t> input_consumed_{};
    std::size_t input_read_slot_{};
    alignas(64) std::atomic<std::uint64_t> output_published_{};
    alignas(64) std::atomic<std::uint64_t> output_consumed_{};
    std::atomic<bool> acceptor_closed_{};
    std::atomic<bool> worker_started_{};
    std::atomic<bool> worker_closed_{};
    std::error_code error_;
    bool output_loaned_{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace ruvia::detail
