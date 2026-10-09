#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/Http3DataWritePlan.h"
#include "ruvia/http/Http3ResponseWriter.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

enum class http3_buffered_response_error : std::uint8_t {
    invalid_response_plan,
    file_body_unsupported,
    response_encoding,
    out_of_memory,
    invalid_state,
    excessive_acknowledgement,
    data_plan,
};

// Sans-I/O cursor over a buffered response. Owns encoded headers in resource
// (nullptr selects the default resource) and borrows the response body. The
// resource and unchanged response body must outlive the cursor. Returned spans
// remain stable until acknowledged; a zero-byte acknowledgement changes nothing.
// Destroying the cursor releases its headers without committing pending output.
// Failures are returned as data for transport callbacks that must not unwind;
// the caller decides whether to retry, reset the stream or close the transport.
class http3_buffered_response_cursor final {
public:
    using error = http3_buffered_response_error;
    using segment = std::span<const char>;

    enum class step : std::uint8_t { bytes,
        fin,
        complete,
        failed };

    [[nodiscard]] static std::variant<http3_buffered_response_cursor, error> create(
        const HttpResponse& response, const HttpBufferedResponseWritePlan& write_plan,
        std::pmr::memory_resource* resource) noexcept;

    // encoded_head must describe the same response and write plan. Its storage
    // is consumed during creation; the cursor retains no borrow from it.
    [[nodiscard]] static std::variant<http3_buffered_response_cursor, error> create(
        const HttpResponse& response, const HttpBufferedResponseWritePlan& write_plan,
        Http3ResponseHead encoded_head, std::pmr::memory_resource* resource) noexcept;

    http3_buffered_response_cursor(const http3_buffered_response_cursor&) = delete;
    http3_buffered_response_cursor& operator=(const http3_buffered_response_cursor&) = delete;
    // Moving is allowed only when no offered span is awaiting acknowledgement.
    http3_buffered_response_cursor(http3_buffered_response_cursor&& other);
    http3_buffered_response_cursor& operator=(http3_buffered_response_cursor&&) = delete;

    // Ordered output: one complete HEADERS frame, then zero or more DATA frame
    // headers and borrowed payload spans. An empty segment means FIN is ready.
    [[nodiscard]] std::variant<segment, error> next() noexcept;
    // Pure query: does not plan, offer, acknowledge, allocate, free or mutate
    // any cursor state.
    // Each next DATA chunk is planned when the preceding segment is acknowledged,
    // so headers-only output reports FIN immediately after its HEADERS ack.
    [[nodiscard]] step next_step() const noexcept;
    [[nodiscard]] std::variant<std::monostate, error> acknowledge(std::size_t count) noexcept;
    // FIN is a separate transport operation. Only acknowledge_fin(true) commits
    // the protocol plan; false terminates the cursor without claiming success.
    [[nodiscard]] std::variant<std::monostate, error> acknowledge_fin(bool successful) noexcept;
    [[nodiscard]] bool fin_ready() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    // RFC 9114 decoded field-list size, including :status and projected fields.
    [[nodiscard]] std::size_t decoded_field_section_size() const noexcept {
        return decoded_field_section_size_;
    }

private:
    enum class state : std::uint8_t { headers,
        data_header,
        data_body,
        fin,
        finished,
        failed };

    explicit http3_buffered_response_cursor(std::pmr::memory_resource* resource);
    [[nodiscard]] std::variant<std::monostate, error> prepare_data() noexcept;
    [[nodiscard]] segment active_segment() const noexcept;
    [[nodiscard]] std::variant<std::monostate, error> fail_data_plan() noexcept;
    [[nodiscard]] static http3_buffered_response_cursor& require_no_outstanding_segment(
        http3_buffered_response_cursor& other);

    std::pmr::vector<char> headers_;
    std::size_t decoded_field_section_size_{0};
    std::string_view body_;
    std::optional<Http3DataWritePlan> data_plan_;
    Http3DataWritePlan::Chunk chunk_{};
    std::size_t segment_offset_{0};
    std::size_t body_offset_{0};
    state state_{state::headers};
    bool chunk_pending_{false};
    bool offered_{false};
};

}  // namespace ruvia
