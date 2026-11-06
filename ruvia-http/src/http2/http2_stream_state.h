#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_known_method.h"

#include "http2/http2_local_content_state.h"
#include "http2/http2_local_send_state.h"
#include "http2/http2_remote_content_state.h"
#include "http2/http2_remote_receive_state.h"
#include "http2/http2_stream_request_data.h"
#include "http2/http2_stream_request_state.h"
#include "http2/http2_tunnel_state.h"

namespace ruvia::detail {

enum class http2_local_request_content_gate : std::uint8_t {
    open,
    awaiting_continue,
    canceled,
};

enum class http2_push_reservation : std::uint8_t { none,
    local,
    remote };

// Ruvia's message-level view of one stream: decoded request/response storage,
// content accounting and the semantic half-states. Framing, stream state and
// flow control belong to the connection's nghttp2 session; this record outlives
// the nghttp2 stream while an owner lease or delivered DATA credit still names it.
class http2_stream_state final {
    std::uint32_t id_{0};
    http2_push_reservation push_reservation_{http2_push_reservation::none};
    http2_remote_content_state remote_content_;
    http2_local_content_state local_content_;
    http2_local_send_state local_send_;
    http2_remote_receive_state remote_receive_;
    bool peer_concurrency_slot_held_{false};
    http_request_expectations expectations_;
    http2_stream_request_state request_state_;
    http2_tunnel_state tunnel_state_;
    // Received DATA bytes delivered to the owner and not yet acknowledged.
    std::uint32_t window_debt_{0};
    http2_stream_request_data message_data_;
    http2_local_request_content_gate local_request_content_gate_{http2_local_request_content_gate::open};
    bool pinned_{false};
    std::size_t pending_events_{0};

public:
    explicit http2_stream_state(std::uint32_t stream_id, std::pmr::memory_resource* resource)
        : id_(stream_id),
          message_data_(resource) {}

    void reserve_push(http2_push_reservation reservation) noexcept {
        push_reservation_ = reservation;
    }
    void activate_push() noexcept {
        push_reservation_ = http2_push_reservation::none;
    }
    [[nodiscard]] http2_push_reservation push_reservation() const noexcept {
        return push_reservation_;
    }

    [[nodiscard]] std::uint32_t id() const noexcept {
        return id_;
    }

    [[nodiscard]] bool pinned() const noexcept {
        return pinned_;
    }

    void set_pinned(bool pinned_value) noexcept {
        pinned_ = pinned_value;
    }

    // Queued connection events naming this stream; an owner-lease release waits for zero.
    [[nodiscard]] bool has_pending_events() const noexcept {
        return pending_events_ != 0;
    }
    void add_pending_event() noexcept {
        ++pending_events_;
    }
    void remove_pending_event() noexcept {
        if (pending_events_ == 0) {
            std::terminate();
        }
        --pending_events_;
    }

    void add_window_debt(std::uint32_t bytes_value) noexcept {
        window_debt_ += bytes_value;
    }

    [[nodiscard]] std::uint32_t window_debt() const noexcept {
        return window_debt_;
    }

    [[nodiscard]] std::uint32_t take_window_debt() noexcept {
        return std::exchange(window_debt_, 0);
    }

    [[nodiscard]] bool take_window_debt(std::uint32_t bytes_value) noexcept {
        if (bytes_value == 0 || bytes_value > window_debt_) {
            return false;
        }
        window_debt_ -= bytes_value;
        return true;
    }

    [[nodiscard]] bool declare_remote_content_length(std::uint64_t value) noexcept {
        if (value > (std::numeric_limits<std::size_t>::max)()) {
            return false;
        }
        return remote_content_.declare_known_length(static_cast<std::size_t>(value));
    }

    [[nodiscard]] bool select_remote_content_metadata_only() noexcept {
        return remote_content_.select_metadata_only();
    }

    [[nodiscard]] http2_remote_content_accounting_result account_remote_content(
        std::size_t bytes_value) noexcept {
        return remote_content_.account(bytes_value);
    }

    [[nodiscard]] const http2_remote_content_state& remote_content() const& noexcept {
        return remote_content_;
    }
    [[nodiscard]] const http2_remote_content_state& remote_content() const&& = delete;

    void begin_local_content_forbidden() noexcept {
        local_content_.begin_forbidden();
    }

    void begin_local_content_unbounded() noexcept {
        local_content_.begin_unbounded();
    }

    void begin_local_content_known_length(std::uint64_t length) noexcept {
        local_content_.begin_known_length(length);
    }

    void commit_local_content(std::size_t bytes_value) noexcept {
        local_content_.commit(bytes_value);
    }

    [[nodiscard]] const http2_local_content_state& local_content() const& noexcept {
        return local_content_;
    }
    [[nodiscard]] const http2_local_content_state& local_content() const&& = delete;

    [[nodiscard]] bool is_aborted() const noexcept {
        return local_send_.aborted() != nullptr;
    }

    [[nodiscard]] const http2_local_send_state& local_send() const& noexcept {
        return local_send_;
    }
    [[nodiscard]] const http2_local_send_state& local_send() const&& = delete;

    [[nodiscard]] const http2_remote_receive_state& remote_receive() const& noexcept {
        return remote_receive_;
    }
    [[nodiscard]] const http2_remote_receive_state& remote_receive() const&& = delete;

    [[nodiscard]] bool hold_peer_concurrency_slot() noexcept {
        if (peer_concurrency_slot_held_) {
            return false;
        }
        peer_concurrency_slot_held_ = true;
        return true;
    }

    [[nodiscard]] bool release_peer_concurrency_slot() noexcept {
        if (!peer_concurrency_slot_held_) {
            return false;
        }
        peer_concurrency_slot_held_ = false;
        return true;
    }

    // Aborting closes both halves together; a stream is never half-aborted.
    [[nodiscard]] bool abort(http2_stream_close_source source_value) noexcept {
        if (remote_receive_.aborted() != nullptr) {
            return false;
        }
        if (!local_send_.abort(source_value)) {
            return false;
        }
        return remote_receive_.abort();
    }

    [[nodiscard]] bool record_remote_head_end_stream() noexcept {
        return remote_receive_.record_head_end_stream();
    }

    [[nodiscard]] bool finalize_remote_content_head() noexcept {
        if (tunnel_state_.not_connect() == nullptr) {
            return false;
        }
        return remote_receive_.finalize_content_head();
    }

    [[nodiscard]] bool finalize_remote_connect_head() noexcept {
        if (tunnel_state_.pending() == nullptr) {
            return false;
        }
        return remote_receive_.finalize_connect_head();
    }

    [[nodiscard]] bool finish_remote_content() noexcept {
        return remote_receive_.finish_content();
    }

    [[nodiscard]] bool mark_remote_connect_tunnel_data() noexcept {
        if (tunnel_state_.pending() == nullptr) {
            return false;
        }
        return remote_receive_.mark_connect_tunnel_data();
    }

    [[nodiscard]] bool finish_remote_pending_connect() noexcept {
        if (tunnel_state_.pending() == nullptr) {
            return false;
        }
        return remote_receive_.finish_pending_connect();
    }

    [[nodiscard]] bool finish_remote_tunnel() noexcept {
        if (tunnel_state_.open() == nullptr) {
            return false;
        }
        return remote_receive_.finish_tunnel();
    }

    [[nodiscard]] bool finish_remote_rejected_connect() noexcept {
        if (tunnel_state_.rejected() == nullptr) {
            return false;
        }
        return remote_receive_.finish_rejected_connect();
    }

    [[nodiscard]] bool begin_local_request_content() noexcept {
        return local_send_.begin_request_content();
    }

    void await_request_continue() noexcept {
        local_request_content_gate_ = http2_local_request_content_gate::awaiting_continue;
    }

    [[nodiscard]] bool request_continue_pending() const noexcept {
        return local_request_content_gate_ == http2_local_request_content_gate::awaiting_continue;
    }

    [[nodiscard]] bool release_request_continue() noexcept {
        if (!request_continue_pending()) {
            return false;
        }
        local_request_content_gate_ = http2_local_request_content_gate::open;
        return true;
    }

    [[nodiscard]] bool cancel_pending_request_continue() noexcept {
        if (!request_continue_pending()) {
            return false;
        }
        local_request_content_gate_ = http2_local_request_content_gate::canceled;
        return true;
    }

    [[nodiscard]] bool request_content_canceled() const noexcept {
        return local_request_content_gate_ == http2_local_request_content_gate::canceled;
    }

    [[nodiscard]] bool begin_local_response_content() noexcept {
        return local_send_.begin_response_content();
    }

    [[nodiscard]] bool commit_local_head_end_stream() noexcept {
        return local_send_.commit_head_end_stream();
    }

    [[nodiscard]] bool begin_local_connect_request() noexcept {
        if (tunnel_state_.pending() == nullptr) {
            return false;
        }
        return local_send_.begin_connect_request();
    }

    [[nodiscard]] bool open_local_connect_tunnel() noexcept {
        if (tunnel_state_.open() == nullptr) {
            return false;
        }
        return local_send_.open_tunnel();
    }

    [[nodiscard]] bool reject_local_connect() noexcept {
        if (tunnel_state_.rejected() == nullptr) {
            return false;
        }
        return local_send_.reject_connect();
    }

    [[nodiscard]] bool commit_local_end_stream() noexcept {
        return local_send_.commit_end_stream();
    }

    void parse_request_expectation_field(std::string_view value) noexcept {
        expectations_.parse_field(value);
    }

    [[nodiscard]] http_request_expectations request_expectations() const noexcept {
        return expectations_;
    }

    [[nodiscard]] http_request_content_indication request_content_indication() const noexcept {
        const auto* known_length = remote_content_.allowed_known_length();
        // An open receive half can still be metadata-only, known-empty, or
        // awaiting only an empty END_STREAM frame.
        const bool content_can_follow =
            remote_receive_.content_open() != nullptr &&
            (remote_content_.allowed_without_length() != nullptr ||
                (known_length != nullptr &&
                    known_length->received_bytes() < known_length->declared_length()));
        return content_can_follow ? http_request_content_indication::will_follow
                                  : http_request_content_indication::no_content;
    }

    void record_request_pseudo_header(http2_request_pseudo_header header) noexcept {
        request_state_.record_pseudo_header(header);
    }

    [[nodiscard]] std::span<const http2_request_pseudo_header> request_pseudo_header_order() const& noexcept {
        return request_state_.pseudo_header_order();
    }
    [[nodiscard]] std::span<const http2_request_pseudo_header> request_pseudo_header_order() const&& = delete;

    [[nodiscard]] std::string_view request_method() const& noexcept {
        return message_data_.method();
    }
    [[nodiscard]] std::string_view request_method() const&& = delete;

    [[nodiscard]] http_known_method request_known_method() const noexcept {
        return message_data_.known_method();
    }

    void assign_request_method(std::string_view method) {
        message_data_.assign_method(method);
    }

    [[nodiscard]] std::string_view request_authority() const& noexcept {
        return message_data_.authority();
    }
    [[nodiscard]] std::string_view request_authority() const&& = delete;

    void assign_request_authority(std::string_view value) {
        message_data_.assign_authority(value);
    }

    [[nodiscard]] std::string_view request_path() const& noexcept {
        return message_data_.path();
    }
    [[nodiscard]] std::string_view request_path() const&& = delete;

    void assign_request_path(std::string_view value) {
        message_data_.assign_path(value);
    }

    void take_request_target_from(http2_stream_state& other) noexcept {
        message_data_.take_request_target_from(other.message_data_);
    }

    [[nodiscard]] std::string_view request_protocol() const& noexcept {
        return message_data_.protocol();
    }
    [[nodiscard]] std::string_view request_protocol() const&& = delete;

    [[nodiscard]] std::string_view request_cookie() const& noexcept {
        return message_data_.cookie();
    }
    [[nodiscard]] std::string_view request_cookie() const&& = delete;

    [[nodiscard]] bool append_request_cookie_header_value(
        std::string_view value, bool has_existing_cookie) {
        return message_data_.append_cookie_header_value(value, has_existing_cookie);
    }

    [[nodiscard]] std::size_t remote_header_count() const noexcept {
        return message_data_.header_count();
    }

    [[nodiscard]] http2_stored_header_view remote_header_at(std::size_t index) const& noexcept {
        return message_data_.header_at(index);
    }
    [[nodiscard]] http2_stored_header_view remote_header_at(std::size_t) const&& = delete;

    [[nodiscard]] bool append_remote_header(
        std::string_view name, std::string_view value, request_header_kind kind) {
        return message_data_.append_header(name, value, kind);
    }
    [[nodiscard]] bool append_remote_trailer(std::string_view name, std::string_view value) {
        return message_data_.append_trailer(name, value);
    }
    [[nodiscard]] std::pmr::vector<http_header> take_remote_trailers() & noexcept {
        return message_data_.take_trailers();
    }

    [[nodiscard]] bool has_method() const noexcept {
        return message_data_.has_method();
    }

    [[nodiscard]] bool has_protocol() const noexcept {
        return request_state_.has_protocol();
    }

    [[nodiscard]] bool protocol_is_websocket() const noexcept {
        return http_ascii_equals_ignore_case(request_protocol(), "websocket");
    }

    void set_protocol(std::string_view value) {
        message_data_.assign_protocol(value);
        request_state_.mark_protocol();
    }

    [[nodiscard]] bool has_scheme() const noexcept {
        return request_state_.has_scheme();
    }

    [[nodiscard]] std::string_view request_scheme() const& noexcept {
        return message_data_.scheme();
    }
    [[nodiscard]] std::string_view request_scheme() const&& = delete;

    void assign_request_scheme(std::string_view value) {
        message_data_.assign_scheme(value);
    }

    void mark_scheme(std::uint16_t default_port) noexcept {
        request_state_.mark_scheme(default_port);
    }

    [[nodiscard]] std::uint16_t scheme_default_port() const noexcept {
        return request_state_.scheme_default_port();
    }

    [[nodiscard]] bool has_authority() const noexcept {
        return request_state_.has_authority();
    }

    void mark_authority() noexcept {
        request_state_.mark_authority();
    }

    [[nodiscard]] bool has_path() const noexcept {
        return request_state_.has_path();
    }

    void mark_path() noexcept {
        request_state_.mark_path();
    }

    [[nodiscard]] bool has_host() const noexcept {
        return request_state_.has_host();
    }

    void mark_host(std::size_t index) noexcept {
        request_state_.mark_host(index);
    }

    [[nodiscard]] std::size_t host_header_index() const noexcept {
        return request_state_.host_header_index();
    }

    [[nodiscard]] bool has_cookie() const noexcept {
        return request_state_.has_cookie();
    }

    void mark_cookie() noexcept {
        request_state_.mark_cookie();
    }

    [[nodiscard]] bool regular_header_seen() const noexcept {
        return request_state_.regular_header_seen();
    }

    void mark_regular_header_seen() noexcept {
        request_state_.mark_regular_header_seen();
    }

    [[nodiscard]] bool begin_standard_connect() noexcept {
        return tunnel_state_.begin(http2_connect_form::standard);
    }

    [[nodiscard]] bool begin_extended_connect() noexcept {
        return tunnel_state_.begin(http2_connect_form::extended);
    }

    [[nodiscard]] bool accept_connect() noexcept {
        if (tunnel_state_.pending() == nullptr || !remote_receive_.can_accept_connect()) {
            return false;
        }
        if (!tunnel_state_.accept()) {
            return false;
        }
        return remote_receive_.accept_connect();
    }

    [[nodiscard]] bool reject_connect() noexcept {
        if (tunnel_state_.pending() == nullptr || !remote_receive_.can_reject_connect()) {
            return false;
        }
        if (!tunnel_state_.reject()) {
            return false;
        }
        return remote_receive_.reject_connect();
    }

    [[nodiscard]] const http2_tunnel_state& tunnel() const& noexcept {
        return tunnel_state_;
    }
    [[nodiscard]] const http2_tunnel_state& tunnel() const&& = delete;

    [[nodiscard]] const http_received_status* response_status() const& noexcept {
        return request_state_.response_status();
    }
    [[nodiscard]] const http_received_status* response_status() const&& = delete;

    [[nodiscard]] bool set_response_status(http_received_status status) noexcept {
        return request_state_.set_response_status(status);
    }

    [[nodiscard]] bool has_informational_response() const noexcept {
        return request_state_.has_informational_response();
    }

    void mark_informational_response() noexcept {
        request_state_.mark_informational_response();
    }

    [[nodiscard]] const std::optional<http_priority_fields>& priority_update() const& noexcept {
        return request_state_.priority_update();
    }

    void set_priority_update(http_priority_fields fields) noexcept {
        request_state_.set_priority_update(fields);
    }
};

}  // namespace ruvia::detail
