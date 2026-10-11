#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/http/http3_client_response.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_control_stream.h"
#include "ruvia/http/http3_message_body.h"
#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/http3_peer_streams.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_stream_frames.h"

namespace ruvia {

struct http3_connection::impl_type final {
    enum class early_data_phase : std::uint8_t { inactive,
        pending,
        accepted,
        rejected };
    struct request_type final {
        request_type(std::pmr::memory_resource* resource, const http3_connection_config& limits, http3_qpack_decoder& decoder)
            : frames_(http3_stream_kind::request, resource,
                  {.max_field_section_size_ = limits.max_encoded_field_section_bytes_}),
              resource_(resource),
              head_resource_(resource),
              limits_(limits),
              decoder_(decoder) {}

        http3_stream_frames frames_;
        std::pmr::memory_resource* resource_;
        // Owner of a request head decoded during the current feed call.
        std::pmr::memory_resource* head_resource_;
        http3_connection_config limits_;
        http3_qpack_decoder& decoder_;
        std::optional<http3_message_body> body_;
        bool connect_{false};
        bool terminal_{false};
        http3_connection_result callback_result_{};
        http3_connection_callback_type callback_{nullptr};
        void* callback_context_{nullptr};
        std::uint64_t stream_id_{0};
    };

    struct client_request_type final {
        client_request_type(std::uint64_t stream_id, http_known_method method, std::pmr::memory_resource* resource,
            const http3_connection_config& limits, http3_qpack_decoder& decoder, impl_type& owner_value)
            : response_(stream_id, method, resource,
                  {.max_field_section_size_ = limits.max_field_section_size_,
                      .max_fields_ = limits.max_fields_,
                      .max_encoded_field_section_bytes_ = limits.max_encoded_field_section_bytes_,
                      .max_push_id_ = limits.max_push_id_},
                  &decoder),
              owner_(owner_value) {}
        http3_client_response response_;
        impl_type& owner_;
        bool response_observed_{false};
        http3_connection_callback_type callback_{nullptr};
        void* callback_context_{nullptr};
    };

    struct push_type final {
        std::optional<http3_message_head> head_;
        std::optional<std::uint64_t> stream_id_;
        bool completed_{false};
        bool canceled_{false};
    };

    struct push_stream_type final {
        std::array<char, 8> id_bytes_{};
        std::size_t used_{0};
        std::optional<std::uint64_t> id_;
        std::optional<http3_client_response> response_;
    };

    struct control_callback_type final {
        impl_type& owner_;
        http3_connection_callback_type callback_;
        void* context_;
    };

    struct push_callback_type final {
        http3_connection_callback_type callback_;
        void* context_;
        std::uint64_t push_id_;
    };

    struct feed_guard_type final {
        explicit feed_guard_type(impl_type& impl)
            : impl_(impl) {
            if (impl.feeding_) {
                throw std::logic_error("recursive http3_connection::feed()");
            }
            impl.feeding_ = true;
        }
        ~feed_guard_type() {
            impl_.feeding_ = false;
        }
        impl_type& impl_;
    };

    impl_type(http3_peer_role role, std::pmr::memory_resource* resource, http3_connection_config limits)
        : role_(role),
          resource_(resource),
          limits_(limits),
          peer_streams_(role, resource, {.max_active_streams_ = limits.max_peer_unidirectional_streams_}),
          requests_(resource),
          client_requests_(resource),
          pushes_(resource),
          push_streams_(resource),
          control_(role == http3_peer_role::server ? http3_control_role::server : http3_control_role::client,
              resource,
              {.max_field_section_size_ = limits.max_encoded_field_section_bytes_}),
          receive_qpack_({.max_table_capacity_ = limits.qpack_max_table_capacity_,
                             .max_blocked_streams_ = limits.qpack_blocked_streams_,
                             .fields_ = {limits.max_encoded_field_section_bytes_, limits.max_field_section_size_, limits.max_fields_}},
              resource),
          transmit_qpack_(std::in_place, http3_qpack_encoder_config{.max_table_capacity_ = 0, .max_blocked_streams_ = 0}, resource),
          control_output_(resource) {}

    [[nodiscard]] static http3_connection_result connection_error(http3_connection_error_code code) noexcept {
        return {http3_connection_status::connection_error, http3_connection_error_scope::connection, code};
    }

    [[nodiscard]] static http3_connection_result stream_error(http3_connection_error_code code) noexcept {
        return {http3_connection_status::stream_error, http3_connection_error_scope::stream, code};
    }

    [[nodiscard]] static http3_connection_result request_error(http3_connection_error_code code) noexcept {
        if (code == http3_connection_error_code::qpack_decompression_failed ||
            code == http3_connection_error_code::excessive_load) {
            return connection_error(code);
        }
        return stream_error(code);
    }

    // The single precondition of every local operation: a live connection,
    // outside feed callbacks, without a latched failure, and in the given role.
    [[nodiscard]] static impl_type* require_open(impl_type* impl, std::optional<http3_peer_role> role = std::nullopt) noexcept {
        if (impl == nullptr || impl->feeding_ || impl->failed_.scope_ != http3_connection_error_scope::none ||
            (role && impl->role_ != *role)) {
            return nullptr;
        }
        return impl;
    }

    // Reserves before the caller commits protocol state, so the append that
    // follows the commit cannot fail.
    void reserve_control_output(std::size_t bytes_value) {
        const auto maximum = control_output_.max_size();
        const auto pending = control_output_.size() - control_consumed_;
        if (bytes_value > maximum - pending) {
            throw std::length_error("HTTP/3 control output exceeds its size limit");
        }
        if (bytes_value > maximum - control_output_.size()) {
            compact_control_output();
        }
        const auto required = control_output_.size() + bytes_value;
        if (required > control_output_.capacity()) {
            const auto capacity = control_output_.capacity();
            const auto grown = capacity + std::min(capacity, maximum - capacity);
            control_output_.reserve(std::max(required, grown));
        }
    }
    void compact_control_output() noexcept {
        if (control_consumed_ == 0) {
            return;
        }
        const auto pending = control_output_.size() - control_consumed_;
        if (pending != 0) {
            std::memmove(control_output_.data(), control_output_.data() + control_consumed_, pending);
        }
        control_output_.resize(pending);
        control_consumed_ = 0;
    }
    void append_control_output(std::span<const char> frame) noexcept {
        control_output_.insert(control_output_.end(), frame.begin(), frame.end());
    }

    [[nodiscard]] http3_field_section_limits local_field_limits(http3_field_section_limits requested) const noexcept {
        requested.max_encoded_bytes_ = std::min(requested.max_encoded_bytes_, limits_.max_encoded_field_section_bytes_);
        requested.max_decoded_bytes_ = std::min(requested.max_decoded_bytes_, limits_.max_field_section_size_);
        requested.max_fields_ = std::min(requested.max_fields_, limits_.max_fields_);
        return requested;
    }

    [[nodiscard]] http3_field_section_limits outbound_field_limits(http3_field_section_limits requested) const noexcept {
        requested = local_field_limits(requested);
        // SETTINGS_MAX_FIELD_SECTION_SIZE limits decoded names, values and the
        // per-field overhead, not compressed QPACK bytes. Before SETTINGS (or
        // when the setting is absent), only the local budget applies.
        const auto& settings = effective_peer_settings();
        if (settings && settings->max_field_section_size_ &&
            std::cmp_less(*settings->max_field_section_size_, requested.max_decoded_bytes_)) {
            requested.max_decoded_bytes_ = static_cast<std::size_t>(*settings->max_field_section_size_);
        }
        return requested;
    }

    [[nodiscard]] const std::optional<http3_settings>& effective_peer_settings() const noexcept {
        return control_.peer_settings() ? control_.peer_settings() : remembered_peer_settings_;
    }
    // Apply actual SETTINGS once, preserving early-data compatibility and the
    // encoder's decoder-stream parser state. Called before control callbacks.
    [[nodiscard]] bool prepare_peer_settings();

    // Receive dispatch (http3_connection_feed.cpp).
    static void on_request_frame(void* opaque, http3_stream_frame_event frame);
    static void finish(request_type& request);
    static void on_client_response(void* opaque, const http3_client_response_event& response_event);
    static void on_control(void* opaque, http3_control_stream_event event);
    static void on_push_response(void* opaque, const http3_client_response_event& source_value);
    // Records a sent or received promise; false latches a connection failure.
    [[nodiscard]] bool accept_promise(std::uint64_t id, const http3_message_head& head);
    [[nodiscard]] http3_connection_result feed_push(std::uint64_t stream_id, std::span<const char> bytes_value, bool fin, bool reset,
        http3_connection_callback_type callback_value, void* context_value);

    http3_peer_role role_;
    std::pmr::memory_resource* resource_;
    http3_connection_config limits_;
    http3_peer_streams peer_streams_;
    std::pmr::unordered_map<std::uint64_t, request_type> requests_;
    std::pmr::unordered_map<std::uint64_t, client_request_type> client_requests_;
    std::pmr::unordered_map<std::uint64_t, push_type> pushes_;
    std::pmr::unordered_map<std::uint64_t, push_stream_type> push_streams_;
    bool feeding_{false};
    bool peer_feed_started_{false};
    bool field_encoding_started_{false};
    early_data_phase early_data_phase_{early_data_phase::inactive};
    std::optional<http3_settings> remembered_peer_settings_;
    http3_control_stream control_;
    http3_qpack_decoder receive_qpack_;
    std::optional<http3_qpack_encoder> transmit_qpack_;
    bool transmit_qpack_configured_{false};
    http3_connection_result failed_{};
    std::optional<std::uint64_t> local_goaway_;
    // Queued control frames; [control_consumed_, size) is still pending.
    std::pmr::vector<char> control_output_;
    std::size_t control_consumed_{0};
};

}  // namespace ruvia
