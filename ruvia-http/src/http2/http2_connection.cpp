#include "http2/http2_connection.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#include "ruvia/http/http_connection_advertisement.h"

#include "http2/http2_advertisement_frames.h"
#include "http2/http2_remote_receive_semantics.h"
#include "util/geometric_reserve.h"

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
    static int on_header(nghttp2_session*, const nghttp2_frame* frame, nghttp2_rcbuf* name,
        nghttp2_rcbuf* value, std::uint8_t, void* user_data) {
        return guard(user_data, [frame, name, value](http2_connection& connection) {
            const auto name_buffer = nghttp2_rcbuf_get_buf(name);
            const auto value_buffer = nghttp2_rcbuf_get_buf(value);
            return connection.on_header(*frame, bytes_view(name_buffer.base, name_buffer.len),
                bytes_view(value_buffer.base, value_buffer.len));
        });
    }
    static int on_frame_recv(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
        return guard(user_data, [frame](http2_connection& connection) {
            return connection.on_frame_recv(*frame);
        });
    }
    static int on_data_chunk_recv(nghttp2_session*, std::uint8_t flags, std::int32_t stream_id,
        const std::uint8_t* data, std::size_t length, void* user_data) {
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
    static int on_extension_chunk(nghttp2_session*, const nghttp2_frame_hd* header, const std::uint8_t* data,
        std::size_t length, void* user_data) {
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
    static nghttp2_ssize pack_extension(nghttp2_session*, std::uint8_t* buffer, std::size_t length,
        const nghttp2_frame* frame, void*) noexcept {
        const auto& payload = *static_cast<const http2_extension_payload*>(frame->ext.payload);
        if (payload.size_ > length) {
            return NGHTTP2_ERR_CANCEL;
        }
        payload.write_(payload, std::span<char>(reinterpret_cast<char*>(buffer), payload.size_));
        return static_cast<nghttp2_ssize>(payload.size_);
    }
    // DATA frames may use the peer's whole SETTINGS_MAX_FRAME_SIZE (RFC 9113
    // §4.2) instead of nghttp2's 16 KiB default.
    static nghttp2_ssize read_length(nghttp2_session*, std::uint8_t, std::int32_t, std::int32_t session_window,
        std::int32_t stream_window, std::uint32_t remote_max_frame_size, void*) noexcept {
        const auto window = (std::min)(session_window, stream_window);
        return static_cast<nghttp2_ssize>(
            (std::max)(std::int64_t{1}, (std::min)(static_cast<std::int64_t>(remote_max_frame_size),
                                            static_cast<std::int64_t>(window))));
    }
};

nghttp2_ssize http2_connection::read_outbound_data(nghttp2_session*, std::int32_t, std::uint8_t* buffer,
    std::size_t length, std::uint32_t* data_flags, nghttp2_data_source* source, void*) noexcept {
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

http2_connection::http2_connection(std::pmr::memory_resource* resource, http2_role role, bool enable_push,
    bool receive_origin_advertisements, bool receive_frame_metadata)
    : resource_(resource),
      memory_(resource),
      output_(resource),
      fields_(resource),
      streams_(resource),
      priorities_(resource),
      events_(resource),
      owner_resets_(resource),
      extension_payload_(resource),
      role_(role) {
    enable_push_ = role == http2_role::client && enable_push;
    receive_origin_advertisements_ = role == http2_role::client && receive_origin_advertisements;
    receive_frame_metadata_ = receive_frame_metadata;
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
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        callbacks.get(), &http2_session_callbacks::on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks.get(), &http2_session_callbacks::on_stream_close);
    nghttp2_session_callbacks_set_before_frame_send_callback(
        callbacks.get(), &http2_session_callbacks::before_frame_send);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks.get(), &http2_session_callbacks::on_frame_send);
    nghttp2_session_callbacks_set_on_extension_chunk_recv_callback(
        callbacks.get(), &http2_session_callbacks::on_extension_chunk);
    nghttp2_session_callbacks_set_unpack_extension_callback(
        callbacks.get(), &http2_session_callbacks::unpack_extension);
    nghttp2_session_callbacks_set_pack_extension_callback2(callbacks.get(), &http2_session_callbacks::pack_extension);
    nghttp2_session_callbacks_set_data_source_read_length_callback2(
        callbacks.get(), &http2_session_callbacks::read_length);

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
    nghttp2_option_set_max_reserved_remote_streams(
        option.get(), static_cast<std::uint32_t>(http2_stream_table::max_push_streams));
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
    nghttp2_option_set_user_recv_extension_type(option.get(), static_cast<std::uint8_t>(http2_frame_type::alternative_service));
    nghttp2_option_set_user_recv_extension_type(option.get(), static_cast<std::uint8_t>(http2_frame_type::origin));
    nghttp2_option_set_builtin_recv_extension_type(option.get(), NGHTTP2_PRIORITY_UPDATE);

    nghttp2_session* raw_session = nullptr;
    const int result = role_ == http2_role::client
                           ? nghttp2_session_client_new3(&raw_session, callbacks.get(), this, option.get(), memory_.table())
                           : nghttp2_session_server_new3(&raw_session, callbacks.get(), this, option.get(), memory_.table());
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
            code = error == NGHTTP2_ERR_SETTINGS_EXPECTED ? http2_error_code::protocol_error
                                                          : http2_error_code::enhance_your_calm;
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
    check_nghttp2(nghttp2_session_set_local_window_size(
        session_.get(), NGHTTP2_FLAG_NONE, 0, static_cast<std::int32_t>(http2_local_settings::initial_window_size)));
    started_ = true;
    flush_session();
}

// --- outbound byte buffer (batched writes) ------------------------------------

http2_output_batch_result http2_connection::take_output_batch(std::size_t max_bytes,
    std::pmr::string& into, http2_data_output_observer_type observer, void* observer_context) {
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
    const auto consumed = nghttp2_session_mem_recv2(
        session_.get(), reinterpret_cast<const std::uint8_t*>(in.data()), in.size());
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
    return local_connection_state_.fatal_failure() != nullptr ? http2_feed_result::protocol_failure
                                                              : http2_feed_result::accepted;
}

int http2_connection::on_begin_frame(const nghttp2_frame_hd& header) {
    switch (header.type) {
        case NGHTTP2_HEADERS:
        case NGHTTP2_PUSH_PROMISE:
            header_block_bytes_ = header.length;
            header_block_in_progress_ = (header.flags & NGHTTP2_FLAG_END_HEADERS) == 0;
            break;
        case NGHTTP2_CONTINUATION:
            header_block_bytes_ += header.length;
            header_block_in_progress_ = (header.flags & NGHTTP2_FLAG_END_HEADERS) == 0;
            if (header_block_bytes_ > max_http2_encoded_header_block_bytes) {
                // A partial block cannot be skipped without desynchronizing the
                // connection-wide HPACK state (RFC 9113 §4.3).
                header_block_.reset();
                fail_connection(http2_error_code::compression_error);
            }
            break;
        case static_cast<std::uint8_t>(http2_frame_type::alternative_service):
        case static_cast<std::uint8_t>(http2_frame_type::origin):
            extension_payload_.clear();
            break;
        default:
            break;
    }
    return 0;
}

int http2_connection::on_extension_chunk(const nghttp2_frame_hd&, std::string_view data) {
    extension_payload_.append(data.data(), data.size());
    return 0;
}

int http2_connection::on_frame_recv(const nghttp2_frame& frame) {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return 0;
    }
    switch (frame.hd.type) {
        case NGHTTP2_DATA:
            if ((frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
                if (auto* stream = streams_.find(frame.hd.stream_id); stream != nullptr && !stream->is_aborted()) {
                    finish_data_frame(*stream);
                }
            }
            break;
        case NGHTTP2_HEADERS:
        case NGHTTP2_PUSH_PROMISE:
            header_block_in_progress_ = false;
            finish_header_block(frame);
            break;
        case NGHTTP2_RST_STREAM:
            process_rst_stream(frame);
            break;
        case NGHTTP2_SETTINGS:
            if ((frame.hd.flags & NGHTTP2_FLAG_ACK) == 0) {
                process_settings(frame);
            }
            break;
        case NGHTTP2_PING:
            process_ping(frame);
            break;
        case NGHTTP2_GOAWAY:
            process_goaway(frame);
            break;
        case NGHTTP2_WINDOW_UPDATE:
            if (receive_frame_metadata_) {
                publish_event(http2_event(http2_received_window_update{
                    .stream_id_ = static_cast<std::uint32_t>(frame.hd.stream_id),
                    .increment_ = static_cast<std::uint32_t>(frame.window_update.window_size_increment)}));
            }
            break;
        case NGHTTP2_PRIORITY_UPDATE:
            process_priority_update(frame);
            break;
        case static_cast<std::uint8_t>(http2_frame_type::alternative_service):
        case static_cast<std::uint8_t>(http2_frame_type::origin):
            process_advertisement(frame.hd);
            break;
        default:
            break;
    }
    return 0;
}

void http2_connection::process_settings(const nghttp2_frame& frame) {
    received_peer_settings_ = true;
    if (!receive_frame_metadata_) {
        return;
    }
    http2_received_settings settings;
    const auto count = (std::min)(frame.settings.niv, http2_received_settings::max_entries);
    for (std::size_t index = 0; index < count; ++index) {
        settings.entries_[index] = http2_setting_entry{
            .id_ = static_cast<http2_setting_id>(frame.settings.iv[index].settings_id),
            .value_ = frame.settings.iv[index].value};
    }
    settings.size_ = count;
    publish_event(http2_event(settings));
}

void http2_connection::process_ping(const nghttp2_frame& frame) {
    if ((frame.hd.flags & NGHTTP2_FLAG_ACK) == 0) {
        return;
    }
    const auto payload = bytes_view(frame.ping.opaque_data, sizeof(frame.ping.opaque_data));
    // The drain PING's ACK completes the round trip that in-flight stream
    // creation was given (RFC 9113 §6.8).
    if (payload == http2_drain_ping_payload) {
        finish_drain_round_trip();
    } else if (payload == http2_liveness_ping_payload) {
        liveness_ping_outstanding_ = false;
    }
}

void http2_connection::process_goaway(const nghttp2_frame& frame) {
    const http2_goaway_event goaway(static_cast<std::uint32_t>(frame.goaway.last_stream_id),
        static_cast<http2_error_code>(frame.goaway.error_code));
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
        std::ranges::sort(std::span(unprocessed_stream_ids).first(unprocessed_count));
    } else {
        streams_.for_each([&](http2_stream_state& stream) {
            if ((stream.id() & 1U) == 0 && stream.id() > goaway.last_stream_id() && !http2_stream_is_closed(stream)) {
                unprocessed_stream_ids[unprocessed_count++] = stream.id();
            }
        });
    }

    // A valid peer GOAWAY is graceful shutdown, not a local connection error. Its
    // sender opens no further streams (RFC 9113 §6.8), so our directional GOAWAY
    // follows at once without a round trip.
    finish_drain();
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
        if (close_stream_impl(stream_id, http2_stream_close_source::peer_goaway, goaway.error(),
                close_notification_type::owner_already_knows)) {
            publish_event(http2_event(http2_request_unprocessed_event(stream_id)));
        }
    }
}

void http2_connection::process_advertisement(const nghttp2_frame_hd& header) {
    if (role_ != http2_role::client) {
        return;
    }
    const auto payload = std::span<const char>(extension_payload_.data(), extension_payload_.size());
    if (header.type == static_cast<std::uint8_t>(http2_frame_type::origin)) {
        if (!receive_origin_advertisements_ || header.stream_id != 0 || (header.flags & 0xf) != 0) {
            return;
        }
        auto advertisement = decode_http_origin_advertisement(payload, resource_);
        if (advertisement.index() == 0) {
            publish_event(http2_event(std::move(std::get<0>(advertisement))));
        }
        return;
    }
    const auto stream_id = static_cast<std::uint32_t>(header.stream_id);
    if (stream_id != 0) {
        // ALTSVC is ignored on a CONNECT tunnel (RFC 7838 §4).
        const auto* stream = streams_.find(stream_id);
        if (stream == nullptr || http2_stream_is_closed(*stream) || stream->tunnel().not_connect() == nullptr) {
            return;
        }
    }
    auto advertisement = decode_http2_alternative_service(stream_id, payload, resource_);
    if (advertisement.index() == 0) {
        publish_event(http2_event(std::move(std::get<0>(advertisement))));
    }
}

void http2_connection::process_priority_update(const nghttp2_frame& frame) {
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
    const bool idle = stream == nullptr && (id & 1U) != 0 && id > last_peer_stream_id_;
    if (stream != nullptr) {
        if (http2_stream_is_closed(*stream) || stream->local_send().end_stream_committed() != nullptr) {
            return;
        }
        stream->set_priority_update(fields);
    } else if (idle) {
        if (!priorities_.contains(id)) {
            // Signals for identifiers the peer skipped become unreachable once a
            // higher stream opens; drop those only when the map would grow.
            std::erase_if(priorities_, [this](const auto& entry_value) {
                return entry_value.first <= last_peer_stream_id_;
            });
        }
        priorities_.insert_or_assign(id, fields);
    } else {
        return;
    }
    publish_event(http2_event(http_priority_update{id, (id & 1U) == 0, fields}));
}

void http2_connection::process_rst_stream(const nghttp2_frame& frame) {
    const auto stream_id = static_cast<std::uint32_t>(frame.hd.stream_id);
    const auto error = static_cast<http2_error_code>(frame.rst_stream.error_code);
    auto* stream = streams_.find(stream_id);
    // RFC 9113 §8.7: REFUSED_STREAM on a stream this client opened means the
    // server did no processing; the owner may replay the request. A refusal
    // after a response head contradicts that and stays a reset.
    const bool refused_request = role_ == http2_role::client && error == http2_error_code::refused_stream &&
                                 (stream_id & 1U) != 0 && stream != nullptr &&
                                 stream->response_status() == nullptr && !stream->has_informational_response();
    if (stream != nullptr && !stream->is_aborted() && !http2_stream_is_closed(*stream)) {
        if (refused_request) {
            if (close_stream_impl(stream_id, http2_stream_close_source::peer, error,
                    close_notification_type::owner_already_knows)) {
                publish_event(http2_event(http2_request_unprocessed_event(stream_id)));
            }
        } else {
            (void)close_stream(stream_id, http2_stream_close_source::peer, error);
        }
    }
    // Rapid reset (CVE-2023-44487): a peer that keeps resetting streams far ahead
    // of the responses it lets complete is cut off.
    ++peer_reset_streams_;
    if (static_cast<std::uint64_t>(peer_reset_streams_) >
        static_cast<std::uint64_t>(completed_responses_) + max_unprocessed_resets) {
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
    if (stream == nullptr || stream->is_aborted() || http2_stream_is_closed(*stream) ||
        local_connection_state_.fatal_failure() != nullptr) {
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
    // A GOAWAY carrying an error is nghttp2's own connection error (or ours,
    // already recorded); graceful drain GOAWAYs carry NO_ERROR.
    if (frame.hd.type == NGHTTP2_GOAWAY && frame.goaway.error_code != NGHTTP2_NO_ERROR &&
        local_connection_state_.fatal_failure() == nullptr) {
        local_connection_state_.fail(static_cast<http2_error_code>(frame.goaway.error_code));
    }
    return 0;
}

// --- local control frames ---------------------------------------------------

void http2_connection::submit_goaway(std::uint32_t last_stream_id) {
    check_nghttp2(nghttp2_submit_goaway(session_.get(), NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(last_stream_id),
        NGHTTP2_NO_ERROR, reinterpret_cast<const std::uint8_t*>(http2_drain_debug.data()), http2_drain_debug.size()));
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
    const auto round_trip_limit = last_peer_stream_id_ > http2_max_stream_id - stream_budget
                                      ? http2_max_stream_id
                                      : last_peer_stream_id_ + stream_budget;
    // nghttp2's shutdown notice is the 2^31-1 GOAWAY that, unlike a final
    // GOAWAY, leaves new peer streams admitted and input read while no stream
    // is active.
    check_nghttp2(nghttp2_submit_shutdown_notice(session_.get()));
    // The PING's ACK proves the peer read the GOAWAY only if the PING follows
    // it on the wire. nghttp2 sends a queued PING ahead of a queued GOAWAY, so
    // the GOAWAY is drained first.
    flush_session();
    check_nghttp2(nghttp2_submit_ping(
        session_.get(), NGHTTP2_FLAG_NONE, reinterpret_cast<const std::uint8_t*>(http2_drain_ping_payload.data())));
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

http2_submit_status http2_connection::submit_ping() {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return http2_submit_status::closed;
    }
    if (liveness_ping_outstanding_) {
        return http2_submit_status::accepted;
    }
    check_nghttp2(nghttp2_submit_ping(
        session_.get(), NGHTTP2_FLAG_NONE, reinterpret_cast<const std::uint8_t*>(http2_liveness_ping_payload.data())));
    liveness_ping_outstanding_ = true;
    flush_session();
    return http2_submit_status::accepted;
}

http2_submit_status http2_connection::submit_priority_update(std::uint32_t stream_id, http_priority_fields fields_value) {
    if (role_ != http2_role::client || local_connection_state_.open() == nullptr || stream_id == 0 ||
        stream_id > http2_max_stream_id ||
        ((stream_id & 1U) == 0 && stream_id > last_peer_stream_id_)) {
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
    check_nghttp2(nghttp2_submit_priority_update(session_.get(), NGHTTP2_FLAG_NONE,
        static_cast<std::int32_t>(stream_id), reinterpret_cast<const std::uint8_t*>(encoded.data()), std::get<0>(size)));
    flush_session();
    return http2_submit_status::accepted;
}

std::uint32_t http2_connection::peer_setting(nghttp2_settings_id id) const noexcept {
    return nghttp2_session_get_remote_settings(session_.get(), id);
}

std::int32_t http2_connection::connection_send_window() const noexcept {
    return nghttp2_session_get_remote_window_size(session_.get());
}

}  // namespace ruvia::detail
