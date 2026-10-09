#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_server_request.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/http_response_stream.h"

#include "http3/http3_server_body_budget.h"
#include "router/route_resolution.h"
#include "server/inbound_buffer_resource.h"

namespace ruvia {
class worker_memory;
}

namespace ruvia::detail {

class route_table;

// Per-connection limits. A caller may additionally lend one worker-wide body
// budget to all sessions owned by that worker.
struct http3_sans_io_session_limits final {
    std::size_t max_buffered_body_bytes_{default_max_buffered_body_bytes};
    std::optional<std::size_t> max_stream_body_bytes_{};
    std::size_t max_stream_backlog_bytes_{64 * 1024};
    std::size_t max_live_streams_{32};
    std::size_t max_buffered_bytes_in_flight_{64 * 1024 * 1024};
    std::size_t max_tunnel_buffered_bytes_{64 * 1024};
    http3_connection_config connection_{.enable_connect_protocol_ = true};
    std::size_t max_quic_datagram_payload_bytes_{};
    std::pmr::memory_resource* inbound_buffer_pool_{};
    std::size_t max_inbound_buffer_bytes_{64 * 1024 * 1024};
};

// Worker-affine receive-side slice of an HTTP/3 Web session. It synchronously
// copies request heads and buffered body bytes while http3_connection's event
// views are valid, and resolves routes at HEADERS. Completed requests remain
// owned until release(). Handler scheduling and response writing are purposely
// not part of this first slice.
class http3_sans_io_session_engine final {
    struct stream_type;

public:
    // Worker-affine move-only dispatch borrow. The session and worker must outlive it.
    // Keep it until every request-backed object (including the response) is destroyed;
    // teardown retains the request until this lease is returned. It never crosses threads.
    // Acquisition, moves and metadata access allocate nothing; access uses stable pointers.
    class request_lease_type final {
    public:
        ~request_lease_type();
        request_lease_type(request_lease_type&& other) noexcept;
        request_lease_type(const request_lease_type&) = delete;
        request_lease_type& operator=(const request_lease_type&) = delete;
        request_lease_type& operator=(request_lease_type&&) = delete;
        [[nodiscard]] const http3_server_request& request() const& noexcept;
        const http3_server_request& request() const&& = delete;
        [[nodiscard]] const route_resolution& resolution() const& noexcept;
        const route_resolution& resolution() const&& = delete;

    private:
        friend class http3_sans_io_session_engine;
        request_lease_type(http3_sans_io_session_engine& owner_value, stream_type& stream) noexcept
            : owner_(&owner_value),
              stream_(&stream) {}
        http3_sans_io_session_engine* owner_{};
        stream_type* stream_{};
    };

    enum class stream_state_type : std::uint8_t {
        receiving,
        ready,
        rejected,
    };

    struct tunnel_read_result_type final {
        std::size_t bytes_{};
        bool ended_{};
        bool reset_{};
        bool overflow_{};
    };
    enum class rejection_type : std::uint8_t {
        none,
        expectation_unsupported,
        connect_unsupported,
        streaming_unsupported,
        websocket_unsupported,
        response_stream_unsupported,
        body_too_large,
        in_flight_body_capacity,
        worker_body_budget_exhausted,
    };

    http3_sans_io_session_engine(const route_table& routes_value, worker_memory& worker_value,
        http3_sans_io_session_limits limits = {});
    // The borrowed worker budget must outlive this engine and every lease it
    // issued. It accounts bodies across sessions; per-connection limits remain
    // independently enforced by http3_sans_io_session_limits.
    http3_sans_io_session_engine(const route_table& routes_value, worker_memory& worker_value,
        http3_server_body_budget& body_budget, http3_sans_io_session_limits limits = {});
    ~http3_sans_io_session_engine();
    http3_sans_io_session_engine(const http3_sans_io_session_engine&) = delete;
    http3_sans_io_session_engine& operator=(const http3_sans_io_session_engine&) = delete;
    http3_sans_io_session_engine(http3_sans_io_session_engine&&) = delete;
    http3_sans_io_session_engine& operator=(http3_sans_io_session_engine&&) = delete;

    [[nodiscard]] http3_connection_result feed(std::uint64_t stream_id,
        std::string_view bytes, bool fin = false, bool reset = false) noexcept;
    // Only a ready, never-dispatched request may be acquired. Read-only raw queries
    // below are for synchronous inspection; asynchronous handlers hold a lease.
    [[nodiscard]] std::optional<request_lease_type> acquire_request(std::uint64_t stream_id) & noexcept;
    std::optional<request_lease_type> acquire_request(std::uint64_t stream_id) && = delete;
    [[nodiscard]] std::optional<std::uint64_t> peer_max_push_id() const noexcept {
        return connection_.peer_max_push_id();
    }
    [[nodiscard]] std::size_t max_remembered_pushes() const noexcept {
        return limits_.connection_.max_remembered_pushes_;
    }
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_connection_error_code> prepare_push_promise(
        std::uint64_t parent_stream_id, std::uint64_t push_id, http_push_request_view request) {
        return connection_.prepare_push_promise(parent_stream_id, push_id, request);
    }
    // Copies validated promise metadata into an ordinary request lease on the
    // actual server UNI stream. Prefix publication remains the driver's job.
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_connection_error_code> admit_push_stream(
        std::uint64_t stream_id, std::uint64_t push_id);
    void observe_push_cancellation(void* context, void (*cancel)(void*, std::uint64_t) noexcept);
    [[nodiscard]] const http3_server_request* request(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] const route_resolution* resolution(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] stream_state_type stream_state(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] rejection_type rejection(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] bool streaming_request(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] bool can_accept_input(std::uint64_t stream_id, std::size_t wire_bytes) const noexcept;
    [[nodiscard]] rejection_type streaming_body_failure(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] const http_request_trailers* request_trailers(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] const std::optional<http_priority>* request_priority_update(std::uint64_t stream_id) const noexcept;
    void bind_control_output_wake(void* context, void (*wake)(void*) noexcept);
    [[nodiscard]] bool queue_origin_advertisement(std::span<const std::string_view> origins);
    [[nodiscard]] std::span<const char> pending_control_output() const noexcept {
        return control_output_;
    }
    [[nodiscard]] bool consume_control_output(std::size_t bytes) noexcept;
    // Worker-PMR bounded tunnel queue. Output is copied synchronously; no
    // borrowed buffer or parser view escapes the feed callback.
    [[nodiscard]] tunnel_read_result_type read_tunnel_data(
        std::uint64_t stream_id, std::span<char> output) noexcept;
    [[nodiscard]] http3_datagram_receive_status receive_datagram(http3_datagram_view datagram);
    [[nodiscard]] std::optional<std::pmr::string> take_datagram(std::uint64_t stream_id);
    [[nodiscard]] http_datagram_session_config datagram_config(std::uint64_t stream_id) const;
    [[nodiscard]] bool tunnel_input_overflowed(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] bool tunnel_receive_ended(std::uint64_t stream_id) const noexcept;
    // Effective peer response field-section limit. nullopt means the peer has
    // not sent the setting or omitted it (RFC 9114 default: unlimited).
    [[nodiscard]] std::optional<std::uint64_t> peer_max_field_section_size() const noexcept;
    [[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_connect_response_head(std::uint64_t stream_id, const http_response& response) {
        return connection_.encode_connect_response_head(stream_id, response);
    }
    [[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_response_head(std::uint64_t stream_id, const http_response& response, http_buffered_response_write_plan plan) {
        return connection_.encode_response_head(stream_id, response, plan);
    }
    [[nodiscard]] std::variant<http3_streaming_response_head, http3_response_head_failure> encode_streaming_response_head(std::uint64_t stream_id, http_response response, http_known_method method, http_response_stream_kind kind, http_response_trailer_intent trailers) {
        return connection_.encode_streaming_response_head(stream_id, std::move(response), method, kind, trailers);
    }
    [[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_interim_response_head(std::uint64_t stream_id, const http_interim_response_head& response) {
        return connection_.encode_interim_response_head(stream_id, response);
    }
    [[nodiscard]] std::variant<http3_response_field_section, http3_response_head_failure> encode_response_trailers(std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields_value) {
        return connection_.encode_response_trailers(stream_id, fields_value);
    }
    [[nodiscard]] std::span<const char> pending_qpack_encoder_output() const noexcept {
        return connection_.pending_qpack_encoder_output();
    }
    [[nodiscard]] std::span<const char> pending_qpack_decoder_output() const noexcept {
        return connection_.pending_qpack_decoder_output();
    }
    [[nodiscard]] bool consume_qpack_encoder_output(std::size_t size) noexcept {
        return connection_.consume_qpack_encoder_output(size);
    }
    [[nodiscard]] bool consume_qpack_decoder_output(std::size_t size) noexcept {
        return connection_.consume_qpack_decoder_output(size);
    }
    [[nodiscard]] std::size_t active_stream_count() const noexcept;
    [[nodiscard]] bool terminated() const noexcept;
    [[nodiscard]] std::pmr::memory_resource* inbound_buffer_pool() noexcept {
        return &inbound_buffers_;
    }
    // Release only after validated FIN (including a rejected request), or
    // report transport RESET via feed() to retire an incomplete receive side.
    // Never silently drop the body budget for a still-delivering or leased
    // stream. stop/reset retain leased storage until its handler releases it.
    [[nodiscard]] bool release(std::uint64_t stream_id) noexcept;
    // Caller must first stop QUIC delivery for this stream. Retires local
    // receive state without fabricating a peer RESET, even before HEADERS finish.
    // A dispatch lease continues to pin its storage and body budget.
    [[nodiscard]] bool cancel_request(std::uint64_t stream_id) noexcept;
    void stop() noexcept;

private:
    struct stream_deleter_type final {
        std::pmr::memory_resource* resource_{};
        void operator()(stream_type* stream) const noexcept;
    };
    using stream_ptr_type = std::unique_ptr<stream_type, stream_deleter_type>;
    static void on_connection_event(void* context, const http3_connection_event& event);
    void handle_event(const http3_connection_event& event);
    void release_stream(std::uint64_t stream_id) noexcept;
    void release_lease(stream_type& stream) noexcept;
    void reject_body(stream_type& stream, rejection_type reason) noexcept;
    void terminate(http3_connection_result failure) noexcept;

    const route_table& routes_;
    worker_memory& worker_;
    const http3_sans_io_session_limits limits_;
    inbound_buffer_resource inbound_buffers_;
    http3_server_body_budget* body_budget_{nullptr};
    std::size_t buffered_bytes_in_flight_{0};
    std::size_t tunnel_bytes_in_flight_{0};
    std::size_t active_leases_{0};
    http3_connection connection_;
    std::pmr::string control_output_;
    void* control_wake_context_{};
    void (*control_wake_)(void*) noexcept {};
    void* push_cancellation_context_{};
    void (*push_cancellation_)(void*, std::uint64_t) noexcept {};
    std::pmr::unordered_map<std::uint64_t, stream_ptr_type> streams_;
    http3_connection_result failure_{http3_connection_status::connection_error,
        http3_connection_error_scope::connection, http3_connection_error_code::internal_error};
    bool terminated_{false};
};

}  // namespace ruvia::detail
