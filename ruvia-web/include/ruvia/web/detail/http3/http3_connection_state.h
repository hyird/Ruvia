#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/core/buffer_pool.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/spsc_ring_queue.h"
#include "ruvia/web/detail/http3/http3_ready_scheduler.h"

namespace ruvia::detail {

struct http3_connection_identity final {
    std::uint64_t epoch{};
    std::uint64_t connection_generation{};

    friend bool operator==(const http3_connection_identity&, const http3_connection_identity&) noexcept = default;
};

// The protocol and handler halves belong to one worker. All observations and
// transitions are synchronous; callback contexts and bound metadata are borrowed.
class http3_connection_state final {
public:
    enum class status : std::uint8_t { changed,
        empty,
        full,
        stale,
        wrong_state,
        unavailable };
    enum class admission_phase : std::uint8_t { vacant,
        reserved,
        bound,
        handler_attached,
        rejected,
        revoked };
    enum class reject_reason : std::uint8_t { capacity,
        construction_failed,
        stopping };
    enum class execution_outcome : std::uint8_t { executed,
        transport_retired,
        stale,
        unavailable,
        invalid };

    struct local_change_callback final {
        void* context;
        void (*changed)(void*) noexcept;
    };
    struct connection_metadata_view final {
        std::string_view remote_address;
        std::string_view client_certificate_subject;
        std::uint16_t remote_port;
    };
    // Text remains valid until handler attachment has copied it. The protocol
    // owner retains its metadata storage until this generation is reset.
    struct binding_snapshot final {
        http3_connection_identity identity{};
        connection_metadata_view metadata{};
        Http3Settings settings{.enableConnectProtocol = true};
        std::size_t max_quic_datagram_payload_bytes{};
    };
    struct admission_seal_snapshot final {
        http3_connection_identity identity{};
        std::size_t expected_admitted_requests{};
        std::uint64_t goaway_id{};
    };
    struct intent_execution_result final {
        execution_outcome outcome{execution_outcome::unavailable};
        std::optional<Http3ServerConnection::PushStreamOpenResult> push_stream{};

        [[nodiscard]] bool completed() const noexcept {
            return outcome == execution_outcome::executed || outcome == execution_outcome::transport_retired;
        }
    };
    struct transport_executor final {
        void* context{};
        intent_execution_result (*execute)(void*, http3_connection_identity, const Http3ServerConnection::TransportIntent&) noexcept {};
    };

    static constexpr std::size_t datagram_capacity = 16;
    static constexpr std::size_t max_datagram_bytes = 1200;
    struct datagram final {
        http3_connection_identity identity{};
        std::uint64_t stream_id{};
        buffer_lease storage{};
        std::size_t size{};

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
            return storage ? storage.bytes().first(size) : std::span<const std::byte>{};
        }
    };

    explicit http3_connection_state(local_change_callback changed = {}, std::pmr::memory_resource* datagram_resource = nullptr);
    ~http3_connection_state();
    http3_connection_state(const http3_connection_state&) = delete;
    http3_connection_state& operator=(const http3_connection_state&) = delete;
    http3_connection_state(http3_connection_state&&) = delete;
    http3_connection_state& operator=(http3_connection_state&&) = delete;

    // A failed reserve leaves no registration. Identities increase
    // lexicographically across reset; stale generations are never reissued.
    [[nodiscard]] status reserve(http3_ready_scheduler& scheduler, std::uint64_t epoch, std::uint64_t connection_generation) noexcept;
    [[nodiscard]] std::optional<http3_connection_identity> available_identity() const noexcept;
    [[nodiscard]] std::optional<http3_connection_identity> identity() const noexcept;
    [[nodiscard]] std::optional<http3_ready_scheduler::registration> registration() const noexcept;
    [[nodiscard]] admission_phase admission() const noexcept;
    void stop_admission() noexcept;
    [[nodiscard]] status bind(http3_connection_identity identity, connection_metadata_view metadata = {}, Http3Settings settings = {.enableConnectProtocol = true}, std::size_t max_quic_datagram_payload_bytes = 0) noexcept;
    [[nodiscard]] std::optional<binding_snapshot> binding() const noexcept;
    [[nodiscard]] status attach_handler(http3_connection_identity identity, Http3ServerConnection& connection) noexcept;
    [[nodiscard]] status reject(http3_connection_identity identity, reject_reason reason) noexcept;
    [[nodiscard]] std::optional<reject_reason> rejection() const noexcept;
    [[nodiscard]] status revoke(http3_connection_identity identity) noexcept;

    // Synchronous worker-local input requires the exact attached generation.
    // Before attachment, the transport retains bytes/FIN/RESET; no request DATA
    // credit or borrowed buffer is consumed by these peer-unidirectional calls.
    [[nodiscard]] std::optional<Http3ServerConnection::EventResult> accept_peer_stream_data(http3_stream_id id, std::span<const std::byte> bytes);
    [[nodiscard]] std::optional<Http3ServerConnection::EventResult> accept_peer_stream_control(const http3_stream_control& control);
    void set_transport_executor(transport_executor executor) noexcept;
    // The caller immediately settles the scheduler's exact offered token after
    // a completed result. This method neither queues nor settles scheduler work.
    [[nodiscard]] intent_execution_result execute_intent(http3_connection_identity identity, const Http3ServerConnection::TransportIntent& intent) noexcept;

    [[nodiscard]] status seal_admission(http3_connection_identity identity, std::size_t expected_admitted_requests, std::uint64_t goaway_id) noexcept;
    [[nodiscard]] std::optional<admission_seal_snapshot> admission_seal() const noexcept;
    [[nodiscard]] status mark_worker_drained(http3_connection_identity identity) noexcept;
    [[nodiscard]] bool worker_drained() const noexcept;
    [[nodiscard]] status start_worker_draining(http3_connection_identity identity) noexcept;
    [[nodiscard]] status mark_transport_retired(http3_connection_identity identity) noexcept;
    [[nodiscard]] bool transport_retired() const noexcept;
    // Caller has stopped and joined all local tasks. Keep the connection alive
    // until retire succeeds: the scheduler detaches its activation there.
    [[nodiscard]] status mark_worker_finalized(http3_connection_identity identity) noexcept;
    [[nodiscard]] bool worker_finalized() const noexcept;
    [[nodiscard]] status retire(http3_connection_identity identity) noexcept;
    [[nodiscard]] bool slot_reusable() const noexcept;
    [[nodiscard]] status reset() noexcept;
    [[nodiscard]] bool ready_to_destroy() const noexcept;

    [[nodiscard]] status publish_request_datagram(http3_connection_identity identity, std::uint64_t stream_id, std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] status publish_response_datagram(http3_connection_identity identity, std::uint64_t stream_id, std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] status pop_request_datagram(datagram& value) noexcept;
    [[nodiscard]] status pop_response_datagram(datagram& value) noexcept;

private:
    struct datagram_storage final {
        buffer_pool pool;
        local_ring_queue<datagram> requests;
        local_ring_queue<datagram> responses;

        explicit datagram_storage(std::pmr::memory_resource* resource);
    };

    [[nodiscard]] bool matches(http3_connection_identity identity) const noexcept;
    [[nodiscard]] status publish_datagram(bool request, http3_connection_identity identity, std::uint64_t stream_id, std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] status pop_datagram(bool request, datagram& value) noexcept;
    void discard_datagrams() noexcept;
    void signal_change() noexcept;
    static void return_datagram(void* context, buffer_credit credit) noexcept;
    [[nodiscard]] static intent_execution_result retired_intent(const Http3ServerConnection::TransportIntent& intent) noexcept;

    const local_change_callback changed_;
    transport_executor executor_{};
    std::unique_ptr<datagram_storage, PmrObjectDeleter<datagram_storage>> datagrams_;
    http3_ready_scheduler* scheduler_{};
    std::optional<http3_ready_scheduler::registration> registration_{};
    Http3ServerConnection* connection_{};
    std::optional<http3_connection_identity> identity_{};
    std::optional<http3_connection_identity> last_identity_{};
    std::optional<binding_snapshot> binding_{};
    std::optional<reject_reason> rejection_{};
    std::optional<admission_seal_snapshot> admission_seal_{};
    admission_phase admission_{admission_phase::vacant};
    bool admission_stopped_{};
    bool worker_draining_{};
    bool worker_drained_{};
    bool transport_retired_{};
    bool worker_finalized_{};
    bool slot_reusable_{};
};

}  // namespace ruvia::detail
