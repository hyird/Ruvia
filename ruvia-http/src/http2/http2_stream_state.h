#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_expectations.h"
#include "ruvia/http/http_known_method.h"

#include "http2/http2_local_content_state.h"
#include "http2/http2_receive_window_credit.h"
#include "http2/http2_remote_content_state.h"
#include "http2/http2_stream_flow_control.h"
#include "http2/http2_stream_header_blocks.h"
#include "http2/http2_stream_lifecycle.h"
#include "http2/http2_stream_request_data.h"
#include "http2/http2_stream_request_state.h"
#include "http2/http2_tunnel_state.h"

namespace ruvia::detail {

class http2_stream_header_decode_transaction;

enum class http2_local_request_content_gate : std::uint8_t {
    open,
    awaiting_continue,
    canceled,
};

enum class http2_push_reservation : std::uint8_t { none,
    local,
    remote };

class http2_stream_state final {
    friend class http2_stream_header_decode_transaction;

    std::uint32_t id_{0};
    http2_push_reservation push_reservation_{http2_push_reservation::none};
    http2_remote_content_state remote_content_;
    http2_local_content_state local_content_;
    http2_stream_lifecycle lifecycle_;
    http_request_expectations expectations_;
    http2_stream_request_state request_state_;
    http2_tunnel_state tunnel_state_;
    http2_stream_flow_control flow_control_;
    std::uint32_t window_debt_{0};
    http2_receive_window_credit receive_window_credit_;
    http2_stream_header_blocks header_blocks_;
    http2_stream_request_data message_data_;
    http2_local_request_content_gate local_request_content_gate_{http2_local_request_content_gate::open};

public:
    explicit http2_stream_state(std::uint32_t stream_id, std::pmr::memory_resource* resource)
        : id_(stream_id),
          header_blocks_(resource),
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

    void set_send_window(std::int32_t window) noexcept {
        flow_control_.set_send_window(window);
    }

    [[nodiscard]] bool add_send_window(std::int64_t delta) noexcept {
        return flow_control_.add_send_window(delta);
    }

    [[nodiscard]] std::int32_t send_window() const noexcept {
        return flow_control_.send_window();
    }

    void consume_send_window(std::size_t bytes_value) noexcept {
        flow_control_.consume_send(bytes_value);
    }

    [[nodiscard]] bool consume_receive_window(std::int32_t bytes_value) noexcept {
        return flow_control_.consume_receive(bytes_value);
    }

    [[nodiscard]] std::int32_t receive_window() const noexcept {
        return flow_control_.receive_window();
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

    [[nodiscard]] http2_receive_window_credit& receive_window_credit() & noexcept {
        return receive_window_credit_;
    }
    [[nodiscard]] http2_receive_window_credit& receive_window_credit() && = delete;

    void restore_receive_window(std::int32_t bytes_value) noexcept {
        flow_control_.restore_receive(bytes_value);
    }

    [[nodiscard]] std::pmr::string& remote_header_block() & noexcept {
        return header_blocks_.remote();
    }
    [[nodiscard]] std::pmr::string& remote_header_block() && = delete;

    [[nodiscard]] const std::pmr::string& remote_header_block() const& noexcept {
        return header_blocks_.remote();
    }
    [[nodiscard]] const std::pmr::string& remote_header_block() const&& = delete;

    [[nodiscard]] std::pmr::string& local_header_block() & noexcept {
        return header_blocks_.local();
    }
    [[nodiscard]] std::pmr::string& local_header_block() && = delete;

    [[nodiscard]] const std::pmr::string& local_header_block() const& noexcept {
        return header_blocks_.local();
    }
    [[nodiscard]] const std::pmr::string& local_header_block() const&& = delete;

    [[nodiscard]] bool declare_remote_content_length(std::size_t value) noexcept {
        return remote_content_.declare_known_length(value);
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

    [[nodiscard]] http2_local_content_check check_local_content_accept(
        std::size_t bytes_value, bool terminal) const noexcept {
        return local_content_.check_accept(bytes_value, terminal);
    }

    void accept_local_content(std::size_t bytes_value) noexcept {
        local_content_.accept(bytes_value);
    }

    void commit_local_content(std::size_t bytes_value) noexcept {
        local_content_.commit(bytes_value);
    }

    [[nodiscard]] const http2_local_content_state& local_content() const& noexcept {
        return local_content_;
    }
    [[nodiscard]] const http2_local_content_state& local_content() const&& = delete;

    [[nodiscard]] bool is_aborted() const noexcept {
        return lifecycle_.aborted();
    }

    [[nodiscard]] const http2_local_send_state& local_send() const& noexcept {
        return lifecycle_.local_send();
    }
    [[nodiscard]] const http2_local_send_state& local_send() const&& = delete;

    [[nodiscard]] const http2_remote_receive_state& remote_receive() const& noexcept {
        return lifecycle_.remote_receive();
    }
    [[nodiscard]] const http2_remote_receive_state& remote_receive() const&& = delete;

    [[nodiscard]] bool queued() const noexcept {
        return lifecycle_.queued();
    }

    [[nodiscard]] bool dispatch_started() const noexcept {
        return lifecycle_.dispatch_started();
    }

    [[nodiscard]] bool hold_peer_concurrency_slot() noexcept {
        return lifecycle_.hold_peer_concurrency_slot();
    }

    [[nodiscard]] bool release_peer_concurrency_slot() noexcept {
        return lifecycle_.release_peer_concurrency_slot();
    }

    [[nodiscard]] bool abort(http2_stream_close_source source_value) noexcept {
        return lifecycle_.abort(source_value);
    }

    [[nodiscard]] bool record_remote_head_end_stream() noexcept {
        return lifecycle_.record_remote_head_end_stream();
    }

    [[nodiscard]] bool rollback_remote_head_end_stream_for_retry() noexcept {
        return lifecycle_.rollback_remote_head_end_stream();
    }

    [[nodiscard]] bool finalize_remote_content_head() noexcept {
        if (tunnel_state_.not_connect() == nullptr) {
            return false;
        }
        return lifecycle_.finalize_remote_content_head();
    }

    [[nodiscard]] bool finalize_remote_connect_head() noexcept {
        if (tunnel_state_.pending() == nullptr) {
            return false;
        }
        return lifecycle_.finalize_remote_connect_head();
    }

    [[nodiscard]] bool finish_remote_content() noexcept {
        return lifecycle_.finish_remote_content();
    }

    [[nodiscard]] bool finish_remote_pending_connect() noexcept {
        if (tunnel_state_.pending() == nullptr) {
            return false;
        }
        return lifecycle_.finish_remote_pending_connect();
    }

    [[nodiscard]] bool finish_remote_tunnel() noexcept {
        if (tunnel_state_.open() == nullptr) {
            return false;
        }
        return lifecycle_.finish_remote_tunnel();
    }

    [[nodiscard]] bool finish_remote_rejected_connect() noexcept {
        if (tunnel_state_.rejected() == nullptr) {
            return false;
        }
        return lifecycle_.finish_remote_rejected_connect();
    }

    [[nodiscard]] bool begin_local_request_content() noexcept {
        return lifecycle_.begin_local_request_content();
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
        return lifecycle_.begin_local_response_content();
    }

    [[nodiscard]] bool begin_local_response_trailers_only() noexcept {
        return lifecycle_.begin_local_response_trailers_only();
    }

    [[nodiscard]] bool commit_local_head_end_stream() noexcept {
        return lifecycle_.commit_local_head_end_stream();
    }

    [[nodiscard]] bool begin_local_connect_request() noexcept {
        if (tunnel_state_.pending() == nullptr) {
            return false;
        }
        return lifecycle_.begin_local_connect_request();
    }

    [[nodiscard]] bool open_local_connect_tunnel() noexcept {
        if (tunnel_state_.open() == nullptr) {
            return false;
        }
        return lifecycle_.open_local_connect_tunnel();
    }

    [[nodiscard]] bool reject_local_connect() noexcept {
        if (tunnel_state_.rejected() == nullptr) {
            return false;
        }
        return lifecycle_.reject_local_connect();
    }

    [[nodiscard]] bool queue_local_end_stream() noexcept {
        return lifecycle_.queue_local_end_stream();
    }

    [[nodiscard]] bool commit_local_end_stream() noexcept {
        return lifecycle_.commit_local_end_stream();
    }

    [[nodiscard]] bool try_mark_queued() noexcept {
        return lifecycle_.try_mark_queued();
    }

    void clear_queued() noexcept {
        lifecycle_.clear_queued();
    }

    [[nodiscard]] bool try_start_dispatch() noexcept {
        return lifecycle_.try_start_dispatch();
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
            lifecycle_.remote_receive().content_open() != nullptr &&
            (remote_content_.allowed_without_length() != nullptr ||
                (known_length != nullptr &&
                    known_length->received_bytes() < known_length->declared_length()));
        return content_can_follow ? http_request_content_indication::will_follow
                                  : http_request_content_indication::no_content;
    }

    [[nodiscard]] http_server_expectation_plan expectation_plan(
        http_unsupported_expectation_policy unsupported_policy) const noexcept {
        return expectations_.server_plan(request_content_indication(), unsupported_policy);
    }

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

    [[nodiscard]] bool remote_headers_full() const noexcept {
        return message_data_.headers_full();
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
    [[nodiscard]] std::span<const http_header> remote_trailers() const& noexcept {
        return message_data_.trailers();
    }
    [[nodiscard]] std::pmr::vector<http_header> take_remote_trailers() & noexcept {
        return message_data_.take_trailers();
    }

    [[nodiscard]] bool has_method() const noexcept {
        return !request_method().empty();
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

    void mark_host() noexcept {
        request_state_.mark_host();
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

    [[nodiscard]] bool mark_singleton_request_header(std::uint32_t bit) noexcept {
        return request_state_.mark_singleton_header(bit);
    }

    [[nodiscard]] bool has_singleton_request_header(std::uint32_t bit) const noexcept {
        return request_state_.has_singleton_header(bit);
    }

    [[nodiscard]] bool mark_singleton_response_header(std::uint32_t bit) noexcept {
        return request_state_.mark_singleton_header(bit);
    }

    [[nodiscard]] bool begin_standard_connect() noexcept {
        return tunnel_state_.begin(http2_connect_form::standard);
    }

    [[nodiscard]] bool begin_extended_connect() noexcept {
        return tunnel_state_.begin(http2_connect_form::extended);
    }

    [[nodiscard]] bool accept_connect() noexcept {
        if (tunnel_state_.pending() == nullptr || !lifecycle_.can_accept_remote_connect()) {
            return false;
        }
        if (!tunnel_state_.accept()) {
            return false;
        }
        return lifecycle_.accept_remote_connect();
    }

    [[nodiscard]] bool reject_connect() noexcept {
        if (tunnel_state_.pending() == nullptr || !lifecycle_.can_reject_remote_connect()) {
            return false;
        }
        if (!tunnel_state_.reject()) {
            return false;
        }
        return lifecycle_.reject_remote_connect();
    }

    [[nodiscard]] const http2_tunnel_state& tunnel() const& noexcept {
        return tunnel_state_;
    }
    [[nodiscard]] const http2_tunnel_state& tunnel() const&& = delete;

    [[nodiscard]] const http_status_code* response_status() const& noexcept {
        return request_state_.response_status();
    }
    [[nodiscard]] const http_status_code* response_status() const&& = delete;

    [[nodiscard]] bool set_response_status(http_status_code status) noexcept {
        return request_state_.set_response_status(status);
    }

    [[nodiscard]] std::uint8_t interim_response_count() const noexcept {
        return request_state_.interim_response_count();
    }

    void count_interim_response() noexcept {
        request_state_.count_interim_response();
    }
};

// Header callbacks write into stream-owned PMR storage and typed protocol state. A
// callback is allowed to throw (most commonly from a request-header allocation), so
// the complete field block needs the same strong exception guarantee as HPACK's
// connection-global dynamic table. The transaction swaps out the prior request data
// without allocating, snapshots the scalar protocol state, and restores both on
// destruction unless the decoder explicitly commits the block.
class http2_stream_header_decode_transaction final {
public:
    explicit http2_stream_header_decode_transaction(
        http2_stream_state& stream, bool isolate_request_data = true) noexcept
        : stream_(&stream),
          message_data_(stream.message_data_.resource()),
          remote_content_(stream.remote_content_),
          local_content_(stream.local_content_),
          lifecycle_(stream.lifecycle_),
          expectations_(stream.expectations_),
          request_state_(stream.request_state_),
          tunnel_state_(stream.tunnel_state_),
          local_request_content_gate_(stream.local_request_content_gate_),
          isolate_request_data_(isolate_request_data),
          remote_headers_checkpoint_(stream.message_data_.header_checkpoint()) {
        if (isolate_request_data_) {
            message_data_.swap(stream.message_data_);
        }
    }

    http2_stream_header_decode_transaction(const http2_stream_header_decode_transaction&) = delete;
    http2_stream_header_decode_transaction& operator=(
        const http2_stream_header_decode_transaction&) = delete;
    http2_stream_header_decode_transaction(http2_stream_header_decode_transaction&&) = delete;
    http2_stream_header_decode_transaction& operator=(http2_stream_header_decode_transaction&&) = delete;

    ~http2_stream_header_decode_transaction() {
        rollback();
    }

    void commit() noexcept {
        active_ = false;
    }

    void rollback() noexcept {
        if (!active_) {
            return;
        }
        if (isolate_request_data_) {
            stream_->message_data_.swap(message_data_);
        } else {
            stream_->message_data_.rollback_headers(remote_headers_checkpoint_);
        }
        stream_->remote_content_ = remote_content_;
        stream_->local_content_ = local_content_;
        stream_->lifecycle_ = lifecycle_;
        stream_->expectations_ = expectations_;
        stream_->request_state_ = request_state_;
        stream_->tunnel_state_ = tunnel_state_;
        stream_->local_request_content_gate_ = local_request_content_gate_;
        active_ = false;
    }

private:
    http2_stream_state* stream_;
    http2_stream_request_data message_data_;
    http2_remote_content_state remote_content_;
    http2_local_content_state local_content_;
    http2_stream_lifecycle lifecycle_;
    http_request_expectations expectations_;
    http2_stream_request_state request_state_;
    http2_tunnel_state tunnel_state_;
    http2_local_request_content_gate local_request_content_gate_;
    bool isolate_request_data_;
    http2_stream_request_data::header_checkpoint_type remote_headers_checkpoint_;
    bool active_{true};
};

}  // namespace ruvia::detail
