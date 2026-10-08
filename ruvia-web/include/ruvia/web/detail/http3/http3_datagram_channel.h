#pragma once

#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <system_error>
#include <thread>

#include <asio/ip/udp.hpp>

#include "ruvia/core/WorkerNotification.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/buffer_pool.h"
#include "ruvia/core/channel_lifecycle.h"
#include "ruvia/core/spsc_ring_queue.h"
#include "ruvia/web/detail/http3/http3_capacity.h"

namespace ruvia::detail {

#if defined(_MSC_VER)
#pragma warning(push)
// Padding deliberately keeps the SPSC counters on separate cache lines.
#pragma warning(disable : 4324)
#endif

// Only descriptors and linear credits cross owners. The Acceptor's single pool
// owns all bytes; its aggregate notification is borrowed, never closed here.
// I bounds routed RX including returned credits not yet reclaimed by the owner;
// O bounds all issued TX, including available, prepared, queued and native sends.
// Reclaim alone releases either quota. Each issued lease returns at most one
// credit, so I + O slots cannot fill before any remaining return callback.
class http3_datagram_channel final {
public:
    using udp = asio::ip::udp;
    static constexpr std::size_t packet_capacity = http3_capacity::packet_bytes;
    static constexpr std::size_t default_input_capacity = Http3ListenConfig{}.datagram_input_capacity;
    static constexpr std::size_t default_output_window = Http3ListenConfig{}.datagram_output_capacity;

    struct datagram_view final {
        std::span<const std::byte> bytes;
        udp::endpoint local_destination;
        udp::endpoint peer;
    };
    struct datagram final {
        buffer_lease storage;
        std::size_t size{};
        udp::endpoint local_destination;
        udp::endpoint peer;
        [[nodiscard]] datagram_view view() const noexcept {
            return {storage.bytes().first(size), local_destination, peer};
        }
    };

    http3_datagram_channel(buffer_pool& pool, WorkerNotification& acceptor_notification,
        std::pmr::memory_resource* resource = nullptr,
        std::size_t input_capacity = default_input_capacity,
        std::size_t output_window = default_output_window);
    ~http3_datagram_channel();
    http3_datagram_channel(const http3_datagram_channel&) = delete;
    http3_datagram_channel& operator=(const http3_datagram_channel&) = delete;

    void stage_worker(WorkerRuntimeContext& worker);
    void worker_start() noexcept;
    [[nodiscard]] WorkerNotification& worker_notification() noexcept;

    // Acceptor-affine. Failed routing leaves the caller's lease untouched.
    [[nodiscard]] bool acceptor_push(datagram&& packet) noexcept;
    [[nodiscard]] std::optional<datagram> acceptor_take_output() noexcept;
    void acceptor_poll() noexcept;
    void acceptor_close(std::error_code error = {}) noexcept;
    // Final ACK follows worker ACK, independent credit draining and UDP retirement.
    [[nodiscard]] bool acceptor_finalize() noexcept;

    // Worker-affine. RX stays borrowed until consume; TX reserves a descriptor
    // and an already-issued lease before the protocol writes any packet bytes.
    [[nodiscard]] std::optional<datagram_view> worker_input() noexcept;
    void worker_consume_input() noexcept;
    [[nodiscard]] std::span<std::byte> worker_output_buffer() noexcept;
    [[nodiscard]] bool worker_send(std::span<const std::byte> bytes,
        const udp::endpoint& local_destination, const udp::endpoint& peer) noexcept;
    void worker_cancel_output() noexcept;
    [[nodiscard]] bool worker_outbound_capacity() const noexcept;
    [[nodiscard]] std::size_t worker_outbound_count() const noexcept;
    [[nodiscard]] bool worker_outbound_quiescent() const noexcept;
    void worker_stop() noexcept;
    void worker_close() noexcept;
    // Cold startup coordinator only; no started worker may be abandoned.
    void abandon_worker() noexcept;
    [[nodiscard]] bool acceptor_closed() const noexcept;
    [[nodiscard]] bool worker_closed() const noexcept;
    [[nodiscard]] std::error_code error() const noexcept;

private:
    struct returned_credit final {
        buffer_credit credit;
        bool output{};
    };
    static void worker_receive_return(void*, buffer_credit) noexcept;
    static void worker_output_return(void*, buffer_credit) noexcept;
    static void acceptor_output_return(void*, buffer_credit) noexcept;
    static void acceptor_pool_return(void*, buffer_credit) noexcept;
    void return_worker_credit(buffer_credit, bool output) noexcept;
    void replenish_output() noexcept;
    void require_acceptor() const noexcept;
    void require_worker() const noexcept;
    void notify_worker() noexcept;

    const std::thread::id acceptor_owner_;
    std::thread::id worker_owner_;
    buffer_pool& pool_;
    WorkerNotification& acceptor_notification_;
    std::optional<WorkerNotification> worker_notification_;
    const std::size_t credit_capacity_;
    spsc_ring_queue<datagram> input_;
    spsc_ring_queue<datagram> output_;
    spsc_ring_queue<buffer_lease> available_output_;
    // Every routed RX and issued TX returns at most one credit before reclaim.
    // Their owner-side bounds are I and O, independent of packet/control lanes.
    spsc_ring_queue<returned_credit> credits_;
    spsc_channel_lifecycle input_lifecycle_;
    spsc_channel_lifecycle output_lifecycle_;
    std::optional<datagram> held_input_;
    // One unused issued lease may stay with its worker after a zero-byte write.
    // Cancelling admission never creates a native credit/replenishment ping-pong.
    std::optional<buffer_lease> cached_output_;
    std::optional<buffer_lease> prepared_output_;
    std::optional<spsc_channel_lifecycle::admission_lease> output_admission_;
    datagram* output_slot_{};
    std::atomic<bool> worker_started_{};
    std::atomic<std::size_t> completed_output_{};
    std::size_t submitted_output_{};
    std::size_t issued_output_{};
    std::size_t routed_input_{};
    std::error_code error_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace ruvia::detail
