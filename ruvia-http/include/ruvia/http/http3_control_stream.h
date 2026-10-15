#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include "ruvia/http/http3_settings.h"
#include "ruvia/http/http3_stream_frames.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_priority.h"

namespace ruvia {

enum class http3_control_role : std::uint8_t { client,
    server };
enum class http3_control_stream_status : std::uint8_t {
    need_more_data,
    closed_critical_stream,
    missing_settings,
    frame_unexpected,
    settings_error,
    id_error,
    frame_error,
    limit,
};

struct http3_control_stream_event final {
    http3_stream_frame_event_kind kind_;
    std::uint64_t id_;
    std::optional<http_priority_update> priority_update_{};
    const http_origin_advertisement* origin_advertisement_{nullptr};
};
using http3_control_stream_callback_type = void (*)(void*, http3_control_stream_event);

// Incrementally consumes a peer's control stream. All retained storage uses the
// caller-provided resource, which must outlive this object.
class http3_control_stream final {
public:
    http3_control_stream(http3_control_role role, std::pmr::memory_resource* resource,
        http3_stream_frames_config config = {}) noexcept;

    [[nodiscard]] http3_control_stream_status feed(std::span<const char> input, bool fin,
        http3_control_stream_callback_type callback = nullptr, void* context = nullptr);
    [[nodiscard]] const std::optional<http3_settings>& peer_settings() const noexcept {
        return settings_;
    }
    [[nodiscard]] std::optional<std::uint64_t> goaway_id() const noexcept {
        return goaway_id_;
    }
    [[nodiscard]] std::optional<std::uint64_t> max_push_id() const noexcept {
        return max_push_id_;
    }
    [[nodiscard]] std::optional<std::uint64_t> last_cancel_push_id() const noexcept {
        return cancel_push_id_;
    }

private:
    static void on_frame(void* context, http3_stream_frame_event event);
    void consume(http3_stream_frame_event event);

    http3_control_role role_;
    std::pmr::memory_resource* resource_;
    http3_stream_frames frames_;
    std::pmr::vector<char> settings_payload_;
    std::optional<http3_settings> settings_;
    std::optional<std::uint64_t> goaway_id_;
    std::optional<std::uint64_t> max_push_id_;
    std::optional<std::uint64_t> cancel_push_id_;
    std::array<char, 8> fixed_payload_{};
    std::size_t fixed_payload_size_{0};
    http3_stream_frame_event_kind fixed_kind_{http3_stream_frame_event_kind::data};
    http3_control_stream_callback_type callback_{nullptr};
    void* context_{nullptr};
    bool feeding_{false};
    http3_control_stream_status error_{http3_control_stream_status::need_more_data};
};

}  // namespace ruvia
