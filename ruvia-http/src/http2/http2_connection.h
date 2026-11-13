#pragma once

// HTTP/2 sans-I/O connection core.
//
// A pure protocol state machine: it never touches a socket, a coroutine, a timer,
// or asio. Inbound bytes go to one nghttp2_session (nghttp2_session_mem_recv2),
// which owns framing, HPACK, stream states, flow control and SETTINGS/PING/GOAWAY
// handling; its callbacks feed Ruvia's message semantics (field validation,
// content accounting, CONNECT/tunnel phases, leases) and publish typed events.
// Every operation drains nghttp2_session_mem_send2 eagerly into the
// frame-segmented output buffer, so the owner sees whole frames through
// take_output_batch(). The I/O loop, concurrency model and timeouts live entirely
// in the caller.
//
// nghttp2 is a C library: exceptions never cross its frames. A callback that
// throws records the exception and fails the nghttp2 call; the connection is
// then failed and the exception rethrown once nghttp2 has returned. Allocation
// failure inside nghttp2 is likewise fatal for the connection.

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/detail/borrowed_view.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http2_event.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http2_observation.h"
#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_push.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_content_plan.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/websocket_handshake.h"

#include "coding/http_content_length.h"
#include "field/binary_field_section_size.h"
#include "field/http_connection_fields.h"
#include "field/http_field_kind.h"
#include "field/http_lower_header_name.h"
#include "field/http_request_field_validation.h"
#include "field/http_te_fields.h"
#include "http2/http2_stream_state.h"
#include "memory/geometric_reserve.h"
#include "memory/http_c_library_memory.h"
#include "parser/http_request_target.h"
#include "request/http_binary_request_fields.h"
#include "response/http_framed_response_head_plan.h"
#include "response/http_response_header_bits.h"
#include "websocket/http_websocket_handshake_fields.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia::detail {

// The field section handed to one nghttp2 HEADERS/PUSH_PROMISE submission.
// Entries either borrow caller text that outlives the submission or own a copy
// (lowercased names, formatted values). nghttp2 copies every name and value
// while submitting, so nothing here is retained past that call; nghttp2's
// session deflater is the only HPACK encoder.
class http2_field_list final {
public:
    http2_field_list() noexcept = default;

    void clear() noexcept {
        owned_.clear();
        entries_.clear();
        nv_.clear();
    }

    // Keep common sections warm; return oversized storage so one large field
    // section cannot pin descriptor buffers to an idle connection.
    void release() noexcept {
        http_clear_string_retaining_small(owned_);
        if (entries_.capacity() > http_retained_string_bytes / sizeof(entry_type)) {
            entries_ = std::vector<entry_type>();
        } else {
            entries_.clear();
        }
        if (nv_.capacity() > http_retained_string_bytes / sizeof(nghttp2_nv)) {
            nv_ = std::vector<nghttp2_nv>();
        } else {
            nv_.clear();
        }
    }

    void reserve(std::size_t fields) {
        entries_.reserve(entries_.size() + fields);
        nv_.reserve(entries_.size() + fields);
    }

    // `name` must already be lowercase; both views must outlive the submission.
    void add(std::string_view name, std::string_view value) {
        reserve_geometric(entries_, entries_.size() + 1);
        entries_.push_back(entry_type{.name_ = text_type::borrowed(name),
            .value_ = text_type::borrowed(value),
            .sensitive_ = http_field_never_indexed(name)});
    }

    // Copies `value`, which may be a temporary formatting buffer.
    void add_owned_value(std::string_view name, std::string_view value) {
        reserve_geometric(entries_, entries_.size() + 1);
        const auto owned_value = own(value);
        entries_.push_back(entry_type{.name_ = text_type::borrowed(name),
            .value_ = owned_value,
            .sensitive_ = http_field_never_indexed(name)});
    }

    // HTTP/2 field names are lowercase on the wire (RFC 9113 §8.2.1).
    void add_lowercase(std::string_view name, std::string_view value) {
        reserve_geometric(entries_, entries_.size() + 1);
        const auto offset = owned_.size();
        const auto lower = http_lower_header_name(name, [this, offset](std::size_t size) {
            reserve_geometric(owned_, offset + size);
            owned_.resize(offset + size);
            return owned_.data() + offset;
        });
        const auto entry_name = lower.data() == name.data()
                                    ? text_type::borrowed(name)
                                    : text_type{.data_ = nullptr, .offset_ = offset, .size_ = lower.size()};
        entries_.push_back(entry_type{.name_ = entry_name,
            .value_ = text_type::borrowed(value),
            .sensitive_ = http_field_never_indexed(lower)});
    }

    template <typename field_range_type>
    void add_lowercase_fields(const field_range_type& fields) {
        for (const auto& field : fields) {
            add_lowercase(field.name(), field.value());
        }
    }

    // Resolves owned offsets once every entry is final.
    [[nodiscard]] std::span<const nghttp2_nv> nv() {
        nv_.clear();
        nv_.reserve(entries_.size());
        for (const auto& entry : entries_) {
            const auto name = resolve(entry.name_);
            const auto value = resolve(entry.value_);
            nv_.push_back(nghttp2_nv{
                .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
                .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
                .namelen = name.size(),
                .valuelen = value.size(),
                .flags = static_cast<std::uint8_t>(entry.sensitive_ ? NGHTTP2_NV_FLAG_NO_INDEX : NGHTTP2_NV_FLAG_NONE),
            });
        }
        return nv_;
    }

private:
    struct text_type final {
        const char* data_{nullptr};
        std::size_t offset_{0};
        std::size_t size_{0};

        [[nodiscard]] static text_type borrowed(std::string_view text) noexcept {
            return text_type{.data_ = text.data(), .offset_ = 0, .size_ = text.size()};
        }
    };
    struct entry_type final {
        text_type name_;
        text_type value_;
        bool sensitive_{false};
    };

    [[nodiscard]] text_type own(std::string_view text) {
        const auto offset = owned_.size();
        reserve_geometric(owned_, offset + text.size());
        owned_.append(text.data(), text.size());
        return text_type{.data_ = nullptr, .offset_ = offset, .size_ = text.size()};
    }

    [[nodiscard]] std::string_view resolve(const text_type& text) const noexcept {
        return text.data_ != nullptr ? std::string_view(text.data_, text.size_)
                                     : std::string_view(owned_).substr(text.offset_, text.size_);
    }

    std::string owned_;
    std::vector<entry_type> entries_;
    std::vector<nghttp2_nv> nv_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// SETTINGS_MAX_HEADER_LIST_SIZE limits the decoded field section, not the
// serialized HPACK block. Huffman coding can spend up to 30 bits per decoded
// octet, so four encoded bytes per decoded-budget byte bound a valid block while
// keeping a hard memory limit for incomplete CONTINUATION sequences.
inline constexpr std::size_t max_http2_encoded_header_block_bytes = 4 * max_http_header_bytes;

// Request TE values are validated with the other request-field semantics
// (validate_request_field: only "trailers", RFC 9113 Section 8.2.2).
[[nodiscard]] inline bool http2_is_valid_regular_header(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_binary_field_name(name) && is_valid_http_header_value(value) &&
           !is_forbidden_http_binary_connection_field(name);
}

struct http2_header_decode_context final {
    explicit http2_header_decode_context(http2_stream_state& stream_value) noexcept
        : stream_(stream_value) {}

    http2_stream_state& stream_;
    binary_field_section_size decoded_header_list_size_{max_http_header_bytes};
    request_field_state request_fields_{request_field_framing::binary};
};

// Call only after authority admission (or validated local push submission).
// HTTP(S) already uses Host syntax; other schemes can contain URI userinfo.
[[nodiscard]] inline bool http2_request_synthesizes_host(const http2_stream_state& stream) noexcept {
    return binary_request_synthesizes_host(
        stream.has_host(), stream.has_authority(), stream.request_scheme(), stream.request_authority());
}

[[nodiscard]] inline std::string_view http2_request_host_field(const http2_stream_state& stream) noexcept {
    if (!stream.has_host()) {
        return {};
    }
    const auto index = stream.host_header_index();
    if (index < stream.remote_header_count()) {
        const auto header_value = stream.remote_header_at(index);
        if (header_value.kind_ == http_field_kind::host) {
            return header_value.value_;
        }
    }
    return {};
}

// Wire :authority if present; otherwise the Host field of a translated
// origin-form request (RFC 9113 §8.3.1).
[[nodiscard]] inline std::string_view http2_effective_request_authority(
    const http2_stream_state& stream) noexcept {
    return stream.has_authority() ? stream.request_authority() : http2_request_host_field(stream);
}

[[nodiscard]] inline bool http2_has_required_request_authority(const http2_stream_state& stream) noexcept {
    if (!binary_regular_request_requires_authority(stream.request_scheme(), stream.request_path())) {
        return true;
    }
    if (stream.has_authority()) {
        return true;
    }
    return is_valid_binary_request_authority(stream.request_scheme(), http2_request_host_field(stream));
}

[[nodiscard]] inline bool http2_accumulate_header_list_bytes(
    http2_header_decode_context& context_value, std::string_view name, std::string_view value) noexcept {
    return context_value.decoded_header_list_size_.add(name, value);
}

[[nodiscard]] inline bool http2_append_cookie_header_value(
    http2_stream_state& stream, std::string_view value) {
    // RFC 9113 section 8.2.3 joins cookie-pairs with "; ". An empty crumb is
    // not a cookie-pair (RFC 6265 section 4.2.1) and would leave a leading or
    // trailing separator in the joined value, so it contributes nothing.
    if (value.empty()) {
        return true;
    }
    if (!stream.append_request_cookie_header_value(value, stream.has_cookie())) {
        return false;
    }
    stream.mark_cookie();
    return true;
}

[[nodiscard]] inline bool http2_on_decoded_initial_header(
    http2_header_decode_context& context_value, std::string_view name, std::string_view value) {
    if (!http2_accumulate_header_list_bytes(context_value, name, value)) {
        return false;
    }

    auto& stream = context_value.stream_;
    if (name.empty()) {
        return false;
    }

    if (name.front() == ':') {
        if (stream.regular_header_seen()) {
            return false;
        }
        if (name == ":method") {
            if (stream.has_method() || !is_valid_http_method_token(value)) {
                return false;
            }
            stream.assign_request_method(value);
            stream.record_request_pseudo_header(http2_request_pseudo_header::method);
            return true;
        }
        if (name == ":protocol") {
            if (stream.has_protocol() || !is_valid_http_header_name(value)) {
                return false;
            }
            stream.set_protocol(value);
            stream.record_request_pseudo_header(http2_request_pseudo_header::protocol);
            return true;
        }
        if (name == ":scheme") {
            if (stream.has_scheme() || !is_valid_uri_scheme(value)) {
                return false;
            }
            stream.assign_request_scheme(value);
            stream.mark_scheme(http_uri_scheme_default_port(value));
            stream.record_request_pseudo_header(http2_request_pseudo_header::scheme);
            return true;
        }
        if (name == ":authority") {
            if (stream.has_authority() || !is_valid_uri_authority(value)) {
                return false;
            }
            stream.assign_request_authority(value);
            stream.mark_authority();
            stream.record_request_pseudo_header(http2_request_pseudo_header::authority);
            return true;
        }
        if (name == ":path") {
            if (stream.has_path() || value.empty()) {
                return false;
            }
            stream.assign_request_path(value);
            stream.mark_path();
            stream.record_request_pseudo_header(http2_request_pseudo_header::path);
            return true;
        }
        return false;
    }

    if (!http2_is_valid_regular_header(name, value)) {
        return false;
    }
    stream.mark_regular_header_seen();
    const auto kind = classify_http_field(name);
    if (validate_request_field(kind, value, http_field_list_role::recipient, context_value.request_fields_) !=
        request_field_status::ok) {
        return false;
    }
    switch (kind) {
        case http_field_kind::host:
            if (stream.has_authority() &&
                !authority_matches_host(stream.request_authority(), value, stream.scheme_default_port())) {
                return false;
            }
            stream.mark_host(stream.remote_header_count());
            break;
        case http_field_kind::cookie:
            return http2_append_cookie_header_value(stream, value);
        case http_field_kind::expect:
            // Expect is an extensible semantic list. Preserve unsupported but
            // syntactically valid members for the Web product's 417 policy.
            stream.parse_request_expectation_field(value);
            break;
        case http_field_kind::content_length: {
            http_content_length_state content_length;
            if (content_length.parse_field(value) != http_content_length_parse_status::ok ||
                !stream.declare_remote_content_length(*content_length.value())) {
                return false;
            }
            break;
        }
        default:
            break;
    }
    return stream.append_remote_header(name, value, kind);
}

[[nodiscard]] inline bool http2_on_decoded_request_trailer(
    http2_header_decode_context& context_value, std::string_view name, std::string_view value) {
    if (!http2_accumulate_header_list_bytes(context_value, name, value)) {
        return false;
    }

    // Pseudo-header and connection-specific fields, including a TE value other
    // than "trailers", are malformed (RFC 9113 sections 8.1 and 8.2.2). A field
    // merely not permitted as a trailer binds only the sender (RFC 9110 section
    // 6.5.1) and is discarded.
    if (!http2_is_valid_regular_header(name, value) ||
        (name == "te" && !is_valid_binary_http_te_field_value(value, http_field_list_role::recipient))) {
        return false;
    }
    return is_forbidden_http_request_trailer_name(name) || context_value.stream_.append_remote_trailer(name, value);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] inline bool http2_is_pending_websocket_connect(const http2_stream_state& stream) noexcept {
    const auto* pending = stream.tunnel().pending();
    return pending != nullptr && pending->form() == http2_connect_form::extended &&
           stream.protocol_is_websocket();
}

[[nodiscard]] inline bool http2_websocket_connect_remote_head_ended(
    const http2_stream_state& stream) noexcept {
    const auto& remote = stream.remote_receive();
    const auto* pending_end = remote.connect_pending_end_stream();
    return remote.head_end_stream_pending() != nullptr ||
           (pending_end != nullptr && pending_end->ended_without_tunnel_data());
}

[[nodiscard]] inline bool http2_websocket_connect_content_acceptable(
    const http2_stream_state& stream) noexcept {
    const auto* known = stream.remote_content().allowed_known_length();
    return stream.remote_content().allowed_without_length() != nullptr ||
           (known != nullptr && known->declared_length() == 0);
}

[[nodiscard]] inline websocket_handshake_validation_result validate_http2_websocket_handshake(
    const http2_stream_state& stream, const http_request& request,
    websocket_handshake_validation_options options = {}) noexcept {
    if (!http2_is_pending_websocket_connect(stream) || !http2_remote_final_head_decoded(stream) ||
        http2_websocket_connect_remote_head_ended(stream) ||
        !http2_websocket_connect_content_acceptable(stream)) {
        return websocket_handshake_validation_result::make_invalid_request();
    }

    // RFC 8441 section 5: :protocol supersedes the RFC 6455 key exchange, so
    // Sec-WebSocket-Key and Sec-WebSocket-Accept are not processed here.
    return validate_extended_connect_websocket_headers(request, options);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Queue records for the events whose public form the facade materializes from
// stream storage or binds to its owner endpoint when the event is taken.
struct http2_message_head_record final {
    std::uint32_t stream_id_{0};
    std::optional<http_client_request_content_signal> request_content_signal_;
    // The HEADERS frame left the peer send half open. Server side: message
    // content events follow this head (a pending CONNECT reads as closed).
    // Client side: the final response head did not carry END_STREAM. Captured
    // when the head is published; the same input batch may already have
    // delivered later frames to the core.
    bool body_open_{false};
    bool initial_end_stream_{false};
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
        http_priority_update, http_origin_advertisement, http_alternative_service_advertisement,
        http2_received_settings, http2_received_window_update, http2_received_priority>;

    template <typename event_type>
        requires(!std::is_same_v<event_type, http2_event>)
    explicit http2_event(event_type event) noexcept
        : value_(std::move(event)) {}
    http2_event(const http2_event&) = delete;
    http2_event& operator=(const http2_event&) = delete;
    http2_event(http2_event&&) noexcept = default;
    http2_event& operator=(http2_event&&) = delete;

    // Visits each stream id whose storage the owner may read when taking this event.
    template <typename visitor_type>
    void for_each_referenced_stream(visitor_type&& visitor) const noexcept {
        std::visit(
            [&visitor](const auto& event) noexcept {
                using event_type = std::decay_t<decltype(event)>;
                if constexpr (std::is_same_v<event_type, http2_push_promise_event>) {
                    visitor(event.associated_stream_id_);
                    visitor(event.promised_stream_id_);
                } else if constexpr (requires { event.stream_id(); }) {
                    visitor(event.stream_id());
                } else if constexpr (std::is_same_v<event_type, http2_message_head_record> ||
                                     std::is_same_v<event_type, http2_message_data_record> ||
                                     std::is_same_v<event_type, http2_message_end_record> ||
                                     std::is_same_v<event_type, http2_tunnel_data_record>) {
                    visitor(event.stream_id_);
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

// Sole owner of HTTP/2 outbound bytes and their consumed prefix. nghttp2
// serializes frames; the connection appends each frame it emits as one segment
// so batches stay frame-aligned and DATA payload bytes stay attributable.
class http2_output_buffer final {
public:
    struct segment_type final {
        // DATA payload spans [begin + http2_frame_header_bytes, end). Raw byte
        // segments have no DATA metadata.
        std::size_t begin_;
        std::size_t end_;
        std::uint32_t stream_id_;
        bool is_data_;
    };
    http2_output_buffer() noexcept = default;

    [[nodiscard]] bool wants_write() const noexcept {
        return consumed_ < bytes_.size();
    }

    // Appends one complete frame (or the client connection preface) as a
    // segment; both reservations precede the copy, so a throwing allocation
    // leaves the pending output unchanged.
    void append_segment(std::string_view bytes_value, std::uint32_t stream_id, bool is_data) {
        if (bytes_value.empty()) {
            return;
        }
        // Reclaim an amortizable prefix before new frames grow the live suffix.
        compact_consumed_prefix();
        reserve_segment();
        if (bytes_value.size() > bytes_.max_size() - bytes_.size()) {
            throw std::length_error("HTTP/2 output buffer size overflow");
        }
        reserve_geometric(bytes_, bytes_.size() + bytes_value.size());
        const auto begin = bytes_.size();
        bytes_.append(bytes_value.data(), bytes_value.size());
        segments_.push_back(segment_type{begin, bytes_.size(), stream_id, is_data});
    }

    [[nodiscard]] http2_output_batch_result take_batch(std::size_t max_bytes, std::string& into,
        http2_data_output_observer_type observer, void* observer_context) {
        if (consumed_ == bytes_.size()) {
            return {http2_output_batch_status::empty, 0};
        }
        // Every append records a segment, and the cursor only advances by whole
        // segments, so the first pending segment always begins at consumed_.
        std::size_t end = consumed_;
        std::size_t segment_count = 0;
        for (std::size_t index = segment_offset_; index < segments_.size(); ++index) {
            const auto& segment = segments_[index];
            if (segment_count != 0 && segment.end_ - consumed_ > max_bytes) {
                break;
            }
            end = segment.end_;
            ++segment_count;
            if (end - consumed_ >= max_bytes) {
                break;
            }
        }
        // Only a storage handoff benefits from compacting this batch in place.
        const auto batch_bytes = end - consumed_;
        const bool complete_batch = end == bytes_.size();
        const bool transferable = into.empty();
        if (complete_batch && transferable) {
            compact_consumed_prefix();
        }
        const auto first_segment = segment_offset_;
        const bool transfer_storage = consumed_ == 0 && complete_batch && transferable;
        if (transfer_storage) {
            into.swap(bytes_);
        } else {
            into.append(bytes_.data() + consumed_, batch_bytes);
        }
        if (observer != nullptr) {
            ++observer_depth_;
        }
        struct observer_depth_guard final {
            http2_output_buffer& buffer_;
            bool active_;
            ~observer_depth_guard() {
                if (!active_) {
                    return;
                }
                --buffer_.observer_depth_;
                if (buffer_.observer_depth_ == 0 &&
                    buffer_.segment_offset_ == buffer_.segments_.size()) {
                    buffer_.segments_.clear();
                    buffer_.segment_offset_ = 0;
                }
            }
        } guard{*this, observer != nullptr};
        if (transfer_storage) {
            reset_after_drain();
        } else {
            consume_batch(batch_bytes, segment_count);
        }
        if (observer != nullptr) {
            // A callback may append or drain more frames. Their vector may
            // reallocate, so retain indices and snapshot each entry by value.
            for (std::size_t i = 0; i < segment_count; ++i) {
                const auto segment = segments_[first_segment + i];
                if (segment.is_data_) {
                    observer(observer_context, segment.stream_id_,
                        segment.end_ - segment.begin_ - http2_frame_header_bytes);
                }
            }
        }
        return {http2_output_batch_status::taken, batch_bytes};
    }

    [[nodiscard]] std::size_t pending_data_bytes(std::uint32_t stream_id) const noexcept {
        std::size_t total = 0;
        for (std::size_t index = segment_offset_; index < segments_.size(); ++index) {
            const auto& segment = segments_[index];
            if (segment.is_data_ && segment.stream_id_ == stream_id) {
                total += segment.end_ - segment.begin_ - http2_frame_header_bytes;
            }
        }
        return total;
    }

private:
    // Advances the cursor past whole segments; the cursor always stays on a
    // segment boundary, so no partial-segment bookkeeping exists.
    void consume_batch(std::size_t bytes_value, std::size_t segment_count) noexcept {
        if (bytes_value == bytes_.size() - consumed_) {
            reset_after_drain();
            return;
        }
        consumed_ += bytes_value;
        segment_offset_ += segment_count;
    }

    void reset_after_drain() noexcept {
        bytes_.clear();
        if (observer_depth_ == 0) {
            segments_.clear();
            segment_offset_ = 0;
        } else {
            segment_offset_ = segments_.size();
        }
        consumed_ = 0;
    }

    void compact_consumed_prefix() noexcept {
        if (observer_depth_ != 0 || consumed_ < 64 * 1024 || consumed_ < bytes_.size() - consumed_) {
            return;
        }
        const auto prefix = consumed_;
        bytes_.erase(0, prefix);
        for (std::size_t index = segment_offset_; index < segments_.size(); ++index) {
            auto& segment = segments_[index];
            segment.begin_ = segment.begin_ > prefix ? segment.begin_ - prefix : 0;
            segment.end_ -= prefix;
        }
        segments_.erase(segments_.begin(),
            segments_.begin() + static_cast<std::ptrdiff_t>(segment_offset_));
        segment_offset_ = 0;
        consumed_ = 0;
    }

    void reserve_segment() {
        if (observer_depth_ == 0 && segment_offset_ >= 32 &&
            segment_offset_ >= segments_.size() - segment_offset_) {
            const auto remaining = segments_.size() - segment_offset_;
            std::move(segments_.begin() + static_cast<std::ptrdiff_t>(segment_offset_),
                segments_.end(), segments_.begin());
            segments_.resize(remaining);
            segment_offset_ = 0;
        }
        // A burst of frames shares one geometric growth policy.
        reserve_geometric(segments_, segments_.size() + 1);
    }

    std::string bytes_;
    std::vector<segment_type> segments_;
    std::size_t segment_offset_{0};
    std::size_t consumed_{0};
    std::size_t observer_depth_{0};
};

// RFC 9113 §5.1.1: stream identifiers are 31-bit values.
inline constexpr std::uint32_t http2_max_stream_id = 0x7fffffffU;

// One compile-time source of truth for every local receive constraint. These values
// drive both the advertised SETTINGS and the matching session options and fixed
// capacities; they are intentionally not runtime knobs that can diverge.
struct http2_local_settings final {
    static constexpr std::uint32_t header_table_size = 4096;
    static constexpr std::uint32_t max_concurrent_streams = 128;
    static constexpr std::uint32_t initial_window_size = 1024 * 1024;
    static constexpr std::uint32_t max_frame_size = http2_default_max_frame_size;
    static constexpr std::uint32_t max_header_list_size =
        static_cast<std::uint32_t>(max_http_header_bytes);
    static constexpr std::uint32_t enable_connect_protocol = 1;
};

static_assert(http2_local_settings::initial_window_size <=
              static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));

class http2_stream_table final {
public:
    // Hard upper bound on live stream records. Reserved push promises do not hold a
    // concurrency slot, so this can exceed SETTINGS_MAX_CONCURRENT_STREAMS.
    static constexpr std::size_t max_streams =
        static_cast<std::size_t>(http2_local_settings::max_concurrent_streams) * 4;
    static constexpr std::size_t max_push_streams =
        max_streams - static_cast<std::size_t>(http2_local_settings::max_concurrent_streams);

    http2_stream_table() noexcept = default;

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] http2_stream_state* find(std::uint32_t stream_id) & noexcept {
        if (stream_id == 0) {
            return nullptr;
        }
        for (auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                return &*slot;
            }
        }
        const auto* const ids = overflow_ids_.data();
        const auto count = overflow_ids_.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == stream_id) {
                return overflow_[i].get();
            }
        }
        return nullptr;
    }
    [[nodiscard]] http2_stream_state* find(std::uint32_t) && = delete;

    [[nodiscard]] const http2_stream_state* find(std::uint32_t stream_id) const& noexcept {
        if (stream_id == 0) {
            return nullptr;
        }
        for (const auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                return &*slot;
            }
        }
        const auto* const ids = overflow_ids_.data();
        const auto count = overflow_ids_.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == stream_id) {
                return overflow_[i].get();
            }
        }
        return nullptr;
    }
    [[nodiscard]] const http2_stream_state* find(std::uint32_t) const&& = delete;

    // NOTE on slot reuse: a closed stream's http2_stream_state is destroyed here (inline
    // slot .reset() / overflow erase), freeing its per-stream strings. We do NOT
    // pool the storage and reset-in-place to retain capacity, deliberately. A correct
    // reset_for_reuse would have to reinitialise ~85 fields across 8 sub-objects, and a
    // single missed field would leak one request's decoded headers / routing / body
    // into the next reused slot -- a cross-request data-disclosure risk not worth taking
    // for a micro-optimisation off the measured hot path.
    [[nodiscard]] http2_stream_state* create(std::uint32_t stream_id) & {
        if (auto* existing = find(stream_id); existing != nullptr) {
            return existing;
        }
        const bool is_push = (stream_id & 1U) == 0;
        if (size_ >= max_streams || (is_push && push_size_ >= max_push_streams)) {
            return nullptr;
        }
        for (auto& slot : inline_) {
            if (!slot) {
                slot.emplace(stream_id);
                ++size_;
                if (is_push) {
                    ++push_size_;
                }
                return &*slot;
            }
        }
        auto stream = std::make_unique<http2_stream_state>(stream_id);
        reserve_geometric(overflow_ids_, overflow_ids_.size() + 1);
        reserve_geometric(overflow_, overflow_.size() + 1);
        auto* result_value = stream.get();
        overflow_ids_.push_back(stream_id);
        overflow_.push_back(std::move(stream));
        ++size_;
        if (is_push) {
            ++push_size_;
        }
        return result_value;
    }
    [[nodiscard]] http2_stream_state* create(std::uint32_t) && = delete;

    bool remove(std::uint32_t stream_id) noexcept {
        if (stream_id == 0) {
            return false;
        }
        for (auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                slot.reset();
                --size_;
                if ((stream_id & 1U) == 0) {
                    --push_size_;
                }
                return true;
            }
        }
        const auto* const ids = overflow_ids_.data();
        const auto count = overflow_ids_.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == stream_id) {
                erase_overflow_at(i);
                --size_;
                if ((stream_id & 1U) == 0) {
                    --push_size_;
                }
                return true;
            }
        }
        return false;
    }

    template <typename callback>
    void for_each(callback&& callback_value) {
        snapshot_iteration_guard_type guard_value(*this);
        for (auto& slot : inline_) {
            if (slot) {
                callback_value(*slot);
            }
        }
        const auto overflow_end = overflow_.size();
        for (std::size_t i = 0; i < overflow_end; ++i) {
            auto& stream = overflow_[i];
            if (stream != nullptr) {
                callback_value(*stream);
            }
        }
    }

private:
    // Two inline slots cover the typical request/response cadence without
    // paying the full concurrent-stream budget up front: each slot embeds a
    // ~1.3 KB http2_stream_state, and the table lives in the connection object
    // for the connection's whole lifetime, so the two slots put ~2.6 KB into
    // every connection. Deeper multiplexing spills to the overflow path.
    static constexpr std::size_t inline_capacity = 2;
    using overflow_stream_type = std::unique_ptr<http2_stream_state>;

    class snapshot_iteration_guard_type final {
    public:
        explicit snapshot_iteration_guard_type(http2_stream_table& table_value) noexcept
            : table_(table_value) {
            ++table_.snapshot_iteration_depth_;
        }

        snapshot_iteration_guard_type(const snapshot_iteration_guard_type&) = delete;
        snapshot_iteration_guard_type& operator=(const snapshot_iteration_guard_type&) = delete;

        ~snapshot_iteration_guard_type() {
            --table_.snapshot_iteration_depth_;
            table_.compact_overflow_if_idle();
        }

    private:
        http2_stream_table& table_;
    };

    void erase_overflow_at(std::size_t index) noexcept {
        if (snapshot_iteration_depth_ != 0) {
            overflow_ids_[index] = 0;
            overflow_[index].reset();
            overflow_needs_compact_ = true;
            return;
        }
        if (index + 1 != overflow_.size()) {
            overflow_ids_[index] = overflow_ids_.back();
            overflow_[index] = std::move(overflow_.back());
        }
        overflow_ids_.pop_back();
        overflow_.pop_back();
    }

    void compact_overflow_if_idle() noexcept {
        if (snapshot_iteration_depth_ != 0 || !overflow_needs_compact_) {
            return;
        }
        for (std::size_t i = 0; i < overflow_.size();) {
            if (overflow_[i] == nullptr) {
                if (i + 1 != overflow_.size()) {
                    overflow_ids_[i] = overflow_ids_.back();
                    overflow_[i] = std::move(overflow_.back());
                }
                overflow_ids_.pop_back();
                overflow_.pop_back();
                continue;
            }
            ++i;
        }
        overflow_needs_compact_ = false;
    }

    std::array<std::optional<http2_stream_state>, inline_capacity> inline_{};
    std::vector<std::uint32_t> overflow_ids_;
    std::vector<overflow_stream_type> overflow_;
    std::size_t size_{0};
    std::size_t push_size_{0};
    std::size_t snapshot_iteration_depth_{0};
    bool overflow_needs_compact_{false};
};

// The local SETTINGS frame of the connection preface. Only a client may
// advertise ENABLE_PUSH=1 (RFC 9113 §6.5.2).
[[nodiscard]] inline std::array<nghttp2_settings_entry, 8> http2_local_settings_entries(bool enable_push) noexcept {
    return {{
        {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, http2_local_settings::header_table_size},
        {NGHTTP2_SETTINGS_ENABLE_PUSH, enable_push ? 1U : 0U},
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, http2_local_settings::max_concurrent_streams},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, http2_local_settings::initial_window_size},
        {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, http2_local_settings::max_frame_size},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, http2_local_settings::max_header_list_size},
        {NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, http2_local_settings::enable_connect_protocol},
        {NGHTTP2_SETTINGS_NO_RFC7540_PRIORITIES, 1},
    }};
}

// The ALTSVC (RFC 7838 §4) encoder. The size function validates the frame against
// `max_frame_size` and returns its payload size; the write function fills
// exactly that many bytes in the caller's destination.
[[nodiscard]] std::variant<std::size_t, http_connection_advertisement_error> http2_alternative_service_payload_size(
    std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size);
void http2_write_alternative_service_payload(
    std::span<char> payload, std::string_view origin, std::string_view field_value) noexcept;

class http2_local_connection_open final {};

// A server drains in two phases (RFC 9113 section 6.8). During the round trip the
// advertised boundary is 2^31-1 and peer streams are still admitted up to
// round_trip_stream_limit(); after the PING ACK (or that limit) the boundary is the
// real last peer stream id and is final. A client drains in the final phase at once.
class http2_local_connection_graceful_drain final {
public:
    [[nodiscard]] constexpr std::uint32_t last_stream_id() const noexcept {
        return last_stream_id_;
    }
    [[nodiscard]] constexpr bool awaiting_round_trip() const noexcept {
        return awaiting_round_trip_;
    }
    // Highest peer stream id still admitted while awaiting the round trip.
    [[nodiscard]] constexpr std::uint32_t round_trip_stream_limit() const noexcept {
        return round_trip_stream_limit_;
    }

private:
    friend class http2_local_connection_state;

    constexpr http2_local_connection_graceful_drain(std::uint32_t last_stream_id,
        bool awaiting_round_trip, std::uint32_t round_trip_stream_limit) noexcept
        : last_stream_id_(last_stream_id),
          round_trip_stream_limit_(round_trip_stream_limit),
          awaiting_round_trip_(awaiting_round_trip) {}

    std::uint32_t last_stream_id_;
    std::uint32_t round_trip_stream_limit_;
    bool awaiting_round_trip_;
};

class http2_local_connection_fatal_failure final {
public:
    [[nodiscard]] constexpr http2_error_code error() const noexcept {
        return error_;
    }

private:
    friend class http2_local_connection_state;

    explicit constexpr http2_local_connection_fatal_failure(http2_error_code error) noexcept
        : error_(error) {}

    http2_error_code error_;
};

// The locally initiated connection lifecycle owns its GOAWAY meaning. A graceful
// drain alone carries the advertised stream boundary; a fatal protocol failure
// alone carries an error code and atomically supersedes any earlier drain.
// Peer GOAWAY and preface progress are directional/orthogonal state elsewhere.
class http2_local_connection_state final {
public:
    [[nodiscard]] constexpr const http2_local_connection_open* open() const& noexcept {
        return std::get_if<http2_local_connection_open>(&state_);
    }
    const http2_local_connection_open* open() const&& = delete;

    [[nodiscard]] constexpr const http2_local_connection_graceful_drain* graceful_drain()
        const& noexcept {
        return std::get_if<http2_local_connection_graceful_drain>(&state_);
    }
    const http2_local_connection_graceful_drain* graceful_drain() const&& = delete;

    [[nodiscard]] constexpr const http2_local_connection_fatal_failure* fatal_failure() const& noexcept {
        return std::get_if<http2_local_connection_fatal_failure>(&state_);
    }
    const http2_local_connection_fatal_failure* fatal_failure() const&& = delete;

    [[nodiscard]] bool begin_graceful_drain(std::uint32_t last_stream_id) noexcept {
        if (open() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_connection_graceful_drain(last_stream_id, false, last_stream_id));
        return true;
    }

    [[nodiscard]] bool begin_round_trip_drain(
        std::uint32_t advertised_stream_id, std::uint32_t round_trip_stream_limit) noexcept {
        if (open() == nullptr) {
            return false;
        }
        state_ = state_type(
            http2_local_connection_graceful_drain(advertised_stream_id, true, round_trip_stream_limit));
        return true;
    }

    [[nodiscard]] bool finish_round_trip_drain(std::uint32_t last_stream_id) noexcept {
        const auto* drain = graceful_drain();
        if (drain == nullptr || !drain->awaiting_round_trip()) {
            return false;
        }
        state_ = state_type(http2_local_connection_graceful_drain(last_stream_id, false, last_stream_id));
        return true;
    }

    void fail(http2_error_code error) noexcept {
        state_ = state_type(http2_local_connection_fatal_failure(error));
    }

private:
    using state_type = std::variant<http2_local_connection_open, http2_local_connection_graceful_drain,
        http2_local_connection_fatal_failure>;

    state_type state_;
};

// Owner-lease releases and DATA credits that could not be applied yet. A pinned
// stream is released only after its delivered DATA credit and queued events are
// gone; a release or credit whose application failed is retried later. Both sets
// are bounded by the stream-table capacity, so they never allocate.
class http2_stream_lease_ledger final {
public:
    enum class release_kind_type : std::uint8_t {
        after_credits,
        after_remote_end,
        abandon,
    };
    struct release_type final {
        std::uint32_t stream_id_;
        release_kind_type kind_;
    };
    struct credit_type final {
        std::uint32_t stream_id_;
        std::uint32_t bytes_;
    };

    [[nodiscard]] const release_type* release(std::uint32_t stream_id) const noexcept {
        for (std::size_t index = 0; index < release_count_; ++index) {
            if (releases_[index].stream_id_ == stream_id) {
                return &releases_[index];
            }
        }
        return nullptr;
    }

    // abandon and after_remote_end override a pending release; after_credits only
    // replaces abandon, whose reset has then been sent.
    void defer_release(std::uint32_t stream_id, release_kind_type kind) noexcept {
        for (std::size_t index = 0; index < release_count_; ++index) {
            auto& existing = releases_[index];
            if (existing.stream_id_ != stream_id) {
                continue;
            }
            if (kind != release_kind_type::after_credits || existing.kind_ == release_kind_type::abandon) {
                existing.kind_ = kind;
            }
            return;
        }
        if (release_count_ == releases_.size()) {
            std::terminate();
        }
        releases_[release_count_++] = release_type{stream_id, kind};
    }

    void erase_release(std::uint32_t stream_id) noexcept {
        for (std::size_t index = 0; index < release_count_; ++index) {
            if (releases_[index].stream_id_ == stream_id) {
                releases_[index] = releases_[--release_count_];
                return;
            }
        }
    }

    [[nodiscard]] std::size_t release_count() const noexcept {
        return release_count_;
    }
    [[nodiscard]] const release_type& release_at(std::size_t index) const noexcept {
        return releases_[index];
    }

    void defer_credit(std::uint32_t stream_id, std::uint32_t bytes) noexcept {
        for (std::size_t index = 0; index < credit_count_; ++index) {
            auto& existing = credits_[index];
            if (existing.stream_id_ != stream_id) {
                continue;
            }
            if (bytes > (std::numeric_limits<std::uint32_t>::max)() - existing.bytes_) {
                std::terminate();
            }
            existing.bytes_ += bytes;
            return;
        }
        if (credit_count_ == credits_.size()) {
            std::terminate();
        }
        credits_[credit_count_++] = credit_type{stream_id, bytes};
    }

    void erase_credit(std::uint32_t stream_id) noexcept {
        for (std::size_t index = 0; index < credit_count_; ++index) {
            if (credits_[index].stream_id_ == stream_id) {
                credits_[index] = credits_[--credit_count_];
                return;
            }
        }
    }

    [[nodiscard]] std::size_t credit_count() const noexcept {
        return credit_count_;
    }
    [[nodiscard]] credit_type credit_at(std::size_t index) const noexcept {
        return credits_[index];
    }

private:
    std::array<release_type, http2_stream_table::max_streams> releases_;
    std::size_t release_count_{0};
    std::array<credit_type, http2_stream_table::max_streams> credits_;
    std::size_t credit_count_{0};
};

// An ORIGIN or ALTSVC payload handed to nghttp2_submit_extension(). It lives on
// the submitting call's stack; nghttp2 packs it while that call drains the
// session, and the single in-tree payload encoder writes exactly size_ bytes.
struct http2_extension_payload final {
    std::size_t size_{0};
    void (*write_)(const http2_extension_payload&, std::span<char>) noexcept {nullptr};
    std::span<const std::string_view> origins_{};
    std::string_view origin_{};
    std::string_view field_value_{};
};

class http2_connection final {
public:
    explicit http2_connection(http2_role role = http2_role::server,
        bool enable_push = false, bool receive_origin_advertisements = false, bool receive_frame_metadata = false,
        http2_frame_observer_type frame_observer = nullptr, http2_header_observer_type header_observer = nullptr,
        void* observer_context = nullptr);
    ~http2_connection();
    http2_connection(const http2_connection&) = delete;
    http2_connection& operator=(const http2_connection&) = delete;

    [[nodiscard]] http2_role role() const noexcept {
        return role_;
    }

    // --- inbound ---------------------------------------------------------------
    // Feed raw bytes read from the peer. nghttp2 consumes the whole span and
    // buffers any incomplete frame itself. Calling while events remain returns
    // events_pending and consumes nothing; drain peek_event()/consume_event()
    // first. protocol_failure is terminal. DATA events borrow the accepted span
    // until the next consuming feed().
    [[nodiscard]] http2_feed_result feed(std::string_view in);
    template <http_temporary_owning_char_string input_type>
    http2_feed_result feed(input_type&&) = delete;
    // The owner materializes the front event before consuming it, so an exception
    // never consumes the only protocol notification for a stream.
    [[nodiscard]] http2_event* peek_event() & noexcept;
    [[nodiscard]] http2_event* peek_event() && = delete;
    void consume_event() noexcept;

    // Access an assembled request head / stream for the owner to build an http_request.
    [[nodiscard]] http2_stream_state* stream(std::uint32_t stream_id) & noexcept;
    [[nodiscard]] http2_stream_state* stream(std::uint32_t) && = delete;
    // New outbound work is closed after a fatal connection error. Owner cleanup
    // uses the raw stream lookup so outstanding leases remain releasable.
    [[nodiscard]] http2_stream_state* find_outbound_stream(std::uint32_t stream_id) noexcept;
    [[nodiscard]] http2_stream_receive_status stream_receive_status(
        std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::optional<http2_server_request_view> server_request_view(
        std::uint32_t stream_id) const noexcept;

    // --- outbound --------------------------------------------------------------
    [[nodiscard]] http2_output_batch_result take_output_batch(std::size_t max_bytes,
        std::string& into, http2_data_output_observer_type observer, void* observer_context);
    [[nodiscard]] bool wants_write() const noexcept {
        return output_.wants_write();
    }

    // A stale/mismatched plan is rejected distinctly before any field or stream
    // mutation. Informational status codes and HTTP/2-unrepresentable control
    // semantics are rejected as well.
    [[nodiscard]] std::variant<http_buffered_response_write_plan, http2_response_head_submit_error>
    submit_response_head(std::uint32_t stream_id,
        const http_response& response, http_buffered_response_write_plan write_plan);
    [[nodiscard]] std::variant<http_response_stream_commit_plan, http2_response_head_submit_error>
    submit_streaming_response_head(
        std::uint32_t stream_id, http_response head,
        http_response_trailer_intent trailer_intent);
    [[nodiscard]] http2_data_submit_result submit_data(std::uint32_t stream_id,
        std::string_view chunk, http2_end_stream end_stream, std::size_t max_payload_bytes);
    [[nodiscard]] http2_submit_status submit_interim_response_head(
        std::uint32_t stream_id, const http_interim_response_head& response);
    [[nodiscard]] std::variant<websocket_extended_connect_negotiation, http2_websocket_handshake_submit_error>
    submit_websocket_handshake(
        std::uint32_t stream_id, const http_request& request,
        const websocket_handshake_validation_result& validation,
        websocket_server_handshake_options options);
    [[nodiscard]] http2_submit_status submit_connect_response_head(
        std::uint32_t stream_id, const http_response& response);
    [[nodiscard]] std::variant<std::uint32_t, http2_push_submit_error> submit_push_promise(
        std::uint32_t associated_stream_id, http_push_request_view request);
    [[nodiscard]] http2_finish_status finish_request(std::uint32_t stream_id,
        std::span<const http_header_view> trailers = {});
    [[nodiscard]] http2_finish_status finish_response(
        std::uint32_t stream_id, const http_response_trailer_section& trailers);
    [[nodiscard]] http2_submit_status submit_origin_advertisement(std::span<const std::string_view> origins);
    [[nodiscard]] http2_submit_status submit_alternative_service_advertisement(std::uint32_t stream_id,
        std::string_view origin, std::string_view field_value);
    [[nodiscard]] http2_submit_status submit_priority_update(std::uint32_t stream_id, http_priority_fields fields);

    [[nodiscard]] http2_submit_status submit_reset(std::uint32_t stream_id, http2_error_code error);
    [[nodiscard]] http2_submit_status submit_ping();
    [[nodiscard]] http2_submit_status submit_max_concurrent_streams(std::uint32_t limit);

    // --- lifecycle -------------------------------------------------------------
    // A connection error is terminal: its GOAWAY has been queued (unless the
    // failure was an allocation failure that left no room for it) and the I/O
    // owner must close the transport after flushing it. Graceful local/peer
    // GOAWAY is deliberately absent from this value.
    [[nodiscard]] std::optional<http2_error_code> connection_error() const noexcept {
        const auto* failure = local_connection_state_.fatal_failure();
        return failure != nullptr ? std::optional<http2_error_code>(failure->error()) : std::nullopt;
    }
    void begin_drain();
    void finish_drain();
    void terminate(http2_error_code error);
    [[nodiscard]] bool draining() const noexcept {
        return local_connection_state_.graceful_drain() != nullptr;
    }
    [[nodiscard]] bool drain_final() const noexcept {
        const auto* drain = local_connection_state_.graceful_drain();
        return drain != nullptr && !drain->awaiting_round_trip();
    }
    [[nodiscard]] bool header_block_in_progress() const noexcept {
        return header_block_in_progress_ || header_block_.has_value();
    }
    // Queues the role-specific connection preface (client magic, SETTINGS and the
    // connection WINDOW_UPDATE) exactly once.
    void begin_connection();

    // --- client role -------------------------------------------------------------
    [[nodiscard]] std::size_t available_local_request_streams(std::size_t local_limit) const noexcept;
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error>
    submit_regular_request_head(std::string_view method,
        std::string_view scheme, std::optional<std::string_view> authority, std::string_view path,
        std::span<const http_header_view> headers, http_request_content_plan content,
        http_client_request_expectation expectation = http_client_request_expectation::none,
        http_request_field_source source = http_request_field_source::generated);
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error>
    submit_connect_request_head(
        std::string_view authority, std::span<const http_header_view> headers = {},
        http_request_field_source source = http_request_field_source::generated);
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error>
    submit_extended_connect_request_head(
        std::string_view protocol, std::string_view scheme, std::string_view authority,
        std::string_view path, std::span<const http_header_view> headers = {},
        http_request_field_source source = http_request_field_source::generated);

    // A DATA event borrows bytes from the accepted input and retains the matching
    // receive-window debt; destroying its credit hands the bytes to
    // nghttp2_session_consume(). A no-op once the stream or its debt is gone;
    // a failed release is deferred to the lease ledger. Never throws.
    void return_received_data(std::uint32_t stream_id, std::uint32_t bytes) noexcept;
    [[nodiscard]] http2_request_content_release_status release_request_content(
        std::uint32_t stream_id) noexcept;
    [[nodiscard]] std::size_t pending_data_output_bytes(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::optional<http2_send_window_state> send_window_state(
        std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool stream_aborted(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool received_peer_settings() const noexcept {
        return received_peer_settings_;
    }

    // A request/response built from a stream holds views into its decoded
    // storage, so the stream must outlive an in-flight handler. While pinned, a
    // peer reset only marks it aborted (emitting stream_closed) instead of
    // freeing it.
    void pin_stream(std::uint32_t stream_id) noexcept;
    void lease_submitted_request(
        const std::variant<std::uint32_t, http2_request_head_submit_error>& result) noexcept;
    // Ends an owner lease. The stream is unpinned once its delivered DATA credit
    // and queued events are gone; until then the release stays deferred.
    void release_stream_lease(std::uint32_t stream_id);
    // Response/tunnel EOF releases the client lease only after both halves close.
    void release_stream_lease_after_remote_end(std::uint32_t stream_id) noexcept;
    // Destruction path of an unreleased lease: resets a live stream, then
    // releases it. Never throws; a failure is retried later.
    void abandon_stream_lease(std::uint32_t stream_id) noexcept;
    void retry_deferred_stream_lease(std::uint32_t stream_id) noexcept;
    void retry_deferred_stream_leases() noexcept;
    // Sends whatever nghttp2 queued outside a flushing operation (WINDOW_UPDATE
    // after a credit, RST_STREAM after an abandoned lease). Failures have
    // already failed the connection and are observed by the next operation.
    void flush_output() noexcept;

private:
    friend struct http2_session_callbacks;

    struct session_deleter final {
        void operator()(nghttp2_session* session) const noexcept {
            nghttp2_session_del(session);
        }
    };

    // The field block nghttp2 is currently delivering through on_header.
    enum class header_block_kind : std::uint8_t {
        request,
        response,
        request_trailers,
        response_trailers,
        push_promise,
    };
    struct header_block_type final {
        header_block_type(header_block_kind kind, http2_stream_state& stream) noexcept
            : kind_(kind),
              context_(stream) {}

        header_block_kind kind_;
        http2_header_decode_context context_;
        // Response heads: a 1xx keeps its fields separately from the final head.
        std::vector<http_header> informational_fields_;
        std::optional<http_received_status> status_;
        http_response_content_semantics content_semantics_{http_response_content_semantics::with_content};
        bool saw_regular_{false};
        // PUSH_PROMISE on a connected tunnel (RFC 9113 §8.5): decoded, then refused.
        bool reject_connected_tunnel_{false};
    };

    // One outbound DATA submission. It lives on submit_data()'s stack and
    // nghttp2 copies from it while the same call drains the session.
    struct outbound_data_type final {
        std::string_view bytes_;
        bool finished_{false};
    };

    // --- nghttp2 plumbing (callbacks run inside mem_recv2/mem_send2) ----------
    void create_session();
    // Drains every frame nghttp2 is ready to send into the output buffer and
    // surfaces any failure recorded by a callback.
    void flush_session();
    // Fails the connection for a negative nghttp2 return code and rethrows a
    // recorded callback exception or allocation failure.
    void fail_on_nghttp2_error(int error);
    // Submission results: fatal codes fail the connection and throw; any other
    // rejection is a caller contract violation (std::logic_error).
    void check_nghttp2(int result);
    // A local connection error: GOAWAY(error) through nghttp2, then terminal state.
    void fail_connection(http2_error_code error);
    void record_callback_failure() noexcept;

    int on_begin_frame(const nghttp2_frame_hd& header);
    nghttp2_ssize receive_observed(std::string_view input);
    void observe_frame_payload(std::string_view input) noexcept;
    void observe_header(http2_header_observation_phase phase,
        std::string_view name = {}, std::string_view value = {}) noexcept;
    int on_begin_headers(const nghttp2_frame& frame);
    int on_header(const nghttp2_frame& frame, std::string_view name, std::string_view value);
    int on_frame_recv(const nghttp2_frame& frame);
    int on_data_chunk_recv(std::uint8_t flags, std::uint32_t stream_id, std::string_view data);
    int on_stream_close(std::uint32_t stream_id, std::uint32_t error_code);
    int before_frame_send(const nghttp2_frame& frame);
    int on_frame_send(const nghttp2_frame& frame);
    int on_extension_chunk(const nghttp2_frame_hd& header, std::string_view data);

    int begin_request_headers(const nghttp2_frame& frame);
    int begin_response_headers(const nghttp2_frame& frame);
    int begin_push_promise(const nghttp2_frame& frame);
    // Validates one decoded field against the active block; false is malformed.
    [[nodiscard]] bool accept_header_field(std::string_view name, std::string_view value);
    void finish_header_block(const nghttp2_frame& frame);
    [[nodiscard]] bool finish_request_head(http2_stream_state& stream);
    [[nodiscard]] bool finish_response_head(http2_stream_state& stream, header_block_type& block);
    [[nodiscard]] bool finish_trailers(http2_stream_state& stream);
    void finish_push_promise(const nghttp2_frame& frame, header_block_type& block);
    // nghttp2 discards a PUSH_PROMISE whose associated stream closes before the
    // block ends; the promised stream it already reserved is cancelled here.
    void cancel_push_promise_on(std::uint32_t associated_stream_id);
    void finish_data_frame(http2_stream_state& stream);
    void process_rst_stream(const nghttp2_frame& frame);
    void process_goaway(const nghttp2_frame& frame);
    void process_settings(const nghttp2_frame& frame);
    void process_ping(const nghttp2_frame& frame);
    void process_priority_update(const nghttp2_frame& frame);
    void process_advertisement(const nghttp2_frame_hd& header);
    // A malformed field block: RST_STREAM(PROTOCOL_ERROR), plus the malformed
    // request-head budget for a server's initial head.
    void reject_header_block(std::uint32_t stream_id, bool malformed_request_head);

    // Emit message_head (and message_end when the peer already ended the stream).
    void emit_request_headers(http2_stream_state& stream);

    // --- stream lifecycle -------------------------------------------------------
    [[nodiscard]] http2_stream_state* create_stream(std::uint32_t stream_id);
    [[nodiscard]] std::optional<http2_request_head_submit_error> local_request_admission_error()
        const noexcept;
    // Queues the staged request fields as the next client stream's HEADERS.
    // A failure consumes neither a stream identifier nor a concurrency slot.
    template <typename prepare_type>
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_local_request_head(
        http2_end_stream end_stream, const prepare_type& prepare);
    void release_local_request_stream_if_closed(http2_stream_state& stream) noexcept;
    void release_local_request_stream(http2_stream_state& stream) noexcept;
    void unpin_stream(std::uint32_t stream_id);
    [[nodiscard]] bool release_received_data(std::uint32_t stream_id, std::uint32_t bytes);
    void abandon_stream_lease_checked(std::uint32_t stream_id);
    // Return consumed receive credit to nghttp2's WINDOW_UPDATE accounting.
    void consume_received(std::uint32_t stream_id, std::size_t bytes);
    // Preserve a removed stream's unacknowledged debt as connection credit.
    void flush_window_debt(http2_stream_state& stream);

    // Submits HEADERS for `stream` from the staged field list.
    void submit_header_frames(http2_stream_state& stream, http2_end_stream end_stream);
    [[nodiscard]] std::optional<http2_response_head_submit_error> response_head_admission_error(
        const http2_stream_state* stream, http_status_code status) const noexcept;
    [[nodiscard]] bool commit_final_response_head(http2_stream_state& stream, const http_response& response,
        const http_framed_response_head_plan& head_plan, http2_end_stream end_stream,
        std::optional<std::uint64_t> content_length);
    void commit_connect_response_head(http2_stream_state& stream, bool terminal_remote_half);
    void apply_response_priority(http2_stream_state& stream, const http_response_headers& headers);
    // Sends `bytes` (already clamped to both send windows) as DATA within this
    // call; nghttp2 copies them while the session drains.
    void send_data(std::uint32_t stream_id, std::string_view bytes, http2_end_stream end_stream);
    // Commits the local END_STREAM, then sends it as empty DATA or with the
    // staged trailer fields.
    void end_local_stream(http2_stream_state& stream, bool empty_data);
    [[nodiscard]] http2_submit_status submit_extension_frame(
        http2_frame_type type, std::uint32_t stream_id, http2_extension_payload& payload);
    // nghttp2 DATA source: copies the submitting call's bytes. A null source is
    // the empty END_STREAM a rejected client CONNECT owes, which may wait for
    // flow-control credit inside nghttp2.
    static nghttp2_ssize read_outbound_data(nghttp2_session* session, std::int32_t stream_id,
        std::uint8_t* buffer, std::size_t length, std::uint32_t* data_flags, nghttp2_data_source* source,
        void* user_data) noexcept;
    // An empty END_STREAM DATA frame needs positive connection and stream send
    // windows (nghttp2 defers zero-window DATA).
    [[nodiscard]] bool can_send_empty_end_stream(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::int32_t connection_send_window() const noexcept;
    [[nodiscard]] std::uint32_t peer_setting(nghttp2_settings_id id) const noexcept;

    // Appends an event into reserved capacity and counts it on every stream it names.
    void publish_event(http2_event event);

    // Peer-induced output is bounded without a clock; every limit ends in
    // GOAWAY(ENHANCE_YOUR_CALM). PING ACK, SETTINGS ACK and each RST_STREAM that
    // answers a peer frame count until output fully drains (Go x/net/http2
    // maxQueuedControlFrames); such resets also consume a lifetime budget, as h2
    // max_local_error_reset_streams.
    static constexpr std::uint32_t max_undrained_control_frames = 1000;
    static constexpr std::uint32_t max_peer_error_resets = 1024;
    // Rapid reset (CVE-2023-44487): peer resets may run this far ahead of the
    // responses this connection let complete.
    static constexpr std::uint32_t max_unprocessed_resets = 1000;
    // Pingora's lifetime bound on malformed server request heads.
    static constexpr std::uint32_t max_malformed_request_streams = 32;
    // CONTINUATION frames following one HEADERS/PUSH_PROMISE (CVE-2024-27316).
    static constexpr std::size_t max_continuation_frames = 1024;

    // Owner/local resets do not count against peer-induced budgets.
    void submit_owner_reset(std::uint32_t stream_id, http2_error_code error);
    // A stream error found in Ruvia's semantics: RST_STREAM and a local close event.
    void reset_stream_for_peer_error(std::uint32_t stream_id, http2_error_code error);

    enum class close_notification_type : std::uint8_t { emit_event,
        owner_already_knows };
    bool close_stream_impl(std::uint32_t stream_id, http2_stream_close_source source,
        http2_error_code error, close_notification_type notification);
    bool close_stream(std::uint32_t stream_id, http2_stream_close_source source, http2_error_code error);
    void finish_drain_round_trip();
    void begin_final_drain();
    void submit_goaway(std::uint32_t last_stream_id);

    http_c_library_memory memory_;
    nghttp2_mem memory_table_;
    std::unique_ptr<nghttp2_session, session_deleter> session_;
    http2_output_buffer output_;
    http2_field_list fields_;
    http2_stream_table streams_;
    // RFC 9218 priority signals received for streams the peer has not opened yet.
    struct idle_priority_entry final {
        std::uint32_t stream_id_{0};
        http_priority_fields fields_{};
    };
    std::vector<idle_priority_entry> priorities_;
    std::vector<http2_event> events_;
    std::size_t event_offset_{0};
    http2_stream_lease_ledger leases_;
    std::vector<std::uint32_t> owner_resets_;
    std::string extension_payload_;
    std::optional<header_block_type> header_block_;
    // A PUSH_PROMISE's request decodes into scratch storage; the promised
    // stream's own receive state awaits the pushed response.
    std::optional<http2_stream_state> push_scratch_;
    std::uint32_t push_associated_stream_id_{0};
    std::exception_ptr callback_failure_;

    http2_local_connection_state local_connection_state_;
    http2_role role_{http2_role::server};
    bool enable_push_{false};
    bool receive_origin_advertisements_{false};
    bool receive_frame_metadata_{false};
    http2_frame_observer_type frame_observer_{nullptr};
    http2_header_observer_type header_observer_{nullptr};
    void* observer_context_{nullptr};
    http2_frame_metadata observed_frame_{};
    std::uint64_t observed_next_sequence_{0};
    std::uint64_t observed_header_sequence_{0};
    std::size_t observed_field_index_{0};
    std::uint32_t observed_header_stream_{0};
    std::size_t observed_payload_remaining_{0};
    std::size_t observed_payload_offset_{0};
    std::array<unsigned char, 6> observed_scalar_{};
    std::size_t observed_scalar_size_{0};
    bool observed_headers_padding_{false};
    bool started_{false};
    bool received_peer_settings_{false};
    bool header_block_in_progress_{false};
    bool liveness_ping_outstanding_{false};
    std::uint32_t last_peer_stream_id_{0};
    std::uint32_t highest_peer_stream_id_{0};
    std::uint32_t active_local_request_streams_{0};
    std::optional<http2_goaway_event> peer_goaway_;
    std::size_t header_block_bytes_{0};

    std::uint32_t malformed_request_streams_{0};
    std::uint64_t peer_reset_streams_{0};
    std::uint64_t completed_responses_{0};
    std::uint32_t undrained_control_frames_{0};
    std::uint32_t peer_error_resets_{0};
};

}  // namespace ruvia::detail
