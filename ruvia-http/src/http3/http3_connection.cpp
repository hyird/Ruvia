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

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/server/http_final_response_control_plan.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_status.h"

#include "field/http_interim_response_validation.h"
#include "response/http_framed_response_head_plan.h"
#include "server/http_date_cache.h"

namespace ruvia {

namespace {

[[nodiscard]] std::pmr::memory_resource* resolve_resource(
    std::pmr::memory_resource* resource) noexcept {
    return resource != nullptr ? resource : std::pmr::get_default_resource();
}

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
            sensitive ? NGHTTP3_NV_FLAG_NEVER_INDEX : NGHTTP3_NV_FLAG_NONE),
    };
}

// One outbound field section. RFC 9114 §4.2 requires lowercase field names;
// response and trailer names keep the application's spelling, so each is
// lowered into one reserved buffer whose views stay valid until submission.
// nghttp3 copies the section when it is submitted.
class outbound_field_section final {
public:
    outbound_field_section(std::pmr::memory_resource* resource, std::size_t name_bytes,
        std::size_t field_count)
        : names_(resource),
          fields_(resource) {
        names_.reserve(name_bytes);
        fields_.reserve(field_count);
    }

    void add_literal(std::string_view name, std::string_view value) {
        fields_.push_back(make_nv(name, value));
    }

    void add_lowercased(std::string_view name, std::string_view value) {
        const auto offset = names_.size();
        if (names_.capacity() - offset < name.size()) {
            throw std::logic_error("HTTP/3 field section name storage underestimated");
        }
        for (const char byte : name) {
            names_.push_back(static_cast<char>(
                http_ascii_to_lower(static_cast<unsigned char>(byte))));
        }
        fields_.push_back(make_nv(
            std::string_view(names_.data() + offset, name.size()), value));
    }

    [[nodiscard]] const nghttp3_nv* data() const noexcept {
        return fields_.data();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return fields_.size();
    }

private:
    std::pmr::string names_;
    std::pmr::vector<nghttp3_nv> fields_;
};

template <typename field_range_type>
[[nodiscard]] std::size_t total_name_bytes(const field_range_type& fields) noexcept {
    std::size_t bytes = 0;
    for (const auto& field : fields) {
        bytes += field.name().size();
    }
    return bytes;
}

}  // namespace

http3_field::http3_field(std::string_view name, std::string_view value,
    std::pmr::memory_resource* resource)
    : name_(name, resolve_resource(resource)),
      value_(value, resolve_resource(resource)) {}

http3_data_event::http3_data_event(std::int64_t stream_id,
    std::string_view bytes, std::pmr::memory_resource* resource,
    bool own_bytes)
    : stream_id_(stream_id),
      bytes_(bytes),
      owned_bytes_(resolve_resource(resource)),
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
    stream_id_ = other.stream_id_;
    owned_bytes_ = std::move(other.owned_bytes_);
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
    struct body_chunk final {
        body_chunk(std::span<const std::byte> bytes,
            std::pmr::memory_resource* resource)
            : bytes_(reinterpret_cast<const char*>(bytes.data()), bytes.size(), resource) {}

        std::pmr::string bytes_;
        std::size_t acknowledged_{0};
        bool provided_{false};
    };

    struct stream_state final {
        explicit stream_state(std::pmr::memory_resource* resource)
            : body_(resource) {}

        std::pmr::deque<body_chunk> body_;
        // Submitted DATA payload not yet acknowledged by the peer.
        std::size_t pending_bytes_{0};
        // The exact content total a declared length binds the stream to.
        std::optional<std::uint64_t> content_length_;
        std::uint64_t submitted_bytes_{0};
        bool head_submitted_{false};
        bool body_source_{false};
        bool finished_{false};
        bool trailers_submitted_{false};
    };

public:
    impl(http3_role role, const http3_connection_options& options)
        : role_(role),
          resource_(resolve_resource(options.resource_)),
          max_field_section_size_(options.max_field_section_size_),
          max_stream_output_bytes_(options.max_stream_output_bytes_),
          own_data_events_(options.own_data_events_),
          events_(resource_),
          streams_(resource_) {
        if (options.max_concurrent_streams_ == 0 ||
            options.max_stream_output_bytes_ == 0 ||
            options.qpack_max_dtable_capacity_ >
                (std::numeric_limits<std::size_t>::max)() ||
            options.qpack_blocked_streams_ >
                (std::numeric_limits<std::size_t>::max)()) {
            throw std::invalid_argument("invalid HTTP/3 connection options");
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
            value.stream_close2 = stream_close_callback;
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

        const int result = role == http3_role::client
                               ? nghttp3_conn_client_new(
                                     &connection_, &callbacks, &settings, nullptr, this)
                               : nghttp3_conn_server_new(
                                     &connection_, &callbacks, &settings, nullptr, this);
        if (result != 0) {
            if (result == NGHTTP3_ERR_NOMEM) {
                throw std::bad_alloc();
            }
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
        if (result == NGHTTP3_ERR_STREAM_NOT_FOUND ||
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
            throw std::bad_alloc();
        }
    }

    void rethrow_callback_error() {
        if (callback_error_) {
            connection_error_ = NGHTTP3_H3_INTERNAL_ERROR;
            auto error = std::exchange(callback_error_, {});
            std::rethrow_exception(error);
        }
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

    static int acked_stream_data_callback(nghttp3_conn*, std::int64_t stream_id,
        std::uint64_t bytes, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            auto position = self.streams_.find(stream_id);
            if (position == self.streams_.end()) {
                return;
            }
            auto& stream = position->second;
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
            self.events_.emplace_back(http3_data_event{stream_id,
                {reinterpret_cast<const char*>(data), size}, self.resource_,
                self.own_data_events_});
        });
    }

    static int deferred_consume_callback(nghttp3_conn*, std::int64_t stream_id,
        std::size_t bytes, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            self.events_.emplace_back(
                http3_deferred_consume_event{stream_id, bytes});
        });
    }

    static int begin_section(impl& self, nghttp3_conn* connection, std::int64_t stream_id, bool trailers) noexcept {
        return callback_guard(self, [&] {
            if (self.building_headers_) {
                throw std::logic_error("overlapping nghttp3 header callbacks");
            }
            auto& stream = self.ensure_stream(stream_id);
            const auto result = nghttp3_conn_set_stream_user_data(
                connection, stream_id, &stream);
            if (result != 0) {
                throw std::logic_error("failed to bind nghttp3 stream state");
            }
            self.building_headers_.emplace(http3_headers_event{
                stream_id, std::pmr::vector<http3_field>(self.resource_), false, trailers});
            self.building_header_bytes_ = 0;
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
        if (!self.building_headers_ ||
            self.building_headers_->stream_id_ != stream_id) {
            self.callback_error_ = std::make_exception_ptr(
                std::logic_error("nghttp3 header callback without section"));
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        const auto name_view = rcbuf_view(name);
        const auto value_view = rcbuf_view(value);
        constexpr std::uint64_t field_overhead = 32;
        const auto name_bytes = static_cast<std::uint64_t>(name_view.size());
        const auto value_bytes = static_cast<std::uint64_t>(value_view.size());
        if (field_overhead > self.max_field_section_size_ ||
            name_bytes > self.max_field_section_size_ - field_overhead ||
            value_bytes >
                self.max_field_section_size_ - field_overhead - name_bytes) {
            self.connection_error_ = NGHTTP3_H3_EXCESSIVE_LOAD;
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        const auto field_bytes = field_overhead + name_bytes + value_bytes;
        if (self.building_header_bytes_ >
            self.max_field_section_size_ - field_bytes) {
            self.connection_error_ = NGHTTP3_H3_EXCESSIVE_LOAD;
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        return callback_guard(self, [&] {
            self.building_headers_->fields_.emplace_back(
                name_view, value_view, self.resource_);
            self.building_header_bytes_ += field_bytes;
        });
    }

    static int end_headers_callback(nghttp3_conn*, std::int64_t stream_id,
        int fin, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            if (!self.building_headers_ ||
                self.building_headers_->stream_id_ != stream_id) {
                throw std::logic_error("nghttp3 ended unknown field section");
            }
            self.building_headers_->stream_ended_ = fin != 0;
            self.events_.emplace_back(std::move(*self.building_headers_));
            self.building_headers_.reset();
            self.building_header_bytes_ = 0;
        });
    }

    static int stop_sending_callback(nghttp3_conn*, std::int64_t stream_id,
        std::uint64_t error, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            self.events_.emplace_back(http3_transport_action_event{
                stream_id, error, http3_transport_action::stop_sending});
        });
    }

    static int reset_stream_callback(nghttp3_conn*, std::int64_t stream_id,
        std::uint64_t error, void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            self.events_.emplace_back(http3_transport_action_event{
                stream_id, error, http3_transport_action::reset_stream});
        });
    }

    static int end_stream_callback(nghttp3_conn*, std::int64_t stream_id,
        void* user_data, void*) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            self.events_.emplace_back(http3_stream_end_event{stream_id});
        });
    }

    static int shutdown_callback(nghttp3_conn*, std::int64_t id, void* user_data) noexcept {
        auto& self = *static_cast<impl*>(user_data);
        return callback_guard(self, [&] {
            self.events_.emplace_back(http3_goaway_event{id});
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
            self.events_.emplace_back(http3_stream_close_event{stream_id, error});
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
        if (stream.finished_ && all_provided) {
            *flags |= NGHTTP3_DATA_FLAG_EOF;
            if (stream.trailers_submitted_) {
                *flags |= NGHTTP3_DATA_FLAG_NO_END_STREAM;
            }
        }
        if (count == 0 && !stream.finished_) {
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
        if (stream.head_submitted_) {
            return http3_status::invalid_state;
        }
        stream.head_submitted_ = true;
        stream.body_source_ = !end_stream;
        stream.finished_ = end_stream;
        stream.content_length_ = content_length;
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
                stream.head_submitted_ = false;
                stream.body_source_ = false;
                stream.finished_ = false;
                stream.content_length_.reset();
            }
        }
        throw_if_no_memory(result);
        return status_from(result);
    }

    [[nodiscard]] http3_status submit_raw_head(std::int64_t stream_id,
        std::span<const http3_field_view> fields, bool end_stream, bool request) {
        std::pmr::vector<nghttp3_nv> native_fields(resource_);
        native_fields.reserve(fields.size());
        for (const auto& field : fields) {
            native_fields.push_back(make_nv(field.name_, field.value_, field.sensitive_));
        }
        return submit_head(stream_id, native_fields.data(), native_fields.size(), end_stream, request);
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
        if (stream == nullptr) {
            return http3_response_head_error::closed;
        }
        if (stream->head_submitted_) {
            return http3_response_head_error::invalid_state;
        }
        return std::nullopt;
    }

    // Admission already proved the stream open and headless, so a failed
    // submission means the stream or the connection is gone.
    [[nodiscard]] static http3_response_head_error head_error_from(http3_status) noexcept {
        return http3_response_head_error::closed;
    }

    [[nodiscard]] std::optional<http3_response_head_error> submit_final_response(
        std::int64_t stream_id, const http_response& response,
        const detail::http_framed_response_head_plan& plan, bool end_stream,
        std::optional<std::uint64_t> content_length) {
        // The writer frames the content (RFC 9114 §4.1); the plan alone emits
        // Content-Length and the chunked hint has no HTTP/3 form.
        constexpr auto writer_framed_bits =
            detail::response_header_content_length | detail::response_header_transfer_encoding;
        const auto known_bits = detail::response_known_header_bits(response);
        const auto status_token = detail::http_status_code_token(plan.body_plan().response_status());
        std::string_view generated_date;
        if ((known_bits & detail::response_header_date) == 0) {
            generated_date = detail::cached_date_value();
        }
        std::array<char, http_max_unsigned_decimal_size> content_length_bytes;
        std::string_view content_length_value;
        if (const auto planned_length = plan.content_length()) {
            content_length_value = http_format_unsigned(content_length_bytes, *planned_length);
        }

        outbound_field_section section(
            resource_, total_name_bytes(response.headers()), response.headers().size() + 3);
        section.add_literal(":status", detail::http_status_code_token_view(status_token));
        for (const auto& header : response.headers()) {
            if ((detail::http_response_header_access::known_bit(header) & writer_framed_bits) != 0) {
                continue;
            }
            section.add_lowercased(header.name(), header.value());
        }
        if (!generated_date.empty()) {
            section.add_literal("date", generated_date);
        }
        if (!content_length_value.empty()) {
            section.add_literal("content-length", content_length_value);
        }
        const auto status = submit_head(
            stream_id, section.data(), section.size(), end_stream, false, content_length);
        if (status != http3_status::accepted) {
            return head_error_from(status);
        }
        return std::nullopt;
    }

    http3_role role_;
    std::pmr::memory_resource* resource_;
    std::uint64_t max_field_section_size_;
    std::size_t max_stream_output_bytes_;
    bool own_data_events_;
    nghttp3_conn* connection_{nullptr};
    std::pmr::vector<http3_event> events_;
    std::size_t event_offset_{0};
    std::pmr::unordered_map<std::int64_t, stream_state> streams_;
    std::optional<http3_headers_event> building_headers_;
    std::uint64_t building_header_bytes_{0};
    std::optional<std::uint64_t> connection_error_;
    std::exception_ptr callback_error_;
    bool control_streams_bound_{false};
    bool packet_input_{false};
    bool drain_final_{false};
};

http3_connection::http3_connection(
    http3_role role, const http3_connection_options& options)
    : impl_(std::make_unique<impl>(role, options)) {}

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
    if (!pending_events) {
        impl_->events_.clear();
        impl_->event_offset_ = 0;
    }
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

std::optional<http3_event> http3_connection::next_event() {
    if (impl_->event_offset_ == impl_->events_.size()) {
        return std::nullopt;
    }
    return std::move(impl_->events_[impl_->event_offset_++]);
}

http3_result http3_connection::peek_output(
    std::size_t max_bytes, http3_stream_output& output) {
    if (impl_->connection_error_) {
        return {http3_status::closed, *impl_->connection_error_};
    }
    output.stream_id_ = -1;
    output.buffer_count_ = 0;
    output.fin_ = false;

    std::array<nghttp3_vec, 16> vectors;
    std::int64_t stream_id = -1;
    int fin = 0;
    const auto result = nghttp3_conn_writev_stream(impl_->connection_,
        &stream_id, &fin, vectors.data(), vectors.size());
    impl_->rethrow_callback_error();
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
    return impl_->status_from(
        nghttp3_conn_add_write_offset(impl_->connection_, stream_id, bytes));
}

http3_status http3_connection::acknowledge_output(
    std::int64_t stream_id, std::uint64_t bytes) noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    return impl_->status_from(nghttp3_conn_add_ack_offset(
        impl_->connection_, stream_id, bytes));
}

http3_status http3_connection::unblock_stream(std::int64_t stream_id) noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    return impl_->status_from(
        nghttp3_conn_unblock_stream(impl_->connection_, stream_id));
}

http3_status http3_connection::submit_request(std::int64_t stream_id,
    std::span<const http3_field_view> fields, bool end_stream) {
    return impl_->submit_raw_head(stream_id, fields, end_stream, true);
}

http3_status http3_connection::submit_response(std::int64_t stream_id,
    std::span<const http3_field_view> fields, bool end_stream) {
    return impl_->submit_raw_head(stream_id, fields, end_stream, false);
}

http3_data_result http3_connection::submit_data(
    std::int64_t stream_id, std::span<const std::byte> bytes) {
    if (impl_->connection_error_) {
        return {http3_status::closed, 0};
    }
    auto* stream = impl_->find_stream(stream_id);
    if (stream == nullptr || !stream->body_source_ || stream->finished_ || bytes.empty()) {
        return {http3_status::invalid_state, 0};
    }
    if (stream->content_length_.has_value() &&
        bytes.size() > *stream->content_length_ - stream->submitted_bytes_) {
        throw std::length_error("HTTP/3 DATA exceeds the declared Content-Length");
    }
    const auto budget = impl_->max_stream_output_bytes_ - (std::min)(impl_->max_stream_output_bytes_, stream->pending_bytes_);
    const auto accepted = (std::min)(bytes.size(), budget);
    if (accepted == 0) {
        return {http3_status::would_block, 0};
    }
    stream->body_.emplace_back(bytes.first(accepted), impl_->resource_);
    stream->pending_bytes_ += accepted;
    stream->submitted_bytes_ += accepted;
    const auto status = impl_->status_from(
        nghttp3_conn_resume_stream(impl_->connection_, stream_id));
    return {status, status == http3_status::accepted ? accepted : 0};
}

std::size_t http3_connection::pending_output_bytes(std::int64_t stream_id) const noexcept {
    const auto position = impl_->streams_.find(stream_id);
    return position == impl_->streams_.end() ? 0 : position->second.pending_bytes_;
}

http3_status http3_connection::finish_stream(std::int64_t stream_id) {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    auto* stream = impl_->find_stream(stream_id);
    if (stream == nullptr || !stream->body_source_ || stream->finished_) {
        return http3_status::invalid_state;
    }
    if (stream->content_length_.has_value() && stream->submitted_bytes_ != *stream->content_length_) {
        throw std::length_error("HTTP/3 content ended before the declared Content-Length");
    }
    stream->finished_ = true;
    return impl_->status_from(
        nghttp3_conn_resume_stream(impl_->connection_, stream_id));
}

http3_status http3_connection::submit_trailers(std::int64_t stream_id,
    std::span<const http3_field_view> fields) {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    auto* stream = impl_->find_stream(stream_id);
    if (stream == nullptr || !stream->body_source_ || stream->finished_) {
        return http3_status::invalid_state;
    }
    std::pmr::vector<nghttp3_nv> native_fields(impl_->resource_);
    native_fields.reserve(fields.size());
    for (const auto& field : fields) {
        native_fields.push_back(make_nv(field.name_, field.value_, field.sensitive_));
    }
    const int result = nghttp3_conn_submit_trailers(impl_->connection_, stream_id,
        native_fields.data(), native_fields.size());
    if (result != 0) {
        impl_->throw_if_no_memory(result);
        return impl_->status_from(result);
    }
    stream->trailers_submitted_ = true;
    stream->finished_ = true;
    return impl_->status_from(
        nghttp3_conn_resume_stream(impl_->connection_, stream_id));
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
    if (detail::http2_final_response_control_plan(response).has_value()) {
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
        detail::http2_final_response_control_plan(head).has_value()) {
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
    if (stream->head_submitted_) {
        return http3_status::invalid_state;
    }
    const auto headers = response.headers();
    detail::http_interim_response_sender_validator validator;
    std::size_t name_bytes = 0;
    for (const auto& header : headers) {
        if (validator.validate(header.name(), header.value()) !=
                detail::http_interim_response_header_validation_status::ok ||
            detail::is_forbidden_http_binary_response_field(header.name())) {
            throw std::invalid_argument("invalid HTTP/3 interim response field");
        }
        name_bytes += header.name().size();
    }
    const auto status_token = detail::http_status_code_token(response.status());
    outbound_field_section section(impl_->resource_, name_bytes, headers.size() + 1);
    section.add_literal(":status", detail::http_status_code_token_view(status_token));
    for (const auto& header : headers) {
        section.add_lowercased(header.name(), header.value());
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
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    auto* stream = impl_->find_stream(stream_id);
    if (stream == nullptr || !stream->body_source_ || stream->finished_) {
        return http3_status::invalid_state;
    }
    if (stream->content_length_.has_value() && stream->submitted_bytes_ != *stream->content_length_) {
        throw std::length_error("HTTP/3 content ended before the declared Content-Length");
    }
    const auto fields = trailers.fields();
    outbound_field_section section(impl_->resource_, total_name_bytes(fields), fields.size());
    for (const auto& field : fields) {
        section.add_lowercased(field.name(), field.value());
    }
    const int result = nghttp3_conn_submit_trailers(impl_->connection_, stream_id, section.data(), section.size());
    if (result != 0) {
        impl_->throw_if_no_memory(result);
        return impl_->status_from(result);
    }
    stream->trailers_submitted_ = true;
    stream->finished_ = true;
    return impl_->status_from(nghttp3_conn_resume_stream(impl_->connection_, stream_id));
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
        impl_->streams_.erase(stream_id);
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
    nghttp3_conn_block_stream(impl_->connection_, stream_id);
    return http3_status::accepted;
}

http3_status http3_connection::shutdown_stream_read(
    std::int64_t stream_id) noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    return impl_->status_from(
        nghttp3_conn_shutdown_stream_read(impl_->connection_, stream_id));
}

http3_status http3_connection::shutdown_stream_write(
    std::int64_t stream_id) noexcept {
    if (impl_->connection_error_) {
        return http3_status::closed;
    }
    nghttp3_conn_shutdown_stream_write(impl_->connection_, stream_id);
    return http3_status::accepted;
}

void http3_connection::set_max_client_streams_bidi(std::uint64_t max_streams) noexcept {
    if (impl_->role_ == http3_role::server) {
        nghttp3_conn_set_max_client_streams_bidi(impl_->connection_, max_streams);
    }
}

}  // namespace ruvia
