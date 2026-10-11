#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/http2_event.h"
#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_push.h"

namespace ruvia::detail {

// Queue records for the events whose public form the facade materializes from
// stream storage or binds to its owner endpoint when the event is taken.
struct http2_message_head_record final {
    std::uint32_t stream_id_{0};
    std::optional<http_client_request_content_signal> request_content_signal_;
};

// The bytes borrow the accepted feed() input until the next consuming feed().
struct http2_message_data_record final {
    std::uint32_t stream_id_{0};
    std::string_view bytes_;
    std::uint32_t flow_control_bytes_{0};
};

struct http2_tunnel_data_record final {
    std::uint32_t stream_id_{0};
    std::string_view bytes_;
    std::uint32_t flow_control_bytes_{0};
};

struct http2_message_end_record final {
    std::uint32_t stream_id_{0};
};

// One queued protocol event. Events that need no owner binding are stored in
// their public form and moved out unchanged. There is deliberately no none
// alternative: an empty queue is represented by peek_event() returning null.
class http2_event final {
public:
    using value_type = std::variant<http2_informational_head_event, http2_message_head_record,
        http2_message_data_record, http2_message_end_record, http2_tunnel_data_record, http2_tunnel_end_event,
        http2_stream_closed_event, http2_request_unprocessed_event, http2_goaway_event, http2_push_promise_event,
        http_priority_update, http_origin_advertisement, http_alternative_service_advertisement>;

    template <typename event_type>
        requires(!std::is_same_v<event_type, http2_event>)
    explicit http2_event(event_type event) noexcept
        : value_(std::move(event)) {}
    http2_event(const http2_event&) = delete;
    http2_event& operator=(const http2_event&) = delete;
    http2_event(http2_event&&) noexcept = default;
    http2_event& operator=(http2_event&&) = delete;

    [[nodiscard]] bool references_stream(std::uint32_t stream_id) const noexcept {
        return std::visit(
            [stream_id](const auto& event) noexcept {
                using event_type = std::decay_t<decltype(event)>;
                if constexpr (std::is_same_v<event_type, http2_push_promise_event>) {
                    return event.promised_stream_id_ == stream_id || event.associated_stream_id_ == stream_id;
                } else if constexpr (requires { event.stream_id(); }) {
                    return event.stream_id() == stream_id;
                } else if constexpr (std::is_same_v<event_type, http2_message_head_record> ||
                                     std::is_same_v<event_type, http2_message_data_record> ||
                                     std::is_same_v<event_type, http2_message_end_record> ||
                                     std::is_same_v<event_type, http2_tunnel_data_record>) {
                    return event.stream_id_ == stream_id;
                } else {
                    return false;
                }
            },
            value_);
    }

    [[nodiscard]] value_type& value() & noexcept {
        return value_;
    }
    value_type& value() && = delete;

private:
    value_type value_;
};

}  // namespace ruvia::detail
