#include "http3_connection.h"

#include <nghttp3/nghttp3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "http_connection_fields.h"
#include "http_header.h"
#include "http_request.h"
namespace ruvia::detail {

// Dynamic ORIGIN frames share the control stream with nghttp3 output. The
// segment ledger preserves their wire order without advancing nghttp3 offsets
// for extension bytes. Each frame owns address-stable storage until its ACK.
class http3_control_extension_output final {
    struct frame final {
        explicit frame(std::size_t size) :
            bytes_(size) {}

        std::vector<std::byte> bytes_;
        std::size_t written_{0};
        std::size_t acknowledged_{0};
    };

    struct segment final {
        bool nghttp3_{true};
        std::size_t bytes_{0};
    };

public:
    http3_control_extension_output() noexcept = default;

    http3_control_extension_output(const http3_control_extension_output&) = delete;
    http3_control_extension_output& operator=(const http3_control_extension_output&) = delete;
    http3_control_extension_output(http3_control_extension_output&&) = delete;
    http3_control_extension_output& operator=(http3_control_extension_output&&) = delete;

    [[nodiscard]] http3_submit_status queue(std::span<const std::string_view> origins, std::size_t limit) {
        if (shutdown_) {
            return http3_submit_status::invalid_state;
        }
        const auto payload_size = http_origin_payload_size(origins, 16384);
        if (payload_size.index() != 0) {
            return http3_submit_status::invalid_message;
        }
        const auto length = std::get<0>(payload_size);
        const auto length_varint_bytes = quic_varint_size(length);
        const auto total_bytes = 1 + length_varint_bytes + length;
        if (total_bytes > limit - (std::min)(limit, pending_bytes_)) {
            return http3_submit_status::backpressured;
        }

        // All allocation precedes publication of counters. Appending never
        // relocates another frame's bytes already borrowed by QUIC.
        auto& queued = frames_.emplace_back(total_bytes);
        auto* const dst = queued.bytes_.data();
        dst[0] = std::byte{0x0c};
        (void)encode_quic_varint({reinterpret_cast<char*>(dst + 1), length_varint_bytes}, length);
        write_http_origin_payload({reinterpret_cast<char*>(dst + 1 + length_varint_bytes), length}, origins);
        pending_bytes_ += total_bytes;
        unwritten_bytes_ += total_bytes;
        return http3_submit_status::accepted;
    }

    // nullopt leaves output to nghttp3; the caller owns the control stream id
    // and initializes output before trying either source.
    [[nodiscard]] std::optional<http3_transport_result> peek(nghttp3_conn* connection, std::int64_t control_id, std::size_t max_bytes, http3_stream_output& output) noexcept {
        peeked_ = false;
        if (blocked_ || shutdown_ || unwritten_bytes_ == 0 || (!frame_in_progress_ && nghttp3_conn_is_stream_flushed(connection, control_id) == 0)) {
            return std::nullopt;
        }
        if (max_bytes == 0) {
            return http3_transport_result{.status_ = http3_transport_status::would_block};
        }
        const auto& current = frames_[write_frame_];
        const auto offered = (std::min)(current.bytes_.size() - current.written_, max_bytes);
        output.stream_id_ = control_id;
        output.buffers_[0] = {current.bytes_.data() + current.written_, offered};
        output.buffer_count_ = 1;
        output.fin_ = false;
        peeked_ = true;
        return http3_transport_result{};
    }

    [[nodiscard]] int commit(nghttp3_conn* connection, std::int64_t id, std::size_t bytes) noexcept {
        const bool from_nghttp3 = !peeked_;
        peeked_ = false;
        if (!from_nghttp3) {
            if (bytes > unwritten_bytes_) {
                return NGHTTP3_ERR_INVALID_ARGUMENT;
            }
            if (bytes != 0) {
                if (!segments_.empty() && !segments_.back().nghttp3_) {
                    segments_.back().bytes_ += bytes;
                } else {
                    try {
                        segments_.push_back({false, bytes});
                    } catch (...) {
                        return NGHTTP3_ERR_H3_INTERNAL_ERROR;
                    }
                }
            }
            auto remaining = bytes;
            while (remaining != 0) {
                auto& current = frames_[write_frame_];
                const auto step = (std::min)(remaining, current.bytes_.size() - current.written_);
                current.written_ += step;
                remaining -= step;
                if (current.written_ == current.bytes_.size()) {
                    ++write_frame_;
                }
            }
            unwritten_bytes_ -= bytes;
            frame_in_progress_ = unwritten_bytes_ != 0;
            return 0;
        }

        const bool extend_last = bytes != 0 && !segments_.empty() && segments_.back().nghttp3_;
        if (bytes != 0 && !extend_last) {
            try {
                segments_.push_back({true, 0});
            } catch (...) {
                return NGHTTP3_ERR_H3_INTERNAL_ERROR;
            }
        }
        const int result = nghttp3_conn_add_write_offset(connection, id, bytes);
        if (result != 0) {
            if (bytes != 0 && !extend_last) {
                segments_.pop_back();
            }
            return result;
        }
        if (bytes != 0) {
            segments_.back().bytes_ += bytes;
        }
        return 0;
    }

    [[nodiscard]] int acknowledge(nghttp3_conn* connection, std::int64_t id, std::uint64_t bytes) noexcept {
        if (!segments_.empty()) {
            while (bytes != 0 && !segments_.empty()) {
                auto& current = segments_.front();
                const auto step = static_cast<std::size_t>((std::min)(bytes, static_cast<std::uint64_t>(current.bytes_)));
                if (current.nghttp3_) {
                    const int result = nghttp3_conn_add_ack_offset(connection, id, step);
                    if (result != 0) {
                        return result;
                    }
                } else {
                    acknowledge_frames((std::min)(step, pending_bytes_ - unwritten_bytes_));
                }
                current.bytes_ -= step;
                bytes -= step;
                if (current.bytes_ == 0) {
                    segments_.pop_front();
                }
            }
            if (bytes == 0) {
                return 0;
            }
        }
        return nghttp3_conn_add_ack_offset(connection, id, bytes);
    }

    void block() noexcept {
        blocked_ = true;
    }

    void unblock() noexcept {
        blocked_ = false;
    }

    void shutdown() noexcept {
        shutdown_ = true;
        reset();
        // Outstanding extension segments still consume their wire ACK ranges;
        // later nghttp3 segments must retain their original relative offsets.
    }

    [[nodiscard]] bool shutdown_requested() const noexcept {
        return shutdown_;
    }

private:
    void acknowledge_frames(std::size_t bytes) noexcept {
        while (bytes != 0) {
            auto& current = frames_.front();
            const auto step = (std::min)(bytes, current.written_ - current.acknowledged_);
            current.acknowledged_ += step;
            pending_bytes_ -= step;
            bytes -= step;
            if (current.acknowledged_ == current.bytes_.size()) {
                frames_.pop_front();
                --write_frame_;
            }
        }
        if (pending_bytes_ == 0) {
            reset();
        }
    }

    void reset() noexcept {
        frames_.clear();
        pending_bytes_ = 0;
        unwritten_bytes_ = 0;
        write_frame_ = 0;
        frame_in_progress_ = false;
    }

    std::deque<frame> frames_;
    std::deque<segment> segments_;
    std::size_t pending_bytes_{0};
    std::size_t unwritten_bytes_{0};
    std::size_t write_frame_{0};
    bool blocked_{false};
    bool shutdown_{false};
    bool peeked_{false};
    bool frame_in_progress_{false};
};

}  // namespace ruvia::detail

namespace ruvia {

namespace {

[[nodiscard]] std::string_view rcbuf_view(nghttp3_rcbuf* buffer) noexcept {
    const auto value = nghttp3_rcbuf_get_buf(buffer);
    return {reinterpret_cast<const char*>(value.base), value.len};
}

[[nodiscard]] nghttp3_nv make_nv(std::string_view name, std::string_view value, bool sensitive = false) noexcept {
    return {
        .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
        .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
        .namelen = name.size(),
        .valuelen = value.size(),
        .flags = static_cast<std::uint8_t>((sensitive || detail::http_field_never_indexed(name)) ? NGHTTP3_NV_FLAG_NEVER_INDEX : NGHTTP3_NV_FLAG_NONE),
    };
}

// One outbound field section. Already-lowercase names borrow their source;
// changed names share one lazily reserved buffer that stays address-stable.
// nghttp3 copies the section when it is submitted.
class outbound_field_section final {
public:
    outbound_field_section(std::size_t name_bytes, std::size_t field_count) :
        name_bytes_(name_bytes) {
        fields_.reserve(field_count);
    }

    outbound_field_section(const outbound_field_section&) = delete;
    outbound_field_section& operator=(const outbound_field_section&) = delete;
    outbound_field_section(outbound_field_section&&) = delete;
    outbound_field_section& operator=(outbound_field_section&&) = delete;

    void clear() {
        names_.clear();
        value_bytes_ = 0;
        fields_.clear();
    }

    void reserve(std::size_t count) {
        fields_.reserve(count);
    }

    void add(std::string_view name, std::string_view value) {
        fields_.append(make_nv(name, value, detail::http_field_never_indexed(name)));
    }

    void add_lowercase(std::string_view name, std::string_view value, bool sensitive = false) {
        const auto lower = detail::http_lower_header_name(name, [this](std::size_t size) {
            const auto offset = names_.size();
            if (names_.empty()) {
                names_.reserve(name_bytes_);
            }
            names_.resize(offset + size);
            return names_.data() + offset;
        });
        fields_.append(make_nv(lower, value, sensitive));
    }

    void add_owned_value(std::string_view name, std::string_view value) {
        if (value.size() > values_.size() - value_bytes_) {
            throw std::logic_error("binary response generated value storage exceeded");
        }
        auto* destination = values_.data() + value_bytes_;
        std::copy(value.begin(), value.end(), destination);
        value_bytes_ += value.size();
        add(name, std::string_view(destination, value.size()));
    }
    [[nodiscard]] const nghttp3_nv* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return fields_.view().data();
    }
    const nghttp3_nv* data() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return fields_.size();
    }

private:
    std::size_t name_bytes_;
    std::string names_;
    std::array<char, 96> values_{};
    std::size_t value_bytes_{0};
    detail::http_header_descriptor_storage<nghttp3_nv> fields_;
};

// Reads HTTP/3 frame headers (RFC 9114 §7.1) off one received stream for the
// frame observer, without buffering: a varint split across inputs is
// accumulated in place. Unidirectional streams other than the control stream
// (§6.2) carry no frames and are ignored after their type.
class received_frame_scanner final {
public:
    explicit received_frame_scanner(bool bidirectional) noexcept :
        phase_(bidirectional ? phase::frame_type : phase::stream_type) {}

    template <typename observe_type>
    void scan(std::span<const std::byte> bytes, observe_type&& observe) {
        std::size_t offset = 0;
        while (offset < bytes.size() && phase_ != phase::ignored) {
            if (phase_ == phase::payload) {
                const auto skipped = static_cast<std::size_t>((std::min)(remaining_, static_cast<std::uint64_t>(bytes.size() - offset)));
                offset += skipped;
                remaining_ -= skipped;
                if (remaining_ == 0) {
                    phase_ = phase::frame_type;
                }
                continue;
            }
            const auto byte = static_cast<std::uint8_t>(bytes[offset++]);
            if (varint_size_ == 0) {
                varint_size_ = static_cast<std::uint8_t>(1U << (byte >> 6));
                value_ = byte & 0x3fU;
                varint_read_ = 1;
            } else {
                value_ = (value_ << 8) | byte;
                ++varint_read_;
            }
            if (varint_read_ != varint_size_) {
                continue;
            }
            varint_size_ = 0;
            if (phase_ == phase::stream_type) {
                // RFC 9114 §6.2.1: only the control stream carries frames.
                phase_ = value_ == 0 ? phase::frame_type : phase::ignored;
            } else if (phase_ == phase::frame_type) {
                type_ = value_;
                phase_ = phase::frame_length;
            } else {
                remaining_ = value_;
                observe(type_, value_);
                phase_ = remaining_ == 0 ? phase::frame_type : phase::payload;
            }
        }
    }

private:
    enum class phase : std::uint8_t { stream_type,
        frame_type,
        frame_length,
        payload,
        ignored };
    phase phase_;
    std::uint8_t varint_size_{0};
    std::uint8_t varint_read_{0};
    std::uint64_t value_{0};
    std::uint64_t type_{0};
    std::uint64_t remaining_{0};
};

}  // namespace

namespace detail {

// Where escaped DATA credits return. Intrusively reference-counted so a credit
// can outlive the codec: the codec detaches the sink before it is destroyed,
// after which returned credit is dropped.
class http3_received_data_ledger final {
public:
    http3_received_data_ledger(http3_received_data_credit_sink_type sink, void* context) noexcept :
        sink_(sink),
        context_(context) {}
    http3_received_data_ledger(const http3_received_data_ledger&) = delete;
    http3_received_data_ledger& operator=(const http3_received_data_ledger&) = delete;

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

    void return_credit(std::int64_t stream_id, std::size_t bytes) const noexcept {
        if (sink_ != nullptr && bytes != 0) {
            sink_(context_, stream_id, bytes);
        }
    }

    void detach() noexcept {
        sink_ = nullptr;
        context_ = nullptr;
    }

private:
    std::size_t references_{1};
    http3_received_data_credit_sink_type sink_;
    void* context_;
};

}  // namespace detail

http3_received_data_credit::http3_received_data_credit(detail::http3_received_data_ledger* ledger, std::int64_t stream_id, std::size_t bytes) noexcept :
    ledger_(ledger),
    stream_id_(stream_id),
    bytes_(bytes) {
    ledger_->retain();
}

http3_received_data_credit::~http3_received_data_credit() {
    if (ledger_ == nullptr) {
        return;
    }
    ledger_->return_credit(stream_id_, bytes_);
    ledger_->release();
}

http3_received_data_credit::http3_received_data_credit(http3_received_data_credit&& other) noexcept :
    ledger_(std::exchange(other.ledger_, nullptr)),
    stream_id_(std::exchange(other.stream_id_, -1)),
    bytes_(std::exchange(other.bytes_, 0)) {}

void http3_received_data_credit::merge(http3_received_data_credit&& other) {
    if (this == &other || !valid() || !other.valid()) {
        throw std::logic_error("invalid HTTP/3 received data credit");
    }
    if (ledger_ != other.ledger_ || stream_id_ != other.stream_id_) {
        throw std::logic_error("HTTP/3 received data credits belong to different streams");
    }
    if (bytes_ > (std::numeric_limits<std::size_t>::max)() - other.bytes_) {
        throw std::overflow_error("HTTP/3 received data credit overflow");
    }
    bytes_ += other.bytes_;
    auto* ledger = std::exchange(other.ledger_, nullptr);
    other.stream_id_ = -1;
    other.bytes_ = 0;
    ledger->release();
}

void http3_received_data_credit::swap(http3_received_data_credit& other) noexcept {
    std::swap(ledger_, other.ledger_);
    std::swap(stream_id_, other.stream_id_);
    std::swap(bytes_, other.bytes_);
}

http3_data_event::http3_data_event(std::int64_t stream_id, std::string_view bytes, bool own_bytes, http3_received_data_credit credit) :
    stream_id_(stream_id),
    bytes_(bytes),
    owns_bytes_(own_bytes),
    credit_(std::move(credit)) {
    if (owns_bytes_) {
        owned_bytes_.assign(bytes);
        bytes_ = owned_bytes_;
    }
}

http3_data_event::http3_data_event(http3_data_event&& other) noexcept :
    stream_id_(other.stream_id_),
    bytes_(other.bytes_),
    owned_bytes_(std::move(other.owned_bytes_)),
    owns_bytes_(other.owns_bytes_),
    credit_(std::move(other.credit_)) {
    if (owns_bytes_) {
        bytes_ = owned_bytes_;
    }
}

http3_data_event& http3_data_event::operator=(http3_data_event&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    owned_bytes_ = std::move(other.owned_bytes_);
    owns_bytes_ = other.owns_bytes_;
    bytes_ = owns_bytes_ ? std::string_view(owned_bytes_) : other.bytes_;
    stream_id_ = other.stream_id_;
    http3_received_data_credit incoming(std::move(other.credit_));
    credit_.swap(incoming);
    return *this;
}

std::size_t http3_stream_output::size() const noexcept {
    std::size_t total = 0;
    for (std::size_t index = 0; index < buffer_count_; ++index) {
        total += buffers_[index].size();
    }
    return total;
}

class http3_connection::impl final {
public:
    struct body_chunk final {
        explicit body_chunk(std::span<const std::byte> bytes) :
            bytes_(reinterpret_cast<const char*>(bytes.data()), bytes.size()) {}

        std::string bytes_;
        std::size_t acknowledged_{0};
        bool provided_{false};
    };

    enum class send_phase : std::uint8_t {
        head_pending,
        head_ended,
        body_open,
        body_ended,
        trailers_ended,
    };
    struct stream_state final {
        stream_state() noexcept :
            building_header_size_(0) {}

        std::deque<body_chunk> body_;
        // The section being decoded; its kind is building_trailers_.
        std::optional<std::vector<http_header>> building_fields_;
        // The received trailer section, published with the message end.
        std::vector<http_header> received_trailers_;
        detail::binary_field_section_size building_header_size_;
        // Submitted DATA payload not yet acknowledged by the peer.
        std::size_t pending_bytes_{0};
        detail::http_local_content_state local_content_;
        detail::http_local_content_state remote_content_;
        http_known_method request_method_{http_known_method::unknown};
        send_phase send_phase_{send_phase::head_pending};
        bool building_trailers_{false};
        bool received_head_{false};
        // Client: the request waits for 100 (Continue) (RFC 9110 §10.1.1).
        bool continue_pending_{false};
        bool connect_{false};
        bool websocket_{false};
        bool early_data_{false};
        bool read_aborted_{false};
        bool write_aborted_{false};
        // Client: reported as http3_request_unprocessed_event; no later
        // event names the stream.
        bool unprocessed_{false};

        [[nodiscard]] bool head_submitted() const noexcept {
            return send_phase_ != send_phase::head_pending;
        }
        [[nodiscard]] bool body_open() const noexcept {
            return !write_aborted_ && send_phase_ == send_phase::body_open;
        }
        [[nodiscard]] bool body_finished() const noexcept {
            return send_phase_ == send_phase::body_ended || send_phase_ == send_phase::trailers_ended;
        }
    };

public:
    // The codec's reference to its credit ledger, released even when the
    // constructor fails after acquiring it.
    struct credit_ledger_reference final {
        explicit credit_ledger_reference(detail::http3_received_data_ledger* ledger) noexcept :
            ledger_(ledger) {}
        credit_ledger_reference(const credit_ledger_reference&) = delete;
        credit_ledger_reference& operator=(const credit_ledger_reference&) = delete;
        ~credit_ledger_reference() {
            ledger_->detach();
            ledger_->release();
        }
        detail::http3_received_data_ledger* ledger_;
    };
    impl(http3_role role, const http3_connection_options& options) :
        role_(role),
        memory_table_(detail::make_c_library_mem_table<nghttp3_mem>(memory_)),
        max_field_section_size_(options.max_field_section_size_),
        max_stream_output_bytes_(options.max_stream_output_bytes_),
        own_data_events_(options.own_data_events_),
        enable_h3_datagram_(options.enable_h3_datagram_),
        frame_observer_(options.frame_observer_),
        field_observer_(options.field_observer_),
        observer_context_(options.observer_context_),
        credit_ledger_(new detail::http3_received_data_ledger(options.received_data_credit_sink_, options.received_data_credit_context_)) {
        if (options.max_concurrent_streams_ == 0 || options.max_stream_output_bytes_ == 0 || options.qpack_max_dtable_capacity_ > (std::numeric_limits<std::size_t>::max)() || options.qpack_blocked_streams_ > (std::numeric_limits<std::size_t>::max)()) {
            throw std::invalid_argument("invalid HTTP/3 connection options");
        }
        if (!options.origins_.empty()) {
            if (role != http3_role::server) {
                throw std::invalid_argument("HTTP/3 client cannot advertise origins");
            }
            const auto payload_size = detail::http_origin_payload_size(options.origins_, 16384);
            if (payload_size.index() != 0) {
                throw std::invalid_argument("invalid HTTP/3 initial ORIGIN advertisement");
            }
            initial_origin_payload_.resize(std::get<0>(payload_size));
            detail::write_http_origin_payload(initial_origin_payload_, options.origins_);
            initial_origin_vec_ = {
                .base = reinterpret_cast<std::uint8_t*>(initial_origin_payload_.data()),
                .len = initial_origin_payload_.size(),
            };
        }
        static const nghttp3_callbacks callbacks = [] {
            nghttp3_callbacks value{};
            value.acked_stream_data = acked_stream_data_callback;
            value.recv_data = recv_data_callback;
            value.deferred_consume = deferred_consume_callback;
            value.begin_headers = begin_headers_callback;
            value.recv_header = recv_header_callback;
            value.end_headers = end_headers_callback;
            value.begin_trailers = begin_trailers_callback;
            value.recv_trailer = recv_header_callback;
            value.end_trailers = end_headers_callback;
            value.stop_sending = stop_sending_callback;
            value.end_stream = end_stream_callback;
            value.reset_stream = reset_stream_callback;
            value.shutdown = shutdown_callback;
            value.recv_origin = recv_origin_callback;
            value.end_origin = end_origin_callback;
            value.stream_close2 = stream_close_callback;
            value.recv_settings2 = recv_settings_callback;
            return value;
        }();

        nghttp3_settings settings;
        nghttp3_settings_default(&settings);
        settings.max_field_section_size = options.max_field_section_size_;
        settings.qpack_max_dtable_capacity = static_cast<std::size_t>(options.qpack_max_dtable_capacity_);
        settings.qpack_encoder_max_dtable_capacity = static_cast<std::size_t>(options.qpack_max_dtable_capacity_);
        settings.qpack_blocked_streams = static_cast<std::size_t>(options.qpack_blocked_streams_);
        settings.enable_connect_protocol = options.enable_connect_protocol_ ? 1 : 0;
        settings.h3_datagram = options.enable_h3_datagram_ ? 1 : 0;
        if (!options.origins_.empty()) {
            settings.origin_list = &initial_origin_vec_;
        }

        const int result = role == http3_role::client ? nghttp3_conn_client_new(&connection_, &callbacks, &settings, &memory_table_, this) : nghttp3_conn_server_new(&connection_, &callbacks, &settings, &memory_table_, this);
        if (result != 0) {
            if (result == NGHTTP3_ERR_NOMEM) {
                memory_.rethrow_allocation_failure();
            }
            memory_.rethrow_if_failed();
            throw std::runtime_error("failed to create nghttp3 connection");
        }
        nghttp3_conn_set_max_concurrent_streams(connection_, options.max_concurrent_streams_);
        if (role == http3_role::server) {
            nghttp3_conn_set_max_client_streams_bidi(connection_, options.max_concurrent_streams_);
        }
    }

    ~impl() {
        // Queued events still own credits; they are destroyed after the sink
        // is detached, like every credit that outlives the codec.
        credit_ledger_.ledger_->detach();
        nghttp3_conn_del(connection_);
    }

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    // Fails the connection with the HTTP/3 code nghttp3 infers for a library
    // error (RFC 9114 §8.1, RFC 9204 §6).
    [[nodiscard]] http3_transport_result fail(int library_error) noexcept {
        if (!connection_error_) {
            connection_error_ = static_cast<http3_error_code>(nghttp3_err_infer_quic_app_error_code(library_error));
        }
        return {.status_ = http3_transport_status::protocol_error, .error_code_ = connection_error_};
    }

    [[nodiscard]] http3_transport_status transport_status_from(int result) noexcept {
        if (result == 0) {
            return http3_transport_status::accepted;
        }
        if (result == NGHTTP3_ERR_WOULDBLOCK) {
            return http3_transport_status::would_block;
        }
        if (result == NGHTTP3_ERR_CONN_CLOSING) {
            return http3_transport_status::closed;
        }
        if (result == NGHTTP3_ERR_STREAM_NOT_FOUND || result == NGHTTP3_ERR_STREAM_IN_USE || result == NGHTTP3_ERR_INVALID_ARGUMENT || result == NGHTTP3_ERR_INVALID_STATE) {
            return http3_transport_status::invalid_state;
        }
        return fail(result).status_;
    }

    // An application submission never fails the connection by its own
    // input; a library failure past validation does, and the stream is gone.
    [[nodiscard]] http3_submit_status submit_status_from(int result) noexcept {
        switch (transport_status_from(result)) {
            case http3_transport_status::accepted:
                return http3_submit_status::accepted;
            case http3_transport_status::invalid_state:
                return http3_submit_status::invalid_state;
            default:
                return http3_submit_status::closed;
        }
    }

    void throw_if_no_memory(int result) {
        if (result == NGHTTP3_ERR_NOMEM) {
            connection_error_ = http3_error_code::internal_error;
            memory_.rethrow_allocation_failure();
        }
        memory_.rethrow_if_failed();
    }

    void rethrow_callback_error() {
        if (callback_error_) {
            connection_error_ = http3_error_code::internal_error;
            auto error = std::exchange(callback_error_, {});
            std::rethrow_exception(error);
        }
        memory_.rethrow_if_failed();
    }

    template <typename function_type>
    static int callback_guard(impl& self, function_type&& function) noexcept {
        try {
            function();
            return 0;
        } catch (...) {
            self.callback_error_ = std::current_exception();
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
    }

    template <typename event_type>
    void publish(event_type event) {
        events_.push_back(http3_event(std::move(event)));
    }

    void publish_action(std::int64_t stream_id, http3_transport_action action, http3_error_code error, http3_transport_action_origin origin) {
        publish(http3_transport_action_event(stream_id, action, error, origin));
    }

    [[nodiscard]] stream_state& ensure_stream(std::int64_t stream_id) {
        auto [position, inserted] = streams_.try_emplace(stream_id);
        (void)inserted;
        return position->second;
    }

    [[nodiscard]] stream_state* find_stream(std::int64_t stream_id) noexcept {
        const auto position = streams_.find(stream_id);
        return position == streams_.end() ? nullptr : &position->second;
    }

    void abandon_header_block(stream_state& stream) noexcept {
        if (stream.building_fields_) {
            stream.building_fields_.reset();
            stream.building_header_size_ = detail::binary_field_section_size(max_field_section_size_);
            --active_header_blocks_;
        }
    }

    void reset_drained_events(bool preserve_packet_events = false) noexcept {
        if (event_offset_ >= events_.size() && (!preserve_packet_events || !packet_input_)) {
            events_.clear();
            event_offset_ = 0;
        }
    }

    void observe_field(std::int64_t stream_id, const stream_state& stream, http3_field_observation_phase phase, std::size_t index, std::string_view name = {}, std::string_view value = {}) const noexcept {
        if (field_observer_ != nullptr) {
            field_observer_(observer_context_,
                http3_field_metadata{
                    .stream_id_ = stream_id,
                    .section_ = stream.building_trailers_ ? http3_field_section_kind::trailer : http3_field_section_kind::header,
                    .field_index_ = index,
                    .phase_ = phase,
                    .name_ = name,
                    .value_ = value,
                });
        }
    }

    void observe_frames(std::int64_t stream_id, std::span<const std::byte> bytes) {
        if (frame_observer_ == nullptr || bytes.empty()) {
            return;
        }
        auto position = std::ranges::find_if(frame_scanners_, [stream_id](const auto& entry_value) noexcept {
            return entry_value.stream_id_ == stream_id;
        });
        if (position == frame_scanners_.end()) {
            frame_scanners_.push_back({stream_id, received_frame_scanner((stream_id & 0x2) == 0)});
            position = std::prev(frame_scanners_.end());
        }
        position->scanner_.scan(bytes, [&](std::uint64_t type, std::uint64_t length) noexcept {
            frame_observer_(observer_context_,
                http3_frame_metadata{
                    .observation_sequence_ = frame_observation_sequence_++,
                    .stream_id_ = stream_id,
                    .type_ = static_cast<http3_frame_type>(type),
                    .payload_length_ = length,
                });
        });
    }

    // A malformed message is a stream error of type H3_MESSAGE_ERROR (RFC 9114
    // §4.1.2): abandon both directions; no later message event names it.
    void reject_received_message(std::int64_t stream_id, stream_state& stream) {
        if (stream.read_aborted_) {
            return;
        }
        stream.read_aborted_ = true;
        publish_action(stream_id, http3_transport_action::stop_sending, http3_error_code::message_error, http3_transport_action_origin::local);
        publish_action(stream_id, http3_transport_action::reset_stream, http3_error_code::message_error, http3_transport_action_origin::local);
    }

    // Client: the server did not process this request (RFC 9114 §4.1.1, §5.2);
    // cancel whichever direction is still open.
    void report_unprocessed(std::int64_t stream_id, stream_state& stream, bool read_open, bool write_open) {
        stream.unprocessed_ = true;
        stream.read_aborted_ = true;
        publish(http3_request_unprocessed_event(stream_id));
        if (read_open) {
            publish_action(stream_id, http3_transport_action::stop_sending, http3_error_code::request_cancelled, http3_transport_action_origin::local);
        }
        if (write_open) {
            publish_action(stream_id, http3_transport_action::reset_stream, http3_error_code::request_cancelled, http3_transport_action_origin::local);
        }
    }

    [[nodiscard]] bool validate_received_response_trailers(stream_state& stream, std::vector<http_header>& fields) {
        if (!stream.received_head_ || !stream.remote_content_.length_complete()) {
            return false;
        }
        auto budget = http_trailer_section_budget();
        for (const auto& field : fields) {
            if (!detail::is_valid_binary_field_name(field.name()) || !is_valid_http_header_value(field.value()) || detail::is_forbidden_http_binary_response_field(field.name()) || !budget.add_field(field.name(), field.value())) {
                return false;
            }
        }
        std::erase_if(fields, [](const http_header& field) {
            return detail::is_forbidden_response_trailer_name(field.name());
        });
        return true;
    }

    // Validates a received response head and queues its event. False rejects
    // the response as malformed (RFC 9114 §4.1.2).
    [[nodiscard]] bool publish_received_response_head(std::int64_t stream_id, stream_state& stream, std::vector<http_header>& fields, bool stream_ended) {
        if (stream.received_head_ || fields.empty() || fields.front().name() != ":status") {
            return false;
        }
        const auto status = http_received_status::try_parse(fields.front().value());
        if (!status || status->status() == http_status::switching_protocols || (status->status().is_informational() && stream_ended)) {
            return false;
        }
        const auto plan = plan_http_response_body(stream.request_method_, status->status());
        detail::http_content_length_state length;
        bool has_length = false;
        for (std::size_t index = 1; index < fields.size(); ++index) {
            const auto& field = fields[index];
            if (!detail::is_valid_binary_field_name(field.name()) || !is_valid_http_header_value(field.value()) || detail::is_forbidden_http_binary_response_field(field.name())) {
                return false;
            }
            if (field.name() == "content-length" && !status->status().is_informational() && plan.content_semantics() != http_response_content_semantics::connect_tunnel) {
                if (length.parse_field(field.value()) != detail::http_content_length_parse_status::ok) {
                    return false;
                }
                has_length = true;
            }
        }
        auto head = detail::http_client_response_head_access::make(*status, http_protocol_version::http3);
        fields.erase(fields.begin());
        if (!fields.empty()) {
            detail::http_client_response_head_access::headers(head) = std::move(fields);
        }
        if (status->status().is_informational()) {
            std::optional<http_client_request_content_signal> signal;
            if (status->status() == http_status::continue_value && stream.continue_pending_) {
                signal = http_client_request_content_signal::continue_value;
            }
            publish(http3_informational_head_event(stream_id, std::move(head), signal));
            stream.continue_pending_ = stream.continue_pending_ && !signal;
            return true;
        }
        auto content = stream.remote_content_;
        if (plan.content_semantics() == http_response_content_semantics::connect_tunnel) {
            content.begin_unbounded();
        } else if (plan.body_suppressed()) {
            content.begin_forbidden();
        } else if (has_length) {
            content.begin_known_length(*length.value());
        } else {
            content.begin_unbounded();
        }
        if (stream_ended && !content.length_complete()) {
            return false;
        }
        if (has_length) {
            detail::http_client_response_head_access::content_length(head, length.value());
        }
        // A final head while Expect still gates the content ends the exchange
        // for it (RFC 9110 §10.1.1).
        const auto signal = stream.continue_pending_ ? std::optional(http_client_request_content_signal::exchange_complete) : std::nullopt;
        publish(http3_response_head_event(stream_id, std::move(head), signal, stream_ended));
        stream.continue_pending_ = false;
        stream.remote_content_ = content;
        stream.received_head_ = true;
        if (plan.content_semantics() == http_response_content_semantics::connect_tunnel) {
            stream.local_content_.begin_unbounded();
        }
        return true;
    }

    static int recv_settings_callback(nghttp3_conn*, const nghttp3_proto_settings* settings, void* user_data) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        if (self.remembered_settings_) {
            const auto& remembered = *self.remembered_settings_;
            if ((remembered.enable_connect_protocol_ && !settings->enable_connect_protocol) || (remembered.h3_datagram_ && !settings->h3_datagram) || settings->max_field_section_size < remembered.max_field_section_size_ || settings->qpack_max_dtable_capacity < remembered.qpack_max_dtable_capacity_ || settings->qpack_blocked_streams < remembered.qpack_blocked_streams_) {
                self.connection_error_ = http3_error_code::settings_error;
                return NGHTTP3_ERR_CALLBACK_FAILURE;
            }
        }
        self.peer_settings_ = http3_peer_settings{settings->enable_connect_protocol != 0, settings->h3_datagram != 0, settings->max_field_section_size, settings->qpack_max_dtable_capacity, settings->qpack_blocked_streams};
        return 0;
    }

    static int acked_stream_data_callback(nghttp3_conn*, std::int64_t stream_id, std::uint64_t bytes, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            auto position = self.streams_.find(stream_id);
            if (position == self.streams_.end()) {
                return;
            }
            auto& stream = position->second;
            if (stream.write_aborted_) {
                return;
            }
            auto& chunks = stream.body_;
            while (bytes != 0 && !chunks.empty()) {
                auto& chunk = chunks.front();
                if (!chunk.provided_) {
                    throw std::logic_error("nghttp3 acknowledged unprovided body data");
                }
                const auto remaining = chunk.bytes_.size() - chunk.acknowledged_;
                const auto consumed = static_cast<std::size_t>((std::min)(bytes, static_cast<std::uint64_t>(remaining)));
                chunk.acknowledged_ += consumed;
                stream.pending_bytes_ -= consumed;
                bytes -= consumed;
                if (chunk.acknowledged_ == chunk.bytes_.size()) {
                    chunks.pop_front();
                }
            }
            if (bytes != 0) {
                throw std::logic_error("nghttp3 acknowledged excess body data");
            }
        });
    }

    static int recv_data_callback(nghttp3_conn*, std::int64_t stream_id, const std::uint8_t* data, std::size_t size, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            auto* stream = self.find_stream(stream_id);
            if (stream == nullptr || stream->read_aborted_) {
                self.publish(http3_deferred_consume_event(stream_id, size));
                return;
            }
            if (self.role_ == http3_role::client) {
                if (!stream->received_head_ || stream->remote_content_.check_accept(size, false) != detail::http_local_content_check::accepted) {
                    self.reject_received_message(stream_id, *stream);
                    self.publish(http3_deferred_consume_event(stream_id, size));
                    return;
                }
                stream->remote_content_.commit(size);
            }
            self.publish(http3_data_event(stream_id, {reinterpret_cast<const char*>(data), size}, self.own_data_events_, http3_received_data_credit(self.credit_ledger_.ledger_, stream_id, size)));
        });
    }

    static int deferred_consume_callback(nghttp3_conn*, std::int64_t stream_id, std::size_t bytes, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            self.publish(http3_deferred_consume_event(stream_id, bytes));
        });
    }

    static int begin_section(impl& self, nghttp3_conn* connection, std::int64_t stream_id, bool trailers) noexcept {
        return callback_guard(self, [&] {
            auto& stream = self.ensure_stream(stream_id);
            if (stream.building_fields_) {
                throw std::logic_error("overlapping nghttp3 header callbacks");
            }
            const auto result = nghttp3_conn_set_stream_user_data(connection, stream_id, &stream);
            if (result != 0) {
                throw std::logic_error("failed to bind nghttp3 stream state");
            }
            stream.building_fields_.emplace();
            stream.building_trailers_ = trailers;
            stream.building_header_size_ = detail::binary_field_section_size(self.max_field_section_size_);
            ++self.active_header_blocks_;
            self.observe_field(stream_id, stream, http3_field_observation_phase::begin, 0);
        });
    }

    static int begin_headers_callback(nghttp3_conn* connection, std::int64_t stream_id, void* user_data, void*) noexcept {
        return begin_section(*static_cast<impl*>(user_data), connection, stream_id, false);
    }

    static int begin_trailers_callback(nghttp3_conn* connection, std::int64_t stream_id, void* user_data, void*) noexcept {
        return begin_section(*static_cast<impl*>(user_data), connection, stream_id, true);
    }

    static int recv_header_callback(nghttp3_conn*, std::int64_t stream_id, std::int32_t, nghttp3_rcbuf* name, nghttp3_rcbuf* value, std::uint8_t, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        auto* stream = self.find_stream(stream_id);
        if (stream == nullptr || !stream->building_fields_) {
            self.callback_error_ = std::make_exception_ptr(std::logic_error("nghttp3 header callback without section"));
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        const auto name_view = rcbuf_view(name);
        const auto value_view = rcbuf_view(value);
        self.observe_field(stream_id, *stream, http3_field_observation_phase::field, stream->building_fields_->size(), name_view, value_view);
        if (!stream->building_header_size_.add(name_view, value_view)) {
            self.connection_error_ = http3_error_code::excessive_load;
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        return callback_guard(self, [&] {
            stream->building_fields_->emplace_back(name_view, value_view);
            if (!stream->building_trailers_ && self.role_ == http3_role::server) {
                if (name_view == ":method") {
                    stream->request_method_ = classify_http_method(value_view);
                    stream->connect_ = value_view == "CONNECT";
                } else if (name_view == ":protocol") {
                    stream->websocket_ = value_view == "websocket";
                }
            }
        });
    }

    // A completed section: a client validates response heads and trailers, a
    // server request heads; trailers wait for the message end.
    void receive_section(std::int64_t stream_id, stream_state& stream, std::vector<http_header> fields, bool trailers, bool stream_ended) {
        if (stream.read_aborted_) {
            return;
        }
        if (role_ == http3_role::client) {
            const bool valid = trailers ? validate_received_response_trailers(stream, fields) : publish_received_response_head(stream_id, stream, fields, stream_ended);
            if (!valid) {
                reject_received_message(stream_id, stream);
            } else if (trailers) {
                stream.received_trailers_ = std::move(fields);
            }
            return;
        }
        if (trailers) {
            stream.received_trailers_ = std::move(fields);
            return;
        }
        stream.received_head_ = true;
        auto head = http3_request_head::try_create(stream_id, std::move(fields), stream_ended, stream.early_data_);
        if (!head) {
            reject_received_message(stream_id, stream);
            return;
        }
        publish(http3_request_head_event(std::move(*head)));
    }

    static int end_headers_callback(nghttp3_conn*, std::int64_t stream_id, int fin, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            auto* stream = self.find_stream(stream_id);
            if (stream == nullptr || !stream->building_fields_) {
                throw std::logic_error("nghttp3 ended unknown field section");
            }
            self.observe_field(stream_id, *stream, http3_field_observation_phase::end, stream->building_fields_->size());
            auto fields = std::move(*stream->building_fields_);
            const bool trailers = stream->building_trailers_;
            self.abandon_header_block(*stream);
            self.receive_section(stream_id, *stream, std::move(fields), trailers, fin != 0);
        });
    }

    // nghttp3 asks for STOP_SENDING or RESET_STREAM itself.
    static int stop_sending_callback(nghttp3_conn*, std::int64_t stream_id, std::uint64_t error, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        if (self.suppress_transport_callbacks_) {
            return 0;
        }
        return callback_guard(self, [&] {
            self.publish_action(stream_id, http3_transport_action::stop_sending, static_cast<http3_error_code>(error), http3_transport_action_origin::local);
        });
    }

    static int reset_stream_callback(nghttp3_conn*, std::int64_t stream_id, std::uint64_t error, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        if (self.suppress_transport_callbacks_) {
            return 0;
        }
        return callback_guard(self, [&] {
            self.publish_action(stream_id, http3_transport_action::reset_stream, static_cast<http3_error_code>(error), http3_transport_action_origin::local);
        });
    }

    static int end_stream_callback(nghttp3_conn*, std::int64_t stream_id, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            auto* stream = self.find_stream(stream_id);
            if (stream == nullptr || stream->read_aborted_) {
                return;
            }
            if (self.role_ == http3_role::client && (!stream->received_head_ || !stream->remote_content_.length_complete())) {
                self.reject_received_message(stream_id, *stream);
                return;
            }
            auto trailers = std::exchange(stream->received_trailers_, std::vector<http_header>{});
            self.publish(http3_message_end_event(stream_id, std::move(trailers)));
        });
    }

    // Client: every open request stream at or above the GOAWAY identifier
    // stays unprocessed (RFC 9114 §5.2), reported in stream order.
    void report_goaway_unprocessed(std::int64_t goaway_id) {
        std::vector<std::int64_t> ids;
        for (const auto& [stream_id, stream] : streams_) {
            if ((stream_id & 3) == 0 && stream_id >= goaway_id && stream.head_submitted() && !stream.unprocessed_) {
                ids.push_back(stream_id);
            }
        }
        std::sort(ids.begin(), ids.end());
        for (const auto stream_id : ids) {
            auto& stream = streams_.find(stream_id)->second;
            report_unprocessed(stream_id, stream, !stream.read_aborted_, !stream.write_aborted_);
        }
    }

    static int shutdown_callback(nghttp3_conn*, std::int64_t id, void* user_data) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            // The shutdown-notice identifiers bound nothing (RFC 9114 §5.2).
            const bool notice = static_cast<std::uint64_t>(id) >= NGHTTP3_SHUTDOWN_NOTICE_STREAM_ID;
            self.publish(http3_goaway_event(notice ? std::nullopt : std::optional<std::int64_t>(id)));
            if (self.role_ == http3_role::client && !notice) {
                self.report_goaway_unprocessed(id);
            }
        });
    }

    static int recv_origin_callback(nghttp3_conn*, const std::uint8_t* origin, std::size_t size, void* user_data) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        if (size > self.max_field_section_size_ || self.building_origin_bytes_ > self.max_field_section_size_ - size) {
            self.connection_error_ = http3_error_code::excessive_load;
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        return callback_guard(self, [&] {
            if (!self.building_origins_) {
                self.building_origins_.emplace();
            }
            self.building_origin_bytes_ += size;
            const std::string_view candidate(reinterpret_cast<const char*>(origin), size);
            if (is_valid_http_serialized_origin(candidate)) {
                self.building_origins_->origins_.emplace_back(candidate);
            }
        });
    }

    static int end_origin_callback(nghttp3_conn*, void* user_data) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            if (!self.building_origins_) {
                self.building_origins_.emplace();
            }
            self.publish(std::move(*self.building_origins_));
            self.building_origins_.reset();
            self.building_origin_bytes_ = 0;
        });
    }

    static int stream_close_callback(nghttp3_conn*, std::uint32_t flags, std::int64_t stream_id, std::uint64_t receive_error, std::uint64_t send_error, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            if (const auto* stream = self.find_stream(stream_id); stream != nullptr && stream->unprocessed_) {
                return;
            }
            std::optional<http3_error_code> error;
            if ((flags & NGHTTP3_STREAM_CLOSE_FLAG_RX_APP_ERROR_CODE_SET) != 0 && receive_error != NGHTTP3_H3_NO_ERROR) {
                error = static_cast<http3_error_code>(receive_error);
            } else if ((flags & NGHTTP3_STREAM_CLOSE_FLAG_TX_APP_ERROR_CODE_SET) != 0 && send_error != NGHTTP3_H3_NO_ERROR) {
                error = static_cast<http3_error_code>(send_error);
            }
            self.publish(http3_stream_close_event(stream_id, error));
        });
    }

    static nghttp3_ssize read_body_callback(nghttp3_conn*, std::int64_t, nghttp3_vec* vectors, std::size_t vector_count, std::uint32_t* flags, void*, void* stream_user_data) noexcept {
        auto& stream = *static_cast<stream_state*>(stream_user_data);
        std::size_t count = 0;
        bool all_provided = true;
        for (auto& chunk : stream.body_) {
            if (chunk.provided_) {
                continue;
            }
            if (count == vector_count) {
                all_provided = false;
                break;
            }
            vectors[count++] = {
                reinterpret_cast<std::uint8_t*>(chunk.bytes_.data()),
                chunk.bytes_.size(),
            };
            chunk.provided_ = true;
        }
        if (stream.body_finished() && all_provided) {
            *flags |= NGHTTP3_DATA_FLAG_EOF;
            if (stream.send_phase_ == send_phase::trailers_ended) {
                *flags |= NGHTTP3_DATA_FLAG_NO_END_STREAM;
            }
        }
        if (count == 0 && !stream.body_finished()) {
            return NGHTTP3_ERR_WOULDBLOCK;
        }
        return static_cast<nghttp3_ssize>(count);
    }

    [[nodiscard]] http3_submit_status submit_head(std::int64_t stream_id, const nghttp3_nv* fields, std::size_t field_count, bool end_stream, bool request, std::optional<std::uint64_t> content_length = std::nullopt) {
        if (connection_error_) {
            return http3_submit_status::closed;
        }
        if ((request && role_ != http3_role::client) || (!request && role_ != http3_role::server)) {
            return http3_submit_status::invalid_state;
        }
        auto position = streams_.find(stream_id);
        bool inserted = false;
        if (position == streams_.end()) {
            auto result = streams_.try_emplace(stream_id);
            position = result.first;
            inserted = result.second;
        }
        auto& stream = position->second;
        if (stream.head_submitted() || stream.write_aborted_) {
            return http3_submit_status::invalid_state;
        }
        const auto previous_content = stream.local_content_;
        stream.send_phase_ = end_stream ? send_phase::head_ended : send_phase::body_open;
        if (end_stream) {
            stream.local_content_.begin_forbidden();
        } else if (content_length) {
            stream.local_content_.begin_known_length(*content_length);
        } else {
            stream.local_content_.begin_unbounded();
        }
        const nghttp3_data_reader reader{.read_data = read_body_callback};
        const int result = request ? nghttp3_conn_submit_request(connection_, stream_id, fields, field_count, end_stream ? nullptr : &reader, &stream) : nghttp3_conn_submit_response(connection_, stream_id, fields, field_count, end_stream ? nullptr : &reader);
        if (result != 0) {
            if (inserted) {
                streams_.erase(position);
            } else {
                stream.send_phase_ = send_phase::head_pending;
                stream.local_content_ = previous_content;
            }
        }
        throw_if_no_memory(result);
        return submit_status_from(result);
    }

    // A server request stream that exists and has no final head yet.
    [[nodiscard]] std::optional<http3_response_head_error> response_head_admission_error(std::int64_t stream_id, std::optional<http_status_code> final_status = std::nullopt) noexcept {
        if (connection_error_) {
            return http3_response_head_error::closed;
        }
        if (role_ != http3_role::server) {
            return http3_response_head_error::invalid_state;
        }
        const auto* stream = find_stream(stream_id);
        if (stream == nullptr || stream->write_aborted_) {
            return http3_response_head_error::closed;
        }
        if (!stream->received_head_ || stream->head_submitted() || (final_status.has_value() && final_status->is_successful() && stream->connect_)) {
            return http3_response_head_error::invalid_state;
        }
        return std::nullopt;
    }

    [[nodiscard]] static http3_response_head_error head_error_from(http3_submit_status status) noexcept {
        return status == http3_submit_status::invalid_state ? http3_response_head_error::invalid_message : http3_response_head_error::closed;
    }

    [[nodiscard]] std::uint64_t outbound_field_limit() const noexcept {
        const auto peer_limit = peer_settings_ ? peer_settings_->max_field_section_size_ : (remembered_settings_ ? remembered_settings_->max_field_section_size_ : std::numeric_limits<std::uint64_t>::max());
        return (std::min)(max_field_section_size_, peer_limit);
    }

    [[nodiscard]] std::optional<http3_response_head_error> submit_final_response(std::int64_t stream_id, const http_response& response, const detail::http_framed_response_head_plan& plan, bool end_stream, std::optional<std::uint64_t> content_length) {
        std::size_t name_bytes = 0;
        for (const auto& header : response.headers()) {
            name_bytes += header.name().size();
        }
        outbound_field_section section(name_bytes, response.headers().size() + 3);
        if (!detail::append_http_binary_response_fields(section, response, plan, outbound_field_limit())) {
            return http3_response_head_error::invalid_message;
        }
        const auto status = submit_head(stream_id, section.data(), section.size(), end_stream, false, content_length);
        if (status != http3_submit_status::accepted) {
            return head_error_from(status);
        }
        const auto parsed = parse_http_priority(response.headers());
        if (const auto* fields = std::get_if<http_priority_fields>(&parsed); fields != nullptr && (fields->urgency_ || fields->incremental_)) {
            nghttp3_pri priority{};
            if (nghttp3_conn_get_stream_priority2(connection_, &priority, stream_id) == 0) {
                if (fields->urgency_) {
                    priority.urgency = fields->urgency_->value();
                }
                if (fields->incremental_) {
                    priority.inc = *fields->incremental_ ? 1 : 0;
                }
                (void)nghttp3_conn_set_server_stream_priority(connection_, stream_id, &priority);
            }
        }
        return std::nullopt;
    }

    // The stream whose open content may take DATA or end now. A gone or
    // abandoned stream is an expected race (closed); content that is not open
    // is a caller ordering error.
    // nullptr: the stream is gone or its send half was abandoned, an expected
    // race (closed).
    [[nodiscard]] stream_state* open_content_stream(std::int64_t stream_id) {
        if (connection_error_) {
            return nullptr;
        }
        auto* stream = find_stream(stream_id);
        if (stream == nullptr || stream->write_aborted_) {
            return nullptr;
        }
        if (!stream->body_open()) {
            throw std::logic_error("HTTP/3 stream content is not open");
        }
        return stream;
    }

    [[nodiscard]] std::variant<stream_state*, http3_finish_status> finishable_stream(std::int64_t stream_id) {
        auto* stream = open_content_stream(stream_id);
        if (stream == nullptr) {
            return http3_finish_status::closed;
        }
        if (!stream->local_content_.length_complete()) {
            return http3_finish_status::invalid_message;
        }
        return stream;
    }

    [[nodiscard]] http3_finish_status finish_status_from(int result) noexcept {
        return transport_status_from(result) == http3_transport_status::accepted ? http3_finish_status::accepted : http3_finish_status::closed;
    }

    [[nodiscard]] http3_finish_status submit_trailer_nv(std::int64_t stream_id, stream_state& stream, const nghttp3_nv* nv, std::size_t count) {
        const int result = nghttp3_conn_submit_trailers(connection_, stream_id, nv, count);
        if (result != 0) {
            throw_if_no_memory(result);
            return finish_status_from(result);
        }
        stream.send_phase_ = send_phase::trailers_ended;
        const int resume_result = nghttp3_conn_resume_stream(connection_, stream_id);
        if (resume_result != 0) {
            throw_if_no_memory(resume_result);
            return finish_status_from(resume_result);
        }
        return http3_finish_status::accepted;
    }

    // Publishes a peer's RESET_STREAM/STOP_SENDING. A client reports a
    // request the server rejected before its final head as unprocessed
    // (RFC 9114 §4.1.1) instead. `read_open`/`write_open`: the stream's
    // directions before this termination.
    [[nodiscard]] http3_transport_status publish_peer_termination(std::int64_t stream_id, std::optional<http3_error_code> peer_error, http3_transport_action action, bool read_open, bool write_open) noexcept {
        if (!peer_error.has_value()) {
            return http3_transport_status::accepted;
        }
        reset_drained_events(true);
        if (callback_guard(*this, [&] {
            auto* stream = find_stream(stream_id);
            if (stream != nullptr && stream->unprocessed_) {
                return;
            }
            if (role_ == http3_role::client && stream != nullptr && *peer_error == http3_error_code::request_rejected && stream->head_submitted() && !stream->received_head_) {
                report_unprocessed(stream_id, *stream, read_open && action == http3_transport_action::stop_sending, write_open && action == http3_transport_action::reset_stream);
                return;
            }
            publish_action(stream_id, action, *peer_error, http3_transport_action_origin::peer);
        }) != 0) {
            return fail(NGHTTP3_ERR_CALLBACK_FAILURE).status_;
        }
        return http3_transport_status::accepted;
    }

    http3_role role_;
    detail::http_c_library_memory memory_;
    nghttp3_mem memory_table_;
    std::uint64_t max_field_section_size_;
    std::size_t max_stream_output_bytes_;
    bool own_data_events_;
    bool enable_h3_datagram_;
    http3_frame_observer_type frame_observer_;
    http3_field_observer_type field_observer_;
    void* observer_context_;
    // Declared before events_ so queued credits never outlive it.
    credit_ledger_reference credit_ledger_;
    std::optional<http3_peer_settings> peer_settings_;
    std::optional<http3_peer_settings> remembered_settings_;
    nghttp3_conn* connection_{nullptr};
    std::vector<http3_event> events_;
    std::size_t event_offset_{0};
    std::unordered_map<std::int64_t, stream_state> streams_;
    // Per received stream, only with a frame observer.
    struct stream_frame_scanner final {
        std::int64_t stream_id_{0};
        received_frame_scanner scanner_;
    };
    std::vector<stream_frame_scanner> frame_scanners_;
    std::uint64_t frame_observation_sequence_{0};
    std::vector<char> initial_origin_payload_;
    nghttp3_vec initial_origin_vec_{};
    std::optional<std::int64_t> control_stream_id_;
    detail::http3_control_extension_output control_output_;
    std::optional<http_origin_advertisement> building_origins_;
    std::uint64_t building_origin_bytes_{0};
    std::size_t active_header_blocks_{0};
    std::optional<http3_error_code> connection_error_;
    std::exception_ptr callback_error_;
    bool control_streams_bound_{false};
    bool packet_input_{false};
    bool suppress_transport_callbacks_{false};
    bool drain_final_{false};
};

http3_connection::http3_connection(http3_role role, const http3_connection_options& options) :
    impl_(std::make_unique<impl>(role, options)) {}

http3_connection http3_connection::client(http3_connection_options options) {
    return http3_connection(http3_role::client, options);
}

http3_connection http3_connection::server(http3_connection_options options) {
    return http3_connection(http3_role::server, options);
}

http3_connection::~http3_connection() = default;
http3_connection::http3_connection(http3_connection&&) noexcept = default;
http3_connection& http3_connection::operator=(http3_connection&&) noexcept = default;

http3_role http3_connection::role() const noexcept {
    return impl_->role_;
}

http3_transport_status http3_connection::bind_control_streams(std::int64_t control_stream_id, std::int64_t qpack_encoder_stream_id, std::int64_t qpack_decoder_stream_id) {
    if (impl_->connection_error_) {
        return http3_transport_status::closed;
    }
    if (impl_->control_streams_bound_ || control_stream_id < 0 || qpack_encoder_stream_id < 0 || qpack_decoder_stream_id < 0 || control_stream_id == qpack_encoder_stream_id || control_stream_id == qpack_decoder_stream_id || qpack_encoder_stream_id == qpack_decoder_stream_id) {
        return http3_transport_status::invalid_state;
    }
    int result = nghttp3_conn_bind_control_stream(impl_->connection_, control_stream_id);
    if (result == 0) {
        result = nghttp3_conn_bind_qpack_streams(impl_->connection_, qpack_encoder_stream_id, qpack_decoder_stream_id);
    }
    impl_->throw_if_no_memory(result);
    if (result != 0) {
        return impl_->transport_status_from(result);
    }
    impl_->control_stream_id_ = control_stream_id;
    impl_->control_streams_bound_ = true;
    return http3_transport_status::accepted;
}

http3_transport_result http3_connection::read_stream(std::int64_t stream_id, std::span<const std::byte> bytes, bool fin, std::uint64_t ts_nanoseconds) {
    const bool pending_events = impl_->event_offset_ < impl_->events_.size();
    if (pending_events && !impl_->packet_input_) {
        return {.status_ = http3_transport_status::events_pending};
    }
    if (impl_->connection_error_) {
        return {.status_ = http3_transport_status::closed, .error_code_ = impl_->connection_error_};
    }
    impl_->reset_drained_events();
    impl_->observe_frames(stream_id, bytes);
    const auto result = nghttp3_conn_read_stream2(impl_->connection_, stream_id, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), fin ? 1 : 0, ts_nanoseconds);
    impl_->rethrow_callback_error();
    impl_->throw_if_no_memory(static_cast<int>(result));
    if (result < 0) {
        return impl_->fail(static_cast<int>(result));
    }
    return {.credit_bytes_ = static_cast<std::size_t>(result)};
}

std::optional<http3_event> http3_connection::next_event() {
    if (impl_->event_offset_ == impl_->events_.size()) {
        return std::nullopt;
    }
    return std::move(impl_->events_[impl_->event_offset_++]);
}

void http3_connection::rethrow_if_failed() {
    impl_->rethrow_callback_error();
}

http3_transport_result http3_connection::peek_output(std::size_t max_bytes, http3_stream_output& output) {
    if (impl_->connection_error_) {
        return {.status_ = http3_transport_status::closed, .error_code_ = impl_->connection_error_};
    }
    output.stream_id_ = -1;
    output.buffer_count_ = 0;
    output.fin_ = false;
    if (impl_->control_stream_id_) {
        if (const auto result = impl_->control_output_.peek(impl_->connection_, *impl_->control_stream_id_, max_bytes, output)) {
            return *result;
        }
    }

    std::array<nghttp3_vec, 16> vectors;
    std::int64_t stream_id = -1;
    int fin = 0;
    const auto result = nghttp3_conn_writev_stream(impl_->connection_, &stream_id, &fin, vectors.data(), vectors.size());
    impl_->rethrow_callback_error();
    impl_->throw_if_no_memory(static_cast<int>(result));
    if (result < 0) {
        return impl_->fail(static_cast<int>(result));
    }
    if (stream_id == -1) {
        return {.status_ = http3_transport_status::would_block};
    }

    std::size_t available = 0;
    for (nghttp3_ssize index = 0; index < result; ++index) {
        if (vectors[static_cast<std::size_t>(index)].len > (std::numeric_limits<std::size_t>::max)() - available) {
            return impl_->fail(NGHTTP3_ERR_H3_INTERNAL_ERROR);
        }
        available += vectors[static_cast<std::size_t>(index)].len;
    }
    if (available != 0 && max_bytes == 0) {
        return {.status_ = http3_transport_status::would_block};
    }
    const auto offered = (std::min)(available, max_bytes);
    std::size_t remaining = offered;
    for (nghttp3_ssize index = 0; index < result && remaining != 0; ++index) {
        const auto& vector = vectors[static_cast<std::size_t>(index)];
        const auto count = (std::min)(vector.len, remaining);
        if (count != 0) {
            output.buffers_[output.buffer_count_++] = {reinterpret_cast<const std::byte*>(vector.base), count};
            remaining -= count;
        }
    }
    output.stream_id_ = stream_id;
    output.fin_ = fin != 0 && offered == available;
    return {};
}

http3_transport_status http3_connection::commit_output(std::int64_t stream_id, std::size_t bytes) noexcept {
    if (impl_->connection_error_) {
        return http3_transport_status::closed;
    }
    if (impl_->control_stream_id_.has_value() && stream_id == *impl_->control_stream_id_) {
        return impl_->transport_status_from(impl_->control_output_.commit(impl_->connection_, stream_id, bytes));
    }
    return impl_->transport_status_from(nghttp3_conn_add_write_offset(impl_->connection_, stream_id, bytes));
}

http3_transport_status http3_connection::acknowledge_output(std::int64_t stream_id, std::uint64_t bytes) noexcept {
    if (impl_->connection_error_) {
        return http3_transport_status::closed;
    }
    if (impl_->control_stream_id_.has_value() && stream_id == *impl_->control_stream_id_) {
        return impl_->transport_status_from(impl_->control_output_.acknowledge(impl_->connection_, stream_id, bytes));
    }
    return impl_->transport_status_from(nghttp3_conn_add_ack_offset(impl_->connection_, stream_id, bytes));
}

http3_transport_status http3_connection::unblock_stream(std::int64_t stream_id) noexcept {
    if (impl_->connection_error_) {
        return http3_transport_status::closed;
    }
    if (impl_->control_stream_id_.has_value() && stream_id == *impl_->control_stream_id_) {
        impl_->control_output_.unblock();
    }
    return impl_->transport_status_from(nghttp3_conn_unblock_stream(impl_->connection_, stream_id));
}

http3_submit_status http3_connection::submit_request_head(std::int64_t stream_id, const http3_regular_request_head_view& request) {
    if (impl_->connection_error_) {
        return http3_submit_status::closed;
    }
    if (impl_->role_ != http3_role::client || stream_id < 0 || (stream_id & 3) != 0) {
        return http3_submit_status::invalid_state;
    }
    const auto& content = request.content_;
    const auto declared_length = content.declared_length();
    const auto* known_length = content.known_length_content();
    // The head ends the stream only when no content can follow it.
    const bool end_stream = content.without_content() != nullptr || (known_length != nullptr && known_length->length().value() == 0);
    const auto content_indication = content.without_content() == nullptr && (!declared_length || declared_length->value() != 0) ? http_request_content_indication::will_follow : http_request_content_indication::no_content;
    const auto method = classify_http_method(request.method_);
    const std::optional<std::string_view> authority = request.authority_ ? std::optional<std::string_view>(*request.authority_) : std::nullopt;
    std::array<char, http_max_unsigned_decimal_size> length_bytes;
    const auto length = declared_length && request.method_ != "TRACE" ? http_format_unsigned(length_bytes, declared_length->value()) : std::string_view{};
    const auto headers = static_cast<std::span<const http_header_view>>(request.headers_);
    if (!detail::http_binary_is_valid_outbound_regular_request_head(request.method_, method, request.scheme_, detail::http_uri_scheme_default_port(request.scheme_), authority, request.target_, headers, content_indication, length, request.expectation_, request.field_source_, impl_->outbound_field_limit())) {
        return http3_submit_status::invalid_message;
    }
    std::size_t name_bytes = 0;
    for (const auto& header : headers) {
        name_bytes += header.name().size();
    }
    outbound_field_section section(name_bytes, headers.size() + 6);
    section.add(":method", request.method_);
    section.add(":scheme", request.scheme_);
    if (authority) {
        section.add(":authority", *authority);
    }
    section.add(":path", request.target_);
    for (const auto& header : headers) {
        section.add_lowercase(header.name(), header.value());
    }
    if (request.expectation_ == http_client_request_expectation::continue_value) {
        section.add("expect", detail::http_continue_expectation_token);
    }
    if (!length.empty()) {
        section.add("content-length", length);
    }
    const auto status = impl_->submit_head(stream_id, section.data(), section.size(), end_stream, true, declared_length ? std::optional<std::uint64_t>(declared_length->value()) : std::nullopt);
    if (status == http3_submit_status::accepted) {
        auto& stream = *impl_->find_stream(stream_id);
        stream.request_method_ = method;
        stream.continue_pending_ = request.expectation_ == http_client_request_expectation::continue_value;
    }
    return status;
}

http3_submit_status http3_connection::submit_request_head(std::int64_t stream_id, const http3_connect_request_head_view& request) {
    if (impl_->connection_error_) {
        return http3_submit_status::closed;
    }
    if (impl_->role_ != http3_role::client || stream_id < 0 || (stream_id & 3) != 0) {
        return http3_submit_status::invalid_state;
    }
    detail::request_target_view target;
    detail::binary_field_section_size budget(impl_->outbound_field_limit());
    detail::request_field_state fields(detail::request_field_framing::binary);
    const auto headers = static_cast<std::span<const http_header_view>>(request.headers_);
    if (!detail::parse_request_target(http_known_method::connect, request.authority_, target) || !budget.add(":method", "CONNECT") || !budget.add(":authority", request.authority_) || !detail::http_binary_are_valid_outbound_request_headers(std::string_view(request.authority_), 0, headers, false, false, budget, fields, request.field_source_)) {
        return http3_submit_status::invalid_message;
    }
    std::size_t name_bytes = 0;
    for (const auto& header : headers) {
        name_bytes += header.name().size();
    }
    outbound_field_section section(name_bytes, headers.size() + 2);
    section.add(":method", "CONNECT");
    section.add(":authority", request.authority_);
    for (const auto& header : headers) {
        section.add_lowercase(header.name(), header.value());
    }
    const auto status = impl_->submit_head(stream_id, section.data(), section.size(), false, true);
    if (status == http3_submit_status::accepted) {
        auto& stream = *impl_->find_stream(stream_id);
        stream.request_method_ = http_known_method::connect;
        stream.connect_ = true;
        stream.local_content_.begin_forbidden();
    }
    return status;
}

http3_submit_status http3_connection::submit_request_head(std::int64_t stream_id, const http3_extended_connect_request_head_view& request) {
    if (impl_->connection_error_) {
        return http3_submit_status::closed;
    }
    if (impl_->role_ != http3_role::client || stream_id < 0 || (stream_id & 3) != 0) {
        return http3_submit_status::invalid_state;
    }
    const auto settings = impl_->peer_settings_ ? impl_->peer_settings_ : impl_->remembered_settings_;
    if (!settings) {
        return http3_submit_status::peer_settings_pending;
    }
    if (!settings->enable_connect_protocol_) {
        return http3_submit_status::peer_capability_unavailable;
    }
    const bool websocket = http_ascii_equals_ignore_case(request.protocol_, "websocket");
    const auto protocol = websocket ? std::string_view("websocket") : std::string_view(request.protocol_);
    const auto port = detail::http_uri_scheme_default_port(request.scheme_);
    detail::binary_field_section_size budget(impl_->outbound_field_limit());
    detail::request_field_state fields(detail::request_field_framing::binary);
    const auto headers = static_cast<std::span<const http_header_view>>(request.headers_);
    if (!is_valid_http_header_name(request.protocol_) || !detail::is_valid_uri_scheme(request.scheme_) || (websocket && port == 0) || !detail::is_valid_binary_request_authority(request.scheme_, request.authority_) || !detail::http_binary_is_valid_extended_connect_path(request.scheme_, request.target_) || !budget.add(":method", "CONNECT") || !budget.add(":protocol", protocol) || !budget.add(":scheme", request.scheme_) || !budget.add(":authority", request.authority_) || !budget.add(":path", request.target_) || !detail::http_binary_are_valid_outbound_request_headers(std::string_view(request.authority_), port, headers, !websocket, false, budget, fields, request.field_source_, websocket)) {
        return http3_submit_status::invalid_message;
    }
    std::size_t name_bytes = 0;
    for (const auto& header : headers) {
        name_bytes += header.name().size();
    }
    outbound_field_section section(name_bytes, headers.size() + 5);
    section.add(":method", "CONNECT");
    section.add(":protocol", protocol);
    section.add(":scheme", request.scheme_);
    section.add(":authority", request.authority_);
    section.add(":path", request.target_);
    for (const auto& header : headers) {
        section.add_lowercase(header.name(), header.value());
    }
    const auto status = impl_->submit_head(stream_id, section.data(), section.size(), false, true);
    if (status == http3_submit_status::accepted) {
        auto& stream = *impl_->find_stream(stream_id);
        stream.request_method_ = http_known_method::connect;
        stream.connect_ = true;
        stream.websocket_ = websocket;
        stream.local_content_.begin_forbidden();
    }
    return status;
}

http3_data_submit_result http3_connection::submit_data(std::int64_t stream_id, std::span<const std::byte> bytes) {
    auto* stream = impl_->open_content_stream(stream_id);
    if (stream == nullptr) {
        return {.status_ = http3_data_submit_status::closed};
    }
    if (bytes.empty()) {
        return {};
    }
    switch (stream->local_content_.check_accept(bytes.size(), false)) {
        case detail::http_local_content_check::accepted:
            break;
        case detail::http_local_content_check::length_exceeded:
        case detail::http_local_content_check::length_incomplete:
            return {.status_ = http3_data_submit_status::invalid_message};
        case detail::http_local_content_check::not_started:
        case detail::http_local_content_check::forbidden:
            throw std::logic_error("HTTP/3 stream content is not allowed");
    }
    const auto budget = available_output_bytes(stream_id);
    const auto accepted = (std::min)(bytes.size(), budget);
    if (accepted == 0) {
        return {.status_ = http3_data_submit_status::backpressured};
    }
    stream->body_.emplace_back(bytes.first(accepted));
    stream->pending_bytes_ += accepted;
    const int result = nghttp3_conn_resume_stream(impl_->connection_, stream_id);
    if (result != 0) {
        stream->body_.pop_back();
        stream->pending_bytes_ -= accepted;
        impl_->throw_if_no_memory(result);
        (void)impl_->transport_status_from(result);
        return {.status_ = http3_data_submit_status::closed};
    }
    stream->local_content_.commit(accepted);
    return {.consumed_bytes_ = accepted, .status_ = accepted == bytes.size() ? http3_data_submit_status::complete : http3_data_submit_status::backpressured};
}

std::size_t http3_connection::pending_output_bytes(std::int64_t stream_id) const noexcept {
    const auto position = impl_->streams_.find(stream_id);
    return position == impl_->streams_.end() ? 0 : position->second.pending_bytes_;
}

bool http3_connection::output_flushed(std::int64_t stream_id) const noexcept {
    return nghttp3_conn_is_stream_flushed(impl_->connection_, stream_id) != 0;
}

std::size_t http3_connection::available_output_bytes(std::int64_t stream_id) const noexcept {
    const auto position = impl_->streams_.find(stream_id);
    if (impl_->connection_error_ || position == impl_->streams_.end() || !position->second.body_open() || position->second.local_content_.forbidden() != nullptr) {
        return 0;
    }
    const auto limit = impl_->max_stream_output_bytes_;
    return limit - (std::min)(limit, position->second.pending_bytes_);
}

http3_finish_status http3_connection::finish_stream(std::int64_t stream_id) {
    const auto validated = impl_->finishable_stream(stream_id);
    if (const auto* status = std::get_if<http3_finish_status>(&validated)) {
        return *status;
    }
    auto* stream = std::get<impl::stream_state*>(validated);
    stream->send_phase_ = impl::send_phase::body_ended;
    const int result = nghttp3_conn_resume_stream(impl_->connection_, stream_id);
    if (result != 0) {
        stream->send_phase_ = impl::send_phase::body_open;
        impl_->throw_if_no_memory(result);
        return impl_->finish_status_from(result);
    }
    return http3_finish_status::accepted;
}

http3_finish_status http3_connection::finish_request(std::int64_t stream_id, std::span<const http_header_view> trailers) {
    if (impl_->role_ != http3_role::client) {
        throw std::logic_error("HTTP/3 server stream has no request to finish");
    }
    if (trailers.empty()) {
        return finish_stream(stream_id);
    }
    const auto validated = impl_->finishable_stream(stream_id);
    if (const auto* status = std::get_if<http3_finish_status>(&validated)) {
        return *status;
    }
    auto* stream = std::get<impl::stream_state*>(validated);
    detail::binary_field_section_size budget(impl_->outbound_field_limit());
    std::size_t name_bytes = 0;
    for (const auto& field : trailers) {
        if (!detail::http_binary_is_valid_outbound_regular_header(field.name(), field.value()) || detail::is_forbidden_http_request_trailer_name(field.name()) || !budget.add(field.name(), field.value())) {
            return http3_finish_status::invalid_message;
        }
        name_bytes += field.name().size();
    }
    outbound_field_section section(name_bytes, trailers.size());
    for (const auto& field : trailers) {
        section.add_lowercase(field.name(), field.value());
    }
    return impl_->submit_trailer_nv(stream_id, *stream, section.data(), section.size());
}

std::variant<http_buffered_response_write_plan, http3_response_head_error> http3_connection::submit_response_head(std::int64_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    if (const auto error = impl_->response_head_admission_error(stream_id, response.status())) {
        return *error;
    }
    if (write_plan.body_plan().request_method() != impl_->find_stream(stream_id)->request_method_ || !write_plan.matches_response(response)) {
        return http3_response_head_error::response_plan_mismatch;
    }
    if (detail::http_binary_final_response_control_plan(response).has_value()) {
        return http3_response_head_error::invalid_message;
    }
    const auto head_plan_result = detail::http_framed_buffered_response_head_plan(write_plan, response);
    const auto* head_plan = std::get_if<detail::http_framed_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr) {
        const auto error = std::get<detail::http_framed_response_head_plan_error>(head_plan_result);
        return error == detail::http_framed_response_head_plan_error::response_status_mismatch || error == detail::http_framed_response_head_plan_error::response_representation_mismatch ? http3_response_head_error::response_plan_mismatch : http3_response_head_error::invalid_message;
    }
    const bool send_body = write_plan.send_body();
    if (const auto error = impl_->submit_final_response(stream_id, response, *head_plan, !send_body, send_body ? std::optional<std::uint64_t>(write_plan.content_length()) : std::nullopt)) {
        return *error;
    }
    return write_plan;
}

std::variant<http_response_stream_commit_plan, http3_response_head_error> http3_connection::submit_streaming_response_head(std::int64_t stream_id, http_response head, http_response_trailer_intent trailer_intent) {
    if (const auto error = impl_->response_head_admission_error(stream_id, head.status())) {
        return *error;
    }
    auto commit_plan = plan_http_response_stream_commit(http_response_stream_framing::http3_frames, impl_->find_stream(stream_id)->request_method_, head.status(), trailer_intent);
    if (!commit_plan.trailer_intent_allowed() || detail::http_binary_final_response_control_plan(head).has_value()) {
        return http3_response_head_error::invalid_message;
    }
    auto stream_head = prepare_http_response_stream_head(std::move(head), commit_plan);
    const auto head_plan_result = detail::http_framed_streaming_response_head_plan(commit_plan.body_plan(), stream_head.response());
    const auto* head_plan = std::get_if<detail::http_framed_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr) {
        return http3_response_head_error::invalid_message;
    }
    const bool end_stream = commit_plan.head_disposition() == http_response_stream_head_disposition::message_ended;
    if (const auto error = impl_->submit_final_response(stream_id, stream_head.response(), *head_plan, end_stream, end_stream ? std::nullopt : head_plan->streaming_content_length())) {
        return *error;
    }
    return commit_plan;
}

http3_submit_status http3_connection::submit_interim_response_head(std::int64_t stream_id, const http_interim_response_head& response) {
    if (impl_->connection_error_) {
        return http3_submit_status::closed;
    }
    const auto* stream = impl_->find_stream(stream_id);
    if (impl_->role_ != http3_role::server || stream == nullptr) {
        return stream == nullptr ? http3_submit_status::closed : http3_submit_status::invalid_state;
    }
    if (!stream->received_head_ || stream->head_submitted()) {
        return http3_submit_status::invalid_state;
    }
    const auto headers = response.headers();
    std::size_t name_bytes = 0;
    for (const auto& header : headers) {
        name_bytes += header.name().size();
    }
    outbound_field_section section(name_bytes, headers.size() + 1);
    if (!detail::append_http_binary_interim_response_fields(section, response, impl_->outbound_field_limit())) {
        return http3_submit_status::invalid_message;
    }
    const int result = nghttp3_conn_submit_info(impl_->connection_, stream_id, section.data(), section.size());
    impl_->throw_if_no_memory(result);
    return impl_->submit_status_from(result);
}

http3_finish_status http3_connection::finish_response(std::int64_t stream_id, const http_response_trailer_section& trailers) {
    if (trailers.empty()) {
        return finish_stream(stream_id);
    }
    const auto validated = impl_->finishable_stream(stream_id);
    if (const auto* status = std::get_if<http3_finish_status>(&validated)) {
        return *status;
    }
    auto* stream = std::get<impl::stream_state*>(validated);
    const auto fields = trailers.fields();
    std::size_t name_bytes = 0;
    detail::binary_field_section_size budget(impl_->outbound_field_limit());
    for (const auto& field : fields) {
        if (!budget.add(field.name(), field.value())) {
            return http3_finish_status::invalid_message;
        }
        name_bytes += field.name().size();
    }
    outbound_field_section section(name_bytes, fields.size());
    for (const auto& field : fields) {
        section.add_lowercase(field.name(), field.value());
    }
    return impl_->submit_trailer_nv(stream_id, *stream, section.data(), section.size());
}

http3_submit_status http3_connection::submit_connect_response_head(std::int64_t stream_id, const http_response& response) {
    if (const auto error = impl_->response_head_admission_error(stream_id)) {
        return *error == http3_response_head_error::closed ? http3_submit_status::closed : http3_submit_status::invalid_state;
    }
    const auto* stream = impl_->find_stream(stream_id);
    if (!stream->received_head_ || !stream->connect_) {
        return http3_submit_status::invalid_state;
    }
    if (!detail::is_valid_http_binary_connect_response_head(response) || detail::http_binary_final_response_control_plan(response).has_value()) {
        return http3_submit_status::invalid_message;
    }
    const auto plan = detail::http_framed_connect_response_head_plan(plan_http_response_body(http_known_method::connect, response.status()));
    const auto* head = std::get_if<detail::http_framed_response_head_plan>(&plan);
    if (head == nullptr) {
        return http3_submit_status::invalid_message;
    }
    if (const auto error = impl_->submit_final_response(stream_id, response, *head, false, std::nullopt)) {
        return *error == http3_response_head_error::closed ? http3_submit_status::closed : http3_submit_status::invalid_message;
    }
    return http3_submit_status::accepted;
}

std::variant<websocket_extended_connect_negotiation, http3_response_head_error> http3_connection::submit_websocket_handshake(std::int64_t stream_id, const http_request& request, const websocket_handshake_validation_result& validation, websocket_server_handshake_options options) {
    if (const auto error = impl_->response_head_admission_error(stream_id)) {
        return *error;
    }
    const auto* stream = impl_->find_stream(stream_id);
    if (!stream->received_head_ || !stream->connect_ || !stream->websocket_ || validation.accepted() == nullptr) {
        return http3_response_head_error::invalid_state;
    }
    auto negotiation = detail::make_websocket_server_negotiation(request, options);
    outbound_field_section section(0, negotiation.response_headers().size() + 4);
    if (!detail::append_http_binary_websocket_response_fields(section, negotiation, impl_->outbound_field_limit())) {
        return http3_response_head_error::invalid_message;
    }
    const auto status = impl_->submit_head(stream_id, section.data(), section.size(), false, false);
    if (status != http3_submit_status::accepted) {
        return impl::head_error_from(status);
    }
    const auto compression = negotiation.compression();
    return websocket_extended_connect_negotiation(std::move(negotiation).take_subprotocol(), compression);
}

http3_transport_result http3_connection::read_datagram(std::span<const std::byte> payload) {
    if (impl_->connection_error_) {
        return {.status_ = http3_transport_status::closed, .error_code_ = impl_->connection_error_};
    }
    if (events_pending() && !impl_->packet_input_) {
        return {.status_ = http3_transport_status::events_pending};
    }
    impl_->reset_drained_events();
    const auto parsed = detail::decode_quic_varint({reinterpret_cast<const char*>(payload.data()), payload.size()});
    if (!impl_->enable_h3_datagram_ || parsed.index() != 0 || std::get<0>(parsed).value_ > ((std::uint64_t{1} << 60) - 1)) {
        impl_->connection_error_ = http3_error_code::datagram_error;
        return {.status_ = http3_transport_status::protocol_error, .error_code_ = impl_->connection_error_};
    }
    const auto id = static_cast<std::int64_t>(std::get<0>(parsed).value_ * 4);
    const auto* stream = impl_->find_stream(id);
    if (stream != nullptr && !stream->read_aborted_ && (stream->received_head_ || (impl_->role_ == http3_role::client && stream->head_submitted()))) {
        const auto content = payload.subspan(std::get<0>(parsed).encoded_bytes_);
        impl_->publish(http3_datagram_event(id, std::string(reinterpret_cast<const char*>(content.data()), content.size())));
    }
    return {};
}

std::optional<http3_datagram_prefix> http3_connection::datagram_prefix(std::int64_t stream_id) const noexcept {
    const auto* stream = impl_->find_stream(stream_id);
    if (impl_->connection_error_ || !impl_->enable_h3_datagram_ || !impl_->peer_settings_ || !impl_->peer_settings_->h3_datagram_ || stream_id < 0 || (stream_id & 3) != 0 || stream == nullptr || stream->write_aborted_) {
        return std::nullopt;
    }
    http3_datagram_prefix prefix;
    const auto encoded = detail::encode_quic_varint({reinterpret_cast<char*>(prefix.bytes_.data()), prefix.bytes_.size()}, static_cast<std::uint64_t>(stream_id) / 4);
    if (encoded.index() != 0) {
        return std::nullopt;
    }
    prefix.size_ = std::get<0>(encoded);
    return prefix;
}

std::optional<http3_peer_settings> http3_connection::peer_settings() const noexcept {
    return impl_->peer_settings_;
}

bool http3_connection::header_block_in_progress() const noexcept {
    return impl_->active_header_blocks_ != 0;
}

http3_submit_status http3_connection::submit_origin_advertisement(std::span<const std::string_view> origins) {
    if (impl_->connection_error_) {
        return http3_submit_status::closed;
    }
    if (impl_->role_ != http3_role::server || !impl_->control_streams_bound_ || impl_->control_output_.shutdown_requested() || impl_->drain_final_) {
        return http3_submit_status::invalid_state;
    }
    return impl_->control_output_.queue(origins, impl_->max_stream_output_bytes_);
}

http3_submit_status http3_connection::submit_priority_update(std::int64_t stream_id, http_priority_fields fields) {
    if (impl_->connection_error_) {
        return http3_submit_status::closed;
    }
    if (impl_->role_ != http3_role::client || !impl_->control_streams_bound_ || stream_id < 0 || (stream_id & 3) != 0) {
        return http3_submit_status::invalid_state;
    }
    std::array<char, 32> encoded{};
    const auto size = encode_http_priority(encoded, fields);
    if (size.index() != 0) {
        return http3_submit_status::invalid_message;
    }
    const int result = nghttp3_conn_set_client_stream_priority(impl_->connection_, stream_id, reinterpret_cast<const std::uint8_t*>(encoded.data()), std::get<0>(size));
    impl_->throw_if_no_memory(result);
    return impl_->submit_status_from(result);
}

std::optional<http_priority> http3_connection::stream_priority(std::int64_t stream_id) const noexcept {
    if (impl_->connection_error_ || impl_->role_ != http3_role::server || stream_id < 0 || (stream_id & 3) != 0) {
        return std::nullopt;
    }
    nghttp3_pri pri{};
    if (nghttp3_conn_get_stream_priority2(impl_->connection_, &pri, stream_id) != 0) {
        return std::nullopt;
    }
    const auto urgency = http_priority_urgency::try_from_value(pri.urgency);
    if (!urgency) {
        return std::nullopt;
    }
    return http_priority{*urgency, pri.inc != 0};
}

void http3_connection::mark_early_data(std::int64_t stream_id) {
    impl_->ensure_stream(stream_id).early_data_ = true;
}

void http3_connection::remember_peer_settings(http3_peer_settings settings) noexcept {
    impl_->remembered_settings_ = settings;
}

http3_transport_status http3_connection::close_stream(std::int64_t stream_id, std::optional<http3_error_code> receive_error, std::optional<http3_error_code> send_error) noexcept {
    if (impl_->connection_error_) {
        return http3_transport_status::closed;
    }
    std::uint32_t flags = 0;
    if (receive_error) {
        flags |= NGHTTP3_STREAM_CLOSE_FLAG_RX_APP_ERROR_CODE_SET;
    }
    if (send_error) {
        flags |= NGHTTP3_STREAM_CLOSE_FLAG_TX_APP_ERROR_CODE_SET;
    }
    const int result = nghttp3_conn_close_stream2(impl_->connection_, flags, stream_id, static_cast<std::uint64_t>(receive_error.value_or(http3_error_code::no_error)), static_cast<std::uint64_t>(send_error.value_or(http3_error_code::no_error)));
    std::erase_if(impl_->frame_scanners_, [stream_id](const auto& entry_value) noexcept {
        return entry_value.stream_id_ == stream_id;
    });
    if (impl_->callback_error_) {
        // Only an allocation of the close event can fail; the connection
        // cannot report the closure any more.
        impl_->callback_error_ = {};
        impl_->connection_error_ = http3_error_code::internal_error;
        return http3_transport_status::protocol_error;
    }
    if (result == 0 || result == NGHTTP3_ERR_STREAM_NOT_FOUND) {
        if (auto* stream = impl_->find_stream(stream_id)) {
            impl_->abandon_header_block(*stream);
            impl_->streams_.erase(stream_id);
        }
        return http3_transport_status::accepted;
    }
    return impl_->transport_status_from(result);
}
http3_submit_status http3_connection::begin_drain() noexcept {
    if (impl_->connection_error_) {
        return http3_submit_status::closed;
    }
    return impl_->submit_status_from(nghttp3_conn_submit_shutdown_notice(impl_->connection_));
}

http3_submit_status http3_connection::finish_drain() noexcept {
    if (impl_->connection_error_) {
        return http3_submit_status::closed;
    }
    if (impl_->drain_final_) {
        return http3_submit_status::accepted;
    }
    const auto status = impl_->submit_status_from(nghttp3_conn_shutdown(impl_->connection_));
    impl_->drain_final_ = status == http3_submit_status::accepted;
    return status;
}

bool http3_connection::drain_final() const noexcept {
    return impl_->drain_final_;
}

bool http3_connection::drained() const noexcept {
    return impl_->drain_final_ && nghttp3_conn_is_drained2(impl_->connection_) != 0;
}

std::optional<http3_error_code> http3_connection::connection_error() const noexcept {
    return impl_->connection_error_;
}

void http3_connection::begin_packet_input() noexcept {
    impl_->packet_input_ = true;
}

void http3_connection::end_packet_input() noexcept {
    impl_->packet_input_ = false;
}

bool http3_connection::events_pending() const noexcept {
    return impl_->event_offset_ < impl_->events_.size();
}

http3_transport_status http3_connection::block_stream(std::int64_t stream_id) noexcept {
    if (impl_->connection_error_) {
        return http3_transport_status::closed;
    }
    if (impl_->control_stream_id_.has_value() && stream_id == *impl_->control_stream_id_) {
        impl_->control_output_.block();
    }
    nghttp3_conn_block_stream(impl_->connection_, stream_id);
    return http3_transport_status::accepted;
}

http3_transport_status http3_connection::shutdown_stream_read(std::int64_t stream_id, std::optional<http3_error_code> peer_error) noexcept {
    if (impl_->connection_error_) {
        return http3_transport_status::closed;
    }
    bool write_open = false;
    if (auto* stream = impl_->find_stream(stream_id)) {
        impl_->abandon_header_block(*stream);
        stream->read_aborted_ = true;
        write_open = !stream->write_aborted_;
    }
    impl_->suppress_transport_callbacks_ = true;
    const int result = nghttp3_conn_shutdown_stream_read(impl_->connection_, stream_id);
    impl_->suppress_transport_callbacks_ = false;
    if (result != 0) {
        return impl_->transport_status_from(result);
    }
    return impl_->publish_peer_termination(stream_id, peer_error, http3_transport_action::reset_stream, false, write_open);
}

http3_transport_status http3_connection::shutdown_stream_write(std::int64_t stream_id, std::optional<http3_error_code> peer_error) noexcept {
    if (impl_->connection_error_) {
        return http3_transport_status::closed;
    }
    if (impl_->control_stream_id_.has_value() && stream_id == *impl_->control_stream_id_) {
        impl_->control_output_.shutdown();
    }
    bool read_open = false;
    if (auto* stream = impl_->find_stream(stream_id)) {
        stream->write_aborted_ = true;
        stream->body_.clear();
        stream->pending_bytes_ = 0;
        read_open = !stream->read_aborted_;
    }
    nghttp3_conn_shutdown_stream_write(impl_->connection_, stream_id);
    return impl_->publish_peer_termination(stream_id, peer_error, http3_transport_action::stop_sending, read_open, false);
}

void http3_connection::set_max_client_streams_bidi(std::uint64_t max_streams) noexcept {
    if (impl_->role_ == http3_role::server) {
        nghttp3_conn_set_max_client_streams_bidi(impl_->connection_, max_streams);
    }
}

http3_submit_status submit_http3_websocket_client_request(http3_connection& connection, std::int64_t stream_id, const websocket_client_negotiation& negotiation, std::string_view scheme, std::string_view authority, std::string_view target, borrowed_http_headers headers, http_request_field_source source) {
    const auto staged_headers = detail::stage_websocket_extended_connect_fields(negotiation, headers.view());
    return connection.submit_request_head(stream_id,
        http3_extended_connect_request_head_view{
            .protocol_ = "websocket",
            .scheme_ = scheme,
            .authority_ = authority,
            .target_ = target,
            .headers_ = staged_headers.view(),
            .field_source_ = source,
        });
}

http3_request_head::http3_request_head(std::int64_t stream_id, std::vector<http_header> fields, bool stream_ended, bool early_data) :
    fields_(std::move(fields)),
    stream_id_(stream_id),
    stream_ended_(stream_ended),
    early_data_(early_data) {}

std::optional<http3_request_head> http3_request_head::try_create(std::int64_t stream_id, std::vector<http_header> fields, bool stream_ended, bool early_data) {
    http3_request_head head(stream_id, std::move(fields), stream_ended, early_data);
    if (!head.validate()) {
        return std::nullopt;
    }
    return head;
}

std::string_view http3_request_head::field_value(std::uint32_t index) const noexcept {
    return index == absent ? std::string_view{} : fields_[index].value();
}

std::string_view http3_request_head::method() const& noexcept {
    return field_value(method_);
}

std::string_view http3_request_head::scheme() const& noexcept {
    return field_value(scheme_);
}

std::string_view http3_request_head::authority() const& noexcept {
    return field_value(authority_);
}

std::string_view http3_request_head::path() const& noexcept {
    return field_value(path_);
}

std::string_view http3_request_head::protocol() const& noexcept {
    return field_value(protocol_);
}

bool http3_request_head::connect() const noexcept {
    return classify_http_method(method()) == http_known_method::connect;
}

websocket_handshake_validation_result validate_http3_websocket_handshake(const http3_request_head& head, const http_request& request, websocket_handshake_validation_options options) noexcept {
    if (!head.connect() || head.protocol() != "websocket" || head.stream_ended() || (head.declared_content_length().has_value() && *head.declared_content_length() != 0)) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    return detail::validate_extended_connect_websocket_headers(request, options);
}

bool http3_request_head::validate() {
    if (fields_.size() > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    detail::request_field_state request_fields{detail::request_field_framing::binary};
    detail::http_content_length_state content_length;
    bool content_length_seen = false;
    bool regular_seen = false;
    const auto assign_pseudo = [](std::uint32_t& slot, std::uint32_t index) noexcept {
        if (slot != absent) {
            return false;
        }
        slot = index;
        return true;
    };

    for (std::uint32_t index = 0; index < fields_.size(); ++index) {
        const auto name = fields_[index].name();
        const auto value = fields_[index].value();
        if (!name.empty() && name.front() == ':') {
            // RFC 9114 §4.3: pseudo-header fields precede every regular field.
            if (regular_seen) {
                return false;
            }
            bool assigned = false;
            if (name == ":method") {
                assigned = is_valid_http_method_token(value) && assign_pseudo(method_, index);
            } else if (name == ":scheme") {
                assigned = detail::is_valid_uri_scheme(value) && assign_pseudo(scheme_, index);
            } else if (name == ":authority") {
                assigned = detail::is_valid_uri_authority(value) && assign_pseudo(authority_, index);
            } else if (name == ":path") {
                assigned = !value.empty() && assign_pseudo(path_, index);
            } else if (name == ":protocol") {
                assigned = is_valid_http_header_name(value) && assign_pseudo(protocol_, index);
            }
            if (!assigned) {
                return false;
            }
            continue;
        }
        regular_seen = true;
        if (!detail::is_valid_binary_field_name(name) || !is_valid_http_header_value(value) || detail::is_forbidden_http_binary_connection_field(name)) {
            return false;
        }
        const auto kind = detail::classify_http_field(name);
        if (detail::validate_request_field(kind, value, detail::http_field_list_role::recipient, request_fields) != detail::request_field_status::ok) {
            return false;
        }
        switch (kind) {
            case detail::http_field_kind::host:
                host_ = index;
                break;
            case detail::http_field_kind::cookie: {
                if (value.empty()) {
                    continue;
                }
                if (first_cookie_ == absent) {
                    if (value.size() > max_http_header_bytes) {
                        return false;
                    }
                    first_cookie_ = index;
                    continue;
                }
                if (!detail::append_binary_cookie_crumb(cookie_, value, true, field_value(first_cookie_))) {
                    return false;
                }
                continue;
            }
            case detail::http_field_kind::expect:
                // An extensible list: unsupported members stay for the Web
                // product's 417 policy.
                expectations_.parse_field(value);
                break;
            case detail::http_field_kind::content_length:
                if (content_length.parse_field(value) != detail::http_content_length_parse_status::ok) {
                    return false;
                }
                content_length_seen = true;
                break;
            default:
                break;
        }
        // The remaining suffix bounds every kept index without a second pass.
        if (regular_.empty()) {
            regular_.reserve(fields_.size() - index);
        }
        regular_.push_back(regular_field{index, kind});
    }

    if (method_ == absent) {
        return false;
    }
    const auto method_value = method();
    const auto known_method = classify_http_method(method_value);
    const bool connect_method = known_method == http_known_method::connect;
    const auto scheme_value = scheme();
    const auto path_value = path();

    if (connect_method && protocol_ == absent) {
        // RFC 9114 §4.4: CONNECT names only its authority.
        const auto target = authority();
        const auto parsed = parse_http_authority(target);
        if (scheme_ != absent || path_ != absent || authority_ == absent || !parsed || parsed->host().empty() || parsed->port_kind() != http_authority_port_kind::value) {
            return false;
        }
    } else {
        if (scheme_ == absent || path_ == absent) {
            return false;
        }
        if (protocol_ != absent) {
            // RFC 9220 §3: only an extended CONNECT carries :protocol.
            if (!connect_method || authority_ == absent || !detail::is_valid_origin_form_target(path_value) || !detail::is_valid_extended_connect_scheme(protocol(), scheme_value)) {
                return false;
            }
        } else if (!detail::is_valid_origin_or_asterisk_form_target(known_method, path_value)) {
            return false;
        }
    }
    if (!detail::validate_binary_request_authority_and_host(scheme_value, path_value, authority_ != absent, authority(), host_ != absent, field_value(host_))) {
        return false;
    }
    if (content_length_seen) {
        declared_content_length_ = content_length.value();
        // A HEADERS frame that ended the stream or a CONNECT request carries no content.
        if ((stream_ended_ || connect_method) && declared_content_length_ != std::uint64_t{0}) {
            return false;
        }
    }
    const bool synthesize_host = detail::binary_request_synthesizes_host(host_ != absent, authority_ != absent, scheme_value, authority());
    const bool has_cookie = first_cookie_ != absent;
    if (regular_.size() + static_cast<std::size_t>(synthesize_host) + static_cast<std::size_t>(has_cookie) > max_http_header_fields) {
        return false;
    }
    return true;
}

http_request http3_request_head::build_request(borrowed_text body) const& {
    const auto cookie_value = cookie_.empty() ? field_value(first_cookie_) : std::string_view(cookie_);
    const detail::binary_request_materialization_spec spec{
        .version_ = http_protocol_version::http3,
        .target_form_ = http_request_target_form::http3,
        .method_ = method(),
        .scheme_ = scheme(),
        .authority_ = authority(),
        .host_ = field_value(host_),
        .path_ = path(),
        .protocol_ = protocol(),
        .cookie_ = cookie_value,
        .body_ = body.view(),
        .regular_header_count_ = regular_.size(),
        .has_authority_ = authority_ != absent,
        .has_host_ = host_ != absent,
        .has_path_ = path_ != absent,
        .has_cookie_ = !cookie_value.empty(),
        .standard_connect_ = connect() && protocol_ == absent,
        .failure_message_ = "validated HTTP/3 request cannot be materialized",
    };
    return detail::materialize_binary_http_request(spec, [this](std::size_t index) noexcept {
        const auto& field = regular_[index];
        return std::pair{http_header_view{fields_[field.index_].name(), fields_[field.index_].value()}, field.kind_};
    });
}

http3_fingerprint_collector::http3_fingerprint_collector(http3_fingerprint_options options) :
    max_frames_(options.max_frames_),
    max_bytes_(options.max_bytes_) {}

void http3_fingerprint_collector::reset() noexcept {
    control_frames_.clear();
    pseudo_headers_.clear();
    fingerprint_.clear();
    frames_seen_ = 0;
    bytes_seen_ = 0;
    first_request_stream_id_ = -1;
    saw_regular_header_ = false;
    phase_ = phase::collecting;
}

void http3_fingerprint_collector::observe_frame(const http3_frame_metadata& frame) noexcept {
    if (phase_ != phase::collecting) {
        return;
    }
    try {
        ++frames_seen_;
        if (frames_seen_ > max_frames_ || frame.payload_length_ > max_bytes_ - bytes_seen_) {
            phase_ = phase::failed;
            return;
        }
        bytes_seen_ += static_cast<std::size_t>(frame.payload_length_);
        // Client unidirectional control stream has stream_id % 4 == 2 (usually stream 2).
        if (frame.stream_id_ >= 0 && (frame.stream_id_ % 4) == 2) {
            if (!control_frames_.empty()) {
                control_frames_.push_back(';');
            }
            http_append_unsigned(control_frames_, static_cast<std::uint64_t>(frame.type_));
            control_frames_.push_back(':');
            http_append_unsigned(control_frames_, frame.payload_length_);
        }
    } catch (...) {
        phase_ = phase::failed;
    }
}

void http3_fingerprint_collector::observe_field(const http3_field_metadata& field) noexcept {
    if (phase_ != phase::collecting || field.section_ != http3_field_section_kind::header) {
        return;
    }
    try {
        switch (field.phase_) {
            case http3_field_observation_phase::begin:
                if (field.stream_id_ < 0 || (field.stream_id_ % 4) != 0) {
                    phase_ = phase::failed;
                    return;
                }
                if (first_request_stream_id_ < 0) {
                    first_request_stream_id_ = field.stream_id_;
                }
                break;
            case http3_field_observation_phase::field: {
                if (first_request_stream_id_ < 0) {
                    if (field.stream_id_ < 0 || (field.stream_id_ % 4) != 0) {
                        phase_ = phase::failed;
                        return;
                    }
                    first_request_stream_id_ = field.stream_id_;
                }
                if (field.stream_id_ != first_request_stream_id_) {
                    return;
                }
                const std::size_t field_bytes = field.name_.size() + field.value_.size();
                if (field_bytes > max_bytes_ - bytes_seen_) {
                    phase_ = phase::failed;
                    return;
                }
                bytes_seen_ += field_bytes;
                if (!field.name_.empty() && field.name_.front() == ':') {
                    if (saw_regular_header_) {
                        phase_ = phase::failed;
                        return;
                    }
                    std::string_view token{};
                    if (field.name_ == ":method") {
                        token = "m";
                    } else if (field.name_ == ":authority") {
                        token = "a";
                    } else if (field.name_ == ":scheme") {
                        token = "s";
                    } else if (field.name_ == ":path") {
                        token = "p";
                    } else if (field.name_ == ":protocol") {
                        token = "pr";
                    } else if (field.name_.size() > 1U) {
                        token = field.name_.substr(1, 1);
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
            case http3_field_observation_phase::end:
                if (field.stream_id_ != first_request_stream_id_ || first_request_stream_id_ < 0) {
                    return;
                }
                if (pseudo_headers_.empty()) {
                    phase_ = phase::failed;
                    return;
                }
                fingerprint_.clear();
                fingerprint_.append(control_frames_);
                fingerprint_.push_back('|');
                http_append_unsigned(fingerprint_, static_cast<std::uint64_t>(first_request_stream_id_));
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
