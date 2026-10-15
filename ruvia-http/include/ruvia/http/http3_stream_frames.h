#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include "ruvia/http/http3_connection_error.h"

namespace ruvia {

enum class http3_stream_kind : std::uint8_t {
    request,
    response,
    control,
};

enum class http3_stream_frame_status : std::uint8_t {
    need_more_data,
    paused,
    message_end,
    frame_unexpected,
    push_promise,
    missing_settings,
    frame_error,
    closed_critical_stream,
    limit,
};

// Returns no value for non-error framing progress/end statuses. PUSH_PROMISE
// maps to FRAME_UNEXPECTED by default; callers may apply a narrower
// context-specific code when push authorization semantics require it.
[[nodiscard]] std::optional<http3_connection_error_code>
http3_connection_error_code_for_stream_frame_status(http3_stream_frame_status status) noexcept;

enum class http3_stream_frame_event_kind : std::uint8_t {
    headers,
    push_promise,
    data,
    settings,
    cancel_push,
    goaway,
    max_push_id,
    request_priority_update,
    push_priority_update,
    origin,
};

struct http3_stream_frame_event final {
    http3_stream_frame_event_kind kind_{http3_stream_frame_event_kind::data};
    std::span<const char> payload_{};
    bool trailers_{false};
    bool end_frame_{false};
    bool fin_{false};
};

using http3_stream_frame_callback_type = void (*)(void*, http3_stream_frame_event);

struct http3_stream_frames_config final {
    std::size_t max_field_section_size_{64 * 1024};
    std::size_t max_settings_payload_bytes_{64 * 1024};
    bool allow_push_{false};
};

// Incrementally decodes one HTTP/3 message direction or control stream. A
// response can have multiple informational HEADERS; its caller must validate
// the decoded status sequence and distinguish final headers from trailers.
// Callback views
// borrow the current feed input, except HEADERS which borrow decoder-owned
// storage; all views are valid only until the callback returns. The supplied
// memory resource must outlive this decoder.
class http3_stream_frames final {
public:
    http3_stream_frames(http3_stream_kind kind, std::pmr::memory_resource* resource,
        http3_stream_frames_config config = {}) noexcept;

    [[nodiscard]] http3_stream_frame_status feed(std::span<const char> input, bool fin,
        http3_stream_frame_callback_type callback, void* context);

    // Called by a HEADERS callback to retain the complete section and stop
    // consuming input. The next feed retries that callback before new bytes.
    void allow_push() noexcept {
        config_.allow_push_ = true;
    }
    void pause() noexcept {
        paused_ = true;
    }
    [[nodiscard]] bool paused() const noexcept {
        return paused_;
    }
    [[nodiscard]] std::size_t consumed_bytes() const noexcept {
        return consumed_;
    }

    [[nodiscard]] const http3_stream_frames_config& config() const noexcept {
        return config_;
    }

private:
    enum class phase_type : std::uint8_t {
        frame_header,
        frame_payload,
        failed,
        ended,
    };

    [[nodiscard]] http3_stream_frame_status begin_frame(std::uint64_t type, std::uint64_t length);
    [[nodiscard]] std::size_t field_section_buffer_limit() const noexcept;
    http3_stream_frame_status finish_frame() noexcept;

    http3_stream_kind kind_;
    http3_stream_frames_config config_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<char> field_section_;
    phase_type phase_{phase_type::frame_header};
    std::uint64_t frame_type_{0};
    std::uint64_t frame_length_{0};
    std::uint64_t remaining_{0};
    std::size_t header_bytes_used_{0};
    char header_[16]{};
    bool paused_{false};
    std::size_t consumed_{0};
    bool first_frame_{true};
    bool headers_seen_{false};
    bool trailers_seen_{false};
    bool settings_seen_{false};
};

}  // namespace ruvia
