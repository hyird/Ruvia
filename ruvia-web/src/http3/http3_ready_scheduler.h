#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <vector>

#include "ruvia/core/worker_handle.h"

#include "http3/http3_server_connection.h"

namespace ruvia::detail {

// Worker-local, bounded fair ready lists. Connection owners retain publications
// and transport intents; the scheduler only chooses one unit of work per step.
class http3_ready_scheduler final {
public:
    using connection_type = http3_server_connection;

    static constexpr std::size_t default_control_burst_limit = 4;
    static constexpr std::uint8_t capacity_data = 1;
    static constexpr std::uint8_t capacity_control = 2;
    static constexpr std::uint8_t capacity_all = capacity_data | capacity_control;

    // Borrowed callback, invoked only on the worker. It may signal/schedule the
    // runner, but must not re-enter the scheduler or a connection owner.
    struct local_ready_callback final {
        void* context_;
        void (*ready_)(void* context) noexcept;
    };

    struct connection_token final {
        std::size_t slot_{};
        std::uint64_t epoch_{};
        std::uint64_t connection_generation_{};
        std::uint64_t slot_generation_{};

        friend bool operator==(const connection_token&, const connection_token&) noexcept = default;
    };

    struct registration final {
        connection_token token_{};
        connection_type::activation_ref_type activation_{};
    };

    enum class step_kind : std::uint8_t { idle,
        publication,
        transport_intent,
        reconciled,
        wrong_worker };

    struct step_result final {
        step_kind kind_{step_kind::idle};
        connection_token connection_{};
        connection_type::publish_attempt_type publication_{};
        connection_type::transport_intent_type intent_{};
    };

    struct snapshot_type final {
        std::array<std::size_t, 4> runnable_{};  // DATA, CONTROL, LOCAL, intent
        std::array<std::size_t, 2> blocked_{};   // DATA, CONTROL publication
        std::size_t attached_connections_{};
        std::size_t free_connections_{};
        std::uint8_t capacity_pass_lanes_{};
        bool wrong_worker_{};
    };

    // Startup-only allocation. The callback context must outlive the scheduler.
    http3_ready_scheduler(const worker_handle& worker_value, std::size_t max_connections,
        std::pmr::memory_resource* resource = nullptr,
        local_ready_callback ready_callback = {},
        std::size_t control_burst_limit = default_control_burst_limit);
    ~http3_ready_scheduler();
    http3_ready_scheduler(const http3_ready_scheduler&) = delete;
    http3_ready_scheduler& operator=(const http3_ready_scheduler&) = delete;
    http3_ready_scheduler(http3_ready_scheduler&&) = delete;
    http3_ready_scheduler& operator=(http3_ready_scheduler&&) = delete;

    // O(1) reservation. Its activation context remains address-stable until retire.
    [[nodiscard]] std::optional<registration> reserve(
        std::uint64_t epoch, std::uint64_t connection_generation) noexcept;
    [[nodiscard]] bool attach(connection_token token, connection_type& connection) noexcept;
    [[nodiscard]] bool abandon(connection_token token) noexcept;

    // After request_stop, fence activations and remove publication work while
    // retaining transport debt. Reuse requires join, intent execution, callback
    // detachment and successful retire.
    [[nodiscard]] bool begin_retirement(connection_token token) noexcept;
    [[nodiscard]] bool retire(connection_token token) noexcept;

    // One publication attempt or one connection-owned intent, never both.
    // A returned intent stays owed until its exact token is acknowledged after
    // actual local transport execution (not merely after offering the plan).
    [[nodiscard]] step_result step() noexcept;
    [[nodiscard]] bool acknowledge_intent(connection_token connection,
        const connection_type::transport_intent_token_type& intent,
        std::optional<connection_type::push_stream_open_result_type> opened = {}) noexcept;

    // A local capacity event retries each already-blocked connection at most
    // once per indicated lane. Failed attempts return to the blocked tail;
    // another event is required. DATA events never extend a CONTROL pass.
    void receive_capacity(std::uint8_t lanes) noexcept;
    [[nodiscard]] snapshot_type snapshot() const noexcept;

private:
    enum class slot_state : std::uint8_t { free,
        reserved,
        active,
        retiring,
        exhausted };
    enum class lane : std::uint8_t { data,
        control,
        local,
        intent };
    enum class queue_id : std::uint8_t { data_runnable,
        control_runnable,
        local_runnable,
        intent_runnable,
        data_blocked,
        control_blocked,
        count };

    struct connection_slot;
    struct link final {
        connection_slot* previous_{};
        connection_slot* next_{};
        std::size_t recovery_sequence_{};
        bool linked_{};
    };
    struct connection_slot final {
        http3_ready_scheduler* scheduler_{};
        std::size_t index_{};
        std::uint64_t epoch_{};
        std::uint64_t connection_generation_{};
        std::uint64_t slot_generation_{};
        connection_type* owner_{};
        slot_state state_{slot_state::free};
        std::optional<connection_type::transport_intent_token_type> offered_intent_{};
        std::array<link, static_cast<std::size_t>(queue_id::count)> links_{};
    };
    struct slot_queue final {
        connection_slot* head_{};
        connection_slot* tail_{};
        std::size_t size_{};
    };

    [[nodiscard]] static constexpr std::size_t queue_index(queue_id id) noexcept {
        return static_cast<std::size_t>(id);
    }
    [[nodiscard]] bool on_worker() const noexcept;
    [[nodiscard]] connection_slot* validate(connection_token token) noexcept;
    [[nodiscard]] static connection_token token_for(const connection_slot& slot) noexcept;
    void set_linked(connection_slot& slot, queue_id id, bool linked) noexcept;
    void push_back(connection_slot& slot, queue_id id) noexcept;
    void remove(connection_slot& slot, queue_id id) noexcept;
    [[nodiscard]] connection_slot* pop_front(queue_id id) noexcept;
    void sync_slot(connection_slot& slot, const connection_type::worker_activation_type& activation) noexcept;
    void sync_owner(connection_slot& slot) noexcept;
    void receive_activation(connection_slot& slot, std::uint64_t epoch,
        std::uint64_t connection_generation, std::uint64_t slot_generation,
        const connection_type::worker_activation_type& activation) noexcept;
    static void activation_thunk(void* context, std::uint64_t epoch,
        std::uint64_t connection_generation, std::uint64_t slot_generation,
        const connection_type::worker_activation_type& activation) noexcept;
    void signal_ready() noexcept;
    void start_capacity_pass(std::uint8_t lanes) noexcept;
    void normalize_capacity_pass() noexcept;
    [[nodiscard]] bool has_recoverable(queue_id id) noexcept;
    [[nodiscard]] bool has_lane_work(lane selected_lane) noexcept;
    [[nodiscard]] std::optional<lane> select_lane() noexcept;
    [[nodiscard]] step_result step_publication(connection_slot& slot, lane selected_lane) noexcept;
    [[nodiscard]] step_result step_intent(connection_slot& slot) noexcept;
    void clear_slot(connection_slot& slot) noexcept;

    const worker_handle& worker_;
    const std::size_t max_connections_;
    const local_ready_callback ready_callback_;
    const std::size_t control_burst_limit_;
    std::pmr::vector<connection_slot> slots_;
    std::pmr::vector<std::size_t> free_slots_;
    std::array<slot_queue, static_cast<std::size_t>(queue_id::count)> queues_{};
    std::size_t attached_connections_{};
    std::size_t blocked_sequence_{};
    std::array<std::size_t, 2> recovery_cutoff_{};
    std::uint8_t capacity_pass_lanes_{};
    lane next_lane_{lane::data};
    std::size_t control_burst_{};
};

}  // namespace ruvia::detail
