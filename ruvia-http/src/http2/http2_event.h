#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_push.h"

#include "http2/http2_frame_types.h"
#include "http2/http2_stream_close_source.h"

namespace ruvia::detail {

class http2_connection;
class http2_event;

// The latest GOAWAY received from the peer. Last-Stream-ID is a connection-level
// processing boundary, not the identifier of an event stream.
class http2_peer_goaway final {
public:
    constexpr http2_peer_goaway(std::uint32_t last_stream_id, http2_error_code error) noexcept
        : last_stream_id_(last_stream_id),
          error_(error) {}

    [[nodiscard]] constexpr std::uint32_t last_stream_id() const noexcept {
        return last_stream_id_;
    }

    [[nodiscard]] constexpr http2_error_code error() const noexcept {
        return error_;
    }

private:
    std::uint32_t last_stream_id_{0};
    http2_error_code error_{http2_error_code::no_error};
};

enum class http2_event_kind : std::uint8_t {
    informational_head,
    message_head,
    message_body_chunk,
    message_end,
    tunnel_data,
    tunnel_end,
    stream_closed,
    request_unprocessed,
    goaway,
    push_promise,
    priority_update,
    origin_advertisement,
    alternative_service_advertisement
};

class http2_informational_head_event final {
public:
    [[nodiscard]] constexpr std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

    [[nodiscard]] const http_client_response_head& head() const& noexcept {
        return head_;
    }
    [[nodiscard]] const http_client_response_head& head() const&& = delete;

    [[nodiscard]] http_client_response_head take_head() && noexcept {
        return std::move(head_);
    }

    [[nodiscard]] constexpr std::optional<http_client_request_content_signal> request_content_signal()
        const noexcept {
        return request_content_signal_;
    }

private:
    friend class http2_event;
    http2_informational_head_event(std::uint32_t stream_id, http_client_response_head head,
        std::optional<http_client_request_content_signal> request_content_signal) noexcept
        : stream_id_(stream_id),
          head_(std::move(head)),
          request_content_signal_(request_content_signal) {}

    std::uint32_t stream_id_{0};
    http_client_response_head head_;
    std::optional<http_client_request_content_signal> request_content_signal_;
};

class http2_message_head_event final {
public:
    [[nodiscard]] constexpr std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

    [[nodiscard]] constexpr std::optional<http_client_request_content_signal> request_content_signal()
        const noexcept {
        return request_content_signal_;
    }

private:
    friend class http2_event;
    explicit constexpr http2_message_head_event(std::uint32_t stream_id,
        std::optional<http_client_request_content_signal> request_content_signal) noexcept
        : stream_id_(stream_id),
          request_content_signal_(request_content_signal) {}
    std::uint32_t stream_id_{0};
    std::optional<http_client_request_content_signal> request_content_signal_;
};

class http2_message_body_chunk_event final {
public:
    [[nodiscard]] constexpr std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

    // This view remains valid until the next feed() call that consumes input.
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] constexpr std::uint32_t flow_control_bytes() const noexcept {
        return flow_control_bytes_;
    }

private:
    friend class http2_event;
    constexpr http2_message_body_chunk_event(
        std::uint32_t stream_id, std::string_view bytes_value, std::uint32_t flow_control_bytes) noexcept
        : stream_id_(stream_id),
          bytes_(bytes_value),
          flow_control_bytes_(flow_control_bytes) {}
    std::uint32_t stream_id_{0};
    std::string_view bytes_{};
    std::uint32_t flow_control_bytes_{0};
};

class http2_message_end_event final {
public:
    [[nodiscard]] constexpr std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

private:
    friend class http2_event;
    explicit constexpr http2_message_end_event(std::uint32_t stream_id) noexcept
        : stream_id_(stream_id) {}
    std::uint32_t stream_id_{0};
};

class http2_tunnel_data_event final {
public:
    [[nodiscard]] constexpr std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

    // This view remains valid until the next feed() call that consumes input.
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] constexpr std::uint32_t flow_control_bytes() const noexcept {
        return flow_control_bytes_;
    }

private:
    friend class http2_event;
    constexpr http2_tunnel_data_event(
        std::uint32_t stream_id, std::string_view bytes_value, std::uint32_t flow_control_bytes) noexcept
        : stream_id_(stream_id),
          bytes_(bytes_value),
          flow_control_bytes_(flow_control_bytes) {}
    std::uint32_t stream_id_{0};
    std::string_view bytes_{};
    std::uint32_t flow_control_bytes_{0};
};

class http2_tunnel_end_event final {
public:
    [[nodiscard]] constexpr std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

private:
    friend class http2_event;
    explicit constexpr http2_tunnel_end_event(std::uint32_t stream_id) noexcept
        : stream_id_(stream_id) {}
    std::uint32_t stream_id_{0};
};

class http2_stream_closed_event final {
public:
    [[nodiscard]] constexpr std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

    [[nodiscard]] constexpr http2_stream_close_source source() const noexcept {
        return source_;
    }

    // RFC 9113 section 6.4 makes this the reason carried by RST_STREAM.
    [[nodiscard]] constexpr http2_error_code error() const noexcept {
        return error_;
    }

private:
    friend class http2_event;
    constexpr http2_stream_closed_event(
        std::uint32_t stream_id, http2_stream_close_source source_value, http2_error_code error) noexcept
        : stream_id_(stream_id),
          source_(source_value),
          error_(error) {}
    std::uint32_t stream_id_{0};
    http2_stream_close_source source_;
    http2_error_code error_{http2_error_code::no_error};
};

// A client request above the peer GOAWAY boundary was not processed and is safe
// to retry. The GOAWAY error remains connection metadata on http2_goaway_event and
// http2_connection::peer_goaway(); it is not duplicated into each request event.
class http2_request_unprocessed_event final {
public:
    [[nodiscard]] constexpr std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

private:
    friend class http2_event;
    explicit constexpr http2_request_unprocessed_event(std::uint32_t stream_id) noexcept
        : stream_id_(stream_id) {}
    std::uint32_t stream_id_{0};
};

class http2_goaway_event final {
public:
    [[nodiscard]] constexpr std::uint32_t last_stream_id() const noexcept {
        return peer_goaway_.last_stream_id();
    }

    [[nodiscard]] constexpr http2_error_code error() const noexcept {
        return peer_goaway_.error();
    }

    [[nodiscard]] constexpr const http2_peer_goaway& peer_goaway() const& noexcept {
        return peer_goaway_;
    }
    [[nodiscard]] constexpr const http2_peer_goaway& peer_goaway() const&& = delete;

private:
    friend class http2_event;
    explicit constexpr http2_goaway_event(http2_peer_goaway peer_goaway) noexcept
        : peer_goaway_(peer_goaway) {}
    http2_peer_goaway peer_goaway_;
};

// A zero-allocation discriminated event. There is deliberately no none
// alternative: next_event() uses std::optional to represent an empty queue, so
// every materialized http2_event has exactly one valid payload.
class http2_event final {
public:
    http2_event(const http2_event&) = delete;
    http2_event& operator=(const http2_event&) = delete;
    http2_event(http2_event&&) noexcept = default;
    http2_event& operator=(http2_event&&) = delete;

    [[nodiscard]] bool references_stream(std::uint32_t stream_id) const noexcept {
        return std::visit(
            [stream_id](const auto& event) noexcept {
                if constexpr (requires { event.stream_id(); }) {
                    return event.stream_id() == stream_id;
                } else if constexpr (std::is_same_v<std::decay_t<decltype(event)>, http2_push_promise_event>) {
                    return event.promised_stream_id_ == stream_id || event.associated_stream_id_ == stream_id;
                } else {
                    return false;
                }
            },
            value_);
    }

    [[nodiscard]] http2_event_kind kind() const noexcept {
        return static_cast<http2_event_kind>(value_.index());
    }

    [[nodiscard]] http2_informational_head_event* informational_head() & noexcept {
        return std::get_if<http2_informational_head_event>(&value_);
    }
    [[nodiscard]] const http2_informational_head_event* informational_head() const& noexcept {
        return std::get_if<http2_informational_head_event>(&value_);
    }
    [[nodiscard]] const http2_informational_head_event* informational_head() const&& = delete;

    [[nodiscard]] const http2_message_head_event* message_head() const& noexcept {
        return std::get_if<http2_message_head_event>(&value_);
    }
    [[nodiscard]] const http2_message_head_event* message_head() const&& = delete;

    [[nodiscard]] const http2_message_body_chunk_event* message_body_chunk() const& noexcept {
        return std::get_if<http2_message_body_chunk_event>(&value_);
    }
    [[nodiscard]] const http2_message_body_chunk_event* message_body_chunk() const&& = delete;

    [[nodiscard]] const http2_message_end_event* message_end() const& noexcept {
        return std::get_if<http2_message_end_event>(&value_);
    }
    [[nodiscard]] const http2_message_end_event* message_end() const&& = delete;

    [[nodiscard]] const http2_tunnel_data_event* tunnel_data() const& noexcept {
        return std::get_if<http2_tunnel_data_event>(&value_);
    }
    [[nodiscard]] const http2_tunnel_data_event* tunnel_data() const&& = delete;

    [[nodiscard]] const http2_tunnel_end_event* tunnel_end() const& noexcept {
        return std::get_if<http2_tunnel_end_event>(&value_);
    }
    [[nodiscard]] const http2_tunnel_end_event* tunnel_end() const&& = delete;

    [[nodiscard]] const http2_stream_closed_event* stream_closed() const& noexcept {
        return std::get_if<http2_stream_closed_event>(&value_);
    }
    [[nodiscard]] const http2_stream_closed_event* stream_closed() const&& = delete;

    [[nodiscard]] const http2_request_unprocessed_event* request_unprocessed() const& noexcept {
        return std::get_if<http2_request_unprocessed_event>(&value_);
    }
    [[nodiscard]] const http2_request_unprocessed_event* request_unprocessed() const&& = delete;

    [[nodiscard]] const http2_goaway_event* goaway() const& noexcept {
        return std::get_if<http2_goaway_event>(&value_);
    }
    [[nodiscard]] const http2_goaway_event* goaway() const&& = delete;

    [[nodiscard]] http2_push_promise_event* push_promise() & noexcept {
        return std::get_if<http2_push_promise_event>(&value_);
    }
    [[nodiscard]] http_origin_advertisement* origin_advertisement() & noexcept {
        return std::get_if<http_origin_advertisement>(&value_);
    }
    [[nodiscard]] const http_origin_advertisement* origin_advertisement() const& noexcept {
        return std::get_if<http_origin_advertisement>(&value_);
    }
    const http_origin_advertisement* origin_advertisement() const&& = delete;
    [[nodiscard]] http_alternative_service_advertisement* alternative_service_advertisement() & noexcept {
        return std::get_if<http_alternative_service_advertisement>(&value_);
    }
    [[nodiscard]] const http_alternative_service_advertisement* alternative_service_advertisement() const& noexcept {
        return std::get_if<http_alternative_service_advertisement>(&value_);
    }
    const http_alternative_service_advertisement* alternative_service_advertisement() const&& = delete;
    [[nodiscard]] const http_priority_update* priority_update() const& noexcept {
        return std::get_if<http_priority_update>(&value_);
    }
    const http_priority_update* priority_update() const&& = delete;

    [[nodiscard]] const http2_push_promise_event* push_promise() const& noexcept {
        return std::get_if<http2_push_promise_event>(&value_);
    }
    const http2_push_promise_event* push_promise() const&& = delete;

private:
    friend class http2_connection;

    using value_type = std::variant<http2_informational_head_event, http2_message_head_event,
        http2_message_body_chunk_event, http2_message_end_event, http2_tunnel_data_event, http2_tunnel_end_event,
        http2_stream_closed_event, http2_request_unprocessed_event, http2_goaway_event, http2_push_promise_event, http_priority_update, http_origin_advertisement, http_alternative_service_advertisement>;

    static_assert(
        static_cast<std::uint8_t>(http2_event_kind::alternative_service_advertisement) + 1 == std::variant_size_v<value_type>);

    template <typename event_type>
    explicit http2_event(event_type event) noexcept
        : value_(std::move(event)) {}

    [[nodiscard]] static http2_event informational_head(std::uint32_t stream_id,
        http_client_response_head head,
        std::optional<http_client_request_content_signal> request_content_signal) noexcept {
        return http2_event(
            http2_informational_head_event(stream_id, std::move(head), request_content_signal));
    }

    [[nodiscard]] static http2_event message_head(
        std::uint32_t stream_id, std::optional<http_client_request_content_signal> request_content_signal =
                                     std::nullopt) noexcept {
        return http2_event(http2_message_head_event(stream_id, request_content_signal));
    }

    [[nodiscard]] static http2_event message_body_chunk(
        std::uint32_t stream_id, std::string_view bytes_value, std::uint32_t flow_control_bytes) noexcept {
        return http2_event(http2_message_body_chunk_event(stream_id, bytes_value, flow_control_bytes));
    }

    [[nodiscard]] static http2_event message_end(std::uint32_t stream_id) noexcept {
        return http2_event(http2_message_end_event(stream_id));
    }

    [[nodiscard]] static http2_event tunnel_data(
        std::uint32_t stream_id, std::string_view bytes_value, std::uint32_t flow_control_bytes) noexcept {
        return http2_event(http2_tunnel_data_event(stream_id, bytes_value, flow_control_bytes));
    }

    [[nodiscard]] static http2_event tunnel_end(std::uint32_t stream_id) noexcept {
        return http2_event(http2_tunnel_end_event(stream_id));
    }

    [[nodiscard]] static http2_event stream_closed(
        std::uint32_t stream_id, http2_stream_close_source source_value, http2_error_code error) noexcept {
        return http2_event(http2_stream_closed_event(stream_id, source_value, error));
    }

    [[nodiscard]] static http2_event request_unprocessed(std::uint32_t stream_id) noexcept {
        return http2_event(http2_request_unprocessed_event(stream_id));
    }

    [[nodiscard]] static http2_event goaway(http2_peer_goaway peer_goaway) noexcept {
        return http2_event(http2_goaway_event(peer_goaway));
    }

    static http2_event origin_advertisement(http_origin_advertisement event) noexcept {
        return http2_event(std::move(event));
    }
    static http2_event alternative_service_advertisement(http_alternative_service_advertisement event) noexcept {
        return http2_event(std::move(event));
    }
    static http2_event priority_update(http_priority_update event) noexcept {
        return http2_event(event);
    }
    static http2_event push_promise(http2_push_promise_event event) noexcept {
        return http2_event(std::move(event));
    }

    value_type value_;
};

}  // namespace ruvia::detail
