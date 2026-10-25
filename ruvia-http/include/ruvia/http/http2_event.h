#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_push.h"
#include "ruvia/http/http_request.h"

namespace ruvia {

class http2_connection;
namespace detail {
class http2_connection;
class http2_connection_owner_endpoint;
}  // namespace detail

class http2_received_data_credit final {
public:
    // A credit is linear. acknowledge() consumes it explicitly; destroying a
    // still-valid token returns the same exact byte credit without throwing.
    ~http2_received_data_credit();
    http2_received_data_credit(const http2_received_data_credit&) = delete;
    http2_received_data_credit& operator=(const http2_received_data_credit&) = delete;
    http2_received_data_credit(http2_received_data_credit&& other) noexcept;
    http2_received_data_credit& operator=(http2_received_data_credit&&) = delete;
    // Combine credits from one stream without returning any bytes. Success
    // consumes other; invalid/mismatched/overflowing tokens throw.
    void merge(http2_received_data_credit&& other);
    [[nodiscard]] bool valid() const noexcept {
        return endpoint_ != nullptr && stream_id_ != 0 && bytes_ != 0;
    }

private:
    friend class http2_connection;
    friend class http2_message_body_chunk_event;
    friend class http2_tunnel_data_event;
    http2_received_data_credit(detail::http2_connection_owner_endpoint* endpoint, std::uint32_t stream_id,
        std::uint32_t bytes_value) noexcept;
    detail::http2_connection_owner_endpoint* endpoint_{nullptr};
    std::uint32_t stream_id_{0};
    std::uint32_t bytes_{0};
};

enum class http2_received_data_acknowledge_status : std::uint8_t {
    acknowledged,
    closed,
};
class http2_informational_head_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] const http_client_response_head& head() const& noexcept {
        return head_;
    }
    const http_client_response_head& head() const&& = delete;
    [[nodiscard]] std::optional<http_client_request_content_signal> request_content_signal()
        const noexcept {
        return signal_;
    }

private:
    friend class detail::http2_connection;
    http2_informational_head_event(std::uint32_t stream_id, http_client_response_head head,
        std::optional<http_client_request_content_signal> signal) noexcept
        : stream_id_(stream_id),
          head_(std::move(head)),
          signal_(signal) {}
    std::uint32_t stream_id_;
    http_client_response_head head_;
    std::optional<http_client_request_content_signal> signal_;
};

// The request-head event is also the stream-storage lease for every view in
// request(). Once processing finishes, release(std::move(event)) consumes the
// event and invalidates those views as one operation.
// content_ follows the core's current receive state when the event is taken,
// so content that already arrived in the same input batch reads as no_content
// (RFC 9110 §10.1.1 then omits a pointless 100 Continue). body_open_ is fixed
// when the head is published: the HEADERS frame left the peer send half open
// and message content events follow this head, whether or not the core has
// already parsed them.
struct http2_server_request_snapshot final {
    http_request_content_indication content_{http_request_content_indication::no_content};
    std::optional<std::size_t> declared_content_length_{};
    std::optional<http_priority_fields> priority_update_{};
    bool body_open_{false};
    bool connect_pending_{false};
};

class http2_request_head_event final {
public:
    ~http2_request_head_event();
    http2_request_head_event(const http2_request_head_event&) = delete;
    http2_request_head_event& operator=(const http2_request_head_event&) = delete;
    http2_request_head_event(http2_request_head_event&& other) noexcept;
    http2_request_head_event& operator=(http2_request_head_event&&) = delete;
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] const http2_server_request_snapshot& snapshot() const& noexcept {
        return snapshot_;
    }
    const http2_server_request_snapshot& snapshot() const&& = delete;
    [[nodiscard]] http2_server_request_view request_view() const& noexcept;
    http2_server_request_view request_view() const&& = delete;
    [[nodiscard]] http_request build_request(
        std::pmr::memory_resource* resource = nullptr, std::string_view body = {}) const;
    [[nodiscard]] http_server_expectation_plan expectation_plan(
        http_unsupported_expectation_policy policy) const noexcept {
        return expectations_.server_plan(content_, policy);
    }

private:
    friend class http2_connection;
    friend class http2_event;
    http2_request_head_event(detail::http2_connection_owner_endpoint* endpoint, std::uint32_t stream_id,
        http_request_expectations expectations,
        http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept;
    std::uint32_t stream_id_;
    http2_server_request_snapshot snapshot_;
    http_request_expectations expectations_;
    http_request_content_indication content_;
    detail::http2_connection_owner_endpoint* endpoint_{nullptr};
};

class http2_response_head_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] const http_client_response_head& head() const& noexcept {
        return head_;
    }
    const http_client_response_head& head() const&& = delete;
    [[nodiscard]] http_client_response_head take_head() && noexcept {
        return std::move(head_);
    }
    [[nodiscard]] std::optional<http_client_request_content_signal> request_content_signal()
        const noexcept {
        return signal_;
    }
    // The final response HEADERS carried END_STREAM: no content or tunnel DATA
    // follows this head. Fixed when the head is published; a later frame of the
    // same input batch that ends the stream (DATA with END_STREAM) never sets it.
    [[nodiscard]] bool stream_ended() const noexcept {
        return stream_ended_;
    }

private:
    friend class http2_event;
    http2_response_head_event(std::uint32_t stream_id, http_client_response_head head,
        std::optional<http_client_request_content_signal> signal, bool stream_ended) noexcept
        : stream_id_(stream_id),
          head_(std::move(head)),
          signal_(signal),
          stream_ended_(stream_ended) {}
    std::uint32_t stream_id_;
    http_client_response_head head_;
    std::optional<http_client_request_content_signal> signal_;
    bool stream_ended_;
};

class http2_message_body_chunk_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::string_view bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] http2_received_data_credit take_credit() & noexcept {
        return std::move(credit_);
    }

private:
    friend class http2_event;
    http2_message_body_chunk_event(detail::http2_connection_owner_endpoint* endpoint,
        std::uint32_t stream_id, std::string_view bytes_value, std::uint32_t credit) noexcept
        : stream_id_(stream_id),
          bytes_(bytes_value),
          credit_(endpoint, stream_id, credit) {}
    std::uint32_t stream_id_;
    std::string_view bytes_;
    http2_received_data_credit credit_;
};

enum class http2_message_content_semantics : std::uint8_t {
    content,
    metadata_only,
};

class http2_message_end_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::span<const http_header> trailers() const& noexcept {
        return trailers_;
    }
    std::span<const http_header> trailers() const&& = delete;
    [[nodiscard]] std::pmr::vector<http_header> take_trailers() && noexcept {
        return std::move(trailers_);
    }
    [[nodiscard]] http2_message_content_semantics content_semantics() const noexcept {
        return content_semantics_;
    }

private:
    friend class http2_event;
    http2_message_end_event(std::uint32_t stream_id,
        std::pmr::vector<http_header> trailers,
        http2_message_content_semantics content_semantics) noexcept
        : stream_id_(stream_id),
          trailers_(std::move(trailers)),
          content_semantics_(content_semantics) {}
    std::uint32_t stream_id_{0};
    std::pmr::vector<http_header> trailers_;
    http2_message_content_semantics content_semantics_{http2_message_content_semantics::content};
};

class http2_tunnel_data_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::string_view bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] http2_received_data_credit take_credit() & noexcept {
        return std::move(credit_);
    }

private:
    friend class http2_event;
    http2_tunnel_data_event(detail::http2_connection_owner_endpoint* endpoint, std::uint32_t stream_id,
        std::string_view bytes_value, std::uint32_t credit) noexcept
        : stream_id_(stream_id),
          bytes_(bytes_value),
          credit_(endpoint, stream_id, credit) {}
    std::uint32_t stream_id_;
    std::string_view bytes_;
    http2_received_data_credit credit_;
};

class http2_tunnel_end_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

private:
    friend class detail::http2_connection;
    explicit constexpr http2_tunnel_end_event(std::uint32_t stream_id) noexcept
        : stream_id_(stream_id) {}
    std::uint32_t stream_id_;
};

class http2_stream_closed_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] http2_stream_close_source source() const noexcept {
        return source_;
    }
    [[nodiscard]] http2_error_code error() const noexcept {
        return error_;
    }

private:
    friend class detail::http2_connection;
    constexpr http2_stream_closed_event(
        std::uint32_t stream_id, http2_stream_close_source source_value, http2_error_code error) noexcept
        : stream_id_(stream_id),
          source_(source_value),
          error_(error) {}
    std::uint32_t stream_id_;
    http2_stream_close_source source_;
    http2_error_code error_;
};

// Client only: the stream this endpoint opened was closed before the server
// processed it, so the request may be retried (RFC 9113 section 8.7). Published
// for streams above a peer GOAWAY's last stream id (after the GOAWAY event) and
// for RST_STREAM(REFUSED_STREAM) received before the final response head; the
// latter leaves the connection usable. No stream_closed event follows.
class http2_request_unprocessed_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

private:
    friend class detail::http2_connection;
    explicit constexpr http2_request_unprocessed_event(std::uint32_t stream_id) noexcept
        : stream_id_(stream_id) {}
    std::uint32_t stream_id_;
};

class http2_goaway_event final {
public:
    [[nodiscard]] std::uint32_t last_stream_id() const noexcept {
        return last_stream_id_;
    }
    [[nodiscard]] http2_error_code error() const noexcept {
        return error_;
    }

private:
    friend class detail::http2_connection;
    constexpr http2_goaway_event(std::uint32_t last_stream_id, http2_error_code error) noexcept
        : last_stream_id_(last_stream_id),
          error_(error) {}
    std::uint32_t last_stream_id_;
    http2_error_code error_;
};

class http2_event final {
public:
    http2_event(const http2_event&) = delete;
    http2_event& operator=(const http2_event&) = delete;
    http2_event(http2_event&&) noexcept = default;
    http2_event& operator=(http2_event&&) = delete;
    [[nodiscard]] const http2_informational_head_event* informational_head() const& noexcept {
        return std::get_if<http2_informational_head_event>(&value_);
    }
    const http2_informational_head_event* informational_head() const&& = delete;
    [[nodiscard]] http2_request_head_event* request_head() & noexcept {
        return std::get_if<http2_request_head_event>(&value_);
    }
    [[nodiscard]] const http2_request_head_event* request_head() const& noexcept {
        return std::get_if<http2_request_head_event>(&value_);
    }
    const http2_request_head_event* request_head() const&& = delete;
    [[nodiscard]] const http2_response_head_event* response_head() const& noexcept {
        return std::get_if<http2_response_head_event>(&value_);
    }
    [[nodiscard]] http2_response_head_event* response_head() & noexcept {
        return std::get_if<http2_response_head_event>(&value_);
    }
    const http2_response_head_event* response_head() const&& = delete;
    [[nodiscard]] http2_message_body_chunk_event* message_body_chunk() & noexcept {
        return std::get_if<http2_message_body_chunk_event>(&value_);
    }
    [[nodiscard]] const http2_message_body_chunk_event* message_body_chunk() const& noexcept {
        return std::get_if<http2_message_body_chunk_event>(&value_);
    }
    const http2_message_body_chunk_event* message_body_chunk() const&& = delete;
    [[nodiscard]] const http2_message_end_event* message_end() const& noexcept {
        return std::get_if<http2_message_end_event>(&value_);
    }
    [[nodiscard]] http2_message_end_event* message_end() & noexcept {
        return std::get_if<http2_message_end_event>(&value_);
    }
    const http2_message_end_event* message_end() const&& = delete;
    [[nodiscard]] http2_tunnel_data_event* tunnel_data() & noexcept {
        return std::get_if<http2_tunnel_data_event>(&value_);
    }
    [[nodiscard]] const http2_tunnel_data_event* tunnel_data() const& noexcept {
        return std::get_if<http2_tunnel_data_event>(&value_);
    }
    const http2_tunnel_data_event* tunnel_data() const&& = delete;
    [[nodiscard]] const http2_tunnel_end_event* tunnel_end() const& noexcept {
        return std::get_if<http2_tunnel_end_event>(&value_);
    }
    const http2_tunnel_end_event* tunnel_end() const&& = delete;
    [[nodiscard]] const http2_stream_closed_event* stream_closed() const& noexcept {
        return std::get_if<http2_stream_closed_event>(&value_);
    }
    const http2_stream_closed_event* stream_closed() const&& = delete;
    [[nodiscard]] const http2_request_unprocessed_event* request_unprocessed() const& noexcept {
        return std::get_if<http2_request_unprocessed_event>(&value_);
    }
    const http2_request_unprocessed_event* request_unprocessed() const&& = delete;
    [[nodiscard]] const http2_goaway_event* goaway() const& noexcept {
        return std::get_if<http2_goaway_event>(&value_);
    }
    const http2_goaway_event* goaway() const&& = delete;

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
    friend class detail::http2_connection_owner_endpoint;
    using value_type = std::variant<http2_informational_head_event, http2_request_head_event,
        http2_response_head_event, http2_message_body_chunk_event, http2_message_end_event,
        http2_tunnel_data_event, http2_tunnel_end_event, http2_stream_closed_event,
        http2_request_unprocessed_event, http2_goaway_event, http2_push_promise_event, http_priority_update, http_origin_advertisement, http_alternative_service_advertisement>;
    template <typename event_type>
    explicit http2_event(event_type event) noexcept
        : value_(std::move(event)) {}
    [[nodiscard]] static http2_event request_head(detail::http2_connection_owner_endpoint* endpoint,
        std::uint32_t id, http_request_expectations expectations,
        http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept {
        return http2_event(http2_request_head_event(
            endpoint, id, expectations, content, snapshot));
    }
    [[nodiscard]] static http2_event response_head(std::uint32_t id, http_client_response_head head,
        std::optional<http_client_request_content_signal> signal, bool stream_ended) noexcept {
        return http2_event(http2_response_head_event(id, std::move(head), signal, stream_ended));
    }
    [[nodiscard]] static http2_event message_body_chunk(detail::http2_connection_owner_endpoint* endpoint,
        std::uint32_t id, std::string_view bytes_value, std::uint32_t credit) noexcept {
        return http2_event(http2_message_body_chunk_event(endpoint, id, bytes_value, credit));
    }
    [[nodiscard]] static http2_event message_end(std::uint32_t id,
        std::pmr::vector<http_header> trailers,
        http2_message_content_semantics content_semantics) noexcept {
        return http2_event(
            http2_message_end_event(id, std::move(trailers), content_semantics));
    }
    [[nodiscard]] static http2_event tunnel_data(detail::http2_connection_owner_endpoint* endpoint,
        std::uint32_t id, std::string_view bytes_value, std::uint32_t credit) noexcept {
        return http2_event(http2_tunnel_data_event(endpoint, id, bytes_value, credit));
    }
    value_type value_;
};

}  // namespace ruvia
