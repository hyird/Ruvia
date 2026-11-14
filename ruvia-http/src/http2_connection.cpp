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

#include "http2_connection.h"

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "http1_client_request.h"
#include "http1_client_response.h"
#include "http1_server_response.h"
#include "http2_internal.h"
#include "http_binary_message.h"
#include "http_connection_fields.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "websocket_handshake.h"

namespace ruvia::detail {

namespace {

constexpr std::string_view http2_drain_debug = "connection draining";
// Opaque payloads of the locally originated PINGs (RFC 9113 §6.7 echoes them
// unchanged); distinct values tell the graceful-drain round trip from liveness.
constexpr std::string_view http2_drain_ping_payload = "ruviadrn";
constexpr std::string_view http2_liveness_ping_payload = "ruvialiv";
static_assert(http2_drain_ping_payload.size() == 8 && http2_liveness_ping_payload.size() == 8);

[[nodiscard]] std::string_view bytes_view(const std::uint8_t* data, std::size_t size) noexcept {
    return size == 0 ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(data), size);
}

// Frame types that cannot advance a stream, judged from the header alone:
// empty DATA without END_STREAM is charged here because nghttp2 completes a
// zero-length frame only once later input arrives. Padding-only DATA is judged
// once received, a PING ACK by whether it answers our PING, and RST_STREAM by
// the Rapid-Reset budget. The first SETTINGS is the mandatory connection
// preface (RFC 9113 section 3.4).
[[nodiscard]] bool non_progress_frame_header(const nghttp2_frame_hd& header, bool received_peer_settings) noexcept {
    switch (header.type) {
        case NGHTTP2_DATA:
            return header.length == 0 && (header.flags & NGHTTP2_FLAG_END_STREAM) == 0;
        case NGHTTP2_HEADERS:
        case NGHTTP2_PUSH_PROMISE:
        case NGHTTP2_CONTINUATION:
        case NGHTTP2_RST_STREAM:
            return false;
        case NGHTTP2_SETTINGS:
            return (header.flags & NGHTTP2_FLAG_ACK) == 0 && received_peer_settings;
        case NGHTTP2_PING:
            return (header.flags & NGHTTP2_FLAG_ACK) == 0;
        default:
            return true;
    }
}

}  // namespace

// Static entry points registered with nghttp2. Each one is noexcept: an
// exception is recorded on the connection and reported to nghttp2 as a fatal
// callback failure, then rethrown after nghttp2 returns.
struct http2_session_callbacks final {
    template <typename function_type>
    static int guard(void* user_data, function_type&& function) noexcept {
        auto& connection = *static_cast<http2_connection*>(user_data);
        try {
            return std::forward<function_type>(function)(connection);
        } catch (...) {
            connection.record_callback_failure();
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    }

    static int on_begin_frame(nghttp2_session*, const nghttp2_frame_hd* header, void* user_data) {
        return guard(user_data, [header](http2_connection& connection) {
            return connection.on_begin_frame(*header);
        });
    }
    static int on_begin_headers(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
        return guard(user_data, [frame](http2_connection& connection) {
            return connection.on_begin_headers(*frame);
        });
    }
    static int on_header(nghttp2_session*, const nghttp2_frame* frame, nghttp2_rcbuf* name, nghttp2_rcbuf* value, std::uint8_t, void* user_data) {
        return guard(user_data, [frame, name, value](http2_connection& connection) {
            const auto name_buffer = nghttp2_rcbuf_get_buf(name);
            const auto value_buffer = nghttp2_rcbuf_get_buf(value);
            return connection.on_header(*frame, bytes_view(name_buffer.base, name_buffer.len), bytes_view(value_buffer.base, value_buffer.len));
        });
    }
    static int on_frame_recv(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
        return guard(user_data, [frame](http2_connection& connection) {
            return connection.on_frame_recv(*frame);
        });
    }
    static int on_data_chunk_recv(nghttp2_session*, std::uint8_t flags, std::int32_t stream_id, const std::uint8_t* data, std::size_t length, void* user_data) {
        return guard(user_data, [=](http2_connection& connection) {
            return connection.on_data_chunk_recv(flags, static_cast<std::uint32_t>(stream_id), bytes_view(data, length));
        });
    }
    static int on_stream_close(nghttp2_session*, std::int32_t stream_id, std::uint32_t error_code, void* user_data) {
        return guard(user_data, [=](http2_connection& connection) {
            return connection.on_stream_close(static_cast<std::uint32_t>(stream_id), error_code);
        });
    }
    static int before_frame_send(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
        return guard(user_data, [frame](http2_connection& connection) {
            return connection.before_frame_send(*frame);
        });
    }
    static int on_frame_send(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
        return guard(user_data, [frame](http2_connection& connection) {
            return connection.on_frame_send(*frame);
        });
    }
    static int on_extension_chunk(nghttp2_session*, const nghttp2_frame_hd* header, const std::uint8_t* data, std::size_t length, void* user_data) {
        return guard(user_data, [=](http2_connection& connection) {
            return connection.on_extension_chunk(*header, bytes_view(data, length));
        });
    }
    // ORIGIN and ALTSVC payloads are decoded from the accumulated chunks in
    // on_frame_recv, so no unpacked object is handed back to nghttp2.
    static int unpack_extension(nghttp2_session*, void** payload, const nghttp2_frame_hd*, void*) noexcept {
        *payload = nullptr;
        return 0;
    }
    static nghttp2_ssize pack_extension(nghttp2_session*, std::uint8_t* buffer, std::size_t length, const nghttp2_frame* frame, void*) noexcept {
        const auto& payload = *static_cast<const http2_extension_payload*>(frame->ext.payload);
        if (payload.size_ > length) {
            return NGHTTP2_ERR_CANCEL;
        }
        payload.write_(payload, std::span<char>(reinterpret_cast<char*>(buffer), payload.size_));
        return static_cast<nghttp2_ssize>(payload.size_);
    }
    // DATA frames may use the peer's whole SETTINGS_MAX_FRAME_SIZE (RFC 9113
    // §4.2) instead of nghttp2's 16 KiB default.
    static nghttp2_ssize read_length(nghttp2_session*, std::uint8_t, std::int32_t, std::int32_t session_window, std::int32_t stream_window, std::uint32_t remote_max_frame_size, void*) noexcept {
        const auto window = (std::min)(session_window, stream_window);
        return static_cast<nghttp2_ssize>((std::max)(std::int64_t{1}, (std::min)(static_cast<std::int64_t>(remote_max_frame_size), static_cast<std::int64_t>(window))));
    }
};

nghttp2_ssize http2_connection::read_outbound_data(nghttp2_session*, std::int32_t, std::uint8_t* buffer, std::size_t length, std::uint32_t* data_flags, nghttp2_data_source* source, void*) noexcept {
    auto* data = static_cast<outbound_data_type*>(source->ptr);
    if (data == nullptr) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    const auto copied = (std::min)(length, data->bytes_.size());
    if (copied != 0) {
        std::memcpy(buffer, data->bytes_.data(), copied);
        data->bytes_.remove_prefix(copied);
    }
    if (data->bytes_.empty()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        data->finished_ = true;
    }
    return static_cast<nghttp2_ssize>(copied);
}

http2_connection::http2_connection(http2_role role, const http2_connection_options& options) :
    memory_table_(make_c_library_mem_table<nghttp2_mem>(memory_)),
    role_(role) {
    enable_push_ = role == http2_role::client && options.enable_push_;
    receive_origin_advertisements_ = role == http2_role::client && options.receive_origin_advertisements_;
    receive_frame_metadata_ = options.receive_frame_metadata_;
    frame_observer_ = options.frame_observer_;
    header_observer_ = options.header_observer_;
    observer_context_ = options.observer_context_;
    max_non_progress_frames_ = options.max_non_progress_frames_;
    create_session();
}

http2_connection::~http2_connection() = default;

void http2_connection::create_session() {
    struct callbacks_deleter final {
        void operator()(nghttp2_session_callbacks* value) const noexcept {
            nghttp2_session_callbacks_del(value);
        }
    };
    struct option_deleter final {
        void operator()(nghttp2_option* value) const noexcept {
            nghttp2_option_del(value);
        }
    };
    nghttp2_session_callbacks* raw_callbacks = nullptr;
    if (nghttp2_session_callbacks_new(&raw_callbacks) != 0) {
        throw std::bad_alloc();
    }
    const std::unique_ptr<nghttp2_session_callbacks, callbacks_deleter> callbacks(raw_callbacks);
    nghttp2_session_callbacks_set_on_begin_frame_callback(callbacks.get(), &http2_session_callbacks::on_begin_frame);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks.get(), &http2_session_callbacks::on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback2(callbacks.get(), &http2_session_callbacks::on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks.get(), &http2_session_callbacks::on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks.get(), &http2_session_callbacks::on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks.get(), &http2_session_callbacks::on_stream_close);
    nghttp2_session_callbacks_set_before_frame_send_callback(callbacks.get(), &http2_session_callbacks::before_frame_send);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks.get(), &http2_session_callbacks::on_frame_send);
    nghttp2_session_callbacks_set_on_extension_chunk_recv_callback(callbacks.get(), &http2_session_callbacks::on_extension_chunk);
    nghttp2_session_callbacks_set_unpack_extension_callback(callbacks.get(), &http2_session_callbacks::unpack_extension);
    nghttp2_session_callbacks_set_pack_extension_callback2(callbacks.get(), &http2_session_callbacks::pack_extension);
    nghttp2_session_callbacks_set_data_source_read_length_callback2(callbacks.get(), &http2_session_callbacks::read_length);

    nghttp2_option* raw_option = nullptr;
    if (nghttp2_option_new(&raw_option) != 0) {
        throw std::bad_alloc();
    }
    const std::unique_ptr<nghttp2_option, option_deleter> option(raw_option);
    // Received DATA is credited only when the owner consumes it.
    nghttp2_option_set_no_auto_window_update(option.get(), 1);
    // Ruvia's request/response validators own HTTP message semantics.
    nghttp2_option_set_no_http_messaging(option.get(), 1);
    // RFC 9113 §6.5.2: no concurrency limit applies until the peer sets one.
    nghttp2_option_set_peer_max_concurrent_streams(option.get(), (std::numeric_limits<std::uint32_t>::max)());
    nghttp2_option_set_max_reserved_remote_streams(option.get(), static_cast<std::uint32_t>(http2_stream_table::max_push_streams));
    nghttp2_option_set_max_send_header_block_length(option.get(), max_http2_encoded_header_block_bytes);
    // The encoder never inserts dynamic-table entries.
    nghttp2_option_set_max_deflate_dynamic_table_size(option.get(), 0);
    nghttp2_option_set_max_settings(option.get(), http2_received_settings::max_frame_entries);
    nghttp2_option_set_max_continuations(option.get(), max_continuation_frames);
    nghttp2_option_set_max_outbound_ack(option.get(), max_undrained_control_frames);
    // The core has no clock: nghttp2's time-based limiters stay out of reach and
    // the deterministic budgets in this class bound abuse instead.
    constexpr auto unlimited = (std::numeric_limits<std::uint64_t>::max)();
    nghttp2_option_set_stream_reset_rate_limit(option.get(), unlimited, 0);
    nghttp2_option_set_glitch_rate_limit(option.get(), unlimited, 0);
    nghttp2_option_set_builtin_recv_extension_type(option.get(), NGHTTP2_PRIORITY_UPDATE);
    // Every other extension type (ALTSVC and ORIGIN included) goes through the
    // framing callbacks instead of being skipped unseen: on_begin_frame charges
    // it to the non-progress budget and gives a wire observer its position.
    // Unknown types remain semantically ignored (RFC 9113 section 5.5).
    for (unsigned type = 0x0a; type <= 0xff; ++type) {
        if (type != NGHTTP2_PRIORITY_UPDATE) {
            nghttp2_option_set_user_recv_extension_type(option.get(), static_cast<std::uint8_t>(type));
        }
    }

    nghttp2_session* raw_session = nullptr;
    const int result = role_ == http2_role::client ? nghttp2_session_client_new3(&raw_session, callbacks.get(), this, option.get(), &memory_table_) : nghttp2_session_server_new3(&raw_session, callbacks.get(), this, option.get(), &memory_table_);
    if (result == NGHTTP2_ERR_NOMEM) {
        memory_.rethrow_allocation_failure();
    }
    if (result != 0) {
        throw std::runtime_error("nghttp2 failed to create an HTTP/2 session");
    }
    session_.reset(raw_session);
}

void http2_connection::record_callback_failure() noexcept {
    if (!callback_failure_) {
        callback_failure_ = std::current_exception();
    }
}

void http2_connection::fail_connection(http2_error_code error) {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return;
    }
    header_block_in_progress_ = false;
    header_block_.reset();
    push_scratch_.reset();
    local_connection_state_.fail(error);
    // Queues GOAWAY(error) and makes nghttp2 ignore every later inbound frame.
    // An allocation failure here only loses the GOAWAY; the state is already terminal.
    (void)nghttp2_session_terminate_session(session_.get(), static_cast<std::uint32_t>(error));
}

void http2_connection::fail_on_nghttp2_error(int error) {
    auto failure = std::exchange(callback_failure_, nullptr);
    auto code = http2_error_code::internal_error;
    switch (error) {
        case NGHTTP2_ERR_FLOODED:
        case NGHTTP2_ERR_TOO_MANY_CONTINUATIONS:
        case NGHTTP2_ERR_SETTINGS_EXPECTED:
            code = error == NGHTTP2_ERR_SETTINGS_EXPECTED ? http2_error_code::protocol_error : http2_error_code::enhance_your_calm;
            break;
        case NGHTTP2_ERR_BAD_CLIENT_MAGIC:
            code = http2_error_code::protocol_error;
            break;
        default:
            break;
    }
    fail_connection(code);
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (error == NGHTTP2_ERR_NOMEM) {
        memory_.rethrow_allocation_failure();
    }
}

void http2_connection::check_nghttp2(int result) {
    if (result >= 0) {
        return;
    }
    if (result == NGHTTP2_ERR_NOMEM || nghttp2_is_fatal(result)) {
        fail_on_nghttp2_error(result);
        throw std::runtime_error(nghttp2_strerror(result));
    }
    throw std::logic_error(nghttp2_strerror(result));
}

void http2_connection::flush_session() {
    for (;;) {
        const std::uint8_t* data = nullptr;
        const auto produced = nghttp2_session_mem_send2(session_.get(), &data);
        if (produced < 0) {
            // A failed mem_send2 leaves nghttp2's outbound state indeterminate,
            // so nothing more is sent on this session.
            const auto error = static_cast<int>(produced);
            if (local_connection_state_.fatal_failure() == nullptr) {
                local_connection_state_.fail(http2_error_code::internal_error);
            }
            if (auto failure = std::exchange(callback_failure_, nullptr)) {
                std::rethrow_exception(failure);
            }
            if (error == NGHTTP2_ERR_NOMEM) {
                memory_.rethrow_allocation_failure();
            }
            throw std::runtime_error(nghttp2_strerror(error));
        }
        if (produced == 0) {
            break;
        }
        const auto bytes = bytes_view(data, static_cast<std::size_t>(produced));
        const auto header = parse_http2_frame_header(std::span<const char>(bytes.data(), bytes.size()));
        const bool preface = bytes.size() == http2_client_preface.size() && bytes == http2_client_preface;
        const bool is_data = !preface && header && header->type_ == static_cast<std::uint8_t>(http2_frame_type::data);
        try {
            output_.append_segment(bytes, is_data ? header->stream_id_ : 0, is_data);
        } catch (...) {
            // nghttp2 already counts these bytes as sent; the wire can no longer
            // be made consistent.
            if (local_connection_state_.fatal_failure() == nullptr) {
                local_connection_state_.fail(http2_error_code::internal_error);
            }
            throw;
        }
    }
}

void http2_connection::begin_connection() {
    if (started_) {
        return;
    }
    const auto entries = http2_local_settings_entries(enable_push_);
    check_nghttp2(nghttp2_submit_settings(session_.get(), NGHTTP2_FLAG_NONE, entries.data(), entries.size()));
    check_nghttp2(nghttp2_session_set_local_window_size(session_.get(), NGHTTP2_FLAG_NONE, 0, static_cast<std::int32_t>(http2_local_settings::initial_window_size)));
    started_ = true;
    flush_session();
}

// --- outbound byte buffer (batched writes) ------------------------------------

http2_output_batch_result http2_connection::take_output_batch(std::size_t max_bytes, std::string& into, http2_data_output_observer_type observer, void* observer_context) {
    const auto result_value = output_.take_batch(max_bytes, into, observer, observer_context);
    if (result_value.status_ == http2_output_batch_status::taken && !output_.wants_write()) {
        undrained_control_frames_ = 0;
    }
    return result_value;
}

// --- event queue --------------------------------------------------------------

http2_event* http2_connection::peek_event() & noexcept {
    if (event_offset_ < events_.size()) {
        return &events_[event_offset_];
    }
    events_.clear();
    event_offset_ = 0;
    return nullptr;
}

void http2_connection::consume_event() noexcept {
    if (event_offset_ >= events_.size()) {
        std::terminate();
    }
    // Snapshot the ids first: a lease retry may publish events and reallocate the queue.
    std::array<std::uint32_t, 2> stream_ids{};
    std::size_t stream_count = 0;
    events_[event_offset_++].for_each_referenced_stream([&](std::uint32_t stream_id) noexcept {
        stream_ids[stream_count++] = stream_id;
    });
    // Only the consumed event's streams can newly satisfy a deferred lease release.
    for (std::size_t index = 0; index < stream_count; ++index) {
        if (auto* stream = streams_.find(stream_ids[index])) {
            stream->remove_pending_event();
        }
        retry_deferred_stream_lease(stream_ids[index]);
    }
    flush_output();
}

void http2_connection::publish_event(http2_event event) {
    reserve_geometric(events_, events_.size() + 1);
    events_.push_back(std::move(event));
    events_.back().for_each_referenced_stream([this](std::uint32_t stream_id) noexcept {
        if (auto* stream = streams_.find(stream_id)) {
            stream->add_pending_event();
        }
    });
}

// --- inbound -----------------------------------------------------------------

http2_feed_result http2_connection::feed(std::string_view in) {
    // The facade begins the connection before it offers any input.
    if (!started_) {
        std::terminate();
    }
    // An unread event may carry a zero-copy view into the previous input, so
    // accepting another span here would invalidate its bytes.
    if (event_offset_ < events_.size()) {
        return http2_feed_result::events_pending;
    }
    if (local_connection_state_.fatal_failure() != nullptr) {
        return http2_feed_result::protocol_failure;
    }
    events_.clear();
    event_offset_ = 0;
    const auto consumed = frame_observer_ != nullptr ? receive_observed(in) : nghttp2_session_mem_recv2(session_.get(), reinterpret_cast<const std::uint8_t*>(in.data()), in.size());
    if (consumed < 0) {
        try {
            fail_on_nghttp2_error(static_cast<int>(consumed));
        } catch (...) {
            try {
                flush_session();
                // NOLINTNEXTLINE(bugprone-empty-catch): the original failure wins.
            } catch (...) {
            }
            throw;
        }
    }
    flush_session();
    return local_connection_state_.fatal_failure() != nullptr ? http2_feed_result::protocol_failure : http2_feed_result::accepted;
}

nghttp2_ssize http2_connection::receive_observed(std::string_view input) {
    const auto original_size = input.size();
    while (!input.empty()) {
        // nghttp2 is the only frame-header parser. Until on_begin_frame gives
        // us a payload length, offer one octet; afterwards offer whole payload
        // fragments. No magic/header bytes are interpreted or retained here.
        const auto count = observed_payload_remaining_ == 0 ? std::size_t{1} : (std::min)(input.size(), observed_payload_remaining_);
        if (observed_payload_remaining_ != 0) {
            observe_frame_payload(input.substr(0, count));
            observed_payload_remaining_ -= count;
            if (observed_payload_remaining_ == 0) {
                frame_observer_(observer_context_, observed_frame_);
            }
        }
        const auto consumed = nghttp2_session_mem_recv2(session_.get(), reinterpret_cast<const std::uint8_t*>(input.data()), count);
        if (consumed < 0) {
            return consumed;
        }
        if (consumed == 0 || local_connection_state_.fatal_failure() != nullptr) {
            break;
        }
        input.remove_prefix(static_cast<std::size_t>(consumed));
    }
    return static_cast<nghttp2_ssize>(original_size);
}

void http2_connection::observe_frame_payload(std::string_view input) noexcept {
    const auto type = observed_frame_.type_;
    const bool settings = type == http2_frame_type::settings;
    const bool window = type == http2_frame_type::window_update;
    const bool priority = type == http2_frame_type::priority || (type == http2_frame_type::headers && (observed_frame_.flags_ & NGHTTP2_FLAG_PRIORITY) != 0);
    if (!settings && !window && !priority) {
        return;
    }
    const auto scalar_bytes = settings ? std::size_t{6} : (window ? std::size_t{4} : std::size_t{5});
    for (const unsigned char byte : input) {
        const auto offset = observed_payload_offset_++;
        if (observed_headers_padding_ && offset == 0) {
            continue;
        }
        if (!settings && observed_scalar_size_ == scalar_bytes) {
            break;
        }
        observed_scalar_[observed_scalar_size_++] = byte;
        if (observed_scalar_size_ != scalar_bytes) {
            continue;
        }
        const auto read_u32 = [this](std::size_t start) noexcept {
            return (std::uint32_t{observed_scalar_[start]} << 24) | (std::uint32_t{observed_scalar_[start + 1]} << 16) | (std::uint32_t{observed_scalar_[start + 2]} << 8) | std::uint32_t{observed_scalar_[start + 3]};
        };
        if (settings) {
            if (observed_frame_.settings_count_ < observed_frame_.settings_.size()) {
                observed_frame_.settings_[observed_frame_.settings_count_++] = {static_cast<http2_setting_id>((std::uint16_t{observed_scalar_[0]} << 8) | observed_scalar_[1]), read_u32(2)};
            }
            observed_scalar_size_ = 0;
        } else if (window) {
            observed_frame_.window_increment_ = read_u32(0) & 0x7fffffffU;
        } else {
            const auto dependency = read_u32(0);
            observed_frame_.priority_ = http2_received_priority{.stream_id_ = observed_frame_.stream_id_, .stream_dependency_ = dependency & 0x7fffffffU, .weight_ = static_cast<std::uint16_t>(observed_scalar_[4] + 1), .exclusive_ = (dependency & 0x80000000U) != 0};
        }
    }
}

void http2_connection::observe_header(http2_header_observation_phase phase, std::string_view name, std::string_view value) noexcept {
    if (header_observer_ == nullptr) {
        return;
    }
    const http2_header_metadata metadata{.first_frame_observation_sequence_ = observed_header_sequence_, .frame_observation_sequence_ = observed_frame_.observation_sequence_, .stream_id_ = observed_header_stream_, .field_index_ = observed_field_index_, .phase_ = phase, .name_ = name, .value_ = value};
    header_observer_(observer_context_, metadata);
    if (phase == http2_header_observation_phase::field) {
        ++observed_field_index_;
    }
}

int http2_connection::on_begin_frame(const nghttp2_frame_hd& header) {
    if (frame_observer_ != nullptr || header_observer_ != nullptr) {
        observed_frame_ = {};
        observed_frame_.observation_sequence_ = observed_next_sequence_++;
        observed_frame_.payload_length_ = static_cast<std::uint32_t>(header.length);
        observed_frame_.stream_id_ = static_cast<std::uint32_t>(header.stream_id);
        observed_frame_.type_ = static_cast<http2_frame_type>(header.type);
        observed_frame_.flags_ = header.flags;
        observed_payload_remaining_ = header.length;
        observed_payload_offset_ = 0;
        observed_scalar_size_ = 0;
        observed_headers_padding_ = header.type == NGHTTP2_HEADERS && (header.flags & NGHTTP2_FLAG_PADDED) != 0;
        if (header.type == NGHTTP2_SETTINGS) {
            observed_frame_.settings_complete_ = header.length % 6 == 0 && header.length / 6 <= observed_frame_.settings_.size();
        }
        if (frame_observer_ != nullptr && header.length == 0) {
            frame_observer_(observer_context_, observed_frame_);
        }
    }
    if (non_progress_frame_header(header, received_peer_settings_)) {
        charge_non_progress_frame();
    }
    switch (header.type) {
        case NGHTTP2_HEADERS:
            if (role_ == http2_role::server && (header.stream_id & 1) != 0) {
                highest_peer_stream_id_ = (std::max)(highest_peer_stream_id_, static_cast<std::uint32_t>(header.stream_id));
            }
            header_block_bytes_ = header.length;
            header_block_in_progress_ = (header.flags & NGHTTP2_FLAG_END_HEADERS) == 0;
            break;
        case NGHTTP2_PUSH_PROMISE:
            header_block_bytes_ = header.length;
            header_block_in_progress_ = (header.flags & NGHTTP2_FLAG_END_HEADERS) == 0;
            break;
        case NGHTTP2_CONTINUATION:
            header_block_in_progress_ = (header.flags & NGHTTP2_FLAG_END_HEADERS) == 0;
            if (header.length > max_http2_encoded_header_block_bytes - header_block_bytes_) {
                // A partial block cannot be skipped without desynchronizing the
                // connection-wide HPACK state (RFC 9113 §4.3).
                fail_connection(http2_error_code::compression_error);
                break;
            }
            header_block_bytes_ += header.length;
            break;
        case static_cast<std::uint8_t>(http2_frame_type::alternative_service):
        case static_cast<std::uint8_t>(http2_frame_type::origin):
            header_block_in_progress_ = false;
            extension_payload_.clear();
            break;
        default:
            header_block_in_progress_ = false;
            break;
    }
    return 0;
}

int http2_connection::on_extension_chunk(const nghttp2_frame_hd& header, std::string_view data) {
    if (role_ == http2_role::client && (header.type == static_cast<std::uint8_t>(http2_frame_type::alternative_service) || (header.type == static_cast<std::uint8_t>(http2_frame_type::origin) && receive_origin_advertisements_))) {
        extension_payload_.append(data.data(), data.size());
    }
    return 0;
}

int http2_connection::on_frame_recv(const nghttp2_frame& frame) {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return 0;
    }
    switch (frame.hd.type) {
        case NGHTTP2_DATA: {
            const bool end_stream = (frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
            const auto content_bytes = frame.hd.length - frame.data.padlen;
            if (content_bytes == 0 && !end_stream) {
                if (frame.hd.length != 0) {
                    charge_non_progress_frame();
                }
                break;
            }
            refund_non_progress_frames(content_bytes);
            if (end_stream) {
                ++received_stream_progress_;
                if (auto* stream = streams_.find(frame.hd.stream_id); stream != nullptr && !stream->is_aborted()) {
                    finish_data_frame(*stream);
                }
            }
            break;
        }
        case NGHTTP2_HEADERS:
        case NGHTTP2_PUSH_PROMISE:
            header_block_in_progress_ = false;
            ++received_stream_progress_;
            refund_non_progress_frames(0);
            finish_header_block(frame);
            break;
        case NGHTTP2_RST_STREAM:
            on_rst_stream_frame(frame);
            break;
        case NGHTTP2_SETTINGS:
            if ((frame.hd.flags & NGHTTP2_FLAG_ACK) == 0) {
                on_settings_frame(frame);
            }
            break;
        case NGHTTP2_PING:
            on_ping_frame(frame);
            break;
        case NGHTTP2_GOAWAY:
            on_goaway_frame(frame);
            break;
        case NGHTTP2_WINDOW_UPDATE:
            if (receive_frame_metadata_) {
                publish_event(http2_event(http2_received_window_update{.stream_id_ = static_cast<std::uint32_t>(frame.hd.stream_id), .increment_ = static_cast<std::uint32_t>(frame.window_update.window_size_increment)}));
            }
            break;
        case NGHTTP2_PRIORITY_UPDATE:
            on_priority_update_frame(frame);
            break;
        case static_cast<std::uint8_t>(http2_frame_type::alternative_service):
        case static_cast<std::uint8_t>(http2_frame_type::origin):
            if (role_ == http2_role::client && (frame.hd.type == static_cast<std::uint8_t>(http2_frame_type::alternative_service) || receive_origin_advertisements_)) {
                on_advertisement_frame(frame.hd);
            }
            break;
        default:
            break;
    }
    return 0;
}

void http2_connection::on_settings_frame(const nghttp2_frame& frame) {
    received_peer_settings_ = true;
    if (!receive_frame_metadata_) {
        return;
    }
    http2_received_settings settings;
    const auto count = (std::min)(frame.settings.niv, http2_received_settings::max_entries);
    for (std::size_t index = 0; index < count; ++index) {
        settings.entries_[index] = http2_setting_entry{.id_ = static_cast<http2_setting_id>(frame.settings.iv[index].settings_id), .value_ = frame.settings.iv[index].value};
    }
    settings.size_ = count;
    publish_event(http2_event(settings));
}

void http2_connection::on_ping_frame(const nghttp2_frame& frame) {
    if ((frame.hd.flags & NGHTTP2_FLAG_ACK) == 0) {
        return;
    }
    const auto payload = bytes_view(frame.ping.opaque_data, sizeof(frame.ping.opaque_data));
    const auto* drain = local_connection_state_.graceful_drain();
    // The drain PING's ACK completes the round trip that in-flight stream
    // creation was given (RFC 9113 §6.8).
    if (payload == http2_drain_ping_payload && drain != nullptr && drain->awaiting_round_trip()) {
        finish_drain_round_trip();
    } else if (payload == http2_liveness_ping_payload && liveness_ping_outstanding_) {
        liveness_ping_outstanding_ = false;
    } else {
        // An ACK that answers no outstanding PING of ours.
        charge_non_progress_frame();
    }
}

void http2_connection::charge_non_progress_frame() {
    if (non_progress_frames_ >= max_non_progress_frames_) {
        fail_connection(http2_error_code::enhance_your_calm);
        return;
    }
    ++non_progress_frames_;
}

void http2_connection::refund_non_progress_frames(std::size_t data_bytes) noexcept {
    const auto refund = std::size_t{non_progress_frame_refund} + data_bytes / non_progress_refund_data_bytes;
    non_progress_frames_ = refund >= non_progress_frames_ ? 0 : non_progress_frames_ - static_cast<std::uint32_t>(refund);
}

void http2_connection::on_goaway_frame(const nghttp2_frame& frame) {
    const http2_goaway_event goaway(static_cast<std::uint32_t>(frame.goaway.last_stream_id), static_cast<http2_error_code>(frame.goaway.error_code));
    // A GOAWAY boundary can cover more live streams than the concurrency budget:
    // reserved push promises hold no concurrency slot (RFC 9113 §5.1.2, §8.4.2)
    // yet remain open and must be closed here.
    std::array<std::uint32_t, http2_stream_table::max_streams> unprocessed_stream_ids;
    std::size_t unprocessed_count = 0;
    if (role_ == http2_role::client) {
        bool response_started_above_last = false;
        streams_.for_each([&](http2_stream_state& stream) {
            if ((stream.id() & 1U) == 0 || stream.id() <= goaway.last_stream_id() || stream.is_aborted()) {
                return;
            }
            // A response head proves that the peer acted on this request; calling
            // it unprocessed would make replay unsafe (RFC 9113 §8.7).
            if (http2_remote_final_head_decoded(stream) || stream.has_informational_response()) {
                response_started_above_last = true;
                return;
            }
            unprocessed_stream_ids[unprocessed_count++] = stream.id();
        });
        if (response_started_above_last) {
            fail_connection(http2_error_code::protocol_error);
            return;
        }
    } else {
        streams_.for_each([&](http2_stream_state& stream) {
            if ((stream.id() & 1U) == 0 && stream.id() > goaway.last_stream_id() && !http2_stream_is_closed(stream)) {
                unprocessed_stream_ids[unprocessed_count++] = stream.id();
            }
        });
    }
    std::ranges::sort(std::span(unprocessed_stream_ids).first(unprocessed_count));

    // A valid peer GOAWAY is graceful shutdown, not a local connection error. Its
    // sender opens no further streams (RFC 9113 §6.8), so our directional GOAWAY
    // follows at once without a round trip.
    if (local_connection_state_.open() != nullptr) {
        begin_final_drain();
    } else {
        finish_drain_round_trip();
    }
    peer_goaway_ = goaway;
    publish_event(http2_event(goaway));
    for (std::size_t i = 0; i < unprocessed_count; ++i) {
        const auto stream_id = unprocessed_stream_ids[i];
        if (role_ == http2_role::server) {
            (void)close_stream(stream_id, http2_stream_close_source::peer_goaway, goaway.error());
            continue;
        }
        // Higher locally initiated streams were never processed and can be
        // replayed on a new connection (RFC 9113 §8.7).
        if (close_stream_impl(stream_id, http2_stream_close_source::peer_goaway, goaway.error(), close_notification_type::owner_already_knows)) {
            publish_event(http2_event(http2_request_unprocessed_event(stream_id)));
        }
    }
}

void http2_connection::on_advertisement_frame(const nghttp2_frame_hd& header) {
    struct payload_guard final {
        std::string& payload_;
        ~payload_guard() {
            http_clear_string_retaining_small(payload_);
        }
    } guard{extension_payload_};
    if (role_ != http2_role::client) {
        return;
    }
    const auto payload = std::span<const char>(extension_payload_.data(), extension_payload_.size());
    if (header.type == static_cast<std::uint8_t>(http2_frame_type::origin)) {
        if (!receive_origin_advertisements_ || header.stream_id != 0 || (header.flags & 0xf) != 0) {
            return;
        }
        auto advertisement = decode_http_origin_advertisement(payload);
        if (advertisement.index() == 0) {
            publish_event(http2_event(std::move(std::get<0>(advertisement))));
        }
        return;
    }
    const auto stream_id = static_cast<std::uint32_t>(header.stream_id);
    if (stream_id != 0) {
        // ALTSVC is ignored on a CONNECT tunnel or a still-reserved push stream (RFC 7838 §4).
        const auto* stream = streams_.find(stream_id);
        if (stream == nullptr || http2_stream_is_closed(*stream) || stream->push_reservation() != http2_push_reservation::none || stream->tunnel().not_connect() == nullptr) {
            return;
        }
    }
    auto advertisement = decode_http2_alternative_service(stream_id, payload);
    if (advertisement.index() == 0) {
        publish_event(http2_event(std::move(std::get<0>(advertisement))));
    }
}

void http2_connection::on_priority_update_frame(const nghttp2_frame& frame) {
    // nghttp2 accepts PRIORITY_UPDATE only on a server and already enforced its
    // push-stream and idle-stream rules; a Prioritized Stream ID of zero is left
    // to this layer (RFC 9218 §7.1).
    const auto& update = *static_cast<const nghttp2_ext_priority_update*>(frame.ext.payload);
    const auto id = static_cast<std::uint32_t>(update.stream_id);
    if (id == 0) {
        fail_connection(http2_error_code::protocol_error);
        return;
    }
    const auto parsed = parse_http_priority(bytes_view(update.field_value, update.field_value_len));
    if (parsed.index() != 0) {
        return;  // A malformed Priority field does not prioritize a stream.
    }
    const auto fields = std::get<0>(parsed);
    auto* stream = streams_.find(id);
    const bool idle = stream == nullptr && (id & 1U) != 0 && id > highest_peer_stream_id_;
    if (stream != nullptr) {
        if (http2_stream_is_closed(*stream) || stream->local_send().end_stream_committed() != nullptr) {
            return;
        }
        stream->set_priority_update(fields);
    } else if (idle) {
        const auto existing = std::ranges::find_if(priorities_, [id](const auto& entry_value) noexcept {
            return entry_value.stream_id_ == id;
        });
        if (existing != priorities_.end()) {
            existing->fields_ = fields;
        } else {
            // Signals for identifiers the peer skipped become unreachable once a
            // higher stream opens; drop those only when the table would grow.
            std::erase_if(priorities_, [this](const auto& entry_value) noexcept {
                return entry_value.stream_id_ <= highest_peer_stream_id_;
            });
            if (priorities_.size() >= http2_local_settings::max_concurrent_streams) {
                return;
            }
            priorities_.push_back({id, fields});
        }
    } else {
        return;
    }
    publish_event(http2_event(http_priority_update{id, (id & 1U) == 0, fields}));
}

void http2_connection::on_rst_stream_frame(const nghttp2_frame& frame) {
    const auto stream_id = static_cast<std::uint32_t>(frame.hd.stream_id);
    const auto error = static_cast<http2_error_code>(frame.rst_stream.error_code);
    auto* stream = streams_.find(stream_id);
    // RFC 9113 §8.7: REFUSED_STREAM on a stream this client opened means the
    // server did no processing; the owner may replay the request. A refusal
    // after a response head contradicts that and stays a reset.
    const bool refused_request = role_ == http2_role::client && error == http2_error_code::refused_stream && (stream_id & 1U) != 0 && stream != nullptr && stream->response_status() == nullptr && !stream->has_informational_response();
    if (stream != nullptr && !stream->is_aborted() && !http2_stream_is_closed(*stream)) {
        if (refused_request) {
            if (close_stream_impl(stream_id, http2_stream_close_source::peer, error, close_notification_type::owner_already_knows)) {
                publish_event(http2_event(http2_request_unprocessed_event(stream_id)));
            }
        } else {
            (void)close_stream(stream_id, http2_stream_close_source::peer, error);
        }
    }
    // Rapid reset (CVE-2023-44487): a peer that keeps resetting streams far ahead
    // of the responses it lets complete is cut off.
    ++peer_reset_streams_;
    if (peer_reset_streams_ > completed_responses_ + max_unprocessed_stream_resets) {
        fail_connection(http2_error_code::enhance_your_calm);
    }
}

int http2_connection::on_stream_close(std::uint32_t stream_id, std::uint32_t error_code) {
    if (const auto owner_reset = std::ranges::find(owner_resets_, stream_id); owner_reset != owner_resets_.end()) {
        owner_resets_.erase(owner_reset);
    }
    if (header_block_.has_value() && &header_block_->context_.stream_ == streams_.find(stream_id)) {
        header_block_.reset();
    }
    auto* stream = streams_.find(stream_id);
    // After a connection error the owner learns of every stream's end from
    // connection_error(); nghttp2 closing streams while tearing down (for
    // example on the GOAWAY that caused the error) is not a stream event.
    if (stream == nullptr || stream->is_aborted() || http2_stream_is_closed(*stream) || local_connection_state_.fatal_failure() != nullptr) {
        return 0;
    }
    // nghttp2 ended a stream Ruvia still considers open: a stream error it
    // detected itself (flow control, frame on a closed half, refused stream).
    (void)close_stream(stream_id, http2_stream_close_source::local, static_cast<http2_error_code>(error_code));
    return 0;
}

int http2_connection::before_frame_send(const nghttp2_frame& frame) {
    const bool ack = (frame.hd.flags & NGHTTP2_FLAG_ACK) != 0;
    if ((frame.hd.type == NGHTTP2_PING || frame.hd.type == NGHTTP2_SETTINGS) && ack) {
        if (undrained_control_frames_ >= max_undrained_control_frames) {
            fail_connection(http2_error_code::enhance_your_calm);
            return NGHTTP2_ERR_CANCEL;
        }
        ++undrained_control_frames_;
        return 0;
    }
    if (frame.hd.type != NGHTTP2_RST_STREAM) {
        return 0;
    }
    const auto stream_id = static_cast<std::uint32_t>(frame.hd.stream_id);
    if (const auto owner_reset = std::ranges::find(owner_resets_, stream_id); owner_reset != owner_resets_.end()) {
        owner_resets_.erase(owner_reset);
        return 0;
    }
    // Every other RST_STREAM answers a peer frame.
    if (peer_error_resets_ >= max_peer_error_resets || undrained_control_frames_ >= max_undrained_control_frames) {
        fail_connection(http2_error_code::enhance_your_calm);
        return NGHTTP2_ERR_CANCEL;
    }
    ++peer_error_resets_;
    ++undrained_control_frames_;
    return 0;
}

int http2_connection::on_frame_send(const nghttp2_frame& frame) {
    if (frame.hd.type == NGHTTP2_DATA || frame.hd.type == NGHTTP2_HEADERS) {
        refund_non_progress_frames(frame.hd.type == NGHTTP2_DATA ? frame.hd.length : 0);
    }
    // A GOAWAY carrying an error is nghttp2's own connection error (or ours,
    // already recorded); graceful drain GOAWAYs carry NO_ERROR.
    if (frame.hd.type == NGHTTP2_GOAWAY && frame.goaway.error_code != NGHTTP2_NO_ERROR && local_connection_state_.fatal_failure() == nullptr) {
        header_block_in_progress_ = false;
        header_block_.reset();
        push_scratch_.reset();
        local_connection_state_.fail(static_cast<http2_error_code>(frame.goaway.error_code));
    } else if (frame.hd.type == NGHTTP2_DATA && (frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
        const auto stream_id = static_cast<std::uint32_t>(frame.hd.stream_id);
        if (auto* stream = streams_.find(stream_id); stream != nullptr && stream->tunnel().rejected() != nullptr && stream->local_send().connect_pending() != nullptr) {
            (void)stream->reject_local_connect();
            release_local_request_stream_if_closed(*stream);
            retry_deferred_stream_lease(stream_id);
        }
    }
    return 0;
}

// --- local control frames ---------------------------------------------------

void http2_connection::submit_goaway(std::uint32_t last_stream_id) {
    check_nghttp2(nghttp2_submit_goaway(session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(last_stream_id), NGHTTP2_NO_ERROR, reinterpret_cast<const std::uint8_t*>(http2_drain_debug.data()), http2_drain_debug.size()));
}

void http2_connection::begin_drain() {
    // Graceful drain (RFC 9113 §6.8) without a connection error: established
    // streams keep running. A server first advertises 2^31-1 and keeps admitting
    // the streams the client may already have in flight; the final boundary
    // follows the ACK of a PING queued right behind that GOAWAY.
    if (local_connection_state_.open() == nullptr) {
        return;
    }
    if (role_ != http2_role::server) {
        begin_final_drain();
        flush_session();
        return;
    }
    // A client that ignores the first GOAWAY and withholds the ACK still cannot
    // extend the round trip indefinitely: one concurrency window of further
    // streams is admitted, then the boundary becomes final.
    const auto stream_budget = std::uint32_t{2} * http2_local_settings::max_concurrent_streams;
    const auto round_trip_limit = last_peer_stream_id_ > http2_max_stream_id - stream_budget ? http2_max_stream_id : last_peer_stream_id_ + stream_budget;
    // nghttp2's shutdown notice is the 2^31-1 GOAWAY that, unlike a final
    // GOAWAY, leaves new peer streams admitted and input read while no stream
    // is active.
    check_nghttp2(nghttp2_submit_shutdown_notice(session_.get()));
    // The PING's ACK proves the peer read the GOAWAY only if the PING follows
    // it on the wire. nghttp2 sends a queued PING ahead of a queued GOAWAY, so
    // the GOAWAY is drained first.
    flush_session();
    check_nghttp2(nghttp2_submit_ping(session_.get(), NGHTTP2_FLAG_NONE, reinterpret_cast<const std::uint8_t*>(http2_drain_ping_payload.data())));
    (void)local_connection_state_.begin_round_trip_drain(http2_max_stream_id, round_trip_limit);
    flush_session();
}

void http2_connection::finish_drain() {
    if (local_connection_state_.open() != nullptr) {
        begin_final_drain();
    } else {
        finish_drain_round_trip();
    }
    flush_session();
}

void http2_connection::begin_final_drain() {
    if (local_connection_state_.open() == nullptr) {
        return;
    }
    submit_goaway(last_peer_stream_id_);
    (void)local_connection_state_.begin_graceful_drain(last_peer_stream_id_);
}

void http2_connection::finish_drain_round_trip() {
    const auto* drain = local_connection_state_.graceful_drain();
    if (drain == nullptr || !drain->awaiting_round_trip()) {
        return;
    }
    submit_goaway(last_peer_stream_id_);
    (void)local_connection_state_.finish_round_trip_drain(last_peer_stream_id_);
}

void http2_connection::terminate(http2_error_code error) {
    // The same terminal path as a detected connection error: GOAWAY(error)
    // names the last peer stream nghttp2 processed (RFC 9113 section 6.8).
    fail_connection(error);
    flush_session();
}

http2_submit_status http2_connection::submit_ping() {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return http2_submit_status::closed;
    }
    if (liveness_ping_outstanding_) {
        return http2_submit_status::accepted;
    }
    check_nghttp2(nghttp2_submit_ping(session_.get(), NGHTTP2_FLAG_NONE, reinterpret_cast<const std::uint8_t*>(http2_liveness_ping_payload.data())));
    liveness_ping_outstanding_ = true;
    flush_session();
    return http2_submit_status::accepted;
}

http2_submit_status http2_connection::submit_max_concurrent_streams(std::uint32_t limit) {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return http2_submit_status::closed;
    }
    if (limit > http2_local_settings::max_concurrent_streams) {
        return http2_submit_status::invalid_message;
    }
    const nghttp2_settings_entry entry{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, limit};
    check_nghttp2(nghttp2_submit_settings(session_.get(), NGHTTP2_FLAG_NONE, &entry, 1));
    flush_session();
    return http2_submit_status::accepted;
}

http2_submit_status http2_connection::submit_priority_update(std::uint32_t stream_id, http_priority_fields fields_value) {
    if (role_ != http2_role::client || local_connection_state_.open() == nullptr || stream_id == 0 || stream_id > http2_max_stream_id || ((stream_id & 1U) == 0 && stream_id > highest_peer_stream_id_)) {
        return http2_submit_status::invalid_state;
    }
    std::array<char, 32> encoded{};
    const auto size = encode_http_priority(encoded, fields_value);
    if (size.index() != 0) {
        return http2_submit_status::invalid_message;
    }
    // nghttp2 sends PRIORITY_UPDATE only to a peer that advertised
    // SETTINGS_NO_RFC7540_PRIORITIES=1 (RFC 9218 §2.1).
    if (peer_setting(NGHTTP2_SETTINGS_NO_RFC7540_PRIORITIES) == 0) {
        return http2_submit_status::peer_capability_unavailable;
    }
    check_nghttp2(nghttp2_submit_priority_update(session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(stream_id), reinterpret_cast<const std::uint8_t*>(encoded.data()), std::get<0>(size)));
    flush_session();
    return http2_submit_status::accepted;
}

std::uint32_t http2_connection::peer_setting(nghttp2_settings_id id) const noexcept {
    return nghttp2_session_get_remote_settings(session_.get(), id);
}

std::int32_t http2_connection::connection_send_window() const noexcept {
    return nghttp2_session_get_remote_window_size(session_.get());
}

// --- flow control & send window -----------------------------------------------

void http2_connection::consume_received(std::uint32_t stream_id, std::size_t bytes) {
    if (bytes == 0 || local_connection_state_.fatal_failure() != nullptr) {
        return;
    }
    check_nghttp2(nghttp2_session_consume(session_.get(), static_cast<std::int32_t>(stream_id), bytes));
}

void http2_connection::flush_window_debt(http2_stream_state& stream) {
    const auto debt = stream.take_window_debt();
    if (debt == 0 || local_connection_state_.fatal_failure() != nullptr) {
        return;
    }
    check_nghttp2(nghttp2_session_consume_connection(session_.get(), debt));
}

bool http2_connection::release_received_data(std::uint32_t stream_id, std::uint32_t bytes_value) {
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr || bytes_value == 0 || bytes_value > stream->window_debt()) {
        return false;
    }
    consume_received(stream_id, bytes_value);
    (void)stream->take_window_debt(bytes_value);
    return true;
}

std::size_t http2_connection::pending_data_output_bytes(std::uint32_t stream_id) const noexcept {
    return output_.pending_data_bytes(stream_id);
}

std::optional<http2_send_window_state> http2_connection::send_window_state(std::uint32_t stream_id) const noexcept {
    const auto* stream = streams_.find(stream_id);
    if (stream == nullptr || http2_stream_is_closed(*stream)) {
        return std::nullopt;
    }
    const auto stream_window = nghttp2_session_get_stream_remote_window_size(session_.get(), static_cast<std::int32_t>(stream_id));
    if (stream_window < 0 && nghttp2_session_find_stream(session_.get(), static_cast<std::int32_t>(stream_id)) == nullptr) {
        return std::nullopt;
    }
    const auto connection_window = connection_send_window();
    return http2_send_window_state{
        .connection_window_ = connection_window,
        .stream_window_ = stream_window,
        .available_ = static_cast<std::uint32_t>((std::max)(0, (std::min)(connection_window, stream_window))),
    };
}

bool http2_connection::can_send_empty_end_stream(std::uint32_t stream_id) const noexcept {
    const auto window = send_window_state(stream_id);
    return window.has_value() && window->connection_window_ > 0 && window->stream_window_ > 0;
}

bool http2_connection::stream_aborted(std::uint32_t stream_id) const noexcept {
    const auto* stream = streams_.find(stream_id);
    return stream == nullptr || stream->is_aborted();
}

// --- stream leases ------------------------------------------------------------

void http2_connection::lease_submitted_request(const std::variant<std::uint32_t, http2_request_head_submit_error>& result) noexcept {
    const auto* submitted = std::get_if<std::uint32_t>(&result);
    if (submitted == nullptr) {
        return;
    }
    pin_stream(*submitted);
}

void http2_connection::release_stream_lease(std::uint32_t stream_id) {
    const auto* stream = streams_.find(stream_id);
    if (stream != nullptr && (stream->window_debt() != 0 || stream->has_pending_events())) {
        leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::after_credits);
        return;
    }
    unpin_stream(stream_id);
    leases_.erase_release(stream_id);
}

void http2_connection::release_stream_lease_after_remote_end(std::uint32_t stream_id) noexcept {
    leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::after_remote_end);
}

void http2_connection::abandon_stream_lease_checked(std::uint32_t stream_id) {
    const auto* stream = streams_.find(stream_id);
    if (stream == nullptr) {
        leases_.erase_release(stream_id);
        return;
    }
    if (!stream->is_aborted() && !http2_stream_is_closed(*stream)) {
        const auto error = stream->local_send().end_stream_committed() != nullptr ? http2_error_code::no_error : http2_error_code::cancel;
        (void)submit_reset(stream_id, error);
    }
    release_stream_lease(stream_id);
}

void http2_connection::return_received_data(std::uint32_t stream_id, std::uint32_t bytes) noexcept {
    try {
        (void)release_received_data(stream_id, bytes);
        retry_deferred_stream_lease(stream_id);
    } catch (...) {
        leases_.defer_credit(stream_id, bytes);
    }
    flush_output();
}

void http2_connection::abandon_stream_lease(std::uint32_t stream_id) noexcept {
    try {
        abandon_stream_lease_checked(stream_id);
    } catch (...) {
        leases_.defer_release(stream_id, http2_stream_lease_ledger::release_kind_type::abandon);
    }
    flush_output();
}

void http2_connection::flush_output() noexcept {
    try {
        flush_session();
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
}

void http2_connection::retry_deferred_stream_lease(std::uint32_t stream_id) noexcept {
    const auto* pending = leases_.release(stream_id);
    if (pending == nullptr) {
        return;
    }
    using kind_type = http2_stream_lease_ledger::release_kind_type;
    if (pending->kind_ == kind_type::abandon) {
        try {
            abandon_stream_lease_checked(stream_id);
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (...) {
        }
        return;
    }
    const auto* stream = streams_.find(stream_id);
    if (stream != nullptr && (stream->window_debt() != 0 || stream->has_pending_events() || (pending->kind_ == kind_type::after_remote_end && !connection_error() && !http2_stream_is_closed(*stream)))) {
        return;
    }
    try {
        unpin_stream(stream_id);
        leases_.erase_release(stream_id);
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
}

void http2_connection::retry_deferred_stream_leases() noexcept {
    std::size_t index = 0;
    while (index < leases_.credit_count()) {
        const auto pending = leases_.credit_at(index);
        try {
            (void)release_received_data(pending.stream_id_, pending.bytes_);
            leases_.erase_credit(pending.stream_id_);
            retry_deferred_stream_lease(pending.stream_id_);
        } catch (...) {
            ++index;
        }
    }
    index = 0;
    while (index < leases_.release_count()) {
        const auto release = leases_.release_at(index);
        retry_deferred_stream_lease(release.stream_id_);
        if (index < leases_.release_count() && leases_.release_at(index).stream_id_ == release.stream_id_) {
            ++index;
        }
    }
    flush_output();
}

// --- stream records & lifecycle -----------------------------------------------

void http2_connection::pin_stream(std::uint32_t stream_id) noexcept {
    if (auto* const stream = streams_.find(stream_id)) {
        stream->set_pinned(true);
    }
}

void http2_connection::unpin_stream(std::uint32_t stream_id) {
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr) {
        std::erase_if(priorities_, [stream_id](const auto& entry_value) noexcept {
            return entry_value.stream_id_ == stream_id;
        });
        return;
    }
    if (stream->is_aborted()) {
        flush_window_debt(*stream);
        std::erase_if(priorities_, [stream_id](const auto& entry_value) noexcept {
            return entry_value.stream_id_ == stream_id;
        });
        release_local_request_stream(*stream);
        streams_.remove(stream_id);
        return;
    }
    if (http2_stream_is_closed(*stream)) {
        flush_window_debt(*stream);
        ++completed_responses_;
        release_local_request_stream_if_closed(*stream);
        std::erase_if(priorities_, [stream_id](const auto& entry_value) noexcept {
            return entry_value.stream_id_ == stream_id;
        });
        streams_.remove(stream_id);
        return;
    }
    const auto error = stream->local_send().end_stream_committed() != nullptr ? http2_error_code::no_error : http2_error_code::cancel;
    (void)submit_reset(stream_id, error);
    if (auto* retained_stream = streams_.find(stream_id)) {
        flush_window_debt(*retained_stream);
        release_local_request_stream(*retained_stream);
        streams_.remove(stream_id);
    }
}

bool http2_connection::close_stream_impl(std::uint32_t stream_id, http2_stream_close_source source_value, http2_error_code error, close_notification_type notification) {
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return false;
    }
    if (header_block_.has_value() && &header_block_->context_.stream_ == stream) {
        header_block_.reset();
    }
    const bool pinned = stream->pinned();
    release_local_request_stream(*stream);
    std::erase_if(priorities_, [stream_id](const auto& entry_value) noexcept {
        return entry_value.stream_id_ == stream_id;
    });
    (void)stream->abort(source_value);
    if (notification == close_notification_type::emit_event) {
        publish_event(http2_event(http2_stream_closed_event(stream_id, source_value, error)));
    }
    if (!pinned) {
        flush_window_debt(*stream);
        streams_.remove(stream_id);
    }
    return true;
}

bool http2_connection::close_stream(std::uint32_t stream_id, http2_stream_close_source source_value, http2_error_code error) {
    return close_stream_impl(stream_id, source_value, error, close_notification_type::emit_event);
}

void http2_connection::submit_owner_reset(std::uint32_t stream_id, http2_error_code error) {
    if (nghttp2_session_find_stream(session_.get(), static_cast<std::int32_t>(stream_id)) == nullptr) {
        return;
    }
    reserve_geometric(owner_resets_, owner_resets_.size() + 1);
    owner_resets_.push_back(stream_id);
    check_nghttp2(nghttp2_submit_rst_stream(session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(stream_id), static_cast<std::uint32_t>(error)));
}

void http2_connection::reset_stream_for_peer_error(std::uint32_t stream_id, http2_error_code error) {
    check_nghttp2(nghttp2_submit_rst_stream(session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(stream_id), static_cast<std::uint32_t>(error)));
    (void)close_stream(stream_id, http2_stream_close_source::local, error);
}

http2_stream_state* http2_connection::find_outbound_stream(std::uint32_t stream_id) noexcept {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return nullptr;
    }
    auto* stream = streams_.find(stream_id);
    return stream != nullptr && !stream->is_aborted() ? stream : nullptr;
}

http2_stream_state* http2_connection::create_stream(std::uint32_t stream_id) {
    return streams_.create(stream_id);
}

std::optional<http2_request_head_submit_error> http2_connection::local_request_admission_error() const noexcept {
    if (role_ != http2_role::client) {
        return http2_request_head_submit_error::invalid_state;
    }
    if (!started_) {
        std::terminate();
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_.has_value() || nghttp2_session_get_next_stream_id(session_.get()) > http2_max_stream_id) {
        return http2_request_head_submit_error::connection_unavailable;
    }
    if (active_local_request_streams_ >= peer_setting(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)) {
        return http2_request_head_submit_error::peer_stream_limit_reached;
    }
    return std::nullopt;
}

std::size_t http2_connection::available_local_request_streams(std::size_t local_limit) const noexcept {
    if (draining() || local_request_admission_error().has_value()) {
        return 0;
    }
    auto limit = (std::min)(local_limit, static_cast<std::size_t>(http2_local_settings::max_concurrent_streams));
    limit = (std::min)(limit, static_cast<std::size_t>(peer_setting(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)));
    return limit > active_local_request_streams_ ? limit - active_local_request_streams_ : 0;
}

void http2_connection::release_local_request_stream(http2_stream_state& stream) noexcept {
    if (stream.release_peer_concurrency_slot() && active_local_request_streams_ != 0) {
        --active_local_request_streams_;
    }
}

void http2_connection::release_local_request_stream_if_closed(http2_stream_state& stream) noexcept {
    if (http2_stream_is_closed(stream)) {
        release_local_request_stream(stream);
    }
}

http2_stream_state* http2_connection::stream(std::uint32_t stream_id) & noexcept {
    return streams_.find(stream_id);
}

http2_stream_receive_status http2_connection::stream_receive_status(std::uint32_t stream_id) const noexcept {
    const auto* current = streams_.find(stream_id);
    if (current == nullptr || current->is_aborted()) {
        return http2_stream_receive_status::closed;
    }
    return current->remote_receive().end_stream() != nullptr ? http2_stream_receive_status::ended : http2_stream_receive_status::open;
}

std::optional<http2_server_request_view> http2_connection::server_request_view(std::uint32_t stream_id) const noexcept {
    if (role_ != http2_role::server) {
        return std::nullopt;
    }
    const auto* stream_state = streams_.find(stream_id);
    if (stream_state == nullptr || stream_state->request_method().empty()) {
        return std::nullopt;
    }
    return http2_server_request_view{
        .method_ = stream_state->request_method(),
        .scheme_ = stream_state->request_scheme(),
        .path_ = stream_state->request_path(),
        .authority_ = http2_effective_request_authority(*stream_state),
        .protocol_ = stream_state->request_protocol(),
        .pseudo_header_order_ = stream_state->request_pseudo_header_order(),
    };
}
}  // namespace ruvia::detail

namespace ruvia {
http2_akamai_fingerprint_collector::http2_akamai_fingerprint_collector(http2_akamai_fingerprint_options options) :
    max_frames_(options.max_frames_),
    max_bytes_(options.max_bytes_) {}

void http2_akamai_fingerprint_collector::reset() noexcept {
    settings_.clear();
    priorities_.clear();
    pseudo_headers_.clear();
    fingerprint_.clear();
    frames_seen_ = 0;
    bytes_seen_ = 0;
    window_update_ = 0;
    first_request_stream_id_ = 0;
    has_window_update_ = false;
    saw_regular_header_ = false;
    phase_ = phase::collecting;
}

void http2_akamai_fingerprint_collector::observe_frame(const http2_frame_metadata& frame) noexcept {
    if (phase_ != phase::collecting) {
        return;
    }
    try {
        ++frames_seen_;
        const std::size_t frame_bytes = 9U + static_cast<std::size_t>(frame.payload_length_);
        if (frames_seen_ > max_frames_ || frame_bytes > max_bytes_ - bytes_seen_) {
            phase_ = phase::failed;
            return;
        }
        bytes_seen_ += frame_bytes;

        const auto append_priority = [&](const http2_received_priority& pri) {
            if (!priorities_.empty()) {
                priorities_.push_back(',');
            }
            http_append_unsigned(priorities_, pri.stream_id_);
            priorities_.push_back(':');
            priorities_.push_back(pri.exclusive_ ? '1' : '0');
            priorities_.push_back(':');
            http_append_unsigned(priorities_, pri.stream_dependency_);
            priorities_.push_back(':');
            http_append_unsigned(priorities_, pri.weight_);
        };

        switch (frame.type_) {
            case http2_frame_type::settings:
                if ((frame.flags_ & 0x01U) != 0) {
                    break;
                }
                if (!frame.settings_complete_ || frame.stream_id_ != 0) {
                    phase_ = phase::failed;
                    return;
                }
                for (const auto& entry : frame.settings()) {
                    if (!settings_.empty()) {
                        settings_.push_back(';');
                    }
                    http_append_unsigned(settings_, static_cast<std::uint16_t>(entry.id_));
                    settings_.push_back(':');
                    http_append_unsigned(settings_, entry.value_);
                }
                break;
            case http2_frame_type::window_update:
                if (frame.stream_id_ == 0 && frame.window_increment_.has_value()) {
                    window_update_ = *frame.window_increment_;
                    has_window_update_ = true;
                }
                break;
            case http2_frame_type::priority:
                if (frame.priority_.has_value()) {
                    append_priority(*frame.priority_);
                }
                break;
            case http2_frame_type::headers:
                if (frame.priority_.has_value() && (first_request_stream_id_ == 0 || frame.stream_id_ == first_request_stream_id_)) {
                    append_priority(*frame.priority_);
                }
                break;
            default:
                break;
        }
    } catch (...) {
        phase_ = phase::failed;
    }
}

void http2_akamai_fingerprint_collector::observe_header(const http2_header_metadata& header) noexcept {
    if (phase_ != phase::collecting) {
        return;
    }
    try {
        switch (header.phase_) {
            case http2_header_observation_phase::begin:
                if (header.stream_id_ == 0) {
                    phase_ = phase::failed;
                    return;
                }
                if (first_request_stream_id_ == 0) {
                    first_request_stream_id_ = header.stream_id_;
                }
                break;
            case http2_header_observation_phase::field: {
                if (first_request_stream_id_ == 0) {
                    if (header.stream_id_ == 0) {
                        phase_ = phase::failed;
                        return;
                    }
                    first_request_stream_id_ = header.stream_id_;
                }
                if (header.stream_id_ != first_request_stream_id_) {
                    return;
                }
                const std::size_t field_bytes = header.name_.size() + header.value_.size();
                if (field_bytes > max_bytes_ - bytes_seen_) {
                    phase_ = phase::failed;
                    return;
                }
                bytes_seen_ += field_bytes;
                if (!saw_regular_header_ && !header.name_.empty() && header.name_.front() == ':') {
                    std::string_view token{};
                    if (header.name_ == ":method") {
                        token = "m";
                    } else if (header.name_ == ":authority") {
                        token = "a";
                    } else if (header.name_ == ":scheme") {
                        token = "s";
                    } else if (header.name_ == ":path") {
                        token = "p";
                    } else if (header.name_.size() > 1U) {
                        token = header.name_.substr(1, 1);
                    }
                    if (!token.empty()) {
                        if (!pseudo_headers_.empty()) {
                            pseudo_headers_.push_back(',');
                        }
                        pseudo_headers_.append(token);
                    }
                } else {
                    saw_regular_header_ = true;
                }
                break;
            }
            case http2_header_observation_phase::end:
                if (header.stream_id_ != first_request_stream_id_ || first_request_stream_id_ == 0) {
                    return;
                }
                if (pseudo_headers_.empty()) {
                    phase_ = phase::failed;
                    return;
                }
                fingerprint_.clear();
                fingerprint_.append(settings_);
                fingerprint_.push_back('|');
                if (has_window_update_) {
                    http_append_unsigned(fingerprint_, window_update_);
                } else {
                    fingerprint_.append("00");
                }
                fingerprint_.push_back('|');
                if (priorities_.empty()) {
                    fingerprint_.push_back('0');
                } else {
                    fingerprint_.append(priorities_);
                }
                fingerprint_.push_back('|');
                fingerprint_.append(pseudo_headers_);
                phase_ = phase::complete;
                break;
        }
    } catch (...) {
        phase_ = phase::failed;
    }
}
}  // namespace ruvia

namespace ruvia {

http2_setting_entry http2_received_settings::at(std::size_t index) const {
    if (index >= size_) {
        throw std::out_of_range("HTTP/2 received SETTINGS index out of range");
    }
    return entries_[index];
}

namespace {

[[nodiscard]] http_client_response_head response_head_from_stream(const detail::http2_stream_state& stream) {
    const auto* status = stream.response_status();
    if (status == nullptr) {
        throw std::logic_error("HTTP/2 final response event has no status");
    }
    auto result_value = detail::http_client_response_head_access::make(*status, http_protocol_version::http2);
    if (const auto* known = stream.remote_content().allowed_known_length()) {
        detail::http_client_response_head_access::content_length(result_value, known->declared_length());
    } else if (const auto* metadata = stream.remote_content().metadata_only_known_length()) {
        detail::http_client_response_head_access::content_length(result_value, metadata->declared_length());
    }
    const auto header_count = stream.remote_header_count();
    if (header_count != 0) {
        auto& headers = detail::http_client_response_head_access::headers(result_value);
        headers.reserve(header_count);
        for (std::size_t index = 0; index < header_count; ++index) {
            const auto field = stream.remote_header_at(index);
            headers.emplace_back(field.name_, field.value_);
        }
    }
    return result_value;
}

[[nodiscard]] http2_message_content_semantics message_content_semantics(const detail::http2_stream_state& stream) noexcept {
    const auto& content = stream.remote_content();
    return content.metadata_only_without_length() != nullptr || content.metadata_only_known_length() != nullptr ? http2_message_content_semantics::metadata_only : http2_message_content_semantics::content;
}

}  // namespace

// Intrusively reference-counted so escaped events or credits can still inspect
// or abandon stream state after the public http2_connection facade is destroyed.
namespace detail {

class http2_connection_owner_endpoint final {
public:
    http2_connection_owner_endpoint(http2_role role, const http2_connection_options& options) :
        connection_(role, options) {}
    http2_connection_owner_endpoint(const http2_connection_owner_endpoint&) = delete;
    http2_connection_owner_endpoint& operator=(const http2_connection_owner_endpoint&) = delete;

    void retain() noexcept {
        ++references_;
    }

    void release() noexcept {
        if (references_ == 0) {
            std::terminate();
        }
        if (--references_ == 0) {
            delete this;
        }
    }

    void abandon_request(std::uint32_t stream_id) noexcept {
        connection_.abandon_stream_lease(stream_id);
    }

    void return_credit(std::uint32_t stream_id, std::uint32_t bytes_value) noexcept {
        connection_.return_received_data(stream_id, bytes_value);
    }
    [[nodiscard]] ::ruvia::http2_event materialize_message_head_event(const detail::http2_message_head_record& value) {
        auto* stream = connection_.stream(value.stream_id_);
        if (connection_.role() == http2_role::client) {
            if (stream == nullptr) {
                throw std::logic_error("HTTP/2 response event has no stream");
            }
            return ::ruvia::http2_event::response_head(value.stream_id_, response_head_from_stream(*stream), value.request_content_signal_, !value.body_open_);
        }
        if (stream == nullptr) {
            throw std::logic_error("HTTP/2 request event has no stream");
        }
        connection_.pin_stream(value.stream_id_);
        const auto content = stream->request_content_indication();
        const auto& remote = stream->remote_receive();
        const bool connect_pending = remote.connect_pending() != nullptr || remote.connect_pending_end_stream() != nullptr;
        std::optional<std::size_t> declared_content_length;
        if (const auto* known = stream->remote_content().allowed_known_length(); known != nullptr) {
            declared_content_length = known->declared_length();
        }
        const http2_server_request_snapshot snapshot{
            .content_ = content,
            .declared_content_length_ = declared_content_length,
            .priority_update_ = stream->priority_update(),
            .body_open_ = value.body_open_,
            .connect_pending_ = connect_pending,
            .initial_end_stream_ = value.initial_end_stream_,
        };
        return ::ruvia::http2_event::request_head(this, value.stream_id_, stream->request_expectations(), content, snapshot);
    }

    template <typename event_record_type>
    [[nodiscard]] ::ruvia::http2_event materialize_public_http2_event(event_record_type& value) {
        using value_type = std::decay_t<event_record_type>;
        const bool client = connection_.role() == http2_role::client;
        if constexpr (std::is_same_v<value_type, detail::http2_message_head_record>) {
            return materialize_message_head_event(value);
        } else if constexpr (std::is_same_v<value_type, detail::http2_message_data_record>) {
            return ::ruvia::http2_event::message_body_chunk(this, value.stream_id_, value.bytes_, value.flow_control_bytes_);
        } else if constexpr (std::is_same_v<value_type, detail::http2_tunnel_data_record>) {
            return ::ruvia::http2_event::tunnel_data(this, value.stream_id_, value.bytes_, value.flow_control_bytes_);
        } else if constexpr (std::is_same_v<value_type, detail::http2_message_end_record>) {
            auto* stream = connection_.stream(value.stream_id_);
            if (stream == nullptr) {
                throw std::logic_error("HTTP/2 message end event has no stream");
            }
            auto result = ::ruvia::http2_event::message_end(value.stream_id_, stream->take_remote_trailers(), message_content_semantics(*stream));
            if (client) {
                connection_.release_stream_lease_after_remote_end(value.stream_id_);
            }
            return result;
        } else {
            if constexpr (std::is_same_v<value_type, http2_tunnel_end_event>) {
                if (client) {
                    connection_.release_stream_lease_after_remote_end(value.stream_id());
                }
            } else if constexpr (std::is_same_v<value_type, http2_stream_closed_event> || std::is_same_v<value_type, http2_request_unprocessed_event>) {
                if (client) {
                    connection_.release_stream_lease(value.stream_id());
                }
            }
            return ::ruvia::http2_event(std::move(value));
        }
    }
    std::size_t references_{1};
    detail::http2_connection connection_;
};

}  // namespace detail

http2_received_data_credit::http2_received_data_credit(detail::http2_connection_owner_endpoint* endpoint, std::uint32_t stream_id, std::uint32_t bytes_value) noexcept :
    endpoint_(endpoint),
    stream_id_(stream_id),
    bytes_(bytes_value) {
    endpoint_->retain();
}

http2_received_data_credit::~http2_received_data_credit() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->return_credit(stream_id_, bytes_);
    endpoint_->release();
}

http2_received_data_credit::http2_received_data_credit(http2_received_data_credit&& other) noexcept :
    endpoint_(std::exchange(other.endpoint_, nullptr)),
    stream_id_(std::exchange(other.stream_id_, 0)),
    bytes_(std::exchange(other.bytes_, 0)) {}

void http2_received_data_credit::merge(http2_received_data_credit&& other) {
    if (this == &other || !valid() || !other.valid()) {
        throw std::logic_error("invalid HTTP/2 received data credit");
    }
    if (endpoint_ != other.endpoint_ || stream_id_ != other.stream_id_) {
        throw std::logic_error("HTTP/2 received data credits belong to different streams");
    }
    if (bytes_ > (std::numeric_limits<std::uint32_t>::max)() - other.bytes_) {
        throw std::overflow_error("HTTP/2 received data credit overflow");
    }
    bytes_ += other.bytes_;
    auto* endpoint = std::exchange(other.endpoint_, nullptr);
    other.stream_id_ = 0;
    other.bytes_ = 0;
    endpoint->release();
}

http2_request_head_event::http2_request_head_event(detail::http2_connection_owner_endpoint* endpoint, std::uint32_t stream_id, http_request_expectations expectations, http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept :
    stream_id_(stream_id),
    snapshot_(snapshot),
    expectations_(expectations),
    content_(content),
    endpoint_(endpoint) {
    endpoint_->retain();
}

http2_request_head_event::~http2_request_head_event() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->abandon_request(stream_id_);
    endpoint_->release();
}

http2_request_head_event::http2_request_head_event(http2_request_head_event&& other) noexcept :
    stream_id_(std::exchange(other.stream_id_, 0)),
    snapshot_(other.snapshot_),
    expectations_(other.expectations_),
    content_(other.content_),
    endpoint_(std::exchange(other.endpoint_, nullptr)) {}

http2_server_request_view http2_request_head_event::request_view() const& noexcept {
    if (endpoint_ == nullptr || stream_id_ == 0) {
        return {};
    }
    return endpoint_->connection_.server_request_view(stream_id_).value_or(http2_server_request_view{});
}

http_request http2_request_head_event::build_request(borrowed_text body) const& {
    if (endpoint_ == nullptr || stream_id_ == 0) {
        throw std::logic_error("HTTP/2 request head event is not active");
    }
    auto* stream = endpoint_->connection_.stream(stream_id_);
    if (stream == nullptr) {
        throw std::logic_error("missing HTTP/2 request stream");
    }
    const auto* pending = stream->tunnel().pending();
    const detail::binary_request_materialization_spec spec{
        .version_ = http_protocol_version::http2,
        .target_form_ = http_request_target_form::http2,
        .method_ = stream->request_method(),
        .scheme_ = stream->request_scheme(),
        .authority_ = stream->request_authority(),
        .host_ = detail::http2_request_host_field(*stream),
        .path_ = stream->request_path(),
        .protocol_ = stream->request_protocol(),
        .cookie_ = stream->request_cookie(),
        .body_ = body.view(),
        .regular_header_count_ = stream->remote_header_count(),
        .has_authority_ = stream->has_authority(),
        .has_host_ = stream->has_host(),
        .has_path_ = stream->has_path(),
        .has_cookie_ = stream->has_cookie(),
        .standard_connect_ = pending != nullptr && pending->form() == detail::http2_connect_form::standard,
        .failure_message_ = "validated HTTP/2 request cannot be materialized",
    };
    return detail::materialize_binary_http_request(spec, [stream](std::size_t index) noexcept {
        const auto header_value = stream->remote_header_at(index);
        return std::pair{http_header_view{header_value.name_, header_value.value_}, header_value.kind_};
    });
}
void http2_connection::impl_deleter_type::operator()(detail::http2_connection_owner_endpoint* value) const noexcept {
    if (value != nullptr) {
        value->release();
    }
}

http2_connection::http2_connection(http2_role role, const http2_connection_options& options) {
    // Even a peer's first WINDOW_UPDATE would end a connection without budget.
    if (options.max_non_progress_frames_ == 0) {
        throw std::invalid_argument("HTTP/2 non-progress frame budget must be positive");
    }
    impl_.reset(new detail::http2_connection_owner_endpoint(role, options));
    impl_->connection_.begin_connection();
}
http2_connection http2_connection::server(http2_connection_options options) {
    // Server push (RFC 9113 section 8.4) and ORIGIN receipt (RFC 8336 section 2)
    // are client roles; a server configured with them would silently ignore them.
    if (options.enable_push_ || options.receive_origin_advertisements_) {
        throw std::invalid_argument("HTTP/2 server connection does not accept client-only options");
    }
    return http2_connection(http2_role::server, options);
}
http2_connection http2_connection::client(http2_connection_options options) {
    return http2_connection(http2_role::client, options);
}
http2_connection::~http2_connection() = default;
http2_connection::http2_connection(http2_connection&&) noexcept = default;
http2_connection& http2_connection::operator=(http2_connection&&) noexcept = default;

websocket_handshake_validation_result validate_http2_websocket_handshake(http2_connection& connection, std::uint32_t stream_id, const http_request& request, websocket_handshake_validation_options options) noexcept {
    const auto* stream = connection.impl_->connection_.stream(stream_id);
    if (stream == nullptr) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    return detail::validate_http2_websocket_handshake(*stream, request, options);
}

std::variant<websocket_extended_connect_negotiation, http2_websocket_handshake_submit_error> http2_connection::submit_websocket_handshake(std::uint32_t stream_id, const http_request& request, const websocket_handshake_validation_result& validation, websocket_server_handshake_options options) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.submit_websocket_handshake(stream_id, request, validation, options);
}

http2_role http2_connection::role() const noexcept {
    return impl_->connection_.role();
}
bool http2_connection::header_block_in_progress() const noexcept {
    return impl_->connection_.header_block_in_progress();
}
bool http2_connection::received_peer_settings() const noexcept {
    return impl_->connection_.received_peer_settings();
}
std::uint64_t http2_connection::received_stream_progress() const noexcept {
    return impl_->connection_.received_stream_progress();
}
http2_feed_result http2_connection::feed(std::string_view input) {
    auto& connection = impl_->connection_;
    connection.retry_deferred_stream_leases();
    const auto result_value = connection.feed(input);
    connection.retry_deferred_stream_leases();
    return result_value;
}

std::optional<http2_event> http2_connection::next_event() {
    auto& connection = impl_->connection_;
    connection.retry_deferred_stream_leases();
    auto* event = connection.peek_event();
    if (event == nullptr) {
        return std::nullopt;
    }
    auto result_value = std::visit([this](auto& value) {
        return impl_->materialize_public_http2_event(value);
    }, event->value());
    connection.consume_event();
    return std::optional<http2_event>(std::move(result_value));
}

http2_output_batch_result http2_connection::take_output_batch(std::size_t max_bytes, std::string& output, http2_data_output_observer_type observer, void* observer_context) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.take_output_batch(max_bytes, output, observer, observer_context);
}
bool http2_connection::wants_write() const noexcept {
    return impl_->connection_.wants_write();
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(const http2_regular_request_head_view& request) {
    impl_->connection_.retry_deferred_stream_leases();
    std::optional<std::string_view> authority;
    if (request.authority_) {
        authority = request.authority_->view();
    }
    auto result_value = impl_->connection_.submit_regular_request_head(request.method_.view(), request.scheme_.view(), authority, request.target_.view(), static_cast<std::span<const http_header_view>>(request.headers_), request.content_, request.expectation_, request.field_source_);
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(const http2_connect_request_head_view& request) {
    impl_->connection_.retry_deferred_stream_leases();
    auto result_value = impl_->connection_.submit_connect_request_head(request.authority_.view(), static_cast<std::span<const http_header_view>>(request.headers_), request.field_source_);
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(const http2_extended_connect_request_head_view& request) {
    impl_->connection_.retry_deferred_stream_leases();
    auto result_value = impl_->connection_.submit_extended_connect_request_head(request.protocol_.view(), request.scheme_.view(), request.authority_.view(), request.target_.view(), static_cast<std::span<const http_header_view>>(request.headers_), request.field_source_);
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

http2_data_submit_result http2_connection::submit_data(std::uint32_t stream_id, std::string_view bytes_value, http2_end_stream end_stream, std::size_t max_payload_bytes) {
    auto& connection = impl_->connection_;
    const auto result_value = connection.submit_data(stream_id, bytes_value, end_stream, max_payload_bytes);
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return result_value;
}
http2_request_content_release_status http2_connection::release_request_content(std::uint32_t stream_id) noexcept {
    return impl_->connection_.release_request_content(stream_id);
}
http2_submit_status http2_connection::submit_connect_response_head(std::uint32_t stream_id, const http_response& response) {
    return impl_->connection_.submit_connect_response_head(stream_id, response);
}

http2_submit_status http2_connection::submit_interim_response_head(std::uint32_t stream_id, const http_interim_response_head& response) {
    return impl_->connection_.submit_interim_response_head(stream_id, response);
}

std::variant<http_buffered_response_write_plan, http2_response_head_submit_error> http2_connection::submit_response_head(std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.submit_response_head(stream_id, response, std::move(write_plan));
}

std::variant<http_response_stream_commit_plan, http2_response_head_submit_error> http2_connection::submit_streaming_response_head(std::uint32_t stream_id, http_response response, http_response_trailer_intent trailer_intent) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.submit_streaming_response_head(stream_id, std::move(response), trailer_intent);
}

http2_finish_status http2_connection::finish_request(std::uint32_t stream_id, std::span<const http_header_view> trailers) {
    auto& connection = impl_->connection_;
    const auto status = connection.finish_request(stream_id, trailers);
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return status;
}

http2_finish_status http2_connection::finish_response(std::uint32_t stream_id, const http_response_trailer_section& trailers) {
    auto& connection = impl_->connection_;
    const auto status = connection.finish_response(stream_id, trailers);
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return status;
}

std::variant<http2_request_head_event, http2_push_submit_error> http2_connection::submit_push_request(std::uint32_t associated_stream_id, http_push_request_view request) {
    impl_->connection_.retry_deferred_stream_leases();
    const auto id = impl_->connection_.submit_push_promise(associated_stream_id, request);
    if ((id.index() != 0)) {
        return std::get<1>(id);
    }
    impl_->connection_.pin_stream(std::get<0>(id));
    return http2_request_head_event(impl_.get(), std::get<0>(id), {}, http_request_content_indication::no_content, {});
}

http2_submit_status http2_connection::submit_origin_advertisement(std::span<const std::string_view> origins) {
    return impl_->connection_.submit_origin_advertisement(origins);
}
http2_submit_status http2_connection::submit_alternative_service_advertisement(std::uint32_t stream_id, std::string_view origin, std::string_view field_value) {
    return impl_->connection_.submit_alternative_service_advertisement(stream_id, origin, field_value);
}
http2_submit_status http2_connection::submit_priority_update(std::uint32_t stream_id, http_priority_fields fields_value) {
    return impl_->connection_.submit_priority_update(stream_id, fields_value);
}

http2_submit_status http2_connection::submit_reset(std::uint32_t stream_id, http2_error_code error) {
    auto& connection = impl_->connection_;
    const auto status = connection.submit_reset(stream_id, error);
    if (status == http2_submit_status::accepted && connection.role() == http2_role::client) {
        // Owner-originated resets deliberately produce no terminal event. The
        // public client facade must therefore release the pin established by
        // submit_request_head() here rather than waiting for next_event().
        connection.release_stream_lease(stream_id);
    }
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return status;
}
http2_submit_status http2_connection::submit_ping() {
    return impl_->connection_.submit_ping();
}
http2_submit_status http2_connection::submit_max_concurrent_streams(std::uint32_t limit) {
    return impl_->connection_.submit_max_concurrent_streams(limit);
}
std::size_t http2_connection::available_request_streams(std::size_t local_limit) const noexcept {
    return impl_->connection_.available_local_request_streams(local_limit);
}

std::size_t http2_connection::pending_data_output_bytes(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.pending_data_output_bytes(stream_id);
}
std::optional<http2_send_window_state> http2_connection::send_window_state(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.send_window_state(stream_id);
}
bool http2_connection::stream_aborted(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.stream_aborted(stream_id);
}
http2_stream_receive_status http2_connection::stream_receive_status(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.stream_receive_status(stream_id);
}
http2_server_request_release_status http2_connection::release(http2_request_head_event&& request) {
    if (request.endpoint_ == nullptr || request.stream_id_ == 0 || request.endpoint_ != impl_.get()) {
        throw std::logic_error("invalid HTTP/2 server request lease");
    }
    impl_->connection_.retry_deferred_stream_leases();
    const bool live = impl_->connection_.stream(request.stream_id_) != nullptr;
    if (live) {
        impl_->connection_.release_stream_lease(request.stream_id_);
    }
    auto* endpoint = std::exchange(request.endpoint_, nullptr);
    request.stream_id_ = 0;
    endpoint->release();
    return live ? http2_server_request_release_status::released : http2_server_request_release_status::closed;
}
void http2_connection::begin_drain() {
    impl_->connection_.begin_drain();
}
void http2_connection::finish_drain() {
    impl_->connection_.finish_drain();
}

void http2_connection::terminate(http2_error_code error) {
    impl_->connection_.terminate(error);
}
bool http2_connection::draining() const noexcept {
    return impl_->connection_.draining();
}
bool http2_connection::drain_final() const noexcept {
    return impl_->connection_.drain_final();
}
std::optional<http2_error_code> http2_connection::connection_error() const noexcept {
    return impl_->connection_.connection_error();
}

std::variant<std::uint32_t, http2_request_head_submit_error> submit_http2_websocket_client_request(http2_connection& connection, const websocket_client_negotiation& negotiation, std::string_view scheme, std::string_view authority, std::string_view target, borrowed_http_headers headers, http_request_field_source source) {
    const auto staged_headers = detail::stage_websocket_extended_connect_fields(negotiation, headers.view());
    return connection.submit_request_head(http2_extended_connect_request_head_view{
        .protocol_ = "websocket",
        .scheme_ = scheme,
        .authority_ = authority,
        .target_ = target,
        .headers_ = staged_headers.view(),
        .field_source_ = source,
    });
}

}  // namespace ruvia
