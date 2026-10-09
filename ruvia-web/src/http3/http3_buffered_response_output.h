#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http3_buffered_response_cursor.h"
#include "ruvia/http/http_response.h"

#include "http3/http3_stream_buffer.h"

namespace ruvia::detail {

enum class http3_buffered_response_output_error : std::uint8_t {
    none,
    invalid_response_plan,
    file_body_unsupported,
    response_encoding,
    out_of_memory,
    invalid_cursor_state,
    cursor_acknowledgement,
    cursor_data_plan,
    peer_field_section_limit,
    buffer_stopped,
    wire_byte_count_overflow,
    stopped,
};

// Worker-affine publisher for one buffered response. The caller owns and must
// retain response until this output completes or fails. worker_memory and the
// buffer must also outlive it. The output owns only the cursor and copies its
// wire bytes into the buffer. It does not drive QUIC:
// partial/WANT handling remains the responsibility of http3_server_stream_output.
class http3_buffered_response_output final {
public:
    using error_type = http3_buffered_response_output_error;
    using message_id_type = http3_stream_id;
    using next_step_type = ruvia::http3_buffered_response_cursor::step;

    enum class status_type : std::uint8_t {
        bytes,
        fin,
        backpressured,
        complete,
        failed,
    };

    enum class block_reason_type : std::uint8_t {
        none,
        data,
        control,
    };

    struct result_type final {
        status_type status_{status_type::failed};
        block_reason_type block_reason_{block_reason_type::none};
        error_type error_{error_type::none};
        std::size_t bytes_accepted_{};
        std::uint64_t published_wire_bytes_{};
    };

    // peer_max_field_section_size is the peer's effective advisory limit, if known.
    // An over-limit response is rejected before any HEADERS bytes can be handed
    // to the buffer. The plan should come from plan_buffered_http_response_write().
    [[nodiscard]] static std::variant<http3_buffered_response_output, error_type> create(
        const http_response& response, const http_buffered_response_write_plan& write_plan,
        worker_memory& worker, http3_stream_buffer& buffer, message_id_type message_id,
        std::optional<std::uint64_t> peer_max_field_section_size = std::nullopt, std::uint64_t initial_published_wire_bytes = 0) noexcept;

    [[nodiscard]] static std::variant<http3_buffered_response_output, error_type> create(
        const http_response& response, const http_buffered_response_write_plan& write_plan, http3_response_head encoded_head,
        worker_memory& worker, http3_stream_buffer& buffer, message_id_type message_id,
        std::optional<std::uint64_t> peer_max_field_section_size = std::nullopt, std::uint64_t initial_published_wire_bytes = 0) noexcept;

    http3_buffered_response_output(const http3_buffered_response_output&) = delete;
    http3_buffered_response_output& operator=(const http3_buffered_response_output&) = delete;
    // As with the cursor, moving is invalid while a published segment is awaiting
    // its acknowledgement. Normally owners construct output items in place.
    http3_buffered_response_output(http3_buffered_response_output&&) = default;
    http3_buffered_response_output& operator=(http3_buffered_response_output&&) = delete;

    // One call publishes at most one clipped DATA block or one FIN control.
    // complete means the FIN was accepted by the buffer, not written to or
    // acknowledged by the QUIC peer. Backpressure never acknowledges the cursor.
    [[nodiscard]] result_type publish_step() noexcept;

    // Terminally abandon unpublished cursor state. Already accepted buffer
    // bytes remain owned by the buffer until the consumer releases them.
    void stop() noexcept;

    [[nodiscard]] next_step_type next_step() const noexcept;
    [[nodiscard]] std::size_t decoded_field_section_size() const noexcept;
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    // Cumulative HTTP/3 wire bytes accepted by the buffer, including frame bytes.
    [[nodiscard]] std::uint64_t published_wire_bytes() const noexcept;
    [[nodiscard]] const message_id_type& message_id() const noexcept {
        return message_id_;
    }

private:
    enum class state_type : std::uint8_t { publishing,
        complete,
        failed };

    http3_buffered_response_output(const http_response& response,
        http3_stream_buffer& buffer, message_id_type message_id,
        ruvia::http3_buffered_response_cursor cursor_value, std::uint64_t initial_published_wire_bytes) noexcept;

    [[nodiscard]] static error_type cursor_error(ruvia::http3_buffered_response_cursor::error error) noexcept;
    [[nodiscard]] result_type fail(error_type error, std::size_t bytes_accepted = 0) noexcept;
    [[nodiscard]] result_type result(status_type status, block_reason_type block_reason = block_reason_type::none,
        error_type error = error_type::none, std::size_t bytes_accepted = 0) const noexcept;

    const http_response* response_{};
    http3_stream_buffer& buffer_;
    const message_id_type message_id_;
    std::optional<ruvia::http3_buffered_response_cursor> cursor_;
    std::uint64_t published_wire_bytes_{};
    state_type state_{state_type::publishing};
    error_type failure_{error_type::none};
};

}  // namespace ruvia::detail
