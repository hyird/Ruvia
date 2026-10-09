#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_response.h"

#include "http3/http3_client_body_budget.h"

namespace ruvia::detail {

// Bounded, worker-affine response retention over the sans-I/O HTTP/3 core.
// This is not a production HTTP client: it owns no Tasks, QUIC/UDP transport,
// request writer, retries, or origin/pool policy. The supplied PMR resource must
// be fixed to the owning worker and outlive this engine and all its responses.
struct http3_client_sans_io_response_limits final {
    std::size_t max_live_streams_{32};
    std::size_t max_body_bytes_per_stream_{16 * 1024 * 1024};
    std::size_t max_total_body_bytes_{64 * 1024 * 1024};
    http3_connection_config connection_{};
};

enum class http3_client_sans_io_session_status : std::uint8_t {
    need_more_data,
    qpack_blocked,
    push_promise_pending,
    message_end,
    reset,
    stream_error,
    connection_error,
    body_limit_exceeded,
    invalid_state,
    stream_limit_exceeded,
    transport_error,
    local_cancelled,
};
struct http3_client_sans_io_session_result final {
    http3_client_sans_io_session_status status_{http3_client_sans_io_session_status::need_more_data};
    http3_connection_error_scope scope_{http3_connection_error_scope::none};
    http3_connection_error_code code_{http3_connection_error_code::no_error};
    std::size_t consumed_bytes_{};
};

// Optional synchronous event sink for an incremental response owner. When set,
// body bytes are not retained by this engine. The sink must own every borrowed
// view it needs before returning, and its caller must ensure capacity for the
// entire feed() input before reading from QUIC; feed() cannot pause midway.
// Only nonterminal informational/final heads, body chunks and trailer fields
// are delivered. The caller must inspect feed()'s successful return before
// committing message end, reset or failure to a consumer. In particular,
// callback work may throw; an EOF delivered from inside the callback could
// not then be withdrawn. The callback must not call register_request(), feed(),
// cancel_request(), release(), or stop() on this engine until feed() returns.
struct http3_client_response_event_sink final {
    http3_connection_callback_type callback_{nullptr};
    void* context_{nullptr};
};

struct http3_client_sans_io_response_header final {
    std::pmr::string name_;
    std::pmr::string value_;
    explicit http3_client_sans_io_response_header(std::pmr::memory_resource* resource)
        : name_(resource),
          value_(resource) {}
};

// Only terminal snapshots are exposed; their views borrow the engine and stay
// valid until release(stream_id) or destruction. In event-sink mode body is
// delivered solely through the sink, not retained in this snapshot.
struct http3_client_sans_io_response_view final {
    std::uint16_t status_{0};
    std::optional<http_response_body_plan> response_body_plan_{};
    std::span<const http3_client_sans_io_response_header> headers_;
    std::span<const http3_client_sans_io_response_header> trailers_;
    std::span<const char> body_;
    bool complete_{false};
    bool reset_{false};
    http3_client_sans_io_session_result result_{};
};

class http3_client_sans_io_session_engine final {
public:
    using limits_type = http3_client_sans_io_response_limits;
    using result_type = http3_client_sans_io_session_result;

    explicit http3_client_sans_io_session_engine(std::pmr::memory_resource* worker_resource,
        limits_type limits = {});
    // Shared budget also accounts for results held outside this engine. It
    // must outlive the engine; event-sink storage is budgeted by its own owner.
    http3_client_sans_io_session_engine(std::pmr::memory_resource* worker_resource,
        http3_client_body_budget& body_budget, limits_type limits = {});
    ~http3_client_sans_io_session_engine();
    http3_client_sans_io_session_engine(const http3_client_sans_io_session_engine&) = delete;
    http3_client_sans_io_session_engine& operator=(const http3_client_sans_io_session_engine&) = delete;

    [[nodiscard]] result_type register_request(std::uint64_t stream_id, http_known_method method,
        http3_client_response_event_sink sink = {});
    // Feed peer control/QPACK streams as well as registered request streams.
    // A connection-scoped failure requires the QUIC owner to close transport
    // and join all socket Tasks before retiring this engine. In particular,
    // body-budget overflow fails the whole connection until per-stream local
    // receive cancellation and HTTP state retirement exist.
    [[nodiscard]] result_type feed(std::uint64_t stream_id, std::span<const char> bytes,
        bool fin = false, bool reset = false);
    [[nodiscard]] std::optional<http3_client_sans_io_response_view> response(
        std::uint64_t stream_id) const noexcept;
    // Transfers a successful buffered body's storage exactly once. The
    // returned string retains this engine's fixed worker allocator, whose
    // owner must outlive it. Existing body views are invalidated by transfer;
    // headers/trailers remain valid until release(). The receive reservation
    // is relinquished: a result owner sharing the budget must acquire its
    // reservation before admitting more input. Sink-mode responses have no
    // buffered body to transfer.
    [[nodiscard]] std::optional<std::pmr::string> take_body(std::uint64_t stream_id);
    // Explicitly relinquishes a terminal response and its accounted storage.
    [[nodiscard]] bool release(std::uint64_t stream_id) noexcept;
    // Called only after QUIC send/receive termination prevents further stream
    // delivery. This is local abandonment, not a synthetic peer RESET. The
    // terminal response remains owned until release(stream_id).
    [[nodiscard]] bool cancel_request(std::uint64_t stream_id) noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }
    [[nodiscard]] std::size_t max_live_streams() const noexcept {
        return limits_.max_live_streams_;
    }
    [[nodiscard]] std::variant<http3_client_request_head, http3_client_request_head_failure> encode_request_head(
        std::uint64_t stream_id, http3_client_request_head_view view) {
        http3_field_section_limits limits{};
        if (const auto& settings = connection_.peer_settings(); settings && settings->max_field_section_size_) {
            limits.max_decoded_bytes_ = static_cast<std::size_t>(std::min<std::uint64_t>(limits.max_decoded_bytes_, *settings->max_field_section_size_));
        }
        return connection_.encode_client_request_head(stream_id, view, limits);
    }
    [[nodiscard]] http3_settings local_settings() const noexcept {
        return connection_.local_settings();
    }
    [[nodiscard]] bool queue_priority_update(std::uint64_t stream_id, http_priority priority);
    [[nodiscard]] bool queue_max_push_id(std::uint64_t maximum);
    [[nodiscard]] bool queue_cancel_push(std::uint64_t push_id);
    [[nodiscard]] bool queue_push_priority_update(std::uint64_t push_id, http_priority priority);
    // Borrowed synchronous push events, including stream association and
    // terminal events. The driver commits terminal results after feed returns.
    void observe_pushes(http3_connection_callback_type callback_value, void* context_value) noexcept {
        push_observer_ = {.callback_ = callback_value, .context_ = context_value};
    }
    // Called after the corresponding QUIC stream can no longer deliver input.
    [[nodiscard]] bool retire_push_stream(std::uint64_t stream_id);
    void observe_origins(http3_connection_callback_type callback_value, void* context_value) noexcept {
        origin_observer_ = {.callback_ = callback_value, .context_ = context_value};
    }
    [[nodiscard]] std::span<const char> pending_control_output() const noexcept {
        return control_output_;
    }
    [[nodiscard]] bool consume_control_output(std::size_t bytes) noexcept;
    [[nodiscard]] std::span<const char> pending_encoder_output() const noexcept {
        return connection_.pending_qpack_encoder_output();
    }
    [[nodiscard]] bool consume_encoder_output(std::size_t bytes_value) noexcept {
        return connection_.consume_qpack_encoder_output(bytes_value);
    }
    [[nodiscard]] std::span<const char> pending_decoder_output() const noexcept {
        return connection_.pending_qpack_decoder_output();
    }
    [[nodiscard]] bool consume_decoder_output(std::size_t bytes_value) noexcept {
        return connection_.consume_qpack_decoder_output(bytes_value);
    }
    [[nodiscard]] std::size_t live_stream_count() const noexcept;
    [[nodiscard]] std::size_t retained_body_bytes() const noexcept;
    // The sole connection driver observes peer SETTINGS and GOAWAY before
    // admitting new requests or classifying already-open stream IDs.
    [[nodiscard]] const std::optional<http3_settings>& peer_settings() const noexcept {
        return connection_.peer_settings();
    }
    [[nodiscard]] bool peer_reports_unprocessed(std::uint64_t stream_id,
        std::optional<std::uint64_t> peer_reset_error_code = {}) const noexcept {
        return connection_.peer_reports_unprocessed(stream_id, peer_reset_error_code);
    }
    [[nodiscard]] std::optional<std::uint64_t> peer_goaway_id() const noexcept {
        return connection_.peer_goaway_id();
    }
    // Called after transport shutdown or fatal I/O to wake every incomplete
    // response. This reports a local failure, not an HTTP/3 peer error code.
    [[nodiscard]] result_type stop() noexcept;

private:
    struct stored_response_type final {
        explicit stored_response_type(std::pmr::memory_resource* resource)
            : headers_(resource),
              trailers_(resource),
              body_(resource) {}
        std::pmr::vector<http3_client_sans_io_response_header> headers_;
        std::pmr::vector<http3_client_sans_io_response_header> trailers_;
        std::pmr::string body_;
        std::uint16_t status_{0};
        std::optional<http_response_body_plan> response_body_plan_{};
        result_type result_{};
        bool complete_{false};
        bool reset_{false};
        bool final_head_seen_{false};
        bool limit_exceeded_{false};
        bool terminal_{false};
        bool body_transferred_{false};
        http3_client_response_event_sink sink_{};
    };
    static void on_event(void* context, const http3_connection_event& event);
    [[nodiscard]] result_type from_connection(http3_connection_result result) const noexcept;
    void fail_connection(result_type failure) noexcept;

    std::pmr::memory_resource* resource_;
    limits_type limits_;
    http3_client_body_budget local_body_budget_;
    http3_client_body_budget* body_budget_;
    http3_connection connection_;
    std::pmr::string control_output_;
    http3_client_response_event_sink origin_observer_{};
    http3_client_response_event_sink push_observer_{};
    std::pmr::unordered_map<std::uint64_t, stored_response_type> responses_;
    std::size_t retained_body_bytes_{0};
    bool feeding_{};
    result_type connection_failure_{};
};

}  // namespace ruvia::detail
