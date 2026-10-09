#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_message_body.h"
#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/http_response.h"

namespace ruvia {

enum class http3_client_response_event_kind : std::uint8_t {
    push_promise,
    informational_head,
    final_head,
    body,
    tunnel_data,
    trailer_field,
    message_end,
    reset,
};

struct http3_client_response_event final {
    http3_client_response_event_kind kind_{http3_client_response_event_kind::body};
    std::uint64_t stream_id_{0};
    const http3_message_head* head_{nullptr};
    http3_field_section_field_view trailer_{};
    std::span<const char> body_{};
    // Final-head and message-end events carry the same value. Informational and
    // all other events leave it empty; unlike head/body views, it is not borrowed.
    std::optional<http_response_body_plan> response_body_plan_{};
    std::optional<std::uint64_t> push_id_{};
    std::optional<http_client_request_content_signal> request_content_signal_{};
};

using http3_client_response_callback_type = void (*)(void*, const http3_client_response_event&);

enum class http3_client_response_status : std::uint8_t {
    need_more_data,
    qpack_blocked,
    message_end,
    reset,
    stream_error,
    connection_error,
};

struct http3_client_response_result final {
    http3_client_response_status status_{http3_client_response_status::need_more_data};
    http3_connection_error_scope scope_{http3_connection_error_scope::none};
    http3_connection_error_code code_{http3_connection_error_code::no_error};
    std::size_t consumed_bytes_{0};
};

struct http3_client_response_limits final {
    std::size_t max_field_section_size_{64 * 1024};
    std::size_t max_fields_{256};
    std::size_t max_encoded_field_section_bytes_{64 * 1024};
    std::optional<std::uint64_t> max_push_id_{};
    bool push_stream_{false};
};

// Sans-I/O receive state for one locally initiated bidirectional request stream.
// All decoded head storage and frame buffering belong to resource. Callback views
// are synchronous-only; head/trailer and body/tunnel-data storage is borrowed and
// valid only during the callback. A successful CONNECT response emits tunnel DATA
// separately and completes on stream FIN; trailers are forbidden. Callbacks must
// not reenter, move, or destroy this object. If a callback throws, the stream is
// terminal and cannot be resumed.
// A non-null decoder is borrowed for this response's entire lifetime. On
// qpack_blocked, retain bytes after consumed_bytes and repeat feed with that
// suffix (and FIN) after delivering encoder instructions to the shared decoder.
class http3_client_response final {
public:
    http3_client_response(std::uint64_t stream_id, http_known_method request_method,
        std::pmr::memory_resource* resource, http3_client_response_limits limits = {},
        http3_qpack_decoder* decoder = nullptr);
    ~http3_client_response();
    http3_client_response(http3_client_response&&) noexcept;
    http3_client_response& operator=(http3_client_response&&) noexcept;
    http3_client_response(const http3_client_response&) = delete;
    http3_client_response& operator=(const http3_client_response&) = delete;

    [[nodiscard]] http3_client_response_result feed(std::span<const char> bytes, bool fin, bool reset,
        http3_client_response_callback_type callback, void* context);
    [[nodiscard]] std::uint64_t stream_id() const noexcept;
    [[nodiscard]] bool authorize_push(std::uint64_t maximum) noexcept;

private:
    struct impl_type;
    std::pmr::memory_resource* resource_;
    impl_type* impl_;
};

}  // namespace ruvia
