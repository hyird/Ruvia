#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <variant>

#include "ruvia/core/buffer_pool.h"
#include "ruvia/core/spsc_ring_queue.h"
#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/http3_critical_stream_output.h"

namespace ruvia::detail {

struct http3_stream_id final {
    std::uint64_t epoch{};
    std::uint64_t connection_generation{};
    std::uint64_t stream_id{};
    // Only real server-initiated response streams bind a Push ID.
    std::optional<std::uint64_t> push_id{};
    bool received_early_data{};
};

struct http3_critical_stream_id final {
    std::uint64_t epoch{};
    std::uint64_t connection_generation{};
    ruvia::http3_critical_stream_output::stream_kind kind{ruvia::http3_critical_stream_output::stream_kind::qpack_encoder};
};
using http3_stream_destination = std::variant<http3_stream_id, http3_critical_stream_id>;

struct http3_stream_control final {
    enum class kind : std::uint8_t { connection_closed,
        stream_reset,
        writable,
        stream_fin,
        tunnel_established };
    kind kind{kind::connection_closed};
    http3_stream_id id{};
    // FIN is the final cumulative DATA count. Peer RESET is the cumulative
    // published DATA count, not QUIC Final Size. Independent lanes can deliver
    // these barriers before DATA; consumers must defer them until that count.
    // Tunnel establishment waits for this count to be accepted by transport.
    std::uint64_t value{};
    Http3ConnectionErrorCode stream_reset_error_code{Http3ConnectionErrorCode::kRequestCancelled};
};

// All operations, reservations and borrows belong to one worker. The pool is
// the sole block/credit authority; consuming a queue slot does not return its
// block until the borrow is released. Neither hot path allocates or wakes a
// native notification target.
class http3_stream_buffer final {
public:
    static constexpr std::size_t max_block_bytes = 4 * 1024;
    static constexpr std::uint8_t data_lane = 1;
    static constexpr std::uint8_t control_lane = 2;

    enum class send_result : std::uint8_t { sent,
        full,
        no_block,
        too_large,
        stopped,
        reservation_active };
    enum class reservation_result : std::uint8_t { reserved,
        full,
        no_block,
        stopped,
        reservation_active };
    enum class commit_result : std::uint8_t { sent,
        zero_bytes,
        too_large,
        inactive };
    enum class control_result : std::uint8_t { sent,
        full,
        stopped };

    struct local_callback final {
        void* context{};
        void (*notify)(void*, std::uint8_t lanes) noexcept {};
    };
    struct local_notifications final {
        local_callback ready{};
        local_callback capacity{};
    };

    // Reserves a DATA queue slot and a linear block before protocol reads.
    // Invalid commit sizes abort and restore both credits. stop() closes new
    // admission but does not revoke a previously admitted reservation.
    class data_reservation final {
    public:
        data_reservation() = default;
        ~data_reservation();
        data_reservation(const data_reservation&) = delete;
        data_reservation& operator=(const data_reservation&) = delete;
        data_reservation(data_reservation&& other) noexcept;
        data_reservation& operator=(data_reservation&& other) noexcept;
        [[nodiscard]] explicit operator bool() const noexcept {
            return owner_ != nullptr;
        }
        [[nodiscard]] std::span<std::byte> writable_bytes() noexcept;
        [[nodiscard]] commit_result commit(std::size_t size) noexcept;
        void abort() noexcept;

    private:
        friend class http3_stream_buffer;
        data_reservation(http3_stream_buffer& owner, buffer_lease&& lease, http3_stream_id id) noexcept;
        http3_stream_buffer* owner_{};
        buffer_lease lease_{};
        http3_stream_id id_{};
    };

    class borrowed_block final {
    public:
        borrowed_block() = default;
        ~borrowed_block();
        borrowed_block(const borrowed_block&) = delete;
        borrowed_block& operator=(const borrowed_block&) = delete;
        borrowed_block(borrowed_block&& other) noexcept;
        borrowed_block& operator=(borrowed_block&& other) noexcept;
        [[nodiscard]] explicit operator bool() const noexcept {
            return owner_ != nullptr;
        }
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
        [[nodiscard]] const http3_stream_id& id() const noexcept {
            return std::get<http3_stream_id>(id_);
        }
        [[nodiscard]] const http3_critical_stream_id* critical() const noexcept {
            return std::get_if<http3_critical_stream_id>(&id_);
        }
        void release() noexcept;

    private:
        friend class http3_stream_buffer;
        borrowed_block(http3_stream_buffer& owner, buffer_lease&& lease, std::size_t size, http3_stream_destination id) noexcept;
        http3_stream_buffer* owner_{};
        buffer_lease lease_{};
        std::size_t size_{};
        http3_stream_destination id_{};
    };

    // Resource and callback contexts outlive this buffer and all external
    // borrows/reservations. Callbacks only signal/schedule local work: they
    // must not reenter buffer operations. Startup binding is allocation-free.
    // Destruction with an external reservation or borrow terminates; queued
    // owner-held blocks are discarded normally after stop.
    http3_stream_buffer(std::uint32_t count, std::uint32_t data_slots, std::uint32_t control_slots,
        std::pmr::memory_resource* resource = nullptr);
    http3_stream_buffer(std::uint32_t count, std::uint32_t data_slots, std::uint32_t control_slots,
        std::pmr::memory_resource* resource, local_notifications notifications);
    ~http3_stream_buffer();
    http3_stream_buffer(const http3_stream_buffer&) = delete;
    http3_stream_buffer& operator=(const http3_stream_buffer&) = delete;
    http3_stream_buffer(http3_stream_buffer&&) = delete;
    http3_stream_buffer& operator=(http3_stream_buffer&&) = delete;

    void set_local_notifications(local_notifications notifications) noexcept;
    [[nodiscard]] send_result try_send(http3_stream_id id, std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] send_result try_send_critical(http3_critical_stream_id id, std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] reservation_result reserve_data(http3_stream_id id, data_reservation& reservation) noexcept;
    [[nodiscard]] control_result try_send_control(const http3_stream_control& event) noexcept;
    [[nodiscard]] bool try_receive(borrowed_block& block) noexcept;
    [[nodiscard]] bool try_receive_control(http3_stream_control& event) noexcept;
    // Stop does not revoke storage. Queued work remains drainable and external
    // borrows return credits immediately, including out-of-order releases.
    [[nodiscard]] bool stop() noexcept;
    [[nodiscard]] bool stopped() const noexcept {
        return stopped_;
    }
    [[nodiscard]] bool quiescent() const noexcept {
        return reserved_slot_ == nullptr && outstanding_borrows_ == 0;
    }
    [[nodiscard]] bool has_pending() const noexcept;
    [[nodiscard]] std::uint32_t block_capacity() const noexcept;

private:
    struct data_slot final {
        buffer_lease lease{};
        std::size_t size{};
        http3_stream_destination id{};
    };
    [[nodiscard]] send_result send_address(http3_stream_destination id, std::span<const std::byte> bytes) noexcept;
    static void reclaim_block(void* context, buffer_credit credit) noexcept;
    void notify_ready(std::uint8_t lanes) noexcept;
    void notify_capacity(std::uint8_t lanes) noexcept;

    // Queues die before the pool and callback state they borrow.
    local_notifications notifications_{};
    bool stopped_{};
    data_slot* reserved_slot_{};
    std::uint32_t outstanding_borrows_{};
    buffer_pool pool_;
    local_ring_queue<data_slot> data_slots_;
    local_ring_queue<http3_stream_control> control_slots_;
};

}  // namespace ruvia::detail
