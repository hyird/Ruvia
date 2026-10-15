#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http3_buffered_response_cursor.h"
#include "ruvia/http/http_response.h"

#include "http3/http3_server_stream_output.h"

namespace ruvia::detail {

// Worker-affine publisher for one buffered response. The caller owns and must
// retain response until this output completes or fails; the stream output must
// also outlive it. Each step writes the cursor's current segment straight to
// QUIC (which copies the accepted prefix) and acknowledges exactly the accepted
// bytes; QUIC stream flow control is the only backpressure.
class http3_buffered_response_output final {
public:
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
    };

    // The plan should come from plan_buffered_http_response_write(). Failure
    // reports the cursor's preparation error.
    [[nodiscard]] static std::variant<http3_buffered_response_output, http3_buffered_response_error> create(
        const http_response& response, const http_buffered_response_write_plan& write_plan,
        worker_memory& worker, http3_server_stream_output& output, std::uint64_t stream_id) noexcept;

    [[nodiscard]] static std::variant<http3_buffered_response_output, http3_buffered_response_error> create(
        const http_response& response, const http_buffered_response_write_plan& write_plan,
        http3_response_head encoded_head, http3_server_stream_output& output, std::uint64_t stream_id) noexcept;

    http3_buffered_response_output(const http3_buffered_response_output&) = delete;
    http3_buffered_response_output& operator=(const http3_buffered_response_output&) = delete;
    // As with the cursor, moving is invalid while a segment is partially
    // accepted. Normally owners construct output items in place.
    http3_buffered_response_output(http3_buffered_response_output&&) = default;
    http3_buffered_response_output& operator=(http3_buffered_response_output&&) = delete;

    // One call writes at most one cursor segment or the FIN. complete means QUIC
    // accepted the FIN, not that the peer received it. A blocked or closed
    // stream never acknowledges the cursor.
    [[nodiscard]] result_type publish_step() noexcept;

    // Terminally abandon unpublished cursor state.
    void stop() noexcept;

    [[nodiscard]] next_step_type next_step() const noexcept;
    [[nodiscard]] std::size_t decoded_field_section_size() const noexcept;

private:
    enum class state_type : std::uint8_t { publishing,
        complete,
        failed };

    http3_buffered_response_output(const http_response& response, http3_server_stream_output& output,
        std::uint64_t stream_id, ruvia::http3_buffered_response_cursor cursor_value) noexcept;

    [[nodiscard]] result_type fail() noexcept;

    const http_response* response_{};
    http3_server_stream_output* output_;
    std::uint64_t stream_id_;
    std::optional<ruvia::http3_buffered_response_cursor> cursor_;
    state_type state_{state_type::publishing};
};

}  // namespace ruvia::detail
