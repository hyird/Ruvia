#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http_response.h"

namespace ruvia {

enum class http3_data_write_error : std::uint8_t {
    frame_header_encoding,
    var_int_out_of_range,
    content_length_overflow,
    content_length_mismatch,
    content_length_forbidden,
    body_not_allowed,
    write_already_pending,
    no_write_pending,
    commit_does_not_match_plan,
    already_finished,
};

// Pure protocol state for planning DATA frame writes for responses and outbound
// requests. It owns no payload and performs no allocation; all state is stored
// inline and needs no PMR resource. Destroying a plan never commits pending
// bytes or implicitly finishes the stream.
class http3_data_write_plan final {
public:
    struct chunk_type final {
        std::array<char, http3_frame_header_max_bytes> frame_header_{};
        std::size_t frame_header_size_{0};
        std::span<const char> payload_{};  // Borrowed from the caller.
        bool emits_data_{false};
        bool finishing_{false};
    };

    http3_data_write_plan(http_response_body_plan body_plan,
        std::optional<std::uint64_t> declared_content_length) noexcept;
    // Request bodies are not subject to response status/method body rules.
    explicit http3_data_write_plan(http3_client_request_body_plan body_plan) noexcept;

    [[nodiscard]] bool body_allowed() const noexcept;
    // Whether the stream may be concluded now without sending more DATA.
    [[nodiscard]] bool fin_allowed() const noexcept;
    [[nodiscard]] std::uint64_t committed_payload_bytes() const noexcept;
    [[nodiscard]] bool finished() const noexcept;

    // The returned payload view borrows the caller's chunk. The runtime must
    // retain that chunk until every partial SSL_write_ex write has completed.
    // To end a headers-only body, plan an empty finishing chunk and commit it;
    // no zero-length DATA frame is emitted.
    [[nodiscard]] std::variant<chunk_type, http3_data_write_error> plan_chunk(
        std::span<const char> payload, bool finishing) noexcept;

    // Call only after the planned frame header and all payload bytes have been
    // fully written. The runtime must signal FIN with SSL_stream_conclude or
    // SSL_write_ex2; on transport failure do not commit and RESET_STREAM instead.
    [[nodiscard]] std::variant<std::monostate, http3_data_write_error> commit_payload(
        std::uint64_t bytes, bool finishing) noexcept;

private:
    std::optional<http_response_body_plan> response_body_plan_;
    std::optional<std::uint64_t> declared_content_length_;
    bool request_body_{false};
    std::uint64_t committed_payload_bytes_{0};
    std::uint64_t pending_payload_bytes_{0};
    bool pending_finishing_{false};
    bool write_pending_{false};
    bool finished_{false};
};

}  // namespace ruvia
