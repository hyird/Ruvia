#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http2_request_content.h"
#include "ruvia/http/http2_request_head_submit_result.h"
#include "ruvia/http/http2_response_head_submit_result.h"
#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_push.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/http/websocket_protocol.h"

namespace ruvia {

namespace detail {
class http2_connection;
class http2_connection_owner_endpoint;
}  // namespace detail

enum class http2_server_request_release_status : std::uint8_t { released,
    closed,
    invalid_lease };

struct http2_connection_options final {
    // The resource must outlive the connection and any escaped events or
    // credits, including their owned heads and trailers.
    std::pmr::memory_resource* resource_{nullptr};
    bool enable_push_{false};
    // Enable only for an authenticated h2 connection to an origin server.
    // RFC 8336 forbids using ORIGIN over h2c or through an explicit proxy.
    bool receive_origin_advertisements_{false};
};

struct http2_websocket_server_handshake_options final {
    std::span<const std::string_view> supported_subprotocols_{};
    std::span<const http_header_view> response_headers_{};
};

struct http2_regular_request_head_view final {
    borrowed_text method_{"GET"};
    borrowed_text scheme_{"https"};
    std::optional<borrowed_text> authority_{};
    borrowed_text target_{"/"};
    std::span<const http_header_view> headers_{};
    http2_request_content content_{http2_request_content::none()};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
};

struct http2_connect_request_head_view final {
    borrowed_text authority_{};
    std::span<const http_header_view> headers_{};
};

struct http2_extended_connect_request_head_view final {
    borrowed_text protocol_{};
    borrowed_text scheme_{"https"};
    borrowed_text authority_{};
    borrowed_text target_{"/"};
    std::span<const http_header_view> headers_{};
};

enum class http2_finish_response_status : std::uint8_t {
    accepted,
    queued,
    closed,
    invalid_state,
    content_length_incomplete,
};

enum class http2_received_data_credit_merge_status : std::uint8_t {
    merged,
    invalid_credit,
    different_stream,
    overflow,
};

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
    // consumes other; failure leaves both tokens unchanged.
    [[nodiscard]] http2_received_data_credit_merge_status merge(
        http2_received_data_credit&& other) noexcept;
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
    invalid_credit
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
    friend class http2_event;
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
struct http2_server_request_snapshot final {
    http_request_content_indication content_{http_request_content_indication::no_content};
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
    [[nodiscard]] const http_request& request() const& noexcept {
        return request_;
    }
    const http_request& request() const&& = delete;
    [[nodiscard]] http_server_expectation_plan expectation_plan(
        http_unsupported_expectation_policy policy) const noexcept {
        return expectations_.server_plan(content_, policy);
    }

private:
    friend class http2_connection;
    friend class http2_event;
    http2_request_head_event(detail::http2_connection_owner_endpoint* endpoint, std::uint32_t stream_id,
        http_request request, http_request_expectations expectations,
        http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept;
    std::uint32_t stream_id_;
    http_request request_;
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

private:
    friend class http2_event;
    http2_response_head_event(std::uint32_t stream_id, http_client_response_head head,
        std::optional<http_client_request_content_signal> signal) noexcept
        : stream_id_(stream_id),
          head_(std::move(head)),
          signal_(signal) {}
    std::uint32_t stream_id_;
    http_client_response_head head_;
    std::optional<http_client_request_content_signal> signal_;
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
    friend class http2_event;
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
    friend class http2_event;
    constexpr http2_stream_closed_event(
        std::uint32_t stream_id, http2_stream_close_source source_value, http2_error_code error) noexcept
        : stream_id_(stream_id),
          source_(source_value),
          error_(error) {}
    std::uint32_t stream_id_;
    http2_stream_close_source source_;
    http2_error_code error_;
};

class http2_request_unprocessed_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

private:
    friend class http2_event;
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
    friend class http2_event;
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
    using value_type = std::variant<http2_informational_head_event, http2_request_head_event,
        http2_response_head_event, http2_message_body_chunk_event, http2_message_end_event,
        http2_tunnel_data_event, http2_tunnel_end_event, http2_stream_closed_event,
        http2_request_unprocessed_event, http2_goaway_event, http2_push_promise_event, http_priority_update, http_origin_advertisement, http_alternative_service_advertisement>;
    template <typename event_type>
    explicit http2_event(event_type event) noexcept
        : value_(std::move(event)) {}
    [[nodiscard]] static http2_event informational_head(std::uint32_t id, http_client_response_head head,
        std::optional<http_client_request_content_signal> signal) noexcept {
        return http2_event(http2_informational_head_event(id, std::move(head), signal));
    }
    [[nodiscard]] static http2_event request_head(detail::http2_connection_owner_endpoint* endpoint,
        std::uint32_t id, http_request request, http_request_expectations expectations,
        http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept {
        return http2_event(http2_request_head_event(
            endpoint, id, std::move(request), expectations, content, snapshot));
    }
    [[nodiscard]] static http2_event response_head(std::uint32_t id, http_client_response_head head,
        std::optional<http_client_request_content_signal> signal) noexcept {
        return http2_event(http2_response_head_event(id, std::move(head), signal));
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
    [[nodiscard]] static http2_event tunnel_end(std::uint32_t id) noexcept {
        return http2_event(http2_tunnel_end_event(id));
    }
    [[nodiscard]] static http2_event stream_closed(
        std::uint32_t id, http2_stream_close_source source_value, http2_error_code error) noexcept {
        return http2_event(http2_stream_closed_event(id, source_value, error));
    }
    [[nodiscard]] static http2_event request_unprocessed(std::uint32_t id) noexcept {
        return http2_event(http2_request_unprocessed_event(id));
    }
    [[nodiscard]] static http2_event goaway(
        std::uint32_t last_stream_id, http2_error_code error) noexcept {
        return http2_event(http2_goaway_event(last_stream_id, error));
    }
    value_type value_;
};

enum class http2_websocket_handshake_submit_error : std::uint8_t { closed,
    invalid_state };

class http2_websocket_negotiation final {
public:
    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

private:
    friend class http2_websocket_handshake_submit_result;
    friend class http2_connection;
    http2_websocket_negotiation(std::string_view subprotocol, websocket_compression compression)
        : subprotocol_(subprotocol),
          compression_(compression) {}
    std::string subprotocol_;
    websocket_compression compression_;
};

class http2_websocket_handshake_submit_failure final {
public:
    [[nodiscard]] constexpr http2_websocket_handshake_submit_error error() const noexcept {
        return error_;
    }

private:
    friend class http2_websocket_handshake_submit_result;
    friend class http2_connection;
    explicit constexpr http2_websocket_handshake_submit_failure(
        http2_websocket_handshake_submit_error error) noexcept
        : error_(error) {}
    http2_websocket_handshake_submit_error error_;
};

class http2_websocket_handshake_submit_result final {
public:
    [[nodiscard]] const http2_websocket_negotiation* submitted() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const http2_websocket_negotiation* submitted() const&& = delete;
    [[nodiscard]] const http2_websocket_handshake_submit_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const http2_websocket_handshake_submit_failure* failure() const&& = delete;

private:
    friend class http2_connection;
    using value_type = std::variant<http2_websocket_negotiation, http2_websocket_handshake_submit_failure>;
    explicit http2_websocket_handshake_submit_result(http2_websocket_negotiation value)
        : value_(std::move(value)) {}
    explicit http2_websocket_handshake_submit_result(http2_websocket_handshake_submit_failure failure)
        : value_(failure) {}
    value_type value_;
};

// Stable HTTP/2 sans-I/O driver. Feed transport bytes, drain typed events, and
// flush pending_output(); the class owns all HPACK, stream and flow-control state.
// The storage behind an accepted feed() input must remain alive and unchanged
// until every event produced by that call has been taken and every event-derived
// byte view has expired. Calling feed() again may invalidate those views; owning
// string temporaries are rejected at compile time.
class http2_connection final {
public:
    [[nodiscard]] static http2_connection server(http2_connection_options options = {});
    [[nodiscard]] static http2_connection client(http2_connection_options options = {});
    ~http2_connection();
    http2_connection(const http2_connection&) = delete;
    http2_connection& operator=(const http2_connection&) = delete;
    http2_connection(http2_connection&&) noexcept;
    http2_connection& operator=(http2_connection&&) noexcept;

    [[nodiscard]] http2_role role() const noexcept;
    [[nodiscard]] bool received_peer_settings() const noexcept;
    // Read-only observation for transport timeout decisions while a CONTINUATION
    // sequence is incomplete.
    [[nodiscard]] bool header_block_in_progress() const noexcept;
    [[nodiscard]] http2_feed_result feed(std::string_view input);
    template <detail::http_temporary_owning_char_string input_type>
    http2_feed_result feed(input_type&&) = delete;
    [[nodiscard]] std::optional<http2_event> next_event();
    [[nodiscard]] std::string_view pending_output() const& noexcept;
    std::string_view pending_output() const&& = delete;
    [[nodiscard]] http2_output_consume_status consume_output(std::size_t bytes) noexcept;
    void take_output(std::pmr::string& output);
    // Copies whole frame boundaries (the first frame may exceed max_bytes). On
    // copy failure the output cursor and observer are untouched. After legacy
    // partial-byte consume_output(), returns unaligned until take_output() drains
    // the remaining suffix; a client preface is treated as one indivisible segment.
    [[nodiscard]] http2_output_batch_result take_output_batch(std::size_t max_bytes,
        std::pmr::string& output, http2_data_output_observer_type observer = nullptr,
        void* observer_context = nullptr);
    [[nodiscard]] bool wants_write() const noexcept;

    [[nodiscard]] http2_request_head_submit_result submit_request_head(
        const http2_regular_request_head_view& request);
    [[nodiscard]] http2_request_head_submit_result submit_request_head(
        const http2_connect_request_head_view& request);
    [[nodiscard]] http2_request_head_submit_result submit_request_head(
        const http2_extended_connect_request_head_view& request);
    [[nodiscard]] http2_data_submit_status submit_data(
        std::uint32_t stream_id, std::string_view bytes, http2_end_stream end_stream);
    [[nodiscard]] http2_finish_request_status finish_request(std::uint32_t stream_id,
        std::span<const http_header_view> trailers = {});
    [[nodiscard]] http2_request_content_release_status release_request_content(
        std::uint32_t stream_id) noexcept;
    // Commits a promise and returns the server request/storage lease used to
    // produce its response. Release or abandon it exactly like an incoming request.
    [[nodiscard]] std::variant<http2_request_head_event, http2_push_submit_error> submit_push_request(
        std::uint32_t associated_stream_id, http_push_request_view request);

    [[nodiscard]] std::variant<std::uint32_t, http2_push_submit_error> submit_push_promise(
        std::uint32_t associated_stream_id, http_push_request_view request);
    [[nodiscard]] http2_submit_status submit_interim_response_head(
        std::uint32_t stream_id, const http_interim_response_head& response);
    [[nodiscard]] http2_submit_status submit_buffered_response(
        std::uint32_t stream_id, const http_response& response);
    [[nodiscard]] http2_submit_status submit_connect_response_head(
        std::uint32_t stream_id, const http_response& response);
    [[nodiscard]] http2_websocket_handshake_submit_result submit_websocket_handshake(
        std::uint32_t stream_id, const http_request& request,
        const websocket_handshake_validation_result& validation);
    [[nodiscard]] http2_websocket_handshake_submit_result submit_websocket_handshake(
        std::uint32_t stream_id, const http_request& request,
        const websocket_handshake_validation_result& validation,
        http2_websocket_server_handshake_options options);
    // Submit only the final buffered-response head. The committed plan tells the
    // caller whether/how much representation remains for DATA or file output.
    [[nodiscard]] http2_response_head_submit_result submit_response_head(
        std::uint32_t stream_id, const http_response& response,
        http_buffered_response_write_plan write_plan);
    // Submit a final generic response head without trailers and leave an eligible
    // response body open for subsequent submit_data() calls. Content-Length is
    // not generated automatically; an explicit value, when present, constrains
    // the total submitted DATA bytes.
    [[nodiscard]] http2_streaming_response_head_submit_result submit_streaming_response_head(
        std::uint32_t stream_id, http_response response, http_response_stream_kind kind,
        http_response_trailer_intent trailer_intent);
    // Convenience form for generic streaming responses without trailers.
    [[nodiscard]] http2_submit_status submit_streaming_response_head(
        std::uint32_t stream_id, http_response response);
    [[nodiscard]] http2_finish_response_status finish_response(
        std::uint32_t stream_id, const http_response_trailer_section& trailers);
    [[nodiscard]] http2_submit_status submit_origin_advertisement(std::span<const std::string_view> origins);
    [[nodiscard]] http2_submit_status submit_alternative_service_advertisement(std::uint32_t stream_id,
        std::string_view origin, std::string_view field_value);
    [[nodiscard]] http2_submit_status submit_priority_update(std::uint32_t stream_id, http_priority_fields fields);

    [[nodiscard]] http2_submit_status submit_reset(std::uint32_t stream_id, http2_error_code error);
    [[nodiscard]] http2_received_data_acknowledge_status acknowledge(http2_received_data_credit&& credit);
    [[nodiscard]] bool has_queued_data(std::uint32_t stream_id) const noexcept;
    // Per-stream protocol ownership of submitted DATA: queued means the core still
    // owns a flow-control-blocked suffix; drained means none remains; aborted means
    // the stream reset/closed and queued DATA was discarded.
    [[nodiscard]] http2_data_queue_state data_queue_state(std::uint32_t stream_id) const noexcept;
    // Payload bytes in serialized DATA frames still owned by the core output buffer.
    [[nodiscard]] std::size_t pending_data_output_bytes(std::uint32_t stream_id) const noexcept;
    // Read-only state used by a response writer to decide whether a send-window
    // wait can make progress. Missing/closed streams have no window snapshot.
    [[nodiscard]] std::optional<http2_send_window_state> send_window_state(
        std::uint32_t stream_id) const noexcept;
    // Missing streams are considered aborted, making the query safe during teardown.
    [[nodiscard]] bool stream_aborted(std::uint32_t stream_id) const noexcept;
    // Reports the protocol receive state without exposing internal stream storage.
    [[nodiscard]] http2_stream_receive_status stream_receive_status(
        std::uint32_t stream_id) const noexcept;
    // Borrowed snapshot of the peer's request metadata; it does not grant
    // access to the connection's stream table or mutable protocol state.
    [[nodiscard]] std::optional<http2_server_request_view> server_request_view(
        std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> take_drained_data_streams() & noexcept;
    std::span<const std::uint32_t> take_drained_data_streams() && = delete;
    // Consume the server request event only after every request-derived view has
    // expired. An event cannot be released through another connection.
    [[nodiscard]] http2_server_request_release_status release(http2_request_head_event&& request);
    void begin_drain();
    [[nodiscard]] bool draining() const noexcept;
    [[nodiscard]] std::optional<http2_error_code> connection_error() const noexcept;

private:
    friend std::variant<http_request, http_protocol_error> make_http2_server_request(
        http2_connection&, std::uint32_t, std::pmr::memory_resource*, std::string_view);
    friend websocket_handshake_validation_result validate_http2_websocket_handshake(
        http2_connection&, std::uint32_t, const http_request&) noexcept;
    [[nodiscard]] static http2_request_head_submit_result pin_submitted_request(
        detail::http2_connection& connection, const http2_request_head_submit_result& result);
    explicit http2_connection(std::pmr::memory_resource* resource, http2_role role, bool enable_push, bool receive_origin_advertisements);
    class impl_type;
    struct impl_deleter_type final {
        void operator()(impl_type* value) const noexcept;
    };
    std::unique_ptr<impl_type, impl_deleter_type> impl_;
};

// Builds a semantic request from a decoded server stream. The request borrows
// stream metadata/body from `connection`, which must outlive the request, and
// owns its header descriptors from `resource`.
[[nodiscard]] std::variant<http_request, http_protocol_error> make_http2_server_request(
    http2_connection& connection, std::uint32_t stream_id,
    std::pmr::memory_resource* resource, std::string_view body);

// Validates an RFC 8441 extended-CONNECT handshake against the decoded stream
// state and its semantic request. The request and connection must remain alive
// for the duration of the call.
[[nodiscard]] websocket_handshake_validation_result validate_http2_websocket_handshake(
    http2_connection& connection, std::uint32_t stream_id,
    const http_request& request) noexcept;

}  // namespace ruvia
