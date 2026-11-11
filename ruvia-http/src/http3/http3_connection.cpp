#include "ruvia/http/http3_connection.h"

#include <nghttp3/nghttp3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <deque>
#include <exception>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_status.h"

#include "coding/http_content_length.h"
#include "field/binary_field_section_size.h"
#include "field/http_connection_fields.h"
#include "field/http_date_calendar.h"
#include "field/http_interim_response_validation.h"
#include "field/http_lower_header_name.h"
#include "field/http_origin_payload.h"
#include "field/http_request_field_validation.h"
#include "http3/http3_control_extension_output.h"
#include "parser/http_header_descriptor_storage.h"
#include "request/http_binary_request_fields.h"
#include "response/http_binary_response_fields.h"
#include "response/http_final_response_control_plan.h"
#include "response/http_framed_response_head_plan.h"
#include "response/http_local_content_state.h"
#include "response/http_response_header_state.h"
#include "util/http_address_overlap.h"
#include "util/http_c_library_memory.h"
#include "util/quic_varint.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia {

namespace {

[[nodiscard]] std::string_view rcbuf_view(nghttp3_rcbuf* buffer) noexcept {
    const auto value = nghttp3_rcbuf_get_buf(buffer);
    return {reinterpret_cast<const char*>(value.base), value.len};
}

[[nodiscard]] nghttp3_nv make_nv(std::string_view name, std::string_view value,
    bool sensitive = false) noexcept {
    return {
        .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
        .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
        .namelen = name.size(),
        .valuelen = value.size(),
        .flags = static_cast<std::uint8_t>(
            (sensitive || detail::http_field_never_indexed(name)) ? NGHTTP3_NV_FLAG_NEVER_INDEX : NGHTTP3_NV_FLAG_NONE),
    };
}

// One outbound field section. Already-lowercase names borrow their source;
// changed names share one lazily reserved buffer that stays address-stable.
// nghttp3 copies the section when it is submitted.
class outbound_field_section final {
public:
    outbound_field_section(std::pmr::memory_resource* resource, std::size_t name_bytes,
        std::size_t field_count)
        : name_bytes_(name_bytes),
          names_(resource),
          fields_(resource) {
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
    std::pmr::string names_;
    std::array<char, 96> values_{};
    std::size_t value_bytes_{0};
    detail::http_header_descriptor_storage<nghttp3_nv> fields_;
};

}  // namespace

http3_field::http3_field(std::string_view name, std::string_view value,
    std::pmr::memory_resource* resource)
    : header_(name, value, detail::http_pmr_resource_or_default(resource)) {}

http3_headers_event::http3_headers_event(std::int64_t stream_id,
    std::pmr::vector<http3_field> fields, bool stream_ended, bool trailers, bool early_data) noexcept
    : stream_id_(stream_id),
      fields_(std::move(fields)),
      stream_ended_(stream_ended),
      trailers_(trailers),
      early_data_(early_data) {}

http3_headers_event::http3_headers_event(const http3_headers_event& other)
    : http3_headers_event(other, other.fields_.get_allocator().resource()) {}

http3_headers_event::http3_headers_event(
    const http3_headers_event& other, std::pmr::memory_resource* resource)
    : stream_id_(other.stream_id_),
      fields_(resource),
      stream_ended_(other.stream_ended_),
      trailers_(other.trailers_),
      status_(other.status_),
      early_data_(other.early_data_) {
    fields_.reserve(other.fields_.size());
    for (const auto& field : other.fields_) {
        fields_.emplace_back(field.name(), field.value(), resource);
    }
}

void http3_headers_event::swap(http3_headers_event& other) noexcept {
    std::swap(stream_id_, other.stream_id_);
    fields_.swap(other.fields_);
    std::swap(stream_ended_, other.stream_ended_);
    std::swap(trailers_, other.trailers_);
    std::swap(early_data_, other.early_data_);
    std::swap(status_, other.status_);
}

http3_headers_event& http3_headers_event::operator=(const http3_headers_event& other) {
    if (this != &other) {
        http3_headers_event candidate(other, fields_.get_allocator().resource());
        swap(candidate);
    }
    return *this;
}

http3_headers_event& http3_headers_event::operator=(http3_headers_event&& other) {
    if (this != &other) {
        // Equivalent resource objects do not share a lifetime for nested fields.
        if (fields_.get_allocator().resource() == other.fields_.get_allocator().resource()) {
            http3_headers_event candidate(std::move(other));
            swap(candidate);
        } else {
            *this = other;
        }
    }
    return *this;
}

http3_datagram_event::http3_datagram_event(std::int64_t stream_id, std::pmr::string payload) noexcept
    : stream_id_(stream_id),
      payload_(std::move(payload)) {}

http3_datagram_event::http3_datagram_event(const http3_datagram_event& other)
    : stream_id_(other.stream_id_),
      payload_(other.payload_, other.payload_.get_allocator()) {}

http3_datagram_event& http3_datagram_event::operator=(const http3_datagram_event& other) {
    if (this != &other) {
        payload_ = other.payload_;
        stream_id_ = other.stream_id_;
    }
    return *this;
}

http3_datagram_event& http3_datagram_event::operator=(http3_datagram_event&& other) {
    if (this != &other) {
        payload_ = std::move(other.payload_);
        stream_id_ = other.stream_id_;
    }
    return *this;
}

http3_data_event::http3_data_event(std::int64_t stream_id,
    std::string_view bytes, std::pmr::memory_resource* resource,
    bool own_bytes)
    : stream_id_(stream_id),
      bytes_(bytes),
      owned_bytes_(detail::http_pmr_resource_or_default(resource)),
      owns_bytes_(own_bytes) {
    if (owns_bytes_) {
        owned_bytes_.assign(bytes);
        bytes_ = owned_bytes_;
    }
}

http3_data_event::http3_data_event(http3_data_event&& other) noexcept
    : stream_id_(other.stream_id_),
      bytes_(other.bytes_),
      owned_bytes_(std::move(other.owned_bytes_)),
      owns_bytes_(other.owns_bytes_) {
    if (owns_bytes_) {
        bytes_ = owned_bytes_;
    }
}

http3_data_event& http3_data_event::operator=(http3_data_event&& other) {
    if (this == &other) {
        return *this;
    }
    const auto borrowed_bytes = other.bytes_;
    if (!other.owns_bytes_ && detail::http_text_aliases_string(borrowed_bytes, owned_bytes_)) {
        std::pmr::string snapshot(borrowed_bytes, owned_bytes_.get_allocator());
        owned_bytes_.swap(snapshot);
        stream_id_ = other.stream_id_;
        owns_bytes_ = true;
        bytes_ = owned_bytes_;
        return *this;
    }
    owned_bytes_ = std::move(other.owned_bytes_);
    stream_id_ = other.stream_id_;
    owns_bytes_ = other.owns_bytes_;
    bytes_ = owns_bytes_ ? std::string_view(owned_bytes_) : borrowed_bytes;
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
        body_chunk(std::span<const std::byte> bytes,
            std::pmr::memory_resource* resource)
            : bytes_(reinterpret_cast<const char*>(bytes.data()), bytes.size(), resource) {}

        std::pmr::string bytes_;
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
        explicit stream_state(std::pmr::memory_resource* resource)
            : body_(resource),
              building_header_size_(0) {}

        std::pmr::deque<body_chunk> body_;
        std::optional<http3_headers_event> building_headers_;
        detail::binary_field_section_size building_header_size_;
        // Submitted DATA payload not yet acknowledged by the peer.
        std::size_t pending_bytes_{0};
        detail::http_local_content_state local_content_;
        detail::http_local_content_state remote_content_;
        http_known_method request_method_{http_known_method::unknown};
        send_phase send_phase_{send_phase::head_pending};
        bool received_head_{false};
        bool connect_{false};
        bool websocket_{false};
        bool early_data_{false};
        bool read_aborted_{false};
        bool write_aborted_{false};

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
    struct queued_event final {
        http3_event event_;
        bool local_transport_action_{false};
    };
    impl(http3_role role, const http3_connection_options& options)
        : role_(role),
          resource_(detail::http_pmr_resource_or_default(options.resource_)),
          memory_(resource_),
          memory_table_(detail::make_c_library_mem_table<nghttp3_mem>(memory_)),
          max_field_section_size_(options.max_field_section_size_),
          max_stream_output_bytes_(options.max_stream_output_bytes_),
          own_data_events_(options.own_data_events_),
          enable_h3_datagram_(options.enable_h3_datagram_),
          events_(resource_),
          streams_(resource_),
          initial_origin_payload_(resource_),
          control_output_(resource_) {
        if (options.max_concurrent_streams_ == 0 ||
            options.max_stream_output_bytes_ == 0 ||
            options.qpack_max_dtable_capacity_ >
                (std::numeric_limits<std::size_t>::max)() ||
            options.qpack_blocked_streams_ >
                (std::numeric_limits<std::size_t>::max)()) {
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
        settings.qpack_max_dtable_capacity =
            static_cast<std::size_t>(options.qpack_max_dtable_capacity_);
        settings.qpack_encoder_max_dtable_capacity =
            static_cast<std::size_t>(options.qpack_max_dtable_capacity_);
        settings.qpack_blocked_streams =
            static_cast<std::size_t>(options.qpack_blocked_streams_);
        settings.enable_connect_protocol = options.enable_connect_protocol_ ? 1 : 0;
        settings.h3_datagram = options.enable_h3_datagram_ ? 1 : 0;
        if (!options.origins_.empty()) {
            settings.origin_list = &initial_origin_vec_;
        }

        const int result = role == http3_role::client
                               ? nghttp3_conn_client_new(
                                     &connection_, &callbacks, &settings, &memory_table_, this)
                               : nghttp3_conn_server_new(
                                     &connection_, &callbacks, &settings, &memory_table_, this);
        if (result != 0) {
            if (result == NGHTTP3_ERR_NOMEM) {
                memory_.rethrow_allocation_failure();
            }
            memory_.rethrow_if_failed();
            throw std::runtime_error("failed to create nghttp3 connection");
        }
        nghttp3_conn_set_max_concurrent_streams(
            connection_, options.max_concurrent_streams_);
        if (role == http3_role::server) {
            nghttp3_conn_set_max_client_streams_bidi(
                connection_, options.max_concurrent_streams_);
        }
    }

    ~impl() {
        nghttp3_conn_del(connection_);
    }

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    [[nodiscard]] http3_result fail(int library_error) noexcept {
        if (!connection_error_) {
            connection_error_ = nghttp3_err_infer_quic_app_error_code(library_error);
        }
        return {http3_status::protocol_error, *connection_error_};
    }

    [[nodiscard]] http3_status status_from(int result) noexcept {
        if (result == 0) {
            return http3_status::accepted;
        }
        if (result == NGHTTP3_ERR_WOULDBLOCK) {
            return http3_status::would_block;
        }
        if (result == NGHTTP3_ERR_CONN_CLOSING) {
            return http3_status::closed;
        }
        if (result == NGHTTP3_ERR_STREAM_NOT_FOUND ||
            result == NGHTTP3_ERR_STREAM_IN_USE ||
            result == NGHTTP3_ERR_INVALID_ARGUMENT ||
            result == NGHTTP3_ERR_INVALID_STATE) {
            return http3_status::invalid_state;
        }
        (void)fail(result);
        return http3_status::protocol_error;
    }
    void throw_if_no_memory(int result) {
        if (result == NGHTTP3_ERR_NOMEM) {
            connection_error_ = NGHTTP3_H3_INTERNAL_ERROR;
            memory_.rethrow_allocation_failure();
        }
        memory_.rethrow_if_failed();
    }

    void rethrow_callback_error() {
        if (callback_error_) {
            connection_error_ = NGHTTP3_H3_INTERNAL_ERROR;
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

    [[nodiscard]] stream_state& ensure_stream(std::int64_t stream_id) {
        auto [position, inserted] = streams_.try_emplace(stream_id, resource_);
        (void)inserted;
        return position->second;
    }

    [[nodiscard]] stream_state* find_stream(std::int64_t stream_id) noexcept {
        const auto position = streams_.find(stream_id);
        return position == streams_.end() ? nullptr : &position->second;
    }

    void abandon_header_block(stream_state& stream) noexcept {
        if (stream.building_headers_) {
            stream.building_headers_.reset();
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

    void reject_received_message(std::int64_t stream_id, stream_state& stream) {
        if (stream.read_aborted_) {
            return;
        }
        stream.read_aborted_ = true;
        const auto error = static_cast<std::uint64_t>(http3_error_code::message_error);
        events_.push_back({http3_transport_action_event{
                               stream_id, error, http3_transport_action::stop_sending},
            true});
        events_.push_back({http3_transport_action_event{
                               stream_id, error, http3_transport_action::reset_stream},
            true});
    }

    [[nodiscard]] bool validate_received_response(stream_state& stream, http3_headers_event& head) {
        if (stream.read_aborted_) {
            return false;
        }
        if (head.trailers_) {
            if (!stream.received_head_ || !stream.remote_content_.length_complete()) {
                return false;
            }
            auto budget = http_trailer_section_budget();
            for (const auto& field : head.fields_) {
                if (!detail::is_valid_binary_field_name(field.name()) ||
                    !is_valid_http_header_value(field.value()) ||
                    detail::is_forbidden_http_binary_response_field(field.name()) ||
                    !budget.add_field(field.name(), field.value())) {
                    return false;
                }
            }
            std::erase_if(head.fields_, [](const http3_field& field) {
                return detail::is_forbidden_response_trailer_name(field.name());
            });
            return true;
        }
        if (stream.received_head_ || head.fields_.empty() || head.fields_.front().name() != ":status") {
            return false;
        }
        const auto status = http_received_status::try_parse(head.fields_.front().value());
        if (!status || status->status() == http_status::switching_protocols ||
            (status->status().is_informational() && head.stream_ended_)) {
            return false;
        }
        const auto plan = plan_http_response_body(stream.request_method_, status->status());
        detail::http_content_length_state length;
        bool has_length = false;
        for (std::size_t index = 1; index < head.fields_.size(); ++index) {
            const auto& field = head.fields_[index];
            if (!detail::is_valid_binary_field_name(field.name()) ||
                !is_valid_http_header_value(field.value()) ||
                detail::is_forbidden_http_binary_response_field(field.name())) {
                return false;
            }
            if (field.name() == "content-length" && !status->status().is_informational() &&
                plan.content_semantics() != http_response_content_semantics::connect_tunnel) {
                if (length.parse_field(field.value()) != detail::http_content_length_parse_status::ok) {
                    return false;
                }
                has_length = true;
            }
        }
        if (!status->status().is_informational()) {
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
            if (head.stream_ended_ && !content.length_complete()) {
                return false;
            }
            stream.remote_content_ = content;
            stream.received_head_ = true;
            if (plan.content_semantics() == http_response_content_semantics::connect_tunnel) {
                stream.local_content_.begin_unbounded();
            }
        }
        head.status_ = *status;
        head.fields_.erase(head.fields_.begin());
        return true;
    }

    static int recv_settings_callback(nghttp3_conn*, const nghttp3_proto_settings* settings,
        void* user_data) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        if (self.remembered_settings_) {
            const auto& remembered = *self.remembered_settings_;
            if ((remembered.enable_connect_protocol_ && !settings->enable_connect_protocol) ||
                (remembered.h3_datagram_ && !settings->h3_datagram) ||
                settings->max_field_section_size < remembered.max_field_section_size_ ||
                settings->qpack_max_dtable_capacity < remembered.qpack_max_dtable_capacity_ ||
                settings->qpack_blocked_streams < remembered.qpack_blocked_streams_) {
                self.connection_error_ = NGHTTP3_H3_SETTINGS_ERROR;
                return NGHTTP3_ERR_CALLBACK_FAILURE;
            }
        }
        self.peer_settings_ = http3_peer_settings{
            settings->enable_connect_protocol != 0, settings->h3_datagram != 0,
            settings->max_field_section_size, settings->qpack_max_dtable_capacity,
            settings->qpack_blocked_streams};
        return 0;
    }

    static int acked_stream_data_callback(nghttp3_conn*, std::int64_t stream_id,
        std::uint64_t bytes, void* user_data, void*) noexcept {
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

    static int recv_data_callback(nghttp3_conn*, std::int64_t stream_id,
        const std::uint8_t* data, std::size_t size, void* user_data,
        void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            if (self.role_ == http3_role::client) {
                auto* stream = self.find_stream(stream_id);
                if (stream == nullptr || stream->read_aborted_) {
                    self.events_.push_back({http3_deferred_consume_event{stream_id, size}});
                    return;
                }
                if (!stream->received_head_ ||
                    stream->remote_content_.check_accept(size, false) != detail::http_local_content_check::accepted) {
                    self.reject_received_message(stream_id, *stream);
                    self.events_.push_back({http3_deferred_consume_event{stream_id, size}});
                    return;
                }
                stream->remote_content_.commit(size);
            }
            self.events_.push_back({http3_data_event{stream_id,
                {reinterpret_cast<const char*>(data), size}, self.resource_,
                self.own_data_events_}});
        });
    }

    static int deferred_consume_callback(nghttp3_conn*, std::int64_t stream_id,
        std::size_t bytes, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            self.events_.push_back(
                {http3_deferred_consume_event{stream_id, bytes}});
        });
    }

    static int begin_section(impl& self, nghttp3_conn* connection, std::int64_t stream_id, bool trailers) noexcept {
        return callback_guard(self, [&] {
            auto& stream = self.ensure_stream(stream_id);
            if (stream.building_headers_) {
                throw std::logic_error("overlapping nghttp3 header callbacks");
            }
            const auto result = nghttp3_conn_set_stream_user_data(
                connection, stream_id, &stream);
            if (result != 0) {
                throw std::logic_error("failed to bind nghttp3 stream state");
            }
            stream.building_headers_.emplace(
                stream_id, std::pmr::vector<http3_field>(self.resource_), false, trailers, stream.early_data_);
            stream.building_header_size_ = detail::binary_field_section_size(self.max_field_section_size_);
            ++self.active_header_blocks_;
        });
    }

    static int begin_headers_callback(nghttp3_conn* connection,
        std::int64_t stream_id, void* user_data, void*) noexcept {
        return begin_section(*static_cast<impl*>(user_data), connection, stream_id, false);
    }

    static int begin_trailers_callback(nghttp3_conn* connection,
        std::int64_t stream_id, void* user_data, void*) noexcept {
        return begin_section(*static_cast<impl*>(user_data), connection, stream_id, true);
    }

    static int recv_header_callback(nghttp3_conn*, std::int64_t stream_id,
        std::int32_t, nghttp3_rcbuf* name, nghttp3_rcbuf* value,
        std::uint8_t, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        auto* stream = self.find_stream(stream_id);
        if (stream == nullptr || !stream->building_headers_) {
            self.callback_error_ = std::make_exception_ptr(
                std::logic_error("nghttp3 header callback without section"));
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        const auto name_view = rcbuf_view(name);
        const auto value_view = rcbuf_view(value);
        if (!stream->building_header_size_.add(name_view, value_view)) {
            self.connection_error_ = NGHTTP3_H3_EXCESSIVE_LOAD;
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        return callback_guard(self, [&] {
            stream->building_headers_->fields_.emplace_back(
                name_view, value_view, self.resource_);
            if (!stream->building_headers_->trailers_) {
                if (name_view == ":method") {
                    stream->connect_ = value_view == "CONNECT";
                } else if (name_view == ":protocol") {
                    stream->websocket_ = value_view == "websocket";
                }
            }
        });
    }

    static int end_headers_callback(nghttp3_conn*, std::int64_t stream_id,
        int fin, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            auto* stream = self.find_stream(stream_id);
            if (stream == nullptr || !stream->building_headers_) {
                throw std::logic_error("nghttp3 ended unknown field section");
            }
            stream->building_headers_->stream_ended_ = fin != 0;
            if (self.role_ == http3_role::client) {
                if (!self.validate_received_response(*stream, *stream->building_headers_)) {
                    self.reject_received_message(stream_id, *stream);
                    self.abandon_header_block(*stream);
                    return;
                }
            } else {
                stream->received_head_ = true;
            }
            self.events_.push_back({std::move(*stream->building_headers_)});
            self.abandon_header_block(*stream);
        });
    }

    static int stop_sending_callback(nghttp3_conn*, std::int64_t stream_id,
        std::uint64_t error, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        if (self.suppress_transport_callbacks_) {
            return 0;
        }
        return callback_guard(self, [&] {
            self.events_.push_back({http3_transport_action_event{
                                        stream_id, error, http3_transport_action::stop_sending},
                true});
        });
    }

    static int reset_stream_callback(nghttp3_conn*, std::int64_t stream_id,
        std::uint64_t error, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        if (self.suppress_transport_callbacks_) {
            return 0;
        }
        return callback_guard(self, [&] {
            self.events_.push_back({http3_transport_action_event{
                                        stream_id, error, http3_transport_action::reset_stream},
                true});
        });
    }

    static int end_stream_callback(nghttp3_conn*, std::int64_t stream_id,
        void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            if (self.role_ == http3_role::client) {
                auto* stream = self.find_stream(stream_id);
                if (stream == nullptr || stream->read_aborted_) {
                    return;
                }
                if (!stream->received_head_ || !stream->remote_content_.length_complete()) {
                    self.reject_received_message(stream_id, *stream);
                    return;
                }
            }
            self.events_.push_back({http3_stream_end_event{stream_id}});
        });
    }

    static int shutdown_callback(nghttp3_conn*, std::int64_t id, void* user_data) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            self.events_.push_back({http3_goaway_event{id}});
        });
    }

    static int recv_origin_callback(nghttp3_conn*, const std::uint8_t* origin,
        std::size_t size, void* user_data) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        if (size > self.max_field_section_size_ ||
            self.building_origin_bytes_ > self.max_field_section_size_ - size) {
            self.connection_error_ = NGHTTP3_H3_EXCESSIVE_LOAD;
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        return callback_guard(self, [&] {
            if (!self.building_origins_) {
                self.building_origins_.emplace(self.resource_);
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
                self.building_origins_.emplace(self.resource_);
            }
            self.events_.push_back({std::move(*self.building_origins_)});
            self.building_origins_.reset();
            self.building_origin_bytes_ = 0;
        });
    }

    static int stream_close_callback(nghttp3_conn*, std::uint32_t flags,
        std::int64_t stream_id, std::uint64_t receive_error, std::uint64_t send_error,
        void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            std::optional<std::uint64_t> error;
            if ((flags & NGHTTP3_STREAM_CLOSE_FLAG_RX_APP_ERROR_CODE_SET) != 0 &&
                receive_error != NGHTTP3_H3_NO_ERROR) {
                error = receive_error;
            } else if ((flags & NGHTTP3_STREAM_CLOSE_FLAG_TX_APP_ERROR_CODE_SET) != 0 &&
                       send_error != NGHTTP3_H3_NO_ERROR) {
                error = send_error;
            }
            self.events_.push_back({http3_stream_close_event{stream_id, error}});
        });
    }

    static nghttp3_ssize read_body_callback(nghttp3_conn*, std::int64_t,
        nghttp3_vec* vectors, std::size_t vector_count, std::uint32_t* flags,
        void*, void* stream_user_data) noexcept {
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

    [[nodiscard]] http3_status submit_head(std::int64_t stream_id,
        const nghttp3_nv* fields, std::size_t field_count, bool end_stream,
        bool request, std::optional<std::uint64_t> content_length = std::nullopt) {
        if (connection_error_) {
            return http3_status::closed;
        }
        if ((request && role_ != http3_role::client) ||
            (!request && role_ != http3_role::server)) {
            return http3_status::invalid_state;
        }
        auto position = streams_.find(stream_id);
        bool inserted = false;
        if (position == streams_.end()) {
            auto result = streams_.try_emplace(stream_id, resource_);
            position = result.first;
            inserted = result.second;
        }
        auto& stream = position->second;
        if (stream.head_submitted() || stream.write_aborted_) {
            return http3_status::invalid_state;
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
        const int result = request
                               ? nghttp3_conn_submit_request(connection_, stream_id,
                                     fields, field_count,
                                     end_stream ? nullptr : &reader, &stream)
                               : nghttp3_conn_submit_response(connection_, stream_id,
                                     fields, field_count,
                                     end_stream ? nullptr : &reader);
        if (result != 0) {
            if (inserted) {
                streams_.erase(position);
            } else {
                stream.send_phase_ = send_phase::head_pending;
                stream.local_content_ = previous_content;
            }
        }
        throw_if_no_memory(result);
        return status_from(result);
    }

    [[nodiscard]] http3_status submit_raw_head(std::int64_t stream_id,
        std::span<const http3_field_view> fields, bool end_stream, bool request) {
        const auto peer_limit = peer_settings_         ? peer_settings_->max_field_section_size_
                                : remembered_settings_ ? remembered_settings_->max_field_section_size_
                                                       : std::numeric_limits<std::uint64_t>::max();
        detail::binary_field_section_size section_size((std::min)(max_field_section_size_, peer_limit));
        for (const auto& field : fields) {
            if (!section_size.add(field.name_, field.value_)) {
                return http3_status::invalid_state;
            }
        }
        detail::http_content_length_state content_length_state;
        bool has_content_length = false;
        detail::http_header_descriptor_storage<nghttp3_nv> native_fields(resource_);
        native_fields.reserve(fields.size());
        for (const auto& field : fields) {
            if (http_ascii_equals_ignore_case(field.name_, "content-length")) {
                if (content_length_state.parse_field(field.value_) !=
                    detail::http_content_length_parse_status::ok) {
                    return http3_status::invalid_state;
                }
                has_content_length = true;
            }
            native_fields.append(make_nv(field.name_, field.value_, field.sensitive_));
        }
        std::optional<std::uint64_t> content_length;
        if (has_content_length) {
            if (end_stream) {
                if (request && *content_length_state.value() != 0) {
                    return http3_status::invalid_state;
                }
            } else {
                content_length = content_length_state.value();
            }
        }
        return submit_head(
            stream_id, native_fields.view().data(), native_fields.size(), end_stream, request, content_length);
    }

    // A server request stream that exists and has no final head yet.
    [[nodiscard]] std::optional<http3_response_head_error> response_head_admission_error(
        std::int64_t stream_id) noexcept {
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
        if (stream->head_submitted()) {
            return http3_response_head_error::invalid_state;
        }
        return std::nullopt;
    }

    [[nodiscard]] static http3_response_head_error head_error_from(http3_status status) noexcept {
        return status == http3_status::invalid_state ? http3_response_head_error::invalid_message
                                                     : http3_response_head_error::closed;
    }

    [[nodiscard]] std::uint64_t outbound_field_limit() const noexcept {
        const auto peer_limit = peer_settings_         ? peer_settings_->max_field_section_size_
                                : remembered_settings_ ? remembered_settings_->max_field_section_size_
                                                       : std::numeric_limits<std::uint64_t>::max();
        return (std::min)(max_field_section_size_, peer_limit);
    }

    [[nodiscard]] std::optional<http3_response_head_error> submit_final_response(
        std::int64_t stream_id, const http_response& response,
        const detail::http_framed_response_head_plan& plan, bool end_stream,
        std::optional<std::uint64_t> content_length) {
        std::size_t name_bytes = 0;
        for (const auto& header : response.headers()) {
            name_bytes += header.name().size();
        }
        outbound_field_section section(resource_, name_bytes, response.headers().size() + 3);
        if (!detail::append_http_binary_response_fields(section, response, plan, outbound_field_limit())) {
            return http3_response_head_error::invalid_message;
        }
        const auto status = submit_head(
            stream_id, section.data(), section.size(), end_stream, false, content_length);
        if (status != http3_status::accepted) {
            return head_error_from(status);
        }
        const auto parsed = parse_http_priority(response.headers());
        if (const auto* fields = std::get_if<http_priority_fields>(&parsed);
            fields != nullptr && (fields->urgency_ || fields->incremental_)) {
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

    [[nodiscard]] std::variant<stream_state*, http3_status> validate_finish_stream(std::int64_t stream_id) {
        if (connection_error_) {
            return http3_status::closed;
        }
        auto* stream = find_stream(stream_id);
        if (stream == nullptr || !stream->body_open()) {
            return http3_status::invalid_state;
        }
        if (!stream->local_content_.length_complete()) {
            throw std::length_error("HTTP/3 content ended before the declared Content-Length");
        }
        return stream;
    }

    [[nodiscard]] http3_status submit_trailer_nv(
        std::int64_t stream_id, stream_state& stream, const nghttp3_nv* nv, std::size_t count) {
        const int result = nghttp3_conn_submit_trailers(connection_, stream_id, nv, count);
        if (result != 0) {
            throw_if_no_memory(result);
            return status_from(result);
        }
        stream.send_phase_ = send_phase::trailers_ended;
        const int resume_result = nghttp3_conn_resume_stream(connection_, stream_id);
        if (resume_result != 0) {
            throw_if_no_memory(resume_result);
            return status_from(resume_result);
        }
        return http3_status::accepted;
    }

    [[nodiscard]] http3_status submit_stream_shutdown_action(std::int64_t stream_id,
        std::optional<std::uint64_t> application_error_code, http3_transport_action action) noexcept {
        if (application_error_code.has_value()) {
            reset_drained_events(true);
            if (callback_guard(*this, [&] {
                    events_.push_back({http3_transport_action_event{stream_id, *application_error_code, action}, false});
                }) != 0) {
                return fail(NGHTTP3_ERR_CALLBACK_FAILURE).status_;
            }
        }
        return http3_status::accepted;
    }

    http3_role role_;
    std::pmr::memory_resource* resource_;
    detail::http_c_library_memory memory_;
    nghttp3_mem memory_table_;
    std::uint64_t max_field_section_size_;
    std::size_t max_stream_output_bytes_;
    bool own_data_events_;
    bool enable_h3_datagram_;
    std::optional<http3_peer_settings> peer_settings_;
    std::optional<http3_peer_settings> remembered_settings_;
    nghttp3_conn* connection_{nullptr};
    std::pmr::vector<queued_event> events_;
    std::size_t event_offset_{0};
    std::pmr::unordered_map<std::int64_t, stream_state> streams_;
    std::pmr::vector<char> initial_origin_payload_;
    nghttp3_vec initial_origin_vec_{};
    std::optional<std::int64_t> control_stream_id_;
    detail::http3_control_extension_output control_output_;
    std::optional<http_origin_advertisement> building_origins_;
    std::uint64_t building_origin_bytes_{0};
    std::size_t active_header_blocks_{0};
    std::optional<std::uint64_t> connection_error_;
    std::exception_ptr callback_error_;
    bool control_streams_bound_{false};
    bool packet_input_{false};
    bool suppress_transport_callbacks_{false};
    bool drain_final_{false};
};

http3_connection::http3_connection(
    http3_role role, const http3_connection_options& options)
    : impl_(detail::make_http_pmr_object<impl>(options.resource_, role, options)) {}

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

http3_status http3_connection::bind_control_streams(
    std::int64_t control_stream_id, std::int64_t qpack_encoder_stream_id,
    std::int64_t qpack_decoder_stream_id) {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (impl_->control_streams_bound_ || control_stream_id < 0 ||
        qpack_encoder_stream_id < 0 || qpack_decoder_stream_id < 0 ||
        control_stream_id == qpack_encoder_stream_id ||
        control_stream_id == qpack_decoder_stream_id ||
        qpack_encoder_stream_id == qpack_decoder_stream_id) {
        return http3_status::invalid_state;
    }
    int result = nghttp3_conn_bind_control_stream(
        impl_->connection_, control_stream_id);
    if (result == 0) {
        result = nghttp3_conn_bind_qpack_streams(impl_->connection_,
            qpack_encoder_stream_id, qpack_decoder_stream_id);
    }
    impl_->throw_if_no_memory(result);
    if (result != 0) {
        return impl_->status_from(result);
    }
    impl_->control_stream_id_ = control_stream_id;
    impl_->control_streams_bound_ = true;
    return http3_status::accepted;
}

http3_result http3_connection::read_stream(std::int64_t stream_id,
    std::span<const std::byte> bytes, bool fin, std::uint64_t ts_nanoseconds) {
    const bool pending_events = impl_->event_offset_ < impl_->events_.size();
    if (pending_events && !impl_->packet_input_) {
        return {http3_status::events_pending, 0};
    }
    if (impl_->connection_error_) {
        return {http3_status::closed, *impl_->connection_error_};
    }
    impl_->reset_drained_events();
    const auto result = nghttp3_conn_read_stream2(impl_->connection_, stream_id,
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
        fin ? 1 : 0, ts_nanoseconds);
    impl_->rethrow_callback_error();
    impl_->throw_if_no_memory(static_cast<int>(result));
    if (result < 0) {
        return impl_->fail(static_cast<int>(result));
    }
    return {http3_status::accepted, 0, static_cast<std::size_t>(result)};
}

std::optional<http3_event> http3_connection::next_event(bool& local_transport_action) {
    if (impl_->event_offset_ == impl_->events_.size()) {
        local_transport_action = false;
        return std::nullopt;
    }
    auto& entry = impl_->events_[impl_->event_offset_++];
    local_transport_action = entry.local_transport_action_;
    return std::move(entry.event_);
}

std::optional<http3_event> http3_connection::next_event() {
    bool ignored = false;
    return next_event(ignored);
}

void http3_connection::rethrow_if_failed() {
    impl_->rethrow_callback_error();
}

http3_result http3_connection::peek_output(
    std::size_t max_bytes, http3_stream_output& output) {
    if (impl_->connection_error_) {
        return {http3_status::closed, *impl_->connection_error_};
    }
    output.stream_id_ = -1;
    output.buffer_count_ = 0;
    output.fin_ = false;
    if (impl_->control_stream_id_) {
        if (const auto result = impl_->control_output_.peek(impl_->connection_,
                *impl_->control_stream_id_, max_bytes, output)) {
            return *result;
        }
    }

    std::array<nghttp3_vec, 16> vectors;
    std::int64_t stream_id = -1;
    int fin = 0;
    const auto result = nghttp3_conn_writev_stream(impl_->connection_,
        &stream_id, &fin, vectors.data(), vectors.size());
    impl_->rethrow_callback_error();
    impl_->throw_if_no_memory(static_cast<int>(result));
    if (result < 0) {
        return impl_->fail(static_cast<int>(result));
    }
    if (stream_id == -1) {
        return {http3_status::would_block, 0};
    }

    std::size_t available = 0;
    for (nghttp3_ssize index = 0; index < result; ++index) {
        if (vectors[static_cast<std::size_t>(index)].len >
            (std::numeric_limits<std::size_t>::max)() - available) {
            return impl_->fail(NGHTTP3_ERR_H3_INTERNAL_ERROR);
        }
        available += vectors[static_cast<std::size_t>(index)].len;
    }
    if (available != 0 && max_bytes == 0) {
        return {http3_status::would_block, 0};
    }
    const auto offered = (std::min)(available, max_bytes);
    std::size_t remaining = offered;
    for (nghttp3_ssize index = 0; index < result && remaining != 0; ++index) {
        const auto& vector = vectors[static_cast<std::size_t>(index)];
        const auto count = (std::min)(vector.len, remaining);
        if (count != 0) {
            output.buffers_[output.buffer_count_++] = {
                reinterpret_cast<const std::byte*>(vector.base), count};
            remaining -= count;
        }
    }
    output.stream_id_ = stream_id;
    output.fin_ = fin != 0 && offered == available;
    return {http3_status::accepted, 0};
}

http3_status http3_connection::commit_output(
    std::int64_t stream_id, std::size_t bytes) noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (impl_->control_stream_id_.has_value() && stream_id == *impl_->control_stream_id_) {
        return impl_->status_from(impl_->control_output_.commit(impl_->connection_, stream_id, bytes));
    }
    return impl_->status_from(
        nghttp3_conn_add_write_offset(impl_->connection_, stream_id, bytes));
}

http3_status http3_connection::acknowledge_output(
    std::int64_t stream_id, std::uint64_t bytes) noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (impl_->control_stream_id_.has_value() && stream_id == *impl_->control_stream_id_) {
        return impl_->status_from(impl_->control_output_.acknowledge(impl_->connection_, stream_id, bytes));
    }
    return impl_->status_from(nghttp3_conn_add_ack_offset(
        impl_->connection_, stream_id, bytes));
}

http3_status http3_connection::unblock_stream(std::int64_t stream_id) noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (impl_->control_stream_id_.has_value() && stream_id == *impl_->control_stream_id_) {
        impl_->control_output_.unblock();
    }
    return impl_->status_from(
        nghttp3_conn_unblock_stream(impl_->connection_, stream_id));
}

http3_status http3_connection::submit_request(std::int64_t stream_id,
    std::span<const http3_field_view> fields, bool end_stream) {
    const auto status = impl_->submit_raw_head(stream_id, fields, end_stream, true);
    if (status == http3_status::accepted) {
        auto& stream = *impl_->find_stream(stream_id);
        for (const auto& field : fields) {
            if (field.name_ == ":method") {
                stream.request_method_ = classify_http_method(field.value_);
                stream.connect_ = stream.request_method_ == http_known_method::connect;
            }
        }
    }
    return status;
}

http3_status http3_connection::submit_request_head(std::int64_t stream_id,
    const http3_regular_request_head_view& request) {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (impl_->role_ != http3_role::client || stream_id < 0 || (stream_id & 3) != 0 ||
        (!request.streaming_ && request.content_length_.value_or(0) != 0)) {
        return http3_status::invalid_state;
    }
    const auto method = classify_http_method(request.method_);
    const std::optional<std::string_view> authority = request.authority_.empty()
                                                          ? std::nullopt
                                                          : std::optional<std::string_view>(request.authority_);
    std::array<char, http_max_unsigned_decimal_size> length_bytes;
    const auto length = request.content_length_ && request.method_ != "TRACE"
                            ? http_format_unsigned(length_bytes, *request.content_length_)
                            : std::string_view{};
    const auto content = request.streaming_ && request.content_length_ != 0
                             ? http_request_content_indication::will_follow
                             : http_request_content_indication::no_content;
    const auto headers = static_cast<std::span<const http_header_view>>(request.headers_);
    if (!detail::http_binary_is_valid_outbound_regular_request_head(request.method_, method,
            request.scheme_, detail::http_uri_scheme_default_port(request.scheme_), authority,
            request.target_, headers, content, length, request.expectation_, request.field_source_,
            impl_->outbound_field_limit())) {
        return http3_status::invalid_state;
    }
    std::size_t name_bytes = 0;
    for (const auto& header : headers) {
        name_bytes += header.name().size();
    }
    outbound_field_section section(impl_->resource_, name_bytes, headers.size() + 6);
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
    const auto status = impl_->submit_head(stream_id, section.data(), section.size(),
        !request.streaming_, true, request.content_length_);
    if (status == http3_status::accepted) {
        impl_->find_stream(stream_id)->request_method_ = method;
    }
    return status;
}

http3_status http3_connection::submit_request_head(std::int64_t stream_id,
    const http3_connect_request_head_view& request) {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (impl_->role_ != http3_role::client || stream_id < 0 || (stream_id & 3) != 0) {
        return http3_status::invalid_state;
    }
    detail::request_target_view target;
    detail::binary_field_section_size budget(impl_->outbound_field_limit());
    detail::request_field_state fields(detail::request_field_framing::binary);
    const auto headers = static_cast<std::span<const http_header_view>>(request.headers_);
    if (!detail::parse_request_target(http_known_method::connect, request.authority_, target) ||
        !budget.add(":method", "CONNECT") || !budget.add(":authority", request.authority_) ||
        !detail::http_binary_are_valid_outbound_request_headers(
            std::string_view(request.authority_), 0, headers, false, false, budget, fields, request.field_source_)) {
        return http3_status::invalid_state;
    }
    std::size_t name_bytes = 0;
    for (const auto& header : headers) {
        name_bytes += header.name().size();
    }
    outbound_field_section section(impl_->resource_, name_bytes, headers.size() + 2);
    section.add(":method", "CONNECT");
    section.add(":authority", request.authority_);
    for (const auto& header : headers) {
        section.add_lowercase(header.name(), header.value());
    }
    const auto status = impl_->submit_head(stream_id, section.data(), section.size(), false, true);
    if (status == http3_status::accepted) {
        auto& stream = *impl_->find_stream(stream_id);
        stream.request_method_ = http_known_method::connect;
        stream.connect_ = true;
        stream.local_content_.begin_forbidden();
    }
    return status;
}

http3_status http3_connection::submit_request_head(std::int64_t stream_id,
    const http3_extended_connect_request_head_view& request) {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    const auto settings = impl_->peer_settings_ ? impl_->peer_settings_ : impl_->remembered_settings_;
    if (impl_->role_ != http3_role::client || stream_id < 0 || (stream_id & 3) != 0 ||
        !settings || !settings->enable_connect_protocol_) {
        return http3_status::invalid_state;
    }
    const bool websocket = http_ascii_equals_ignore_case(request.protocol_, "websocket");
    const auto protocol = websocket ? std::string_view("websocket") : std::string_view(request.protocol_);
    const auto port = detail::http_uri_scheme_default_port(request.scheme_);
    detail::binary_field_section_size budget(impl_->outbound_field_limit());
    detail::request_field_state fields(detail::request_field_framing::binary);
    const auto headers = static_cast<std::span<const http_header_view>>(request.headers_);
    if (!is_valid_http_header_name(request.protocol_) || !detail::is_valid_uri_scheme(request.scheme_) ||
        (websocket && port == 0) || !detail::is_valid_binary_request_authority(request.scheme_, request.authority_) ||
        !detail::http_binary_is_valid_extended_connect_path(request.scheme_, request.target_) ||
        !budget.add(":method", "CONNECT") || !budget.add(":protocol", protocol) ||
        !budget.add(":scheme", request.scheme_) || !budget.add(":authority", request.authority_) ||
        !budget.add(":path", request.target_) ||
        !detail::http_binary_are_valid_outbound_request_headers(std::string_view(request.authority_),
            port, headers, !websocket, false, budget, fields, request.field_source_, websocket)) {
        return http3_status::invalid_state;
    }
    std::size_t name_bytes = 0;
    for (const auto& header : headers) {
        name_bytes += header.name().size();
    }
    outbound_field_section section(impl_->resource_, name_bytes, headers.size() + 5);
    section.add(":method", "CONNECT");
    section.add(":protocol", protocol);
    section.add(":scheme", request.scheme_);
    section.add(":authority", request.authority_);
    section.add(":path", request.target_);
    for (const auto& header : headers) {
        section.add_lowercase(header.name(), header.value());
    }
    const auto status = impl_->submit_head(stream_id, section.data(), section.size(), false, true);
    if (status == http3_status::accepted) {
        auto& stream = *impl_->find_stream(stream_id);
        stream.request_method_ = http_known_method::connect;
        stream.connect_ = true;
        stream.websocket_ = websocket;
        stream.local_content_.begin_forbidden();
    }
    return status;
}

http3_data_result http3_connection::submit_data(
    std::int64_t stream_id, std::span<const std::byte> bytes) {
    if (impl_->connection_error_) {
        return {http3_status::closed, 0};
    }
    auto* stream = impl_->find_stream(stream_id);
    if (stream == nullptr || !stream->body_open() || bytes.empty()) {
        return {http3_status::invalid_state, 0};
    }
    if (stream->local_content_.check_accept(bytes.size(), false) != detail::http_local_content_check::accepted) {
        throw std::length_error("HTTP/3 DATA exceeds the declared Content-Length");
    }
    const auto budget = available_output_bytes(stream_id);
    const auto accepted = (std::min)(bytes.size(), budget);
    if (accepted == 0) {
        return {http3_status::would_block, 0};
    }
    stream->body_.emplace_back(bytes.first(accepted), impl_->resource_);
    stream->pending_bytes_ += accepted;
    const int result = nghttp3_conn_resume_stream(impl_->connection_, stream_id);
    if (result != 0) {
        stream->body_.pop_back();
        stream->pending_bytes_ -= accepted;
        impl_->throw_if_no_memory(result);
        return {impl_->status_from(result), 0};
    }
    stream->local_content_.commit(accepted);
    return {http3_status::accepted, accepted};
}

std::size_t http3_connection::pending_output_bytes(std::int64_t stream_id) const noexcept {
    const auto position = impl_->streams_.find(stream_id);
    return position == impl_->streams_.end() ? 0 : position->second.pending_bytes_;
}

std::size_t http3_connection::available_output_bytes(std::int64_t stream_id) const noexcept {
    const auto position = impl_->streams_.find(stream_id);
    if (impl_->connection_error_ || position == impl_->streams_.end() ||
        !position->second.body_open() || position->second.local_content_.forbidden() != nullptr) {
        return 0;
    }
    const auto limit = impl_->max_stream_output_bytes_;
    return limit - (std::min)(limit, position->second.pending_bytes_);
}

http3_status http3_connection::finish_stream(std::int64_t stream_id) {
    const auto validated = impl_->validate_finish_stream(stream_id);
    if (const auto* status = std::get_if<http3_status>(&validated)) {
        return *status;
    }
    auto* stream = std::get<impl::stream_state*>(validated);
    stream->send_phase_ = impl::send_phase::body_ended;
    const int result = nghttp3_conn_resume_stream(impl_->connection_, stream_id);
    if (result != 0) {
        stream->send_phase_ = impl::send_phase::body_open;
        impl_->throw_if_no_memory(result);
        return impl_->status_from(result);
    }
    return http3_status::accepted;
}

http3_status http3_connection::submit_trailers(std::int64_t stream_id,
    std::span<const http3_field_view> fields) {
    const auto validated = impl_->validate_finish_stream(stream_id);
    if (const auto* status = std::get_if<http3_status>(&validated)) {
        return *status;
    }
    auto* stream = std::get<impl::stream_state*>(validated);
    detail::binary_field_section_size budget(impl_->outbound_field_limit());
    std::size_t name_bytes = 0;
    for (const auto& field : fields) {
        const auto name = std::string_view(field.name_);
        const auto value = std::string_view(field.value_);
        const bool forbidden = impl_->role_ == http3_role::client
                                   ? detail::is_forbidden_http_request_trailer_name(name)
                                   : detail::is_forbidden_response_trailer_name(name);
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value) ||
            detail::is_forbidden_http_binary_response_field(name) || forbidden ||
            !budget.add(name, value)) {
            return http3_status::invalid_state;
        }
        name_bytes += name.size();
    }
    outbound_field_section section(impl_->resource_, name_bytes, fields.size());
    for (const auto& field : fields) {
        section.add_lowercase(field.name_, field.value_, field.sensitive_);
    }
    return impl_->submit_trailer_nv(stream_id, *stream, section.data(), section.size());
}

std::variant<http_buffered_response_write_plan, http3_response_head_error>
http3_connection::submit_response_head(std::int64_t stream_id, const http_response& response,
    http_buffered_response_write_plan write_plan) {
    if (const auto error = impl_->response_head_admission_error(stream_id)) {
        return *error;
    }
    if (!write_plan.matches_response(response)) {
        return http3_response_head_error::response_plan_mismatch;
    }
    if (detail::http_binary_final_response_control_plan(response).has_value()) {
        return http3_response_head_error::invalid_message;
    }
    const auto head_plan_result = detail::http_framed_buffered_response_head_plan(write_plan, response);
    const auto* head_plan = std::get_if<detail::http_framed_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr) {
        const auto error = std::get<detail::http_framed_response_head_plan_error>(head_plan_result);
        return error == detail::http_framed_response_head_plan_error::response_status_mismatch ||
                       error == detail::http_framed_response_head_plan_error::response_representation_mismatch
                   ? http3_response_head_error::response_plan_mismatch
                   : http3_response_head_error::invalid_message;
    }
    const bool send_body = write_plan.send_body();
    if (const auto error = impl_->submit_final_response(stream_id, response, *head_plan, !send_body,
            send_body ? std::optional<std::uint64_t>(write_plan.content_length()) : std::nullopt)) {
        return *error;
    }
    return write_plan;
}

std::variant<http_response_stream_commit_plan, http3_response_head_error>
http3_connection::submit_streaming_response_head(std::int64_t stream_id,
    http_known_method request_method, http_response head, http_response_trailer_intent trailer_intent) {
    if (const auto error = impl_->response_head_admission_error(stream_id)) {
        return *error;
    }
    auto commit_plan = plan_http_response_stream_commit(
        http_response_stream_framing::http3_frames, request_method, head.status(), trailer_intent);
    if (!commit_plan.trailer_intent_allowed() ||
        detail::http_binary_final_response_control_plan(head).has_value()) {
        return http3_response_head_error::invalid_message;
    }
    auto stream_head = prepare_http_response_stream_head(std::move(head), commit_plan);
    const auto head_plan_result =
        detail::http_framed_streaming_response_head_plan(commit_plan.body_plan(), stream_head.response());
    const auto* head_plan = std::get_if<detail::http_framed_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr) {
        return http3_response_head_error::invalid_message;
    }
    const bool end_stream =
        commit_plan.head_disposition() == http_response_stream_head_disposition::message_ended;
    if (const auto error = impl_->submit_final_response(stream_id, stream_head.response(), *head_plan,
            end_stream, end_stream ? std::nullopt : head_plan->streaming_content_length())) {
        return *error;
    }
    return commit_plan;
}

http3_status http3_connection::submit_interim_response_head(
    std::int64_t stream_id, const http_interim_response_head& response) {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    const auto* stream = impl_->find_stream(stream_id);
    if (impl_->role_ != http3_role::server || stream == nullptr) {
        return stream == nullptr ? http3_status::closed : http3_status::invalid_state;
    }
    if (stream->head_submitted()) {
        return http3_status::invalid_state;
    }
    const auto headers = response.headers();
    std::size_t name_bytes = 0;
    for (const auto& header : headers) {
        name_bytes += header.name().size();
    }
    outbound_field_section section(impl_->resource_, name_bytes, headers.size() + 1);
    if (!detail::append_http_binary_interim_response_fields(section, response, impl_->outbound_field_limit())) {
        throw std::invalid_argument("invalid HTTP/3 interim response field section");
    }
    const int result = nghttp3_conn_submit_info(impl_->connection_, stream_id, section.data(), section.size());
    impl_->throw_if_no_memory(result);
    return impl_->status_from(result);
}

http3_status http3_connection::finish_response(
    std::int64_t stream_id, const http_response_trailer_section& trailers) {
    if (trailers.empty()) {
        return finish_stream(stream_id);
    }
    const auto validated = impl_->validate_finish_stream(stream_id);
    if (const auto* status = std::get_if<http3_status>(&validated)) {
        return *status;
    }
    auto* stream = std::get<impl::stream_state*>(validated);
    const auto fields = trailers.fields();
    std::size_t name_bytes = 0;
    detail::binary_field_section_size budget(impl_->outbound_field_limit());
    for (const auto& field : fields) {
        if (!budget.add(field.name(), field.value())) {
            return http3_status::invalid_state;
        }
        name_bytes += field.name().size();
    }
    outbound_field_section section(impl_->resource_, name_bytes, fields.size());
    for (const auto& field : fields) {
        section.add_lowercase(field.name(), field.value());
    }
    return impl_->submit_trailer_nv(stream_id, *stream, section.data(), section.size());
}

std::optional<http3_response_head_error> http3_connection::submit_connect_response_head(
    std::int64_t stream_id, const http_response& response) {
    if (const auto error = impl_->response_head_admission_error(stream_id)) {
        return error;
    }
    const auto* stream = impl_->find_stream(stream_id);
    if (!stream->received_head_ || !stream->connect_) {
        return http3_response_head_error::invalid_state;
    }
    if (!detail::is_valid_http_binary_connect_response_head(response) ||
        detail::http_binary_final_response_control_plan(response).has_value()) {
        return http3_response_head_error::invalid_message;
    }
    const auto plan = detail::http_framed_connect_response_head_plan(
        plan_http_response_body(http_known_method::connect, response.status()));
    const auto* head = std::get_if<detail::http_framed_response_head_plan>(&plan);
    if (head == nullptr) {
        return http3_response_head_error::invalid_message;
    }
    return impl_->submit_final_response(stream_id, response, *head, false, std::nullopt);
}

std::variant<websocket_extended_connect_negotiation, http3_response_head_error>
http3_connection::submit_websocket_handshake(std::int64_t stream_id, const http_request& request,
    const websocket_handshake_validation_result& validation, websocket_server_handshake_options options) {
    if (const auto error = impl_->response_head_admission_error(stream_id)) {
        return *error;
    }
    const auto* stream = impl_->find_stream(stream_id);
    if (!stream->received_head_ || !stream->connect_ || !stream->websocket_ || validation.accepted() == nullptr) {
        return http3_response_head_error::invalid_state;
    }
    auto negotiation = detail::make_websocket_server_negotiation(request, options, impl_->resource_);
    outbound_field_section section(impl_->resource_, 0, negotiation.response_headers().size() + 4);
    if (!detail::append_http_binary_websocket_response_fields(section, negotiation, impl_->outbound_field_limit())) {
        return http3_response_head_error::invalid_message;
    }
    const auto status = impl_->submit_head(stream_id, section.data(), section.size(), false, false);
    if (status != http3_status::accepted) {
        return impl::head_error_from(status);
    }
    const auto compression = negotiation.compression();
    return websocket_extended_connect_negotiation(std::move(negotiation).take_subprotocol(), compression);
}

http3_result http3_connection::read_datagram(std::span<const std::byte> payload) {
    if (impl_->connection_error_) {
        return {http3_status::closed, *impl_->connection_error_};
    }
    if (events_pending() && !impl_->packet_input_) {
        return {http3_status::events_pending, 0};
    }
    impl_->reset_drained_events();
    const auto parsed = detail::decode_quic_varint(
        {reinterpret_cast<const char*>(payload.data()), payload.size()});
    if (!impl_->enable_h3_datagram_ || parsed.index() != 0 ||
        std::get<0>(parsed).value_ > ((std::uint64_t{1} << 60) - 1)) {
        impl_->connection_error_ = static_cast<std::uint64_t>(http3_error_code::datagram_error);
        return {http3_status::protocol_error, *impl_->connection_error_};
    }
    const auto id = static_cast<std::int64_t>(std::get<0>(parsed).value_ * 4);
    const auto* stream = impl_->find_stream(id);
    if (stream != nullptr && !stream->read_aborted_ &&
        (stream->received_head_ || (impl_->role_ == http3_role::client && stream->head_submitted()))) {
        const auto content = payload.subspan(std::get<0>(parsed).encoded_bytes_);
        impl_->events_.push_back({http3_datagram_event{
            id, std::pmr::string(reinterpret_cast<const char*>(content.data()), content.size(), impl_->resource_)}});
    }
    return {};
}

std::optional<http3_datagram_prefix> http3_connection::datagram_prefix(std::int64_t stream_id) const noexcept {
    const auto* stream = impl_->find_stream(stream_id);
    if (impl_->connection_error_ || !impl_->enable_h3_datagram_ || !impl_->peer_settings_ ||
        !impl_->peer_settings_->h3_datagram_ || stream_id < 0 || (stream_id & 3) != 0 ||
        stream == nullptr || stream->write_aborted_) {
        return std::nullopt;
    }
    http3_datagram_prefix prefix;
    const auto encoded = detail::encode_quic_varint(
        {reinterpret_cast<char*>(prefix.bytes_.data()), prefix.bytes_.size()}, static_cast<std::uint64_t>(stream_id) / 4);
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

http3_status http3_connection::submit_origin_advertisement(
    std::span<const std::string_view> origins) {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (impl_->role_ != http3_role::server || !impl_->control_streams_bound_ ||
        impl_->control_output_.shutdown_requested() || impl_->drain_final_) {
        return http3_status::invalid_state;
    }
    return impl_->control_output_.queue(origins, impl_->max_stream_output_bytes_);
}

http3_status http3_connection::submit_priority_update(
    std::int64_t stream_id, http_priority_fields fields) {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (impl_->role_ != http3_role::client || !impl_->control_streams_bound_ ||
        stream_id < 0 || (stream_id & 3) != 0) {
        return http3_status::invalid_state;
    }
    std::array<char, 32> encoded{};
    const auto size = encode_http_priority(encoded, fields);
    if (size.index() != 0) {
        return http3_status::invalid_state;
    }
    const int result = nghttp3_conn_set_client_stream_priority(
        impl_->connection_, stream_id,
        reinterpret_cast<const std::uint8_t*>(encoded.data()),
        std::get<0>(size));
    impl_->throw_if_no_memory(result);
    return impl_->status_from(result);
}

std::optional<http_priority> http3_connection::stream_priority(
    std::int64_t stream_id) const noexcept {
    if (impl_->connection_error_ || impl_->role_ != http3_role::server ||
        stream_id < 0 || (stream_id & 3) != 0) {
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

http3_status http3_connection::close_stream(std::int64_t stream_id,
    std::optional<std::uint64_t> receive_error,
    std::optional<std::uint64_t> send_error) noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    std::uint32_t flags = 0;
    if (receive_error) {
        flags |= NGHTTP3_STREAM_CLOSE_FLAG_RX_APP_ERROR_CODE_SET;
    }
    if (send_error) {
        flags |= NGHTTP3_STREAM_CLOSE_FLAG_TX_APP_ERROR_CODE_SET;
    }
    const int result = nghttp3_conn_close_stream2(impl_->connection_, flags,
        stream_id, receive_error.value_or(0), send_error.value_or(0));
    if (impl_->callback_error_) {
        // Only an allocation of the close event can fail; the connection
        // cannot report the closure any more.
        impl_->callback_error_ = {};
        impl_->connection_error_ = NGHTTP3_H3_INTERNAL_ERROR;
        return http3_status::protocol_error;
    }
    if (result == 0 || result == NGHTTP3_ERR_STREAM_NOT_FOUND) {
        if (auto* stream = impl_->find_stream(stream_id)) {
            impl_->abandon_header_block(*stream);
            impl_->streams_.erase(stream_id);
        }
        return http3_status::accepted;
    }
    return impl_->status_from(result);
}
http3_status http3_connection::begin_drain() noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    return impl_->status_from(
        nghttp3_conn_submit_shutdown_notice(impl_->connection_));
}

http3_status http3_connection::finish_drain() noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (impl_->drain_final_) {
        return http3_status::accepted;
    }
    const auto status = impl_->status_from(nghttp3_conn_shutdown(impl_->connection_));
    impl_->drain_final_ = status == http3_status::accepted;
    return status;
}

bool http3_connection::drain_final() const noexcept {
    return impl_->drain_final_;
}

bool http3_connection::drained() const noexcept {
    return impl_->drain_final_ && nghttp3_conn_is_drained2(impl_->connection_) != 0;
}

std::optional<std::uint64_t> http3_connection::connection_error() const noexcept {
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

http3_status http3_connection::block_stream(std::int64_t stream_id) noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (impl_->control_stream_id_.has_value() && stream_id == *impl_->control_stream_id_) {
        impl_->control_output_.block();
    }
    nghttp3_conn_block_stream(impl_->connection_, stream_id);
    return http3_status::accepted;
}

http3_status http3_connection::shutdown_stream_read(
    std::int64_t stream_id, std::optional<std::uint64_t> application_error_code) noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (auto* stream = impl_->find_stream(stream_id)) {
        impl_->abandon_header_block(*stream);
        stream->read_aborted_ = true;
    }
    impl_->suppress_transport_callbacks_ = true;
    const int result = nghttp3_conn_shutdown_stream_read(impl_->connection_, stream_id);
    impl_->suppress_transport_callbacks_ = false;
    if (result != 0) {
        return impl_->status_from(result);
    }
    return impl_->submit_stream_shutdown_action(
        stream_id, application_error_code, http3_transport_action::reset_stream);
}

http3_status http3_connection::shutdown_stream_write(
    std::int64_t stream_id, std::optional<std::uint64_t> application_error_code) noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    if (impl_->control_stream_id_.has_value() && stream_id == *impl_->control_stream_id_) {
        impl_->control_output_.shutdown();
    }
    if (auto* stream = impl_->find_stream(stream_id)) {
        stream->write_aborted_ = true;
        stream->body_.clear();
        stream->pending_bytes_ = 0;
    }
    nghttp3_conn_shutdown_stream_write(impl_->connection_, stream_id);
    return impl_->submit_stream_shutdown_action(
        stream_id, application_error_code, http3_transport_action::stop_sending);
}

void http3_connection::set_max_client_streams_bidi(std::uint64_t max_streams) noexcept {
    if (impl_->role_ == http3_role::server) {
        nghttp3_conn_set_max_client_streams_bidi(impl_->connection_, max_streams);
    }
}

}  // namespace ruvia
